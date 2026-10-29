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

## [1.0.0] - 2026-10-25

- Generic native Chargewire ODBC driver over HTTP/HTTPS and Arrow IPC.
- Shared ODBC implementation and profile hooks for thin product wrappers.
- chargewire://, https:// and http:// URIs with verified TLS by default.
- Retain GizmoSQL compatibility tests; add URI, Unicode, binary/NULL parameter,
  cancellation, timeout and transaction regressions.
- URI-aware Windows DSN editor, preserved advanced options and certificate sign-in.
- Signed Windows DLL/MSI; Linux x64/ARM64 and macOS ARM64 libraries.
