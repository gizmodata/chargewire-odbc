"""ODBC regressions particular to native Warp SQL (run against a real server)."""
import concurrent.futures
import decimal
import os
import time
import unittest

import pyodbc
import requests

pyodbc.pooling = False
DRIVER = os.environ['HARDCHARGER_DRIVER']
HOST = os.environ.get('HARDCHARGER_HOST', '127.0.0.1')
PORT = os.environ.get('HARDCHARGER_PORT', '31337')
TLS = os.environ.get('HARDCHARGER_USE_ENCRYPTION', 'false')
CA = os.environ.get('HARDCHARGER_TRUSTED_CERTS', '')
BASE = f'Driver={DRIVER};host={HOST};port={PORT};useEncryption={TLS};'
if CA:
    BASE += f'trustedCerts={CA};'
AUTH = 'uid=hardcharger_user;pwd=hardcharger_password;'


def connect(extra='', *, autocommit=True):
    return pyodbc.connect(BASE + AUTH + extra, autocommit=autocommit, timeout=5)


class WarpTests(unittest.TestCase):
    def test_branding_and_version(self):
        with connect() as c:
            self.assertIn('Hardcharger', c.getinfo(pyodbc.SQL_DRIVER_NAME))
            self.assertEqual(c.getinfo(pyodbc.SQL_DBMS_NAME), 'Hardcharger')
            self.assertEqual(c.getinfo(pyodbc.SQL_DRIVER_VER), '01.00.0000')

    def test_unicode_parameters_and_results(self):
        with connect() as c:
            for value in ['Olá, São Paulo', '東京', '🏎️ Hardcharger 🚀']:
                self.assertEqual(c.execute('SELECT CAST(? AS VARCHAR)', value).fetchval(), value)

    def test_decimal_binary_null_and_dates(self):
        with connect() as c:
            row = c.execute("SELECT 12345.67::DECIMAL(12,2), from_hex('0001ff'), NULL::VARCHAR, DATE '2026-10-25'").fetchone()
            self.assertEqual(row[0], decimal.Decimal('12345.67'))
            self.assertEqual(bytes(row[1]), b'\x00\x01\xff')
            self.assertIsNone(row[2])
            self.assertEqual(str(row[3]), '2026-10-25')

    def test_bad_credentials_and_bad_catalog_return(self):
        with self.assertRaises(pyodbc.Error):
            pyodbc.connect(BASE + 'uid=hardcharger_user;pwd=wrong;', timeout=3)
        with self.assertRaises(pyodbc.Error):
            connect('defaultCatalog=no_such_catalog;')

    def test_sql_errors_and_recovery(self):
        with connect() as c:
            with self.assertRaises(pyodbc.Error):
                c.execute('SELECT * FROM no_such_table_odbc')
            self.assertEqual(c.execute('SELECT 42').fetchval(), 42)

    def test_prepared_reexecution(self):
        with connect() as c:
            cursor = c.cursor()
            for i in range(10):
                self.assertEqual(cursor.execute('SELECT CAST(? AS BIGINT) + 1', i).fetchval(), i + 1)

    def test_large_stream_and_early_close(self):
        with connect() as c:
            cur = c.execute('SELECT i FROM range(100000) t(i)')
            self.assertEqual(sum(r[0] for r in cur), 4999950000)
            cur.execute('SELECT i FROM range(1000000) t(i)')
            cur.fetchone()
            cur.close()
            self.assertEqual(c.execute('SELECT 7').fetchval(), 7)

    def test_row_count_and_cross_reference(self):
        with connect() as c:
            c.execute('CREATE TEMP TABLE odbc_parent (a INTEGER PRIMARY KEY)')
            c.execute('CREATE TEMP TABLE odbc_child (a INTEGER REFERENCES odbc_parent(a))')
            self.assertEqual(c.execute('INSERT INTO odbc_parent VALUES (1), (2)').rowcount, 2)
            rows = c.cursor().foreignKeys(table='odbc_parent', foreignTable='odbc_child').fetchall()
            self.assertEqual(len(rows), 1)

    def test_access_token_aliases(self):
        scheme = 'https' if TLS == 'true' else 'http'
        response = requests.post(f'{scheme}://{HOST}:{PORT}/warp/v1/auth/token',
                                 json={'grant_type': 'password', 'username': 'hardcharger_user', 'password': 'hardcharger_password'},
                                 verify=CA or True, timeout=5)
        response.raise_for_status()
        token = response.json()['access_token']
        with pyodbc.connect(BASE + f'authType=access_token;token={token};', autocommit=True) as c:
            self.assertEqual(c.execute('SELECT 11').fetchval(), 11)
        with pyodbc.connect(BASE + f'authType=token;token={token};', autocommit=True) as c:
            self.assertEqual(c.execute('SELECT 12').fetchval(), 12)

    def test_cancel_running_query(self):
        with connect() as c:
            cur = c.cursor()
            def run():
                try:
                    cur.execute('SELECT sum(a.i*b.i) FROM range(10000000) a(i), range(10000000) b(i)').fetchone()
                except pyodbc.Error:
                    return True
                return False
            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                future = pool.submit(run)
                time.sleep(0.3)
                cur.cancel()
                self.assertTrue(future.result(timeout=10))
            self.assertEqual(c.execute('SELECT 13').fetchval(), 13)

    def test_query_timeout(self):
        with connect() as c:
            c.timeout = 1
            with self.assertRaises(pyodbc.Error):
                c.execute('SELECT sum(a.i*b.i) FROM range(10000000) a(i), range(10000000) b(i)').fetchone()
            self.assertEqual(c.execute('SELECT 14').fetchval(), 14)

    def test_tls_certificate_rejection(self):
        if TLS != 'true':
            return
        with self.assertRaises(pyodbc.Error):
            pyodbc.connect(f'Driver={DRIVER};host={HOST};port={PORT};useEncryption=true;' + AUTH, timeout=3)


if __name__ == '__main__':
    unittest.main(verbosity=2)
