# Regression coverage

Baseline: GizmoSQL ODBC 1.1.3, commit `99d236a`, with 174 GoogleTest cases.
The generic Warp SQL binary has 189 cases: the original assertions, equivalent native
TCP/TLS option tests, five connection-option/string regressions eight URI regressions and two DSN-editor regressions.
Product wrappers compile these same sources and can add profile tests.

Inherited live coverage: 15 C metadata/parameter/key cases, 9 C transaction cases and
6 pyodbc cases. C fixtures fail if any case is skipped. Additional native Warp coverage
has 20 cases: branding/version, Unicode values and metadata, untyped/binary/NULL parameters,
errors/recovery, prepared reuse across commit/rollback, large streams/early close, update
counts and key metadata, access tokens, cancellation, timeouts, JSON query tags, URI
connections, DSN lookup and credential redaction, certificate sign-in and certificate rejection.
Certificate cases apply to TLS. Hardcharger-specific session/instrumentation behavior
is tested in the product wrapper.

Every platform runs the same core suites against released Hardcharger 2.0.0. Python
coverage runs over HTTP and verified TLS. Windows publication additionally installs
the signed MSI and repeats the Python suites through its installed signed DLL.
Missing binaries, failures, unsigned files or installer failures block publication.
