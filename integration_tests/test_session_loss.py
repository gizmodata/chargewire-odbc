"""A session the server ended because its transaction sat idle (Chargewire 2.1 §5.6/§5.7).

Run against a server started with a 1 s idle-in-transaction timeout (scripts/test-live.py
does this). The driver never replaces a lost session: the statement that finds the session
ended fails with SQLSTATE 40000 naming the reason, and nothing runs again in autocommit on a
new session (which would commit it on its own after the transaction's earlier statements
were rolled back).
"""
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
# The server ends a session within 5 s after its 1 s timeout passes.
IDLE_SECONDS = 8


SCHEME = 'https' if TLS == 'true' else 'http'
API = f'{SCHEME}://{HOST}:{PORT}/chargewire/v1'
HARDCHARGER = os.environ.get('ODBC_EXPECTED_DBMS_NAME', 'Hardcharger') == 'Hardcharger'


def connect(*, autocommit=True):
    return pyodbc.connect(BASE + AUTH, autocommit=autocommit, timeout=5)


def rows(table):
    with connect() as c:
        return c.execute(f'SELECT count(*) FROM {table}').fetchval()


class SessionLossTests(unittest.TestCase):
    def assert_idle_in_transaction(self, error):
        state, message = error.exception.args[0], error.exception.args[1]
        self.assertEqual(state, '40000', message)
        self.assertIn('idle_in_transaction', message)
        self.assertIn('rolled back', message)

    def test_begin_in_sql(self):
        # The driver can't see a transaction begun in SQL; only the server's reason says one was lost.
        c = connect()
        c.execute('CREATE OR REPLACE TABLE odbc_idle_py_sql (i INTEGER)')
        c.execute('BEGIN')
        c.execute('INSERT INTO odbc_idle_py_sql VALUES (1)')
        time.sleep(IDLE_SECONDS)
        with self.assertRaises(pyodbc.Error) as error:
            c.execute('INSERT INTO odbc_idle_py_sql VALUES (2)')
        self.assert_idle_in_transaction(error)
        # The connection stays dead: nothing runs on it again, in autocommit or otherwise.
        with self.assertRaises(pyodbc.Error):
            c.execute('INSERT INTO odbc_idle_py_sql VALUES (3)')
        c.close()
        self.assertEqual(rows('odbc_idle_py_sql'), 0)

    def test_autocommit_off(self):
        c = connect(autocommit=False)
        c.execute('CREATE OR REPLACE TABLE odbc_idle_py_manual (i INTEGER)')
        c.commit()
        c.execute('INSERT INTO odbc_idle_py_manual VALUES (1)')
        time.sleep(IDLE_SECONDS)
        with self.assertRaises(pyodbc.Error) as error:
            c.execute('INSERT INTO odbc_idle_py_manual VALUES (2)')
        self.assert_idle_in_transaction(error)
        # The server rolled the transaction back already; rolling it back again succeeds.
        c.rollback()
        c.close()
        self.assertEqual(rows('odbc_idle_py_manual'), 0)

    def end_session(self, how):
        """Open a connection, end its session from outside as `how` says, and return it."""
        token = requests.post(f'{API}/auth/token', json={'grant_type': 'password', 'username': 'hardcharger_user',
                                                         'password': 'hardcharger_password'},
                              verify=CA or True, timeout=5).json()['access_token']
        headers = {'Authorization': f'Bearer {token}'}
        c = connect()
        session = c.execute('SELECT CAST(HARDCHARGER_CURRENT_SESSION() AS VARCHAR)').fetchval()
        if how == 'closed':
            requests.delete(f'{API}/sessions/{session}', headers=headers, verify=CA or True, timeout=5).raise_for_status()
        else:
            admin = requests.post(f'{API}/sessions', headers=headers, json={}, verify=CA or True, timeout=5).json()['session_id']
            requests.post(f'{API}/sessions/{admin}/statements', headers={**headers, 'Prefer': 'wait=10'},
                          json={'sql': f"KILL SESSION '{session}'"}, verify=CA or True, timeout=15).raise_for_status()
        return c

    def test_session_not_found(self):
        if not HARDCHARGER:
            return  # HARDCHARGER_CURRENT_SESSION() names the session to close
        c = self.end_session('closed')
        with self.assertRaises(pyodbc.Error) as error:
            c.execute('SELECT 1')
        self.assertEqual(error.exception.args[0], '08S01', error.exception.args[1])
        self.assertIn('no longer has this connection', error.exception.args[1])
        c.close()

    def test_session_killed(self):
        if not HARDCHARGER:
            return  # KILL SESSION is Hardcharger's
        c = self.end_session('killed')
        with self.assertRaises(pyodbc.Error) as error:
            c.execute('SELECT 1')
        self.assertEqual(error.exception.args[0], '08S01', error.exception.args[1])
        self.assertIn('reason killed', error.exception.args[1])
        c.close()

    def test_active_transaction_is_kept(self):
        # Control: requests within the timeout keep the transaction, and it commits.
        with connect(autocommit=False) as c:
            c.execute('CREATE OR REPLACE TABLE odbc_idle_py_active (i INTEGER)')
            c.commit()
            for i in range(3):
                c.execute('INSERT INTO odbc_idle_py_active VALUES (?)', i)
                time.sleep(.3)
            c.commit()
        self.assertEqual(rows('odbc_idle_py_active'), 3)


if __name__ == '__main__':
    unittest.main(verbosity=2)
