**Chargewire ODBC Driver 2.4.0**, built on Chargewire C++ 2.4.1.

- **Connecting to a stopped server starts nothing** (Chargewire §11.2, lazy connect). When a stopped
  server's stand-in says so, `SQLDriverConnect` returns without signing in; connection checks,
  `SQLGetInfo`, `SQLDisconnect`, autocommit changes and `SQLEndTran` send nothing; the first
  statement or catalog function signs in (starting the server) and runs a product's session
  settings first.
  Driver-manager pools and BI tools that open and check connections no longer wake a stopped
  auto-resume cluster.
- **A connection whose token was finally refused reports itself dead** (`token_revoked`,
  `credentials_changed`, `principal_dropped`), so pools replace it instead of reusing it.
- **Closes and cancels never sign in**, now including `SQLCancel` and closing a partly read split
  result (Chargewire C++ 2.4.0).

The release gates run the unit and live suites on all four platforms against released Hardcharger 2.7.0, over HTTP and verified TLS. Windows DLL and MSI are EV-signed and verified before publication.
