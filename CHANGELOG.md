# Changelog

## [Unreleased]

## [2.3.1] - 2026-11-05

For Chargewire 2.2.0 and Hardcharger 2.7.0. Built on the Chargewire C++ client of release 2.2.2.

### Fixed

- **A fully qualified host name connects over TLS.** A host written with a final dot
  (`db.example.com.`, in a URI or the `host` property) failed every connection with "the TLS
  handshake ... failed: certificate verify failed": the client sent the name with the dot as
  the TLS server name and checked the certificate against it. Chargewire C++ client 2.2.2 uses
  the name without the final dot for both, as certificates and SNI carry it (RFC 6066 §3).

## [2.3.0] - 2026-11-05

For Chargewire 2.2.0 and Hardcharger 2.7.0. Built on the Chargewire C++ client of release 2.2.0.

### Added

- **A product wrapper can choose the default port per host.** `DriverProfile` has a new
  optional `default_port_for_host` hook: when a DSN or connection string gives a `host` but no
  `port`, the driver uses `default_port_for_host(host)`, and without the hook the profile's
  `default_port` for every host, as before. An explicit `port` (or a URI's port) always wins.
  `DefaultPortForHost(profile, host)` applies the rule, and `BuildConnectOptions` takes an
  optional profile so wrappers can test it. The generic driver sets no hook, so its default
  stays 443 for every host. Existing wrappers compile unchanged.

## [2.2.0] - 2026-11-05

For Chargewire 2.2.0 and Hardcharger 2.7.0. Built on the Chargewire C++ client of release 2.2.0.

### Added

- **Connecting to a server that is resuming.** A server that was stopped and is starting
  again (for example one a hosting service stopped while idle and wakes on the first sign-in)
  answers `server_resuming` (Chargewire spec §11.2) until it serves. The driver now waits the
  time the server asks for and sends the same request again, the sign-in included, so
  connecting just takes longer while the server starts. The new `ResumeTimeout` property
  (DSN or connection string, seconds, default `600`, `0` turns the retry off) bounds the
  wait; after it the call fails with SQLSTATE `HYT01` and the server's message. A value that
  is not a whole number of seconds from 0 to 86400 is refused with `HY024`.

### Fixed

- `SQLExecDirect` without bound parameters runs the statement directly on Chargewire's
  statements endpoint instead of preparing it first, so statements a server refuses to prepare
  run through the driver: Hardcharger's `SET hardcharger.*` (for example
  `SET hardcharger.idle_in_transaction_session_timeout = 60` or `SET hardcharger.query_timeout = 30`)
  and `KILL SESSION '<id>'` failed with "can't be prepared; run them directly" (issue #3). It also
  saves a round trip per statement. Result columns, `SQLRowCount`, `SQLMoreResults`, server
  SQLSTATEs, cancellation, query timeouts and the session-loss handling of 2.1.0 (`40000` for
  `idle_in_transaction`, `SQL_ATTR_CONNECTION_DEAD`, nothing ever run again) work as before.
  `SQLPrepare` + `SQLExecute` still prepare.
- `SQLExecDirect` with bound parameters sends them. Before, it ignored them, so
  `SQLBindParameter` + `SQLExecDirect("... ?")` failed with "Values were not provided"; single
  parameters and parameter arrays (`SQL_ATTR_PARAMSET_SIZE`, status array, rows processed) now
  work as with `SQLPrepare` + `SQLExecute`.
- A statement without rows (DML, DDL, `SET`) run on a handle whose previous statement returned
  rows reported that statement's result columns (`SQLNumResultCols`, pyodbc's
  `cursor.description`), and `SQLFetch` read the previous result. It now has no result columns
  and no cursor.
- `SQLFetch` and `SQLGetData` on a statement with no result set (after a statement without rows,
  or after `SQLFreeStmt(SQL_CLOSE)`) crashed the application when it called the driver without
  a driver manager; they now fail with `24000` (invalid cursor state). `SQLCloseCursor` without
  an open cursor reported `28000` (invalid authorization) and now reports `24000`.

### Changed

- Built on the Chargewire C++ client of release 2.2.0 (`third_party/chargewire`), which retries
  `server_resuming`, waiting at least 1 s between attempts. The client identifies itself as
  `chargewire-client-cpp/2.2.0`. `SQL_ATTR_LOGIN_TIMEOUT` bounds the TCP connection and TLS
  handshake only, as before, now documented: a sign-in a resuming server holds (up to 25 s)
  is still answered.
- New live tests: `SET hardcharger.*` read back from `hardcharger_settings()`, a timeout set in
  SQL ending an idle transaction (`40000`), `KILL SESSION` of a second connection (`08S01`,
  reason `killed`, connection dead), bound-parameter `SQLExecDirect`, and a C suite
  (`test_no_manager`) that loads the driver library and calls it without a driver manager.

## [2.1.0] - 2026-11-04

For Chargewire 2.1.0 and Hardcharger 2.7.0. Built on the Chargewire C++ client of release 2.1.0.

### Fixed

- A session the server ended because its transaction sat idle (Chargewire 2.1 `session_ended`,
  reason `idle_in_transaction`, Hardcharger's `idle_in_transaction_session_timeout`) now fails
  the statement with SQLSTATE `40000` (transaction rollback) and a message that names the reason
  and says the transaction was rolled back. Before, it reported `28000` (invalid authorization)
  with only the server's text. The driver never replaces a lost session and never runs a
  statement again, so a statement can't commit on its own after its transaction was rolled back.
- A connection whose session the server no longer has (`session_not_found`: closed, evicted
  after idling, or the server restarted) or ended for any reason now reports
  `SQL_ATTR_CONNECTION_DEAD` true, so connection pools discard it instead of handing it out
  again; before, it stayed "alive" and every statement on it failed. `session_not_found` and a
  `killed` session report `08S01` (they reported `HY000` and `28000`), and the message says what
  happened and that the application must reconnect. A rollback of the transaction the server
  already rolled back succeeds, and disconnecting makes no request.

### Changed

- The C and Python live suites run against Hardcharger 2.7.0, and again on a server restarted
  with a 1 s idle-in-transaction timeout for the new session-loss suites. A regression checks
  that nested schemas (`CREATE SCHEMA a.b`) come back from `SQLTables`, `SQLColumns` and
  `SQLPrimaryKeys` by their path (`a.b`, `a."dot.ted"`) and that the schema filters match that
  spelling; this needed no driver change.
- The client identifies itself as `chargewire-client-cpp/2.1.0` (it said 1.0.1).
## [2.0.0] - 2026-10-29

The driver for the protocol under its new name, Chargewire (Chargewire ODBC Driver, `chargewire-odbc`).
It works with servers on either side of the rename, and pyodbc `fast_executemany` works.

- Initialize wrapper submodules before Windows signing and print failed negative-control diagnostics.
- Add the Homebrew unixODBC runtime path to Linux formula installations.

### Changed

- The driver speaks Chargewire, the protocol's new name. It registers as Chargewire ODBC
  Driver, ships as `chargewire-odbc`, and takes `chargewire://` URIs with the default mount
  path `/chargewire`. Connection strings and DSN settings written before the rename keep
  working unchanged: their URI scheme and default mount path are read as the new ones.
  A DSN or connection string that names the driver by its earlier registered name must
  name Chargewire ODBC Driver instead.
- The driver also works with servers from before the rename: on its first request it
  finds out which spelling of the protocol a server speaks and uses that one.

### Fixed

- pyodbc `fast_executemany` crashed the process (segfault) on every batch, even 5 rows. Arrays of
  parameters now work: the driver honors `SQL_ATTR_PARAMSET_SIZE`, column-wise and row-wise
  `SQL_ATTR_PARAM_BIND_TYPE`, `SQL_ATTR_PARAM_BIND_OFFSET_PTR`, `SQL_ATTR_PARAM_OPERATION_PTR`,
  `SQL_ATTR_PARAMS_PROCESSED_PTR` and the parameter status array, and sends all parameter sets
  in one request (one execution per set). Before, it read pyodbc's offset-relative buffer
  addresses as real pointers, and an ordinary column-wise array inserted only its first row
  while reporting success.
- Parameter values the driver cannot send (data-at-execution, invalid lengths or indicators, a
  null data pointer for a non-NULL value, an unsupported C type, a value out of the column's
  range) now fail with `SQL_ERROR`, a diagnostic naming the parameter set and parameter, and
  `SQL_PARAM_ERROR` in the status array, instead of reading invalid memory or being truncated.
- `SQLDescribeParam` reports DECIMAL precision and scale, and the fractional-second digits of
  TIMESTAMP and TIME parameters (6 for microseconds). pyodbc sizes its fast-path buffers from
  these values: before, it refused decimals with a fraction and cut timestamps to milliseconds.
- `SQL_C_NUMERIC`, `SQL_C_GUID` and `SQL_C_TYPE_TIME` parameters, integer and float conversions
  between C and column types, and `SQL_C_CHAR`/`SQL_C_WCHAR` values without a terminator are
  now converted exactly.
- `SQLFreeStmt(SQL_RESET_PARAMS)` releases parameter bindings instead of doing nothing, and a
  prepared statement no longer re-sends the previous execution's parameters when none are bound.

## [1.0.0] - 2026-10-25

- Generic native Chargewire ODBC driver over HTTP/HTTPS and Arrow IPC.
- Shared ODBC implementation and profile hooks for thin product wrappers.
- chargewire://, https:// and http:// URIs with verified TLS by default.
- Retain GizmoSQL compatibility tests; add URI, Unicode, binary/NULL parameter,
  cancellation, timeout and transaction regressions.
- URI-aware Windows DSN editor, preserved advanced options and certificate sign-in.
- Signed Windows DLL/MSI; Linux x64/ARM64 and macOS ARM64 libraries.
