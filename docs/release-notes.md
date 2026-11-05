**Chargewire ODBC Driver 2.3.0**, for Chargewire 2.2.0 and Hardcharger 2.7.0.

- **Product wrappers can choose the default port per host.** The driver profile has a new
  optional `default_port_for_host` hook, used when a DSN or connection string gives a `host`
  but no `port`; an explicit port always wins. The generic driver sets no hook, so its
  default stays 443 for every host, and existing wrappers compile unchanged.

See CHANGELOG.md for details. Built on the Chargewire C++ client 2.2.0 and tested against
released Hardcharger 2.7.0 on all four platforms, over HTTP and verified TLS. GizmoData
EV-signed Windows x64 DLL and MSI; macOS 15+ ARM64 and Linux x64/ARM64 libraries (glibc 2.39
or later). See README.md for installation and connection strings.
