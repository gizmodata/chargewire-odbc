<p align="center">
  <a href="https://github.com/gizmodata/hardcharger">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://raw.githubusercontent.com/gizmodata/hardcharger-odbc-driver/main/docs/assets/hardcharger-banner-dark.png">
      <img src="https://raw.githubusercontent.com/gizmodata/hardcharger-odbc-driver/main/docs/assets/hardcharger-banner-light.png" alt="Hardcharger™" width="400">
    </picture>
  </a>
</p>
<p align="center">
  <a href="https://github.com/gizmodata/hardcharger-odbc-driver/actions/workflows/ci.yml"><img src="https://github.com/gizmodata/hardcharger-odbc-driver/actions/workflows/ci.yml/badge.svg" alt="Build and tests"></a>
  <a href="https://github.com/gizmodata/hardcharger-odbc-driver/releases/latest"><img src="https://img.shields.io/github/v/release/gizmodata/hardcharger-odbc-driver" alt="Release"></a>
  <a href="https://github.com/gizmodata/warp-sql"><img src="https://img.shields.io/badge/protocol-Warp%20SQL-31a8db.svg" alt="Warp SQL"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache%202.0-brightgreen.svg" alt="Apache 2.0"></a>
  <a href="https://gizmodata.com"><img src="https://img.shields.io/badge/by-GizmoData%E2%84%A2-2563eb.svg" alt="GizmoData™"></a>
</p>
<h3 align="center">Connect ODBC applications to Hardcharger over Warp SQL.</h3>

The official ODBC driver for [Hardcharger™](https://github.com/gizmodata/hardcharger),
GizmoData™'s DuckDB 2.0 server. Uses the native Warp SQL C++ client over HTTP/HTTPS,
with Apache Arrow IPC streaming. There is no Flight SQL, gRPC or protobuf transport.

Based on the [GizmoSQL ODBC driver's](https://github.com/gizmodata/gizmosql-odbc-driver)
ODBC compatibility layer, retaining its accessor, conversion, metadata, parameter and
transaction regression suites. It adds native Warp SQL sessions, prepared statements,
password/token/browser/certificate sign-in, cancellation and statement timeouts.

## Install

Download the package for your platform from [Releases](https://github.com/gizmodata/hardcharger-odbc-driver/releases/latest):

| Platform | Package |
|---|---|
| Windows x64 | `Hardcharger-ODBC-Driver-x64.msi` (recommended), signed DLL and ZIP |
| macOS Apple Silicon | `hardcharger-odbc-macos-arm64.tar.gz` |
| Linux x64 | `hardcharger-odbc-linux-amd64.tar.gz` |
| Linux ARM64 | `hardcharger-odbc-linux-arm64.tar.gz` |

Windows releases use GizmoData's EV code-signing certificate with a SHA-256 digest and
RFC 3161 timestamp. Both DLL and MSI signatures must pass Windows Authenticode and
`signtool verify /pa` checks before publication. The MSI installs and registers
**Hardcharger ODBC Driver** alongside any GizmoSQL driver.

On macOS, install `unixodbc` (`brew install unixodbc`). On Linux, install your
distribution's `unixodbc` package. Extract the archive to a permanent location:

```sh
mkdir -p "$HOME/.local/hardcharger-odbc"
tar -xzf hardcharger-odbc-macos-arm64.tar.gz -C "$HOME/.local/hardcharger-odbc"
```

Register the extracted library in `odbcinst.ini` (find the location with `odbcinst -j`):

```ini
[Hardcharger ODBC Driver]
Description = Hardcharger ODBC Driver (Warp SQL)
Driver = /absolute/path/lib/libhardcharger-odbc.dylib
```

Use `libhardcharger-odbc.so` on Linux. The native Arrow, Warp SQL, Boost and OpenSSL
libraries are linked statically in release builds; unixODBC's installer library and
the platform's C/C++ runtime remain system dependencies.

Add a DSN to `~/.odbc.ini` (Windows: use ODBC Data Sources, 64-bit):

```ini
[Hardcharger]
Driver = Hardcharger ODBC Driver
host = localhost
port = 31337
uid = hardcharger
pwd = change-me
useEncryption = true
```

TLS is on by default. For a local server without TLS, explicitly set
`useEncryption=false`. For a private CA, use `trustedCerts=/path/to/ca.pem`.

## Python / pyodbc

```python
import pyodbc

with pyodbc.connect(
    "Driver={Hardcharger ODBC Driver};host=localhost;port=31337;"
    "uid=hardcharger;pwd=change-me;useEncryption=false;",
    autocommit=True,
) as connection:
    row = connection.execute("SELECT CAST(? AS VARCHAR) AS greeting, 42 AS answer", "hello").fetchone()
    print(row.greeting, row.answer)
```

`pyodbc` defaults to autocommit off: call `commit()` or `rollback()` as appropriate.
Changing autocommit from off to on commits the pending transaction; disconnecting
rolls it back. The driver also supports the ODBC C API, DSNs, row binding,
`SQLGetData`, prepared parameters, catalog/schema/table/column discovery, type info,
and primary/imported/exported key metadata.

## Connection options

Property names are case insensitive.

| Property | Default | Meaning |
|---|---|---|
| `host` | `localhost` | Server DNS name or IP address |
| `port` | `31337` | Warp SQL port |
| `uid` / `user` / `user id` | | Username |
| `pwd` / `password` | | Password |
| `authType` | `basic` | `basic`/`password`, `token`/`access_token`, `token_exchange`, `external`, or `client_certificate`/`mtls` |
| `token` | | Bearer access token; with `token_exchange`, an identity-provider sign-in token |
| `useEncryption` | `true` | HTTPS; `false` explicitly selects HTTP |
| `trustedCerts` | system trust | PEM CA bundle path |
| `disableCertificateVerification` | `false` | Development-only TLS verification bypass |
| `clientCertificate`, `clientKey` | | PEM certificate chain and private key for mutual TLS |
| `basePath` | `/warp` | Protocol mount path |
| `defaultCatalog`, `defaultSchema` | server defaults | Initial session catalog/schema |
| `queryTag` | | JSON object recorded with statements |
| `sessionTag` | | JSON object tagging the session |
| `oauthTimeoutSeconds` | `300` | Browser sign-in deadline |
| `oauthBrowser` | system browser | Browser executable path, or `none` to print the sign-in URL |
| `StringColumnLength` | `1024` fallback | Reported variable-length column size |
| `UseWideChar` | `true` Windows; `false` Unix | Report strings as wide ODBC types |

Browser sign-in uses the server's configured OpenID Connect provider:
`authType=external`. Certificate sign-in uses `authType=client_certificate` together
with `clientCertificate` and `clientKey`; the server must enable certificate sign-in.
Password and certificate credentials renew automatically through the Warp SQL client.
Browser sessions and externally supplied access tokens require a fresh sign-in when
they expire. `SQL_ATTR_LOGIN_TIMEOUT` controls connection establishment;
`SQL_ATTR_QUERY_TIMEOUT` sends the server's statement timeout. `SQLCancel` cancels
work through Warp SQL even while `SQLExecute` is waiting for the result.

## Build and test

See [BUILDING.md](docs/BUILDING.md). The release workflow builds and tests Linux
amd64/arm64, macOS arm64 and Windows amd64 against released Hardcharger 2.0.0.
Tests require a real server; missing binaries or failed live suites fail the build.

```sh
git clone --recurse-submodules https://github.com/gizmodata/hardcharger-odbc-driver.git
cd hardcharger-odbc-driver
python scripts/build-arrow.py --prefix .deps/install
python scripts/build-boost.py --prefix .deps/install
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PWD/.deps/install"
cmake --build build --parallel 3
ctest --test-dir build --output-on-failure
python -m pip install pyodbc requests
python scripts/test-live.py --server /path/to/hardcharger-server --driver build/Release/lib/libhardcharger-odbc.so
```

## License and attribution

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). The original ODBC layer is
copyright Dremio Corporation and GizmoData LLC; the new Warp SQL implementation is
copyright GizmoData LLC. The build pins Warp SQL 1.0.1, Arrow 26.0.0, Boost 1.92.0
(Unix), spdlog 1.17.0 and GoogleTest 1.14.0. The Windows dependency baseline is pinned
in the CI workflow. See [RELEASING.md](RELEASING.md) for reproducible release and signing steps.

<p align="center">
  <a href="https://gizmodata.com"><img src="https://raw.githubusercontent.com/gizmodata/hardcharger-odbc-driver/main/docs/assets/gizmodata-logo.png" alt="GizmoData™" width="56"></a><br>
  <sub>Built by <a href="https://gizmodata.com">GizmoData™</a></sub>
</p>
<p align="center"><sub>Hardcharger™ and GizmoData™ are trademarks of GizmoData LLC.</sub></p>
