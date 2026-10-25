# Building Hardcharger ODBC

CMake 3.28+, Ninja, a C++20 compiler, Python 3.14+, OpenSSL 3 development headers,
RapidJSON and an ODBC development SDK are required. On macOS:

```sh
brew install cmake ninja unixodbc openssl@3 rapidjson
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
```

On Ubuntu:

```sh
sudo apt-get install cmake ninja-build unixodbc-dev libssl-dev rapidjson-dev
```

Initialize the pinned Warp SQL submodule (`git submodule update --init --recursive`).
Run `scripts/build-arrow.py --prefix .deps/install` and
`scripts/build-boost.py --prefix .deps/install`, then the CMake commands in the README.
An existing static Arrow/ArrowCompute and Boost installation can be selected with
`CMAKE_PREFIX_PATH`. Arrow is built with IPC, compute, LZ4 and ZSTD; Flight is disabled.

On Windows, use an MSVC x64 developer shell and the pinned vcpkg commit in
`.github/workflows/ci.yml`. Bootstrap vcpkg and install this manifest with
`--triplet x64-windows-static-md`. Pass its toolchain to the Arrow helper and to
CMake, alongside `-DVCPKG_TARGET_TRIPLET=x64-windows-static-md`. CI is the executable
reference for all four platforms.

`ctest --test-dir build --output-on-failure` runs the retained GoogleTest suite.
Install `pyodbc` and `requests` and run `scripts/test-live.py --server PATH --driver PATH`
for C metadata/transactions and Python suites over both HTTP and verified HTTPS.
It starts isolated servers with disposable databases and refuses an occupied test port (31337
by default; use `--port` to select another),
loads TPC-H for the metadata fixtures, and fails if any test fails. Server logs are
written to `live-plain.log` and `live-tls.log`. Tests never connect to production.

The C fixtures remain the GizmoSQL driver's fixtures, adapted to Hardcharger's
credentials and naming; their metadata/parameter/PK/FK and transaction assertions
are retained. The two transport location tests now assert native Warp SQL connection
options. Additional Warp SQL tests cover Unicode including surrogate pairs, binary,
decimal and null values, failed authentication/catalogs, diagnostics and recovery,
large streams and abandoned results, repeated preparation, cross-reference metadata,
access tokens, cancellation, query timeout and certificate rejection.
