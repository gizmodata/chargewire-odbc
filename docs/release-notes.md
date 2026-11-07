**Chargewire ODBC Driver 2.3.3**, built on Chargewire C++ 2.3.0.

- **Disconnecting and freeing statements never sign in.** A disconnect late in a token's life
  used to sign in again just to close, which woke a stopped auto-resume server for nobody (or
  undid a person's Stop) and could wait up to `ResumeTimeout` for it. Disconnects now use the
  token the connection already has and wait at most a few seconds; freeing a prepared statement
  sends nothing. Requests that do work renew the token as before.

The release gates run the unit and live suites on all four platforms against released Hardcharger 2.7.0, over HTTP and verified TLS. Windows DLL and MSI are EV-signed and verified before publication.
