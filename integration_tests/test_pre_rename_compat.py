"""Connection strings and servers from before the protocol's rename to Chargewire keep working.

The old URI scheme and default mount path are spelled only here, in
chargewire/pre_rename_compat.cc and in its unit test. PreRenameServer runs when
CHARGEWIRE_ODBC_TEST_LEGACY_SERVER_BINARY names a reference server built before the
rename (the SDK's warp-duckdb); it starts that server itself, over plain HTTP.
"""
import os
import pathlib
import socket
import subprocess
import tempfile
import time
import unittest

import pyodbc

pyodbc.pooling = False
DRIVER = os.environ['HARDCHARGER_DRIVER']
HOST = os.environ.get('HARDCHARGER_HOST', '127.0.0.1')
PORT = os.environ.get('HARDCHARGER_PORT', '31337')
TLS = os.environ.get('HARDCHARGER_USE_ENCRYPTION', 'false')
CA = os.environ.get('HARDCHARGER_TRUSTED_CERTS', '')
AUTH = 'uid=hardcharger_user;pwd=hardcharger_password;'
TRUST = f'trustedCerts={CA};' if CA else ''


def query(options):
    with pyodbc.connect(f'Driver={DRIVER};' + options + AUTH + TRUST, autocommit=True, timeout=5) as c:
        dbms = c.getinfo(pyodbc.SQL_DBMS_NAME)
        return dbms, c.execute('SELECT CAST(? AS VARCHAR), 40 + 2', 'before the rename').fetchone()


class PreRenameConnectionStrings(unittest.TestCase):
    def check(self, options):
        dbms, row = query(options)
        self.assertEqual(tuple(row), ('before the rename', 42))
        # Server information is read through the current protocol, not a degraded fallback.
        self.assertEqual(dbms, os.environ.get('ODBC_EXPECTED_DBMS_NAME', 'Hardcharger'))

    def test_old_uri_scheme(self):
        self.check(f'uri=warp://{HOST}:{PORT}' + ('' if TLS == 'true' else '?tls=false') + ';')

    def test_old_default_mount_path(self):
        scheme = 'https' if TLS == 'true' else 'http'
        self.check(f'uri={scheme}://{HOST}:{PORT}/warp;')
        self.check(f'host={HOST};port={PORT};useEncryption={TLS};basePath=/warp;')


LEGACY_SERVER = os.environ.get('CHARGEWIRE_ODBC_TEST_LEGACY_SERVER_BINARY', '')


@unittest.skipUnless(LEGACY_SERVER, 'CHARGEWIRE_ODBC_TEST_LEGACY_SERVER_BINARY is not set')
class PreRenameServer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='chargewire-odbc-legacy-')
        with socket.socket() as s:
            s.bind(('127.0.0.1', 0))
            cls.port = s.getsockname()[1]
        env = dict(os.environ, WARP_DUCKDB_PASSWORD='hardcharger_password')
        cls.log = pathlib.Path(cls.tmp.name) / 'server.log'
        with cls.log.open('w') as out:
            cls.server = subprocess.Popen([LEGACY_SERVER, '--host', '127.0.0.1', '--port', str(cls.port),
                                           '--database', str(pathlib.Path(cls.tmp.name) / 'legacy.duckdb'),
                                           '--username', 'hardcharger_user'],
                                          env=env, stdout=out, stderr=subprocess.STDOUT)
        for _ in range(100):
            if cls.server.poll() is not None:
                raise RuntimeError('legacy server exited: ' + cls.log.read_text())
            try:
                with socket.create_connection(('127.0.0.1', cls.port), timeout=.2):
                    return
            except OSError:
                time.sleep(.1)
        cls.tearDownClass()
        raise RuntimeError('legacy server not ready')

    @classmethod
    def tearDownClass(cls):
        cls.server.terminate()
        try:
            cls.server.wait(timeout=15)
        except subprocess.TimeoutExpired:
            cls.server.kill()
            cls.server.wait()
        cls.tmp.cleanup()

    def connect(self, options=None, *, autocommit=True):
        options = options or f'host=127.0.0.1;port={self.port};useEncryption=false;'
        return pyodbc.connect(f'Driver={DRIVER};' + options + AUTH, autocommit=autocommit, timeout=5)

    def test_connect_every_way(self):
        for options in (None, f'uri=chargewire://127.0.0.1:{self.port}?tls=false;',
                        f'uri=warp://127.0.0.1:{self.port}?tls=false;', f'uri=http://127.0.0.1:{self.port}/warp;',
                        f'uri=http://127.0.0.1:{self.port};'):
            with self.subTest(options=options), self.connect(options) as c:
                # The server's own discovery document was read, not a fallback.
                self.assertEqual(c.getinfo(pyodbc.SQL_DBMS_NAME), 'warp-duckdb')
                self.assertEqual(c.execute('SELECT 40 + 2').fetchval(), 42)

    def test_prepared_parameters_and_types(self):
        with self.connect() as c:
            cur = c.cursor()
            for i in range(3):
                self.assertEqual(tuple(cur.execute('SELECT CAST(? AS VARCHAR), CAST(? AS BIGINT) + 1', '東京', i).fetchone()),
                                 ('東京', i + 1))
            self.assertEqual(sum(r[0] for r in c.execute('SELECT i FROM range(100000) t(i)')), 4999950000)

    def test_sql_error_has_sqlstate(self):
        with self.connect() as c:
            with self.assertRaises(pyodbc.Error) as error:
                c.execute('SELECT * FROM no_such_table_odbc')
            self.assertEqual(error.exception.args[0], '42P01')
            self.assertEqual(c.execute('SELECT 7').fetchval(), 7)

    def test_tables_and_columns(self):
        with self.connect() as c:
            c.execute('CREATE TABLE legacy_meta (id INTEGER PRIMARY KEY, label VARCHAR)')
            self.assertEqual([r.table_name for r in c.cursor().tables(table='legacy_meta')], ['legacy_meta'])
            self.assertEqual([r.column_name for r in c.cursor().columns(table='legacy_meta')], ['id', 'label'])

    def test_transaction_rollback(self):
        with self.connect(autocommit=False) as c:
            c.execute('CREATE TABLE legacy_txn (i INTEGER)')
            c.commit()
            c.execute('INSERT INTO legacy_txn VALUES (1)')
            c.rollback()
            self.assertEqual(c.execute('SELECT count(*) FROM legacy_txn').fetchval(), 0)

    def test_query_timeout(self):
        with self.connect() as c:
            c.timeout = 1
            with self.assertRaises(pyodbc.Error) as error:
                c.execute('SELECT sum(a.i*b.i) FROM range(10000000) a(i), range(10000000) b(i)').fetchone()
            self.assertIn(error.exception.args[0], {'HYT00', '57014'})
            self.assertEqual(c.execute('SELECT 14').fetchval(), 14)


if __name__ == '__main__':
    unittest.main(verbosity=2)
