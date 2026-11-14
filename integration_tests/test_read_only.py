"""Requires a server with read-only sessions; run with the native ODBC driver."""
import os
import pyodbc
import unittest

pyodbc.pooling = False
SQL_MODE_READ_ONLY = 1
SQL_MODE_READ_WRITE = 0
BASE = (
    f"Driver={os.environ['HARDCHARGER_DRIVER']};host={os.environ.get('HARDCHARGER_HOST', '127.0.0.1')};"
    f"port={os.environ['HARDCHARGER_PORT']};useEncryption={os.environ.get('HARDCHARGER_USE_ENCRYPTION', 'false')};"
    "uid=hardcharger_user;pwd=hardcharger_password;"
)


if os.environ.get("HARDCHARGER_TRUSTED_CERTS"):
    BASE += f"trustedCerts={os.environ['HARDCHARGER_TRUSTED_CERTS']};"


class ReadOnlyTests(unittest.TestCase):

    def test_read_only_property(self):
        with pyodbc.connect(BASE + "ReadOnly=true;", autocommit=True) as connection:
            assert connection.execute("SELECT 42").fetchval() == 42
            with self.assertRaises(expected_exception=pyodbc.Error):
                connection.execute("CREATE TABLE forbidden_odbc (n INTEGER)")
            assert connection.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name='forbidden_odbc'").fetchval() == 0
            with self.assertRaises(expected_exception=pyodbc.Error):
                connection.set_attr(pyodbc.SQL_ATTR_ACCESS_MODE, SQL_MODE_READ_WRITE)


    def test_read_only_standard_preconnect_attribute(self):
        with pyodbc.connect(BASE, autocommit=True, attrs_before={pyodbc.SQL_ATTR_ACCESS_MODE: SQL_MODE_READ_ONLY}) as connection:
            assert connection.execute("SELECT 42").fetchval() == 42
            with self.assertRaises(expected_exception=pyodbc.Error):
                connection.execute("CREATE TABLE forbidden_odbc_attr (n INTEGER)")


    def test_read_only_live_transition(self):
        with pyodbc.connect(BASE, autocommit=True) as connection:
            connection.execute("CREATE TEMP TABLE before_read_only (n INTEGER)")
            connection.set_attr(pyodbc.SQL_ATTR_ACCESS_MODE, SQL_MODE_READ_ONLY)
            assert connection.execute("SELECT count(*) FROM before_read_only").fetchval() == 0
            with self.assertRaises(expected_exception=pyodbc.Error):
                connection.execute("INSERT INTO before_read_only VALUES (1)")
            with self.assertRaises(expected_exception=pyodbc.Error):
                connection.set_attr(pyodbc.SQL_ATTR_ACCESS_MODE, SQL_MODE_READ_WRITE)


if __name__ == "__main__":
    unittest.main()
