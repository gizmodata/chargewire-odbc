# Connection configuration

See the [README options and URI tables](../README.md#connect) for the complete reference.
Use uri=warp://host[:port] (TLS by default), https:// or http://. The generic URI defaults
are 443 for TLS and 80 for HTTP. Without a URI, host/port default to localhost/443.
Explicit ODBC properties override URI values. Credentials belong in uid/pwd or token
properties, not a URI. Product schemes require the corresponding thin wrapper.

The Windows DSN editor accepts either a host or a URI in Host Name / URI. A URI owns
its port and TLS choice; change them in the URI. Extra native options, including client
certificate/key and query tags, remain editable in Advanced properties and survive
reopening a DSN. Certificate sign-in is an authentication choice in the editor.
