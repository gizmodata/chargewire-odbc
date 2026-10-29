"""Connection strings written before the protocol's rename to Chargewire keep working.

The old URI scheme and default mount path are spelled only here, in
chargewire/pre_rename_compat.cc and in its unit test.
"""
import os
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


if __name__ == '__main__':
    unittest.main(verbosity=2)
