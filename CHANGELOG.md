# Changelog

## [Unreleased]

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
