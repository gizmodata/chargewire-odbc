**Chargewire ODBC Driver 2.3.1**, for Chargewire 2.2.0 and Hardcharger 2.7.0.

- **A fully qualified host name connects over TLS.** A host written with a final dot
  (`db.example.com.`) failed the TLS certificate check; the Chargewire C++ client 2.2.2 now
  sends and checks the name without the final dot.

2.3.0 added an optional `default_port_for_host` hook to the driver profile, so a product
wrapper can choose the default port per host. See CHANGELOG.md for details. Built on the
Chargewire C++ client 2.2.2 and tested against released Hardcharger 2.7.0 on all four
platforms, over HTTP and verified TLS. GizmoData EV-signed Windows x64 DLL and MSI; macOS 15+
ARM64 and Linux x64/ARM64 libraries (glibc 2.39 or later). See README.md for installation and
connection strings.
