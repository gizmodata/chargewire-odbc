/*
 * Copyright (C) 2026 GizmoData LLC. Apache-2.0.
 *
 * A session the server ended because its transaction sat idle (Chargewire 2.1 §5.6/§5.7:
 * session_ended, reason idle_in_transaction). Run against a server started with a 1 s
 * idle-in-transaction timeout (scripts/test-live.py does this).
 *
 * The driver never replaces a lost session, so it never runs a statement again on a new
 * one in autocommit after the server rolled the transaction back. It must fail the
 * statement with a transaction-rollback SQLSTATE (40000) naming the reason, report the
 * connection dead (SQL_ATTR_CONNECTION_DEAD) so pools discard it, accept a rollback of the
 * transaction the server already rolled back, and disconnect cleanly.
 *
 * KILL SESSION run through the driver (SQLExecDirect, no prepare) ends another connection's
 * session: that connection's next statement fails with 08S01 naming the reason killed, and
 * it reports itself dead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define SLEEP_SECONDS(s) Sleep((s) * 1000)
#else
#include <unistd.h>
#define SLEEP_SECONDS(s) sleep(s)
#endif
#include <sql.h>
#include <sqlext.h>

#define MAX_CONN_STR 1024
/* The server ends a session within 5 s after its 1 s timeout passes. */
#define IDLE_SECONDS 8

static int g_passed = 0;
static int g_failed = 0;

static void print_diagnostics(SQLSMALLINT type, SQLHANDLE handle) {
  SQLCHAR state[8], msg[2048];
  SQLINTEGER native;
  SQLSMALLINT len;
  SQLSMALLINT i = 1;
  while (SQLGetDiagRec(type, handle, i++, state, &native, msg, sizeof(msg), &len) == SQL_SUCCESS) {
    fprintf(stderr, "  [%s] %s\n", state, msg);
  }
}

static int open_connection(SQLHENV *env, SQLHDBC *dbc) {
  const char *driver = getenv("HARDCHARGER_DRIVER");
  const char *host = getenv("HARDCHARGER_HOST");
  const char *port = getenv("HARDCHARGER_PORT");
  char conn[MAX_CONN_STR];
  snprintf(conn, sizeof(conn),
           "Driver=%s;host=%s;port=%s;uid=hardcharger_user;pwd=hardcharger_password;useEncryption=false",
           driver ? driver : "", host ? host : "127.0.0.1", port ? port : "31337");
  if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, env) != SQL_SUCCESS) return 1;
  SQLSetEnvAttr(*env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);
  if (SQLAllocHandle(SQL_HANDLE_DBC, *env, dbc) != SQL_SUCCESS) return 1;
  SQLCHAR out[MAX_CONN_STR];
  SQLSMALLINT out_len;
  SQLRETURN rc = SQLDriverConnect(*dbc, NULL, (SQLCHAR *)conn, SQL_NTS, out, sizeof(out), &out_len,
                                  SQL_DRIVER_NOPROMPT);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    print_diagnostics(SQL_HANDLE_DBC, *dbc);
    return 1;
  }
  return 0;
}

static void free_connection(SQLHENV env, SQLHDBC dbc) {
  SQLFreeHandle(SQL_HANDLE_DBC, dbc);
  SQLFreeHandle(SQL_HANDLE_ENV, env);
}

static int exec_ok(SQLHDBC dbc, const char *sql) {
  SQLHSTMT stmt;
  SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
  SQLRETURN rc = SQLExecDirect(stmt, (SQLCHAR *)sql, SQL_NTS);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: '%s' (rc=%d)\n", sql, rc);
    print_diagnostics(SQL_HANDLE_STMT, stmt);
    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    return 1;
  }
  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
  return 0;
}

static SQLUINTEGER connection_dead(SQLHDBC dbc) {
  SQLUINTEGER dead = 12345;
  SQLRETURN rc = SQLGetConnectAttr(dbc, SQL_ATTR_CONNECTION_DEAD, &dead, sizeof(dead), NULL);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    print_diagnostics(SQL_HANDLE_DBC, dbc);
    return 12345;
  }
  return dead;
}

/* The statement after the idle gap must fail with 40000 naming idle_in_transaction. */
static int expect_idle_in_transaction_error(SQLHDBC dbc, const char *sql) {
  SQLHSTMT stmt;
  SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
  SQLRETURN rc = SQLExecDirect(stmt, (SQLCHAR *)sql, SQL_NTS);
  SQLCHAR state[8] = "", msg[2048] = "";
  SQLINTEGER native;
  SQLSMALLINT len;
  SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state, &native, msg, sizeof(msg), &len);
  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
  printf("  rc=%d [%s] %s\n", rc, state, msg);
  if (rc != SQL_ERROR) {
    fprintf(stderr, "FAIL: '%s' after the idle gap returned %d, expected SQL_ERROR\n", sql, rc);
    return 1;
  }
  if (strcmp((const char *)state, "40000") != 0) {
    fprintf(stderr, "FAIL: SQLSTATE %s, expected 40000 (transaction rollback)\n", state);
    return 1;
  }
  if (!strstr((const char *)msg, "idle_in_transaction")) {
    fprintf(stderr, "FAIL: the message does not name the reason idle_in_transaction\n");
    return 1;
  }
  return 0;
}

static int count_rows(const char *table) {
  SQLHENV env;
  SQLHDBC dbc;
  if (open_connection(&env, &dbc)) return -1;
  SQLHSTMT stmt;
  SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
  char sql[256];
  snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s", table);
  SQLBIGINT n = -1;
  SQLRETURN rc = SQLExecDirect(stmt, (SQLCHAR *)sql, SQL_NTS);
  if ((rc == SQL_SUCCESS || rc == SQL_SUCCESS_WITH_INFO) && SQLFetch(stmt) == SQL_SUCCESS) {
    SQLGetData(stmt, 1, SQL_C_SBIGINT, &n, 0, NULL);
  } else {
    print_diagnostics(SQL_HANDLE_STMT, stmt);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
  SQLDisconnect(dbc);
  free_connection(env, dbc);
  return (int)n;
}

/* After the error: dead, a rollback succeeds, and disconnect succeeds. */
static int expect_dead_connection(SQLHDBC dbc) {
  SQLUINTEGER dead = connection_dead(dbc);
  if (dead != SQL_CD_TRUE) {
    fprintf(stderr, "FAIL: SQL_ATTR_CONNECTION_DEAD=%u after the session ended, expected SQL_CD_TRUE\n",
            (unsigned)dead);
    return 1;
  }
  SQLRETURN rc = SQLEndTran(SQL_HANDLE_DBC, dbc, SQL_ROLLBACK);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: rollback of the transaction the server already rolled back (rc=%d)\n", rc);
    print_diagnostics(SQL_HANDLE_DBC, dbc);
    return 1;
  }
  rc = SQLDisconnect(dbc);
  if (rc != SQL_SUCCESS && rc != SQL_SUCCESS_WITH_INFO) {
    fprintf(stderr, "FAIL: SQLDisconnect of the dead connection (rc=%d)\n", rc);
    print_diagnostics(SQL_HANDLE_DBC, dbc);
    return 1;
  }
  return 0;
}

/* A transaction begun in SQL, which the driver does not track. */
static int test_begin_in_sql(void) {
  printf("\n--- Test: BEGIN in SQL, idle past the timeout ---\n");
  SQLHENV env;
  SQLHDBC dbc;
  if (open_connection(&env, &dbc)) return 1;
  if (exec_ok(dbc, "CREATE OR REPLACE TABLE odbc_idle_sql (i INTEGER)") || exec_ok(dbc, "BEGIN") ||
      exec_ok(dbc, "INSERT INTO odbc_idle_sql VALUES (1)")) {
    SQLDisconnect(dbc);
    free_connection(env, dbc);
    return 1;
  }
  if (connection_dead(dbc) != SQL_CD_FALSE) {
    fprintf(stderr, "FAIL: a live connection reports itself dead\n");
    return 1;
  }
  SLEEP_SECONDS(IDLE_SECONDS);
  int failed = expect_idle_in_transaction_error(dbc, "INSERT INTO odbc_idle_sql VALUES (2)");
  if (!failed) failed = expect_dead_connection(dbc);
  free_connection(env, dbc);
  /* Neither insert committed: not the rolled-back one, and not the second one on its own. */
  int rows = count_rows("odbc_idle_sql");
  if (!failed && rows != 0) {
    fprintf(stderr, "FAIL: %d rows committed, expected 0\n", rows);
    failed = 1;
  }
  if (!failed) printf("  PASS\n");
  return failed;
}

/* A transaction the driver opened (autocommit off). */
static int test_autocommit_off(void) {
  printf("\n--- Test: autocommit off, idle past the timeout ---\n");
  SQLHENV env;
  SQLHDBC dbc;
  if (open_connection(&env, &dbc)) return 1;
  if (exec_ok(dbc, "CREATE OR REPLACE TABLE odbc_idle_manual (i INTEGER)")) return 1;
  SQLSetConnectAttr(dbc, SQL_ATTR_AUTOCOMMIT, (SQLPOINTER)SQL_AUTOCOMMIT_OFF, 0);
  if (exec_ok(dbc, "INSERT INTO odbc_idle_manual VALUES (1)")) return 1;
  SLEEP_SECONDS(IDLE_SECONDS);
  int failed = expect_idle_in_transaction_error(dbc, "INSERT INTO odbc_idle_manual VALUES (2)");
  if (!failed) failed = expect_dead_connection(dbc);
  free_connection(env, dbc);
  int rows = count_rows("odbc_idle_manual");
  if (!failed && rows != 0) {
    fprintf(stderr, "FAIL: %d rows committed, expected 0\n", rows);
    failed = 1;
  }
  if (!failed) printf("  PASS\n");
  return failed;
}

/* KILL SESSION, run through the driver, ends a second connection's session. */
static int test_kill_session(void) {
  printf("\n--- Test: KILL SESSION of another connection's session ---\n");
  const char *dbms = getenv("ODBC_EXPECTED_DBMS_NAME");
  if (dbms && strcmp(dbms, "Hardcharger") != 0) {
    printf("  KILL SESSION is Hardcharger's; not run against %s\n", dbms);
    return 0;
  }
  SQLHENV venv, kenv;
  SQLHDBC victim, killer;
  if (open_connection(&venv, &victim)) return 1;
  if (open_connection(&kenv, &killer)) return 1;
  SQLHSTMT stmt;
  SQLAllocHandle(SQL_HANDLE_STMT, victim, &stmt);
  SQLCHAR session[128] = "";
  SQLLEN len = 0;
  SQLRETURN rc = SQLExecDirect(stmt, (SQLCHAR *)"SELECT CAST(HARDCHARGER_CURRENT_SESSION() AS VARCHAR)", SQL_NTS);
  if (!SQL_SUCCEEDED(rc) || SQLFetch(stmt) != SQL_SUCCESS ||
      !SQL_SUCCEEDED(SQLGetData(stmt, 1, SQL_C_CHAR, session, sizeof(session), &len)) || !session[0]) {
    fprintf(stderr, "FAIL: read the victim's session id\n");
    print_diagnostics(SQL_HANDLE_STMT, stmt);
    return 1;
  }
  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
  char kill[256];
  snprintf(kill, sizeof(kill), "KILL SESSION '%s'", (const char *)session);
  if (exec_ok(killer, kill)) return 1;
  /* The killer's own session carries on. */
  if (exec_ok(killer, "SELECT 1") || connection_dead(killer) != SQL_CD_FALSE) {
    fprintf(stderr, "FAIL: the connection that ran KILL SESSION is affected\n");
    return 1;
  }
  SQLAllocHandle(SQL_HANDLE_STMT, victim, &stmt);
  rc = SQLExecDirect(stmt, (SQLCHAR *)"SELECT 1", SQL_NTS);
  SQLCHAR state[8] = "", msg[2048] = "";
  SQLINTEGER native;
  SQLSMALLINT mlen;
  SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state, &native, msg, sizeof(msg), &mlen);
  SQLFreeHandle(SQL_HANDLE_STMT, stmt);
  printf("  rc=%d [%s] %s\n", rc, state, msg);
  int failed = 0;
  if (rc != SQL_ERROR || strcmp((const char *)state, "08S01") != 0 || !strstr((const char *)msg, "reason killed")) {
    fprintf(stderr, "FAIL: the killed connection's statement: expected SQL_ERROR [08S01] naming reason killed\n");
    failed = 1;
  }
  if (!failed && connection_dead(victim) != SQL_CD_TRUE) {
    fprintf(stderr, "FAIL: the killed connection does not report itself dead\n");
    failed = 1;
  }
  if (!SQL_SUCCEEDED(SQLDisconnect(victim)) && !failed) {
    fprintf(stderr, "FAIL: SQLDisconnect of the killed connection\n");
    print_diagnostics(SQL_HANDLE_DBC, victim);
    failed = 1;
  }
  free_connection(venv, victim);
  SQLDisconnect(killer);
  free_connection(kenv, killer);
  if (!failed) printf("  PASS\n");
  return failed;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  printf("=== Integration Tests (Session loss) ===\n");
  if (test_begin_in_sql()) g_failed++; else g_passed++;
  if (test_autocommit_off()) g_failed++; else g_passed++;
  if (test_kill_session()) g_failed++; else g_passed++;
  printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
  return g_failed ? 1 : 0;
}
