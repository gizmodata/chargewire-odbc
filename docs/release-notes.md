**Chargewire ODBC Driver 2.1.0**, for Chargewire 2.1.0 and Hardcharger 2.7.0.

- A session the server ended because its transaction sat idle (`session_ended`, reason
  `idle_in_transaction`) fails the statement with SQLSTATE `40000` and a message naming the
  reason; the driver never runs the statement again on a new session.
- A connection whose session the server ended or no longer has reports
  `SQL_ATTR_CONNECTION_DEAD`, so connection pools discard it; `session_not_found` and killed
  sessions report `08S01`.
- Nested schemas (`a.b`) pass through the catalog functions by their path.

See CHANGELOG.md for details. Built on the Chargewire C++ client 2.1.0 and tested against
released Hardcharger 2.7.0 on all four platforms, over HTTP and verified TLS. GizmoData
EV-signed Windows x64 DLL and MSI; macOS 15+ ARM64 and Linux x64/ARM64 libraries (glibc 2.39
or later). See README.md for installation and connection strings.
