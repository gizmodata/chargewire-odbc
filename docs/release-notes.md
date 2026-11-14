**Chargewire ODBC Driver 2.5.0**, built on published Chargewire C++ SDK 2.5.0.

- `ReadOnly=true` requests an enforced read-only session at creation, including lazy connections. The driver requires server confirmation; unsupported or ignored requests fail.
- `SQL_ATTR_ACCESS_MODE=SQL_MODE_READ_ONLY` now requires enforcement. The restriction is one-way, including before connection. An ignored transition closes the connection.
- Ordinary connections remain compatible with servers without read-only sessions. Lost sessions still require an explicit reconnect.

Release gates run unit and HTTP/TLS live suites on all four platforms, including read-only coverage. They use Hardcharger 2.13.0 from its published release, or verified artifacts from a successful main workflow while that release is pending. Windows DLLs and MSIs are EV-signed, signature-verified and installed-driver tested before publication.
