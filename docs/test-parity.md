# Initial-release regression coverage

Baseline: GizmoSQL ODBC 1.1.3, commit `99d236a`. Its GoogleTest binary lists
174 cases. The Hardcharger binary lists 179 cases:

- All baseline conversion, accessor, parameter, JSON, metadata transformer, escape
  sequence, utility and connection-attribute assertions are retained under Hardcharger names.
- The TCP and TLS Flight location tests are replaced by equivalent Warp SQL connection-option tests.
- Five additional unit cases cover port validation, mutual-TLS/path options, and connection-string
  escaping, empty values and malformed input.

The inherited live suites retain their assertions and run against released Hardcharger 2.0.0:
15 C metadata/parameter/primary-key/foreign-key cases, 9 C transaction cases and 6 pyodbc cases.
The C fixtures must not skip cases. pyodbc runs over HTTP and verified TLS on every platform.

The additional Warp SQL suite has 15 cases: driver branding/version, Unicode parameters/results,
decimal/binary/null/date values, invalid credentials/catalogs, SQL errors/recovery, repeated
prepared execution, cached prepared handles across commit/rollback, large streaming results/early close, update counts/cross-reference metadata,
access-token aliases, cancellation, statement timeouts, query/session instrumentation tags,
client-certificate sign-in and certificate rejection. The last two apply to the TLS run.

The release workflow requires Linux amd64/arm64, macOS arm64 and Windows amd64. Windows release
verification also installs the signed MSI and repeats Python live suites through its installed
driver, over both HTTP and TLS. Missing server binaries, test failures, signature failures and
installer failures block publication. The test harness and CI workflow are the executable reference.
