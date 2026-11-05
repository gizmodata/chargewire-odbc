# Regression coverage

Baseline: GizmoSQL ODBC 1.1.3, commit `99d236a`, with 174 GoogleTest cases.
The generic Chargewire binary has 198 cases: the original assertions, equivalent native
TCP/TLS option tests, five connection-option/string regressions eight URI regressions, two DSN-editor regressions
and nine parameter-array regressions (column-wise/row-wise addresses, bind offsets, refusals, conversions).
Product wrappers compile these same sources and can add profile tests.

Inherited live coverage: 15 C metadata/parameter/key cases, 9 C transaction cases and
6 pyodbc cases. C fixtures fail if any case is skipped. Additional native Chargewire coverage
has 20 cases: branding/version, Unicode values and metadata, untyped/binary/NULL parameters,
errors/recovery, prepared reuse across commit/rollback, large streams/early close, update
counts and key metadata, access tokens, cancellation, timeouts, JSON query tags, URI
connections, DSN lookup and credential redaction, certificate sign-in and certificate rejection.
Arrays of parameters have 4 C cases (5000 column-wise sets, row-wise sets at bind-offset-relative
addresses as pyodbc binds them, SQL_PARAM_IGNORE, and refusals that must return a specific SQLSTATE)
and 6 pyodbc `fast_executemany` cases that read every row back and compare it with what was sent.
Session loss has 2 C cases (a transaction begun in SQL and one with autocommit off, each left
idle past the server's 1 s idle-in-transaction timeout: SQLSTATE 40000 naming the reason,
`SQL_ATTR_CONNECTION_DEAD`, rollback and disconnect succeed, nothing committed) and 5 pyodbc
cases (the same two, a control transaction kept alive, a closed session and a killed one, 08S01).
Certificate cases apply to TLS. Hardcharger-specific session/instrumentation behavior
is tested in the product wrapper.

Every platform runs the same core suites against released Hardcharger 2.7.0. Python
coverage runs over HTTP and verified TLS. Windows publication additionally installs
the signed MSI and repeats the Python suites through its installed signed DLL.
Missing binaries, failures, unsigned files or installer failures block publication.
