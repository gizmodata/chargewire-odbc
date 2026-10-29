/*
 * Copyright (C) 2026 GizmoData LLC
 *
 * See "LICENSE" for license information.
 */

/*
 * Integration test for arrays of parameters (SQL_ATTR_PARAMSET_SIZE > 1).
 *
 *   1. Column-wise binding of 5000 parameter sets (more than one result batch):
 *      integers, CHAR with SQL_NTS and explicit lengths, DOUBLE, TIMESTAMP with
 *      microseconds, NULLs; SQL_ATTR_PARAMS_PROCESSED_PTR, the status array and
 *      SQLRowCount; every row read back and compared.
 *   2. Row-wise binding (SQL_ATTR_PARAM_BIND_TYPE = struct size) with buffer
 *      addresses relative to SQL_ATTR_PARAM_BIND_OFFSET_PTR, the way pyodbc's
 *      fast_executemany binds: WCHAR (with a surrogate pair), BIGINT, NUMERIC,
 *      DATE and BIT, in two executions that move the offset.
 *   3. SQL_ATTR_PARAM_OPERATION_PTR with SQL_PARAM_IGNORE.
 *   4. Inputs the driver must refuse with SQL_ERROR and a specific SQLSTATE,
 *      never a crash: data-at-execution, negative lengths, null data pointers,
 *      lengths beyond the element, out-of-range values, unsupported C types,
 *      a zero paramset size, and executing with the bindings reset.
 *
 * Requires: an ODBC driver manager and a Hardcharger server (HARDCHARGER_HOST,
 * HARDCHARGER_PORT, HARDCHARGER_UID, HARDCHARGER_PWD; HARDCHARGER_DRIVER).
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>

#define DEFAULT_DRIVER_PATH "/tmp/libhardcharger-odbc.so"
#define MAX_CONN_STR 1024

static int g_passed = 0;
static int g_failed = 0;

static const char *env_or(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value && value[0] ? value : fallback;
}

static void print_diagnostics(SQLSMALLINT handleType, SQLHANDLE handle) {
  SQLCHAR state[8], msg[1024];
  SQLINTEGER native;
  SQLSMALLINT msgLen;
  SQLSMALLINT i = 1;
  while (SQLGetDiagRec(handleType, handle, i++, state, &native, msg, sizeof(msg), &msgLen) == SQL_SUCCESS) {
    fprintf(stderr, "  [%s] %s (native=%d)\n", state, msg, (int)native);
  }
}

#define CHECK(rc, msg) do { \
  if ((rc) != SQL_SUCCESS && (rc) != SQL_SUCCESS_WITH_INFO) { \
    fprintf(stderr, "FAIL: %s (rc=%d)\n", msg, (int)(rc)); \
    print_diagnostics(SQL_HANDLE_STMT, hStmt); \
    return 1; \
  } \
} while (0)

#define EXPECT(cond, ...) do { \
  if (!(cond)) { \
    fprintf(stderr, "FAIL: "); \
    fprintf(stderr, __VA_ARGS__); \
    fprintf(stderr, "\n"); \
    return 1; \
  } \
} while (0)

/* Restore every parameter-array attribute this suite changes. */
static void reset_stmt(SQLHSTMT hStmt) {
  SQLFreeStmt(hStmt, SQL_CLOSE);
  SQLFreeStmt(hStmt, SQL_UNBIND);
  SQLFreeStmt(hStmt, SQL_RESET_PARAMS);
  SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)1, 0);
  SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_BIND_TYPE, (SQLPOINTER)SQL_PARAM_BIND_BY_COLUMN, 0);
  SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, NULL, 0);
  SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMS_PROCESSED_PTR, NULL, 0);
  SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_STATUS_PTR, NULL, 0);
  SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_OPERATION_PTR, NULL, 0);
}

static int exec_sql(SQLHSTMT hStmt, const char *sql) {
  SQLRETURN rc = SQLExecDirect(hStmt, (SQLCHAR *)sql, SQL_NTS);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO && rc != SQL_NO_DATA) {
    fprintf(stderr, "FAIL: exec '%s' (rc=%d)\n", sql, (int)rc);
    print_diagnostics(SQL_HANDLE_STMT, hStmt);
    return 1;
  }
  SQLFreeStmt(hStmt, SQL_CLOSE);
  return 0;
}

static long query_long(SQLHSTMT hStmt, const char *sql) {
  SQLBIGINT value = -1;
  SQLLEN ind = 0;
  if (!SQL_SUCCEEDED(SQLExecDirect(hStmt, (SQLCHAR *)sql, SQL_NTS)) || !SQL_SUCCEEDED(SQLFetch(hStmt)) ||
      !SQL_SUCCEEDED(SQLGetData(hStmt, 1, SQL_C_SBIGINT, &value, 0, &ind))) {
    print_diagnostics(SQL_HANDLE_STMT, hStmt);
    value = -1;
  }
  SQLFreeStmt(hStmt, SQL_CLOSE);
  return (long)value;
}

/* The first diagnostic's SQLSTATE matches, and its message contains text. */
static int expect_error(SQLHSTMT hStmt, SQLRETURN rc, const char *state, const char *text, const char *what) {
  SQLCHAR got[8] = {0}, msg[1024] = {0};
  SQLINTEGER native;
  SQLSMALLINT msgLen;
  if (rc != SQL_ERROR) {
    fprintf(stderr, "FAIL: %s: expected SQL_ERROR, got rc=%d\n", what, (int)rc);
    return 1;
  }
  SQLGetDiagRec(SQL_HANDLE_STMT, hStmt, 1, got, &native, msg, sizeof(msg), &msgLen);
  if (strcmp((const char *)got, state) != 0 || (text && !strstr((const char *)msg, text))) {
    fprintf(stderr, "FAIL: %s: expected [%s] containing '%s', got [%s] %s\n", what, state, text ? text : "", got, msg);
    return 1;
  }
  printf("  refused as expected: [%s] %s\n", got, msg);
  return 0;
}

/* ---------------------------------------------------------------------------
 * 1. Column-wise binding
 * ------------------------------------------------------------------------ */
#define CW_ROWS 5000
#define CW_NAME 24
static SQLINTEGER cw_id[CW_ROWS];
static SQLSMALLINT cw_small[CW_ROWS];
static SQLLEN cw_small_ind[CW_ROWS];
static SQLCHAR cw_name[CW_ROWS][CW_NAME];
static SQLLEN cw_name_ind[CW_ROWS];
static SQLDOUBLE cw_ratio[CW_ROWS];
static SQLLEN cw_ratio_ind[CW_ROWS];
static SQL_TIMESTAMP_STRUCT cw_ts[CW_ROWS];
static SQLLEN cw_ts_ind[CW_ROWS];
static SQLUSMALLINT cw_status[CW_ROWS];

static void cw_expected_name(int r, char *out) { snprintf(out, CW_NAME, "name-%05d-%c", r, 'a' + r % 26); }

static int test_column_wise(SQLHSTMT hStmt) {
  printf("\n--- Test: column-wise parameter array (%d sets) ---\n", CW_ROWS);
  if (exec_sql(hStmt, "DROP TABLE IF EXISTS param_cw") ||
      exec_sql(hStmt, "CREATE TABLE param_cw (id INTEGER, small SMALLINT, name VARCHAR, ratio DOUBLE, ts TIMESTAMP)"))
    return 1;

  for (int r = 0; r < CW_ROWS; ++r) {
    char expected[CW_NAME];
    cw_id[r] = r;
    cw_small[r] = (SQLSMALLINT)(r % 30000 - 15000);
    cw_small_ind[r] = r % 11 == 3 ? SQL_NULL_DATA : 0;
    cw_expected_name(r, expected);
    memset(cw_name[r], 'X', CW_NAME);
    if (r % 7 == 1) {
      cw_name_ind[r] = SQL_NULL_DATA;
    } else if (r % 2) {
      memcpy(cw_name[r], expected, strlen(expected) + 1);
      cw_name_ind[r] = SQL_NTS;
    } else {
      memcpy(cw_name[r], expected, strlen(expected)); /* no terminator: the length decides */
      cw_name_ind[r] = (SQLLEN)strlen(expected);
    }
    cw_ratio[r] = r * 1.25 - 1000.5;
    cw_ratio_ind[r] = r % 13 == 5 ? SQL_NULL_DATA : 0;
    cw_ts[r].year = (SQLSMALLINT)(1960 + r % 80);
    cw_ts[r].month = (SQLUSMALLINT)(1 + r % 12);
    cw_ts[r].day = (SQLUSMALLINT)(1 + r % 28);
    cw_ts[r].hour = (SQLUSMALLINT)(r % 24);
    cw_ts[r].minute = (SQLUSMALLINT)(r % 60);
    cw_ts[r].second = (SQLUSMALLINT)((r * 7) % 60);
    cw_ts[r].fraction = (SQLUINTEGER)((r * 37 % 1000000) * 1000); /* microseconds, in ns */
    cw_ts_ind[r] = r % 17 == 9 ? SQL_NULL_DATA : 0;
    cw_status[r] = 0xFFFF;
  }

  SQLULEN processed = 0;
  SQLRETURN rc = SQLPrepare(hStmt, (SQLCHAR *)"INSERT INTO param_cw VALUES (?, ?, ?, ?, ?)", SQL_NTS);
  CHECK(rc, "SQLPrepare");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_BIND_TYPE, (SQLPOINTER)SQL_PARAM_BIND_BY_COLUMN, 0), "bind type");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)(SQLULEN)CW_ROWS, 0), "paramset size");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), "processed ptr");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_STATUS_PTR, cw_status, 0), "status ptr");
  CHECK(SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, cw_id, 0, NULL), "bind id");
  CHECK(SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_SSHORT, SQL_SMALLINT, 0, 0, cw_small, 0, cw_small_ind), "bind small");
  CHECK(SQLBindParameter(hStmt, 3, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, CW_NAME, 0, cw_name, CW_NAME, cw_name_ind), "bind name");
  CHECK(SQLBindParameter(hStmt, 4, SQL_PARAM_INPUT, SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, cw_ratio, 0, cw_ratio_ind), "bind ratio");
  CHECK(SQLBindParameter(hStmt, 5, SQL_PARAM_INPUT, SQL_C_TYPE_TIMESTAMP, SQL_TYPE_TIMESTAMP, 26, 6, cw_ts, 0, cw_ts_ind), "bind ts");
  CHECK(SQLExecute(hStmt), "SQLExecute");

  SQLLEN rowCount = -1;
  CHECK(SQLRowCount(hStmt, &rowCount), "SQLRowCount");
  reset_stmt(hStmt);
  const long inserted = query_long(hStmt, "SELECT COUNT(*) FROM param_cw");
  EXPECT(processed == CW_ROWS && rowCount == CW_ROWS && inserted == CW_ROWS,
         "SQL_ATTR_PARAMS_PROCESSED_PTR %lu, SQLRowCount %ld, rows in the table %ld; expected %d each",
         (unsigned long)processed, (long)rowCount, inserted, CW_ROWS);
  for (int r = 0; r < CW_ROWS; ++r) EXPECT(cw_status[r] == SQL_PARAM_SUCCESS, "status[%d] = %u", r, cw_status[r]);
  CHECK(SQLExecDirect(hStmt, (SQLCHAR *)"SELECT id, small, name, ratio, ts FROM param_cw ORDER BY id", SQL_NTS), "select");
  for (int r = 0; r < CW_ROWS; ++r) {
    SQLINTEGER id = -1;
    SQLSMALLINT small = 0;
    SQLCHAR name[64];
    SQLDOUBLE ratio = 0;
    SQL_TIMESTAMP_STRUCT ts;
    SQLLEN ind[5];
    char expected[CW_NAME];
    CHECK(SQLFetch(hStmt), "fetch");
    CHECK(SQLGetData(hStmt, 1, SQL_C_SLONG, &id, 0, &ind[0]), "get id");
    CHECK(SQLGetData(hStmt, 2, SQL_C_SSHORT, &small, 0, &ind[1]), "get small");
    CHECK(SQLGetData(hStmt, 3, SQL_C_CHAR, name, sizeof(name), &ind[2]), "get name");
    CHECK(SQLGetData(hStmt, 4, SQL_C_DOUBLE, &ratio, 0, &ind[3]), "get ratio");
    CHECK(SQLGetData(hStmt, 5, SQL_C_TYPE_TIMESTAMP, &ts, sizeof(ts), &ind[4]), "get ts");
    EXPECT(id == r, "row %d: id %d", r, (int)id);
    if (cw_small_ind[r] == SQL_NULL_DATA) EXPECT(ind[1] == SQL_NULL_DATA, "row %d: small should be NULL", r);
    else EXPECT(ind[1] != SQL_NULL_DATA && small == cw_small[r], "row %d: small %d, expected %d", r, small, cw_small[r]);
    cw_expected_name(r, expected);
    if (cw_name_ind[r] == SQL_NULL_DATA) EXPECT(ind[2] == SQL_NULL_DATA, "row %d: name should be NULL", r);
    else EXPECT(ind[2] != SQL_NULL_DATA && strcmp((char *)name, expected) == 0, "row %d: name '%s', expected '%s'", r, name, expected);
    if (cw_ratio_ind[r] == SQL_NULL_DATA) EXPECT(ind[3] == SQL_NULL_DATA, "row %d: ratio should be NULL", r);
    else EXPECT(ind[3] != SQL_NULL_DATA && ratio == cw_ratio[r], "row %d: ratio %.17g, expected %.17g", r, ratio, cw_ratio[r]);
    if (cw_ts_ind[r] == SQL_NULL_DATA) {
      EXPECT(ind[4] == SQL_NULL_DATA, "row %d: ts should be NULL", r);
    } else {
      EXPECT(ind[4] != SQL_NULL_DATA && ts.year == cw_ts[r].year && ts.month == cw_ts[r].month && ts.day == cw_ts[r].day &&
                 ts.hour == cw_ts[r].hour && ts.minute == cw_ts[r].minute && ts.second == cw_ts[r].second &&
                 ts.fraction == cw_ts[r].fraction,
             "row %d: ts %04d-%02u-%02u %02u:%02u:%02u.%09u, expected %04d-%02u-%02u %02u:%02u:%02u.%09u", r,
             ts.year, ts.month, ts.day, ts.hour, ts.minute, ts.second, (unsigned)ts.fraction, cw_ts[r].year,
             cw_ts[r].month, cw_ts[r].day, cw_ts[r].hour, cw_ts[r].minute, cw_ts[r].second, (unsigned)cw_ts[r].fraction);
    }
  }
  EXPECT(SQLFetch(hStmt) == SQL_NO_DATA, "extra rows after %d", CW_ROWS);
  reset_stmt(hStmt);
  printf("  PASS (%d rows, values checked)\n", CW_ROWS);
  return 0;
}

/* ---------------------------------------------------------------------------
 * 2. Row-wise binding with bind offsets (pyodbc fast_executemany's layout)
 * ------------------------------------------------------------------------ */
#define RW_ROWS 1500
#define RW_FIRST 600
typedef struct {
  SQLINTEGER id;
  SQLLEN id_ind;
  SQLWCHAR label[12];
  SQLLEN label_ind;
  SQLBIGINT big;
  SQLLEN big_ind;
  char pad; /* misalign what follows, as a packed application struct may */
  SQL_NUMERIC_STRUCT amount;
  SQLLEN amount_ind;
  SQL_DATE_STRUCT day;
  SQLLEN day_ind;
  unsigned char flag;
  SQLLEN flag_ind;
} RowParams;

static RowParams rw[RW_ROWS];

static void rw_expected_label(int r, char *out, size_t size) {
  /* "L<r>" then U+1F680 (UTF-8 F0 9F 9A 80), sent as a UTF-16 surrogate pair. */
  snprintf(out, size, "L%d\xF0\x9F\x9A\x80", r);
}

static long long rw_amount(int r) { return (r % 2 ? -1LL : 1LL) * (1000000LL * r + 4321); } /* scale 4 */

static int bind_row_wise(SQLHSTMT hStmt) {
  /* Addresses relative to a fake base of 16; SQL_ATTR_PARAM_BIND_OFFSET_PTR supplies the base. */
  char *base = (char *)16;
  CHECK(SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, base + offsetof(RowParams, id), 0,
                         (SQLLEN *)(base + offsetof(RowParams, id_ind))), "bind id");
  CHECK(SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_WCHAR, SQL_WVARCHAR, 11, 0, base + offsetof(RowParams, label),
                         sizeof(rw[0].label), (SQLLEN *)(base + offsetof(RowParams, label_ind))), "bind label");
  CHECK(SQLBindParameter(hStmt, 3, SQL_PARAM_INPUT, SQL_C_SBIGINT, SQL_BIGINT, 0, 0, base + offsetof(RowParams, big), 0,
                         (SQLLEN *)(base + offsetof(RowParams, big_ind))), "bind big");
  CHECK(SQLBindParameter(hStmt, 4, SQL_PARAM_INPUT, SQL_C_NUMERIC, SQL_DECIMAL, 18, 4, base + offsetof(RowParams, amount),
                         sizeof(SQL_NUMERIC_STRUCT), (SQLLEN *)(base + offsetof(RowParams, amount_ind))), "bind amount");
  CHECK(SQLBindParameter(hStmt, 5, SQL_PARAM_INPUT, SQL_C_TYPE_DATE, SQL_TYPE_DATE, 10, 0, base + offsetof(RowParams, day), 0,
                         (SQLLEN *)(base + offsetof(RowParams, day_ind))), "bind day");
  CHECK(SQLBindParameter(hStmt, 6, SQL_PARAM_INPUT, SQL_C_BIT, SQL_BIT, 1, 0, base + offsetof(RowParams, flag), 0,
                         (SQLLEN *)(base + offsetof(RowParams, flag_ind))), "bind flag");
  return 0;
}

static int test_row_wise_with_offsets(SQLHSTMT hStmt) {
  printf("\n--- Test: row-wise parameter array with bind offsets (%d sets, two executions) ---\n", RW_ROWS);
  if (exec_sql(hStmt, "DROP TABLE IF EXISTS param_rw") ||
      exec_sql(hStmt, "CREATE TABLE param_rw (id INTEGER, label VARCHAR, big BIGINT, amount DECIMAL(18,4), day DATE, flag BOOLEAN)"))
    return 1;

  memset(rw, 0, sizeof(rw));
  for (int r = 0; r < RW_ROWS; ++r) {
    char digits[16];
    rw[r].id = r;
    rw[r].id_ind = 0;
    int n = snprintf(digits, sizeof(digits), "L%d", r);
    for (int i = 0; i < n; ++i) rw[r].label[i] = (SQLWCHAR)digits[i];
    rw[r].label[n] = 0xD83D;
    rw[r].label[n + 1] = 0xDE80;
    /* Alternate explicit byte lengths (no terminator) and SQL_NTS. */
    if (r % 2) {
      rw[r].label[n + 2] = 0;
      rw[r].label_ind = SQL_NTS;
    } else {
      rw[r].label[n + 2] = 'Z'; /* must not be sent */
      rw[r].label_ind = (SQLLEN)((n + 2) * sizeof(SQLWCHAR));
    }
    if (r % 9 == 4) rw[r].label_ind = SQL_NULL_DATA;
    rw[r].big = (SQLBIGINT)r * 3000000000LL - 7;
    rw[r].big_ind = r % 10 == 7 ? SQL_NULL_DATA : 0;
    long long amount = rw_amount(r);
    unsigned long long magnitude = (unsigned long long)(amount < 0 ? -amount : amount);
    rw[r].amount.precision = 18;
    rw[r].amount.scale = 4;
    rw[r].amount.sign = amount < 0 ? 0 : 1;
    for (int i = 0; i < 8; ++i) rw[r].amount.val[i] = (SQLCHAR)(magnitude >> (8 * i));
    rw[r].amount_ind = r % 8 == 2 ? SQL_NULL_DATA : (SQLLEN)sizeof(SQL_NUMERIC_STRUCT);
    rw[r].day.year = (SQLSMALLINT)(1890 + r % 200);
    rw[r].day.month = (SQLUSMALLINT)(1 + r % 12);
    rw[r].day.day = (SQLUSMALLINT)(1 + r % 28);
    rw[r].day_ind = r % 12 == 6 ? SQL_NULL_DATA : 0;
    rw[r].flag = (unsigned char)(r % 3 == 0);
    rw[r].flag_ind = r % 14 == 13 ? SQL_NULL_DATA : 0;
  }

  SQLULEN processed = 0;
  static SQLUSMALLINT status[RW_ROWS];
  CHECK(SQLPrepare(hStmt, (SQLCHAR *)"INSERT INTO param_rw VALUES (?, ?, ?, ?, ?, ?)", SQL_NTS), "SQLPrepare");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_BIND_TYPE, (SQLPOINTER)(SQLULEN)sizeof(RowParams), 0), "bind type");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), "processed ptr");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_STATUS_PTR, status, 0), "status ptr");
  SQLULEN offset = (SQLULEN)(rw) - 16;
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_BIND_OFFSET_PTR, &offset, 0), "bind offset ptr");
  if (bind_row_wise(hStmt)) return 1;

  /* First execution: sets [0, RW_FIRST). */
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)(SQLULEN)RW_FIRST, 0), "paramset size");
  CHECK(SQLExecute(hStmt), "SQLExecute (first part)");
  EXPECT(processed == RW_FIRST, "processed %lu after the first part", (unsigned long)processed);
  /* Second execution: the same bindings, the offset moved to set RW_FIRST. */
  offset = (SQLULEN)(&rw[RW_FIRST]) - 16;
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)(SQLULEN)(RW_ROWS - RW_FIRST), 0), "paramset size");
  CHECK(SQLExecute(hStmt), "SQLExecute (second part)");
  EXPECT(processed == RW_ROWS - RW_FIRST, "processed %lu after the second part", (unsigned long)processed);
  for (int r = 0; r < RW_ROWS - RW_FIRST; ++r) EXPECT(status[r] == SQL_PARAM_SUCCESS, "status[%d] = %u", r, status[r]);
  reset_stmt(hStmt);

  EXPECT(query_long(hStmt, "SELECT COUNT(*) FROM param_rw") == RW_ROWS, "row count in the table");
  CHECK(SQLExecDirect(hStmt, (SQLCHAR *)"SELECT id, label, big, CAST(amount AS VARCHAR), day, flag FROM param_rw "
                                         "ORDER BY id", SQL_NTS), "select");
  int seen = 0;
  for (int r = 0; r < RW_ROWS; ++r) {
    SQLINTEGER id = -1;
    SQLCHAR label[64], amount[64];
    SQLBIGINT big = 0;
    SQL_DATE_STRUCT day;
    unsigned char flag = 2;
    SQLLEN ind[6];
    CHECK(SQLFetch(hStmt), "fetch");
    CHECK(SQLGetData(hStmt, 1, SQL_C_SLONG, &id, 0, &ind[5]), "get id");
    CHECK(SQLGetData(hStmt, 2, SQL_C_CHAR, label, sizeof(label), &ind[0]), "get label");
    CHECK(SQLGetData(hStmt, 3, SQL_C_SBIGINT, &big, 0, &ind[1]), "get big");
    CHECK(SQLGetData(hStmt, 4, SQL_C_CHAR, amount, sizeof(amount), &ind[2]), "get amount");
    CHECK(SQLGetData(hStmt, 5, SQL_C_TYPE_DATE, &day, sizeof(day), &ind[3]), "get day");
    CHECK(SQLGetData(hStmt, 6, SQL_C_BIT, &flag, 0, &ind[4]), "get flag");
    EXPECT(id == r, "row %d: id %d", r, (int)id);
    char expected[64];
    rw_expected_label(r, expected, sizeof(expected));
    if (rw[r].label_ind == SQL_NULL_DATA) EXPECT(ind[0] == SQL_NULL_DATA, "row %d: label should be NULL", r);
    else EXPECT(ind[0] != SQL_NULL_DATA && strcmp((char *)label, expected) == 0, "row %d: label '%s', expected '%s'", r, label, expected);
    if (rw[r].big_ind == SQL_NULL_DATA) EXPECT(ind[1] == SQL_NULL_DATA, "row %d: big should be NULL", r);
    else EXPECT(ind[1] != SQL_NULL_DATA && big == rw[r].big, "row %d: big %lld", r, (long long)big);
    if (rw[r].amount_ind == SQL_NULL_DATA) {
      EXPECT(ind[2] == SQL_NULL_DATA, "row %d: amount should be NULL", r);
    } else {
      long long a = rw_amount(r);
      char want[64];
      snprintf(want, sizeof(want), "%s%lld.%04lld", a < 0 ? "-" : "", (a < 0 ? -a : a) / 10000, (a < 0 ? -a : a) % 10000);
      EXPECT(ind[2] != SQL_NULL_DATA && strcmp((char *)amount, want) == 0, "row %d: amount '%s', expected '%s'", r, amount, want);
    }
    if (rw[r].day_ind == SQL_NULL_DATA) EXPECT(ind[3] == SQL_NULL_DATA, "row %d: day should be NULL", r);
    else EXPECT(ind[3] != SQL_NULL_DATA && day.year == rw[r].day.year && day.month == rw[r].day.month && day.day == rw[r].day.day,
                "row %d: day %04d-%02u-%02u", r, day.year, day.month, day.day);
    if (rw[r].flag_ind == SQL_NULL_DATA) EXPECT(ind[4] == SQL_NULL_DATA, "row %d: flag should be NULL", r);
    else EXPECT(ind[4] != SQL_NULL_DATA && flag == rw[r].flag, "row %d: flag %u", r, flag);
    ++seen;
  }
  EXPECT(SQLFetch(hStmt) == SQL_NO_DATA, "extra rows after %d", RW_ROWS);
  EXPECT(seen == RW_ROWS, "fetched %d rows, expected %d", seen, RW_ROWS);
  reset_stmt(hStmt);
  printf("  PASS (%d rows, values checked)\n", RW_ROWS);
  return 0;
}

/* ---------------------------------------------------------------------------
 * 3. SQL_PARAM_IGNORE
 * ------------------------------------------------------------------------ */
static int test_param_ignore(SQLHSTMT hStmt) {
  printf("\n--- Test: SQL_ATTR_PARAM_OPERATION_PTR / SQL_PARAM_IGNORE ---\n");
  if (exec_sql(hStmt, "DROP TABLE IF EXISTS param_ignore") || exec_sql(hStmt, "CREATE TABLE param_ignore (id INTEGER)"))
    return 1;
  SQLINTEGER ids[6] = {10, 11, 12, 13, 14, 15};
  SQLUSMALLINT ops[6] = {SQL_PARAM_PROCEED, SQL_PARAM_IGNORE, SQL_PARAM_PROCEED, SQL_PARAM_IGNORE, SQL_PARAM_IGNORE, SQL_PARAM_PROCEED};
  SQLUSMALLINT status[6];
  SQLULEN processed = 0;
  CHECK(SQLPrepare(hStmt, (SQLCHAR *)"INSERT INTO param_ignore VALUES (?)", SQL_NTS), "SQLPrepare");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)6, 0), "paramset size");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_OPERATION_PTR, ops, 0), "operation ptr");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_STATUS_PTR, status, 0), "status ptr");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0), "processed ptr");
  CHECK(SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_INTEGER, 0, 0, ids, 0, NULL), "bind");
  CHECK(SQLExecute(hStmt), "SQLExecute");
  EXPECT(processed == 3, "processed %lu, expected 3", (unsigned long)processed);
  for (int r = 0; r < 6; ++r) {
    SQLUSMALLINT want = ops[r] == SQL_PARAM_IGNORE ? SQL_PARAM_UNUSED : SQL_PARAM_SUCCESS;
    EXPECT(status[r] == want, "status[%d] = %u, expected %u", r, status[r], want);
  }
  reset_stmt(hStmt);
  EXPECT(query_long(hStmt, "SELECT COUNT(*) FROM param_ignore") == 3, "three rows inserted");
  EXPECT(query_long(hStmt, "SELECT SUM(id) FROM param_ignore") == 10 + 12 + 15, "the proceeding rows were inserted");
  printf("  PASS\n");
  return 0;
}

/* ---------------------------------------------------------------------------
 * 4. Inputs that must be refused, not crash
 * ------------------------------------------------------------------------ */
static int prepare_refusal(SQLHSTMT hStmt, SQLULEN sets) {
  reset_stmt(hStmt);
  SQLRETURN rc = SQLPrepare(hStmt, (SQLCHAR *)"INSERT INTO param_bad VALUES (?, ?)", SQL_NTS);
  CHECK(rc, "SQLPrepare");
  CHECK(SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)sets, 0), "paramset size");
  return 0;
}

static int test_refusals(SQLHSTMT hStmt) {
  printf("\n--- Test: invalid parameter arrays are refused with a diagnostic ---\n");
  if (exec_sql(hStmt, "DROP TABLE IF EXISTS param_bad") ||
      exec_sql(hStmt, "CREATE TABLE param_bad (s SMALLINT, v VARCHAR)"))
    return 1;
  SQLSMALLINT s[3] = {1, 2, 3};
  SQLCHAR v[3][8] = {"a", "b", "c"};
  SQLLEN vind[3] = {SQL_NTS, SQL_NTS, SQL_NTS};
  SQLUSMALLINT status[3];
  SQLULEN processed = 99;

  /* Data-at-execution is not supported. */
  if (prepare_refusal(hStmt, 3)) return 1;
  vind[1] = SQL_DATA_AT_EXEC;
  SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_SSHORT, SQL_SMALLINT, 0, 0, s, 0, NULL);
  SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 8, 0, v, 8, vind);
  SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_STATUS_PTR, status, 0);
  SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0);
  if (expect_error(hStmt, SQLExecute(hStmt), "HYC00", "Parameter set 2, parameter 2: data-at-execution", "SQL_DATA_AT_EXEC")) return 1;
  EXPECT(status[0] == SQL_PARAM_UNUSED && status[1] == SQL_PARAM_ERROR && status[2] == SQL_PARAM_UNUSED,
         "status after DAE refusal: %u %u %u", status[0], status[1], status[2]);
  EXPECT(processed == 2, "processed %lu after the refusal of set 2", (unsigned long)processed);

  vind[1] = SQL_LEN_DATA_AT_EXEC(5);
  if (expect_error(hStmt, SQLExecute(hStmt), "HYC00", "data-at-execution", "SQL_LEN_DATA_AT_EXEC")) return 1;

  /* A negative length that is not a defined indicator. */
  vind[1] = -7;
  if (expect_error(hStmt, SQLExecute(hStmt), "HY090", "invalid length", "length -7")) return 1;

  /* A length beyond the column-wise element would read the next element. */
  vind[1] = 9;
  if (expect_error(hStmt, SQLExecute(hStmt), "HY090", "exceeds the array element", "length > BufferLength")) return 1;
  vind[1] = SQL_NTS;

  /* A column-wise character array without an element size. */
  if (prepare_refusal(hStmt, 3)) return 1;
  SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_SSHORT, SQL_SMALLINT, 0, 0, s, 0, NULL);
  SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 8, 0, v, 0, vind);
  if (expect_error(hStmt, SQLExecute(hStmt), "HY090", "positive BufferLength", "BufferLength 0")) return 1;

  /* A non-NULL fixed-length value with a null data pointer. */
  if (prepare_refusal(hStmt, 3)) return 1;
  SQLLEN sind[3] = {0, 0, 0};
  SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_SSHORT, SQL_SMALLINT, 0, 0, NULL, 0, sind);
  SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 8, 0, v, 8, vind);
  if (expect_error(hStmt, SQLExecute(hStmt), "HY009", "null data pointer", "null data pointer")) return 1;

  /* A C type the driver cannot convert. */
  if (prepare_refusal(hStmt, 3)) return 1;
  SQL_INTERVAL_STRUCT iv[3];
  memset(iv, 0, sizeof(iv));
  SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_INTERVAL_DAY, SQL_SMALLINT, 0, 0, iv, sizeof(iv[0]), NULL);
  SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 8, 0, v, 8, vind);
  if (expect_error(hStmt, SQLExecute(hStmt), "HY003", "unsupported C type", "SQL_C_INTERVAL_DAY")) return 1;

  /* A value out of the column's range is refused before anything runs. */
  if (prepare_refusal(hStmt, 3)) return 1;
  SQLINTEGER wide[3] = {1, 70000, 3};
  SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_SMALLINT, 0, 0, wide, 0, NULL);
  SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 8, 0, v, 8, vind);
  SQLSetStmtAttr(hStmt, SQL_ATTR_PARAM_STATUS_PTR, status, 0);
  SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMS_PROCESSED_PTR, &processed, 0);
  if (expect_error(hStmt, SQLExecute(hStmt), "22003", "Parameter set 2, parameter 1: integer value 70000 is out of range", "70000 into SMALLINT")) return 1;
  EXPECT(status[0] == SQL_PARAM_UNUSED && status[1] == SQL_PARAM_ERROR && status[2] == SQL_PARAM_UNUSED,
         "status after the range refusal: %u %u %u", status[0], status[1], status[2]);

  /* After SQL_RESET_PARAMS nothing is bound: the statement must not run with old buffers. */
  wide[1] = 2;
  if (prepare_refusal(hStmt, 3)) return 1;
  SQLBindParameter(hStmt, 1, SQL_PARAM_INPUT, SQL_C_SLONG, SQL_SMALLINT, 0, 0, wide, 0, NULL);
  SQLBindParameter(hStmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_VARCHAR, 8, 0, v, 8, vind);
  CHECK(SQLExecute(hStmt), "SQLExecute (valid twin of the refusals)");
  SQLFreeStmt(hStmt, SQL_RESET_PARAMS);
  if (expect_error(hStmt, SQLExecute(hStmt), "07002", "Parameter count mismatch", "execute after SQL_RESET_PARAMS")) return 1;

  /* A paramset size of zero. */
  reset_stmt(hStmt);
  if (expect_error(hStmt, SQLSetStmtAttr(hStmt, SQL_ATTR_PARAMSET_SIZE, (SQLPOINTER)0, 0), "HY024", NULL, "paramset size 0")) return 1;

  reset_stmt(hStmt);
  /* Only the valid twin's three rows were inserted: no refusal ran anything. */
  EXPECT(query_long(hStmt, "SELECT COUNT(*) FROM param_bad") == 3, "param_bad has %ld rows, expected 3",
         query_long(hStmt, "SELECT COUNT(*) FROM param_bad"));
  printf("  PASS\n");
  return 0;
}

int main(int argc, char **argv) {
  SQLHENV hEnv = SQL_NULL_HENV;
  SQLHDBC hDbc = SQL_NULL_HDBC;
  SQLHSTMT hStmt = SQL_NULL_HSTMT;

  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  printf("=== Integration Tests (Parameter arrays) ===\n");

  const char *driver = env_or("HARDCHARGER_DRIVER", DEFAULT_DRIVER_PATH);
  char connBuf[MAX_CONN_STR];
  snprintf(connBuf, sizeof(connBuf), "Driver=%s;host=%s;port=%s;uid=%s;pwd=%s;useEncryption=false", driver,
           env_or("HARDCHARGER_HOST", "127.0.0.1"), env_or("HARDCHARGER_PORT", "31337"),
           env_or("HARDCHARGER_UID", "hardcharger_user"), env_or("HARDCHARGER_PWD", "hardcharger_password"));
  printf("Driver: %s\n", driver);

  if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv) != SQL_SUCCESS ||
      SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0) != SQL_SUCCESS ||
      SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc) != SQL_SUCCESS) {
    fprintf(stderr, "FAIL: allocate handles\n");
    return 1;
  }
  SQLRETURN rc = SQLDriverConnect(hDbc, NULL, (SQLCHAR *)connBuf, SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
  if (!SQL_SUCCEEDED(rc)) {
    fprintf(stderr, "FAIL: DriverConnect (rc=%d)\n", (int)rc);
    print_diagnostics(SQL_HANDLE_DBC, hDbc);
    return 1;
  }
  if (SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt) != SQL_SUCCESS) {
    fprintf(stderr, "FAIL: AllocStmt\n");
    return 1;
  }

  /* Arguments select tests by name (column_wise row_wise ignore refusals); none runs all. */
  static const struct { const char *name; int (*run)(SQLHSTMT); } tests[] = {
      {"column_wise", test_column_wise},
      {"row_wise", test_row_wise_with_offsets},
      {"ignore", test_param_ignore},
      {"refusals", test_refusals},
  };
  const int count = (int)(sizeof(tests) / sizeof(tests[0]));
  int selected = 0;
  for (int t = 0; t < count; ++t) {
    int run = argc <= 1;
    for (int a = 1; a < argc; ++a) run = run || strcmp(argv[a], tests[t].name) == 0;
    if (!run) continue;
    ++selected;
    if (tests[t].run(hStmt)) g_failed++; else g_passed++;
    reset_stmt(hStmt);
  }

  exec_sql(hStmt, "DROP TABLE IF EXISTS param_cw");
  exec_sql(hStmt, "DROP TABLE IF EXISTS param_rw");
  exec_sql(hStmt, "DROP TABLE IF EXISTS param_ignore");
  exec_sql(hStmt, "DROP TABLE IF EXISTS param_bad");
  SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
  SQLDisconnect(hDbc);
  SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
  SQLFreeHandle(SQL_HANDLE_ENV, hEnv);

  printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
  if (g_failed > 0 || selected == 0 || g_passed != selected) {
    fprintf(stderr, "FAIL: %d test(s) failed\n", g_failed);
    return 1;
  }
  printf("All parameter-array tests passed.\n");
  return 0;
}
