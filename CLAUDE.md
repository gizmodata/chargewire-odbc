# Chargewire ODBC driver

Native Chargewire C++ client (`third_party/chargewire`, pinned), Arrow IPC + compute,
and the GizmoSQL ODBC driver's compatibility layer. No Flight SQL/gRPC transport.
See docs/BUILDING.md, README.md and RELEASING.md for the supported build/test/release flow.

Always preserve and run the inherited GoogleTest and C/pyodbc live suites. Add meaningful
regressions for changes. Live tests require a real Hardcharger binary and must fail rather
than skip missing coverage. Update CHANGELOG.md with behavior changes. Preserve upstream
licenses and attribution. Release Windows DLLs and MSIs must use GizmoData's existing
certificate and pass the signature checks; no unsigned release fallback.
