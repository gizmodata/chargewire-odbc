The first **Chargewire ODBC driver**, connecting ODBC applications to Chargewire servers
over HTTP/HTTPS and Apache Arrow IPC with the native C++ client.

- Generic `chargewire://`, `https://` and `http://` URIs; verified TLS by default.
- Prepared parameters, streaming results, metadata, transactions, cancellation and timeouts.
- Password, bearer/subject token, browser and mutual-TLS certificate sign-in.
- Shared implementation and regression suites for thin product drivers such as Hardcharger.
- GizmoData EV-signed Windows x64 DLL and MSI; macOS 15+ ARM64 and Linux x64/ARM64 libraries.

Built and tested against released Hardcharger 2.0.0. Linux requires glibc 2.39 or later.
See README.md for installation and connection strings.
