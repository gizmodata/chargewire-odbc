# Browser and certificate sign-in

Use `authType=external` to start the server's Chargewire browser sign-in flow. The server
must configure an OpenID Connect provider and offer `oauth_browser`. The client opens
the returned authorize URL, then polls the flow using its private poll secret. It never
logs the poll secret or bearer token. `oauthBrowser` accepts a browser executable path
(one URL argument, without a shell); `none` prints the URL for manual opening.
`oauthTimeoutSeconds` defaults to 300. A browser access token that expires requires
interactive sign-in again.

For mutual TLS, supply `clientCertificate` and `clientKey` (PEM). For authentication
by the certificate itself, also set `authType=client_certificate`; the server must enable
certificate sign-in and trust the certificate issuer. `trustedCerts` sets the CA bundle
used to verify the server. Certificate and password grants renew through the native client.

`authType=token` takes an already issued bearer access token. `authType=token_exchange`
exchanges an identity-provider sign-in token when the server is configured to trust it.
