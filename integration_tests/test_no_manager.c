/*
 * Copyright (C) 2026 GizmoData LLC. Apache-2.0.
 *
 * The driver's own answers, with no driver manager in between: the library is loaded
 * directly (HARDCHARGER_DRIVER_LIBRARY) and its exports are called, as an application that
 * links a driver directly does. A driver manager that tracks statement states refuses some
 * calls itself, which hides what the driver would do.
 *
 * A statement without rows leaves no cursor: SQLFetch, SQLGetData and SQLCloseCursor are an
 * invalid cursor state (24000), never the previous statement's rows and never a crash (the
 * driver dereferenced a missing result set after SQLFreeStmt(SQL_CLOSE)). SQLExecDirect
 * runs statements the server refuses to prepare (issue #3).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define LOAD_LIBRARY(path) ((void *)LoadLibraryA(path))
#define LOAD_SYMBOL(lib, name) ((void *)GetProcAddress((HMODULE)(lib), name))
#else
#include <dlfcn.h>
#define LOAD_LIBRARY(path) dlopen(path, RTLD_NOW | RTLD_LOCAL)
#define LOAD_SYMBOL(lib, name) dlsym(lib, name)
#endif
#include <sql.h>
#include <sqlext.h>

typedef SQLRETURN(SQL_API *AllocHandleFn)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *);
typedef SQLRETURN(SQL_API *SetEnvAttrFn)(SQLHENV, SQLINTEGER, SQLPOINTER, SQLINTEGER);
typedef SQLRETURN(SQL_API *DriverConnectFn)(SQLHDBC, SQLHWND, SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT,
                                            SQLSMALLINT *, SQLUSMALLINT);
typedef SQLRETURN(SQL_API *ExecDirectFn)(SQLHSTMT, SQLCHAR *, SQLINTEGER);
typedef SQLRETURN(SQL_API *StmtFn)(SQLHSTMT);
typedef SQLRETURN(SQL_API *NumResultColsFn)(SQLHSTMT, SQLSMALLINT *);
typedef SQLRETURN(SQL_API *GetDataFn)(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT, SQLPOINTER, SQLLEN, SQLLEN *);
typedef SQLRETURN(SQL_API *FreeStmtFn)(SQLHSTMT, SQLUSMALLINT);
typedef SQLRETURN(SQL_API *GetDiagRecFn)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT, SQLCHAR *, SQLINTEGER *, SQLCHAR *,
                                         SQLSMALLINT, SQLSMALLINT *);
typedef SQLRETURN(SQL_API *FreeHandleFn)(SQLSMALLINT, SQLHANDLE);

static AllocHandleFn pAllocHandle;
static SetEnvAttrFn pSetEnvAttr;
static DriverConnectFn pDriverConnect;
static ExecDirectFn pExecDirect;
static StmtFn pFetch, pCloseCursor;
static NumResultColsFn pNumResultCols;
static GetDataFn pGetData;
static FreeStmtFn pFreeStmt;
static GetDiagRecFn pGetDiagRec;
static FreeHandleFn pFreeHandle;
static StmtFn pDisconnect;

static int g_failed = 0;

#define FAIL(...) do { fprintf(stderr, "FAIL: " __VA_ARGS__); fprintf(stderr, "\n"); g_failed++; } while (0)

static void diag(SQLSMALLINT type, SQLHANDLE handle, SQLCHAR state[8], SQLCHAR *msg, SQLSMALLINT size) {
  SQLINTEGER native;
  SQLSMALLINT len;
  state[0] = 0;
  msg[0] = 0;
  pGetDiagRec(type, handle, 1, state, &native, msg, size, &len);
}

static int ok(SQLHSTMT stmt, SQLRETURN rc, const char *what) {
  if (SQL_SUCCEEDED(rc)) return 1;
  SQLCHAR state[8], msg[1024];
  diag(SQL_HANDLE_STMT, stmt, state, msg, sizeof(msg));
  FAIL("%s (rc=%d) [%s] %s", what, (int)rc, state, msg);
  return 0;
}

static void expect_no_cursor(SQLHSTMT stmt, SQLRETURN rc, const char *what) {
  SQLCHAR state[8], msg[1024];
  diag(SQL_HANDLE_STMT, stmt, state, msg, sizeof(msg));
  if (rc != SQL_ERROR || strcmp((const char *)state, "24000") != 0) {
    FAIL("%s: expected SQL_ERROR [24000], got rc=%d [%s] %s", what, (int)rc, state, msg);
  } else {
    printf("  %s: [%s] %s\n", what, state, msg);
  }
}

static void expect_columns(SQLHSTMT stmt, SQLSMALLINT want, const char *what) {
  SQLSMALLINT cols = -1;
  if (ok(stmt, pNumResultCols(stmt, &cols), "SQLNumResultCols") && cols != want) {
    FAIL("%s: %d result columns, expected %d", what, (int)cols, (int)want);
  }
}

#define BIND(var, type, name) \
  if (!(var = (type)LOAD_SYMBOL(lib, name))) { fprintf(stderr, "FAIL: the driver does not export %s\n", name); return 1; }

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  printf("=== Integration Tests (driver without a driver manager) ===\n");
  const char *path = getenv("HARDCHARGER_DRIVER_LIBRARY");
  if (!path || !path[0]) {
    fprintf(stderr, "FAIL: HARDCHARGER_DRIVER_LIBRARY names no driver library\n");
    return 1;
  }
  void *lib = LOAD_LIBRARY(path);
  if (!lib) {
    fprintf(stderr, "FAIL: cannot load %s\n", path);
    return 1;
  }
  BIND(pAllocHandle, AllocHandleFn, "SQLAllocHandle");
  BIND(pSetEnvAttr, SetEnvAttrFn, "SQLSetEnvAttr");
  BIND(pDriverConnect, DriverConnectFn, "SQLDriverConnect");
  BIND(pExecDirect, ExecDirectFn, "SQLExecDirect");
  BIND(pFetch, StmtFn, "SQLFetch");
  BIND(pCloseCursor, StmtFn, "SQLCloseCursor");
  BIND(pNumResultCols, NumResultColsFn, "SQLNumResultCols");
  BIND(pGetData, GetDataFn, "SQLGetData");
  BIND(pFreeStmt, FreeStmtFn, "SQLFreeStmt");
  BIND(pGetDiagRec, GetDiagRecFn, "SQLGetDiagRec");
  BIND(pFreeHandle, FreeHandleFn, "SQLFreeHandle");
  BIND(pDisconnect, StmtFn, "SQLDisconnect");

  SQLHENV env = SQL_NULL_HENV;
  SQLHDBC dbc = SQL_NULL_HDBC;
  SQLHSTMT stmt = SQL_NULL_HSTMT;
  char conn[1024];
  const char *host = getenv("HARDCHARGER_HOST"), *port = getenv("HARDCHARGER_PORT");
  snprintf(conn, sizeof(conn), "host=%s;port=%s;uid=hardcharger_user;pwd=hardcharger_password;useEncryption=false",
           host && host[0] ? host : "127.0.0.1", port && port[0] ? port : "31337");
  if (!SQL_SUCCEEDED(pAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env)) ||
      !SQL_SUCCEEDED(pSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0)) ||
      !SQL_SUCCEEDED(pAllocHandle(SQL_HANDLE_DBC, env, &dbc))) {
    fprintf(stderr, "FAIL: allocate handles\n");
    return 1;
  }
  SQLRETURN rc = pDriverConnect(dbc, NULL, (SQLCHAR *)conn, SQL_NTS, NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
  if (!SQL_SUCCEEDED(rc)) {
    SQLCHAR state[8], msg[1024];
    diag(SQL_HANDLE_DBC, dbc, state, msg, sizeof(msg));
    fprintf(stderr, "FAIL: SQLDriverConnect (rc=%d) [%s] %s\n", (int)rc, state, msg);
    return 1;
  }
  if (!SQL_SUCCEEDED(pAllocHandle(SQL_HANDLE_STMT, dbc, &stmt))) {
    fprintf(stderr, "FAIL: allocate a statement\n");
    return 1;
  }

  printf("\n--- A statement without rows after one with rows, on the same handle ---\n");
  SQLINTEGER value = 0;
  SQLLEN len = 0;
  if (ok(stmt, pExecDirect(stmt, (SQLCHAR *)"SELECT 7 AS a", SQL_NTS), "SELECT 7")) {
    expect_columns(stmt, 1, "SELECT");
    /* The application doesn't close the cursor before the next statement. */
    if (ok(stmt, pExecDirect(stmt, (SQLCHAR *)"CREATE OR REPLACE TEMP TABLE no_manager (i INTEGER)", SQL_NTS),
           "CREATE")) {
      expect_columns(stmt, 0, "CREATE after SELECT");
      expect_no_cursor(stmt, pFetch(stmt), "SQLFetch after CREATE");
      expect_no_cursor(stmt, pGetData(stmt, 1, SQL_C_SLONG, &value, 0, &len), "SQLGetData after CREATE");
      expect_no_cursor(stmt, pCloseCursor(stmt), "SQLCloseCursor without a cursor");
    }
  }

  printf("\n--- SQLFetch after SQLFreeStmt(SQL_CLOSE) ---\n");
  if (ok(stmt, pExecDirect(stmt, (SQLCHAR *)"SELECT 8 AS b", SQL_NTS), "SELECT 8")) {
    pFreeStmt(stmt, SQL_CLOSE);
    expect_no_cursor(stmt, pFetch(stmt), "SQLFetch after SQLFreeStmt(SQL_CLOSE)");
  }

  printf("\n--- Rows after all that ---\n");
  if (ok(stmt, pExecDirect(stmt, (SQLCHAR *)"INSERT INTO no_manager VALUES (9)", SQL_NTS), "INSERT") &&
      ok(stmt, pExecDirect(stmt, (SQLCHAR *)"SELECT i FROM no_manager", SQL_NTS), "SELECT i") &&
      ok(stmt, pFetch(stmt), "SQLFetch") && ok(stmt, pGetData(stmt, 1, SQL_C_SLONG, &value, 0, &len), "SQLGetData")) {
    if (value != 9) FAIL("read %d, expected 9", (int)value);
    if (pFetch(stmt) != SQL_NO_DATA) FAIL("a second row");
  }
  pFreeStmt(stmt, SQL_CLOSE);

  const char *dbms = getenv("ODBC_EXPECTED_DBMS_NAME");
  if (!dbms || strcmp(dbms, "Hardcharger") == 0) {
    printf("\n--- SET hardcharger.* through SQLExecDirect ---\n");
    if (ok(stmt, pExecDirect(stmt, (SQLCHAR *)"SET hardcharger.query_timeout = 30", SQL_NTS), "SET")) {
      expect_columns(stmt, 0, "SET");
    }
  }

  pFreeHandle(SQL_HANDLE_STMT, stmt);
  pDisconnect(dbc);
  pFreeHandle(SQL_HANDLE_DBC, dbc);
  pFreeHandle(SQL_HANDLE_ENV, env);
  printf("\n=== Results: %s ===\n", g_failed ? "FAILED" : "passed");
  return g_failed ? 1 : 0;
}
