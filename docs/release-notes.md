**Chargewire ODBC Driver 2.2.0**, for Chargewire 2.2.0 and Hardcharger 2.7.0.

- **Connecting to a server that is resuming.** A server that was stopped and is starting again
  answers `server_resuming`; the driver waits the time the server asks for (at least 1 s, at
  most 60 s) and sends the same request again, the sign-in included, for up to the new
  `ResumeTimeout` (seconds, default 600, `0` turns it off), then fails with `HYT01`.
- `SQLExecDirect` without bound parameters runs the statement directly instead of preparing
  it, so Hardcharger's `SET hardcharger.*` and `KILL SESSION` run through the driver (#3). With
  bound parameters it now sends them (it ignored them).
- A statement without rows no longer reports the previous statement's result columns or reads
  its result; `SQLFetch`/`SQLGetData`/`SQLCloseCursor` without a result set report `24000`.

See CHANGELOG.md for details. Built on the Chargewire C++ client 2.2.0 and tested against
released Hardcharger 2.7.0 on all four platforms, over HTTP and verified TLS. GizmoData
EV-signed Windows x64 DLL and MSI; macOS 15+ ARM64 and Linux x64/ARM64 libraries (glibc 2.39
or later). See README.md for installation and connection strings.
