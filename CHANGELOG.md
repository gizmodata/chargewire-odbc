# Changelog

## [Unreleased]

## [1.0.0] - 2026-10-25

### Added
- Initial Hardcharger ODBC driver with native Warp SQL over HTTP/HTTPS and Arrow IPC streaming.
- Windows x64 signed DLL and MSI, macOS arm64 and Linux amd64/arm64 libraries.
- Password, bearer token, browser OAuth and client-certificate authentication.
- Prepared parameters, catalog/schema/table/column/type metadata, primary and foreign keys,
  transactional commit/rollback, cancellation and server-side statement timeouts.
- The GizmoSQL ODBC regression suites adapted to Hardcharger, plus native Warp SQL regressions.
- Hardcharger branding matching the Hardcharger ADBC repository.

### Fixed
- Wide column and descriptor names now report character counts instead of byte counts,
  preventing garbage names and UTF-16 decoding errors in pyodbc.
- Bound NULL and empty parameters remain bound even when their data pointer is null.
- Parameters in untyped expressions such as `SELECT ?` now derive their Arrow types from ODBC
  bindings; binary parameters support embedded NUL bytes in both typed and untyped statements.
- Cancellation no longer waits behind the statement mutex held by a blocked query/fetch, and
  it leaves the executing thread in control of result buffers and diagnostics.
- Cached prepared statements now start a new transaction after commit or rollback when
  autocommit is off. Previously, reusing a pyodbc prepared handle could commit its next
  write immediately because only preparing a new handle re-established manual transactions.
- Legacy diagnostic iteration now terminates after the last record, preventing driver managers
  from looping when a connection fails.
- ODBC trace logs no longer write connection strings containing passwords or bearer tokens.
- Connection strings now handle multiple braced values, embedded semicolons and escaped closing braces.
- Query/session tags are validated as JSON objects and recorded in Hardcharger instrumentation.
- UTF-16 conversion now handles supplementary characters such as emoji in parameters and results.

The ODBC compatibility layer is derived from GizmoSQL ODBC 1.1.3. Its history and attribution
are retained in Git and NOTICE; the Hardcharger release line starts at 1.0.0.
