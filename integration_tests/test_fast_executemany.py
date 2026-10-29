"""pyodbc fast_executemany against a real server (parameter arrays).

pyodbc's fast path binds every parameter row-wise at addresses relative to
SQL_ATTR_PARAM_BIND_OFFSET_PTR and sends the whole batch in one SQLExecute
(SQL_ATTR_PARAMSET_SIZE > 1). The driver once ignored the offset and the array
size, dereferenced the relative addresses and crashed the Python process.
Every test reads the rows back and compares each value with what was sent.
"""
import datetime
import decimal
import os
import unittest
import uuid

import pyodbc

pyodbc.pooling = False
DRIVER = os.environ['HARDCHARGER_DRIVER']
HOST = os.environ.get('HARDCHARGER_HOST', '127.0.0.1')
PORT = os.environ.get('HARDCHARGER_PORT', '31337')
TLS = os.environ.get('HARDCHARGER_USE_ENCRYPTION', 'false')
CA = os.environ.get('HARDCHARGER_TRUSTED_CERTS', '')
UID = os.environ.get('HARDCHARGER_UID', 'hardcharger_user')
PWD = os.environ.get('HARDCHARGER_PWD', 'hardcharger_password')
CONN_STR = f'Driver={DRIVER};host={HOST};port={PORT};useEncryption={TLS};uid={UID};pwd={PWD};'
if CA:
    CONN_STR += f'trustedCerts={CA};'

ROWS = 3000
COLUMNS = ('id', 'big', 'small', 'tiny', 'name', 'note', 'amount', 'ratio', 'flag', 'day', 'ts', 'payload', 'uid')
CREATE = ('CREATE TABLE {} (id INTEGER, big BIGINT, small SMALLINT, tiny TINYINT, name VARCHAR, note VARCHAR, '
          'amount DECIMAL(18,4), ratio DOUBLE, flag BOOLEAN, day DATE, ts TIMESTAMP, payload BLOB, uid UUID)')


def connect(*, autocommit=True):
    return pyodbc.connect(CONN_STR, autocommit=autocommit, timeout=10)


def make_row(i):
    """A row whose every value differs from its neighbours'. Some columns are NULL
    in a pattern that starts at row 0, so pyodbc also has to rebind between
    batches (it picks each column's C type from the first row of a batch)."""
    sign = -1 if i % 2 else 1
    return (
        i,
        sign * (i * 3_000_000_011 + 5),
        None if i % 11 == 0 else sign * (i % 30_000),
        None if i % 13 == 4 else i % 128 - 64,
        f'name-{i:05d}',
        None if i % 7 == 0 else f'Olá 東京 🚀 #{i} ' + 'x' * (i % 50),
        None if i % 9 == 0 else decimal.Decimal(sign * (i * 10_000 + 4321)).scaleb(-4),
        None if i % 10 == 3 else sign * (i / 8 + 0.125),
        None if i % 14 == 0 else i % 3 == 0,
        None if i % 12 == 0 else datetime.date(1890 + i % 200, 1 + i % 12, 1 + i % 28),
        None if i % 15 == 0 else datetime.datetime(1960 + i % 80, 1 + i % 12, 1 + i % 28, i % 24, i % 60,
                                                    (i * 7) % 60, (i * 37) % 1_000_000),
        None if i % 16 == 0 else bytes((i + k) % 256 for k in range(i % 40)),
        None if i % 17 == 0 else uuid.UUID(int=i * 0x9E3779B97F4A7C15 % (1 << 128)),
    )


def read_back(cur, table):
    rows = cur.execute(f'SELECT {", ".join(COLUMNS)} FROM {table} ORDER BY id').fetchall()
    out = []
    for row in rows:
        values = list(row)
        payload, uid = COLUMNS.index('payload'), COLUMNS.index('uid')
        values[payload] = None if values[payload] is None else bytes(values[payload])
        values[uid] = None if values[uid] is None else uuid.UUID(str(values[uid]))
        out.append(tuple(values))
    return out


class FastExecuteManyTests(unittest.TestCase):
    def setUp(self):
        self.conn = connect()
        self.cur = self.conn.cursor()

    def tearDown(self):
        self.conn.close()

    def fresh_table(self, table):
        self.cur.execute(f'DROP TABLE IF EXISTS {table}')
        self.cur.execute(CREATE.format(table))

    def assertRowsEqual(self, actual, expected):
        self.assertEqual(len(actual), len(expected))
        for got, want in zip(actual, expected):
            for column, a, b in zip(COLUMNS, got, want):
                self.assertEqual(a, b, f'row id={want[0]} column {column}: got {a!r}, sent {b!r}')

    def test_many_types_nulls_and_thousands_of_rows(self):
        self.fresh_table('fem_types')
        rows = [make_row(i) for i in range(ROWS)]
        self.cur.fast_executemany = True
        self.cur.executemany(f'INSERT INTO fem_types VALUES ({", ".join("?" * len(COLUMNS))})', rows)
        self.assertRowsEqual(read_back(self.cur, 'fem_types'), rows)

    def test_columns_land_in_their_own_places(self):
        # Same-typed neighbours with values that differ per row and per column:
        # a stride or offset error would move values between rows or columns.
        self.cur.execute('DROP TABLE IF EXISTS fem_order')
        self.cur.execute('CREATE TABLE fem_order (a INTEGER, b INTEGER, c VARCHAR, d VARCHAR)')
        rows = [(i, -i - 1, f'c{i}', f'd{ROWS - i}') for i in range(ROWS)]
        self.cur.fast_executemany = True
        self.cur.executemany('INSERT INTO fem_order VALUES (?, ?, ?, ?)', rows)
        got = [tuple(r) for r in self.cur.execute('SELECT a, b, c, d FROM fem_order ORDER BY a').fetchall()]
        self.assertEqual(got, rows)

    def test_matches_plain_executemany(self):
        rows = [make_row(i) for i in range(0, 400, 3)]
        for fast, table in ((True, 'fem_fast'), (False, 'fem_plain')):
            self.fresh_table(table)
            self.cur.fast_executemany = fast
            self.cur.executemany(f'INSERT INTO {table} VALUES ({", ".join("?" * len(COLUMNS))})', rows)
        self.assertRowsEqual(read_back(self.cur, 'fem_fast'), read_back(self.cur, 'fem_plain'))
        self.assertRowsEqual(read_back(self.cur, 'fem_fast'), rows)

    def test_transaction_commit_and_rollback(self):
        self.fresh_table('fem_txn')
        with connect(autocommit=False) as conn:
            cur = conn.cursor()
            cur.fast_executemany = True
            cur.executemany(f'INSERT INTO fem_txn VALUES ({", ".join("?" * len(COLUMNS))})',
                            [make_row(i) for i in range(50)])
            conn.rollback()
            self.assertEqual(cur.execute('SELECT COUNT(*) FROM fem_txn').fetchval(), 0)
            conn.commit()
            cur.executemany(f'INSERT INTO fem_txn VALUES ({", ".join("?" * len(COLUMNS))})',
                            [make_row(i) for i in range(50)])
            conn.commit()
        self.assertRowsEqual(read_back(self.cur, 'fem_txn'), [make_row(i) for i in range(50)])

    def test_out_of_range_value_is_an_error_not_a_crash(self):
        self.cur.execute('DROP TABLE IF EXISTS fem_range')
        self.cur.execute('CREATE TABLE fem_range (id INTEGER, s SMALLINT)')
        self.cur.fast_executemany = True
        with self.assertRaises(pyodbc.Error) as caught:
            self.cur.executemany('INSERT INTO fem_range VALUES (?, ?)', [(1, 1), (2, 70000), (3, 3)])
        self.assertIn('22003', str(caught.exception))
        self.assertIn('Parameter set 2, parameter 2: integer value 70000 is out of range', str(caught.exception))
        # The batch is refused before it is sent, and the cursor stays usable.
        self.assertEqual(self.cur.execute('SELECT COUNT(*) FROM fem_range').fetchval(), 0)
        self.cur.executemany('INSERT INTO fem_range VALUES (?, ?)', [(1, 1), (2, 2)])
        self.assertEqual([tuple(r) for r in self.cur.execute('SELECT id, s FROM fem_range ORDER BY id')], [(1, 1), (2, 2)])

    def test_cursor_reuse_after_fast_executemany(self):
        # pyodbc's fast path leaves the bind type row-wise and its bindings at
        # relative addresses; ordinary statements on the same cursor must not use them.
        self.cur.execute('DROP TABLE IF EXISTS fem_reuse')
        self.cur.execute('CREATE TABLE fem_reuse (a INTEGER, b VARCHAR)')
        self.cur.fast_executemany = True
        self.cur.executemany('INSERT INTO fem_reuse VALUES (?, ?)', [(i, f'v{i}') for i in range(10)])
        self.assertEqual(self.cur.execute('SELECT b FROM fem_reuse WHERE a = ?', 7).fetchval(), 'v7')
        self.assertEqual(self.cur.execute('SELECT COUNT(*) FROM fem_reuse').fetchval(), 10)
        self.cur.execute('INSERT INTO fem_reuse VALUES (?, ?)', 99, 'single')
        self.assertEqual(self.cur.execute('SELECT b FROM fem_reuse WHERE a = 99').fetchval(), 'single')


if __name__ == '__main__':
    unittest.main(verbosity=2)
