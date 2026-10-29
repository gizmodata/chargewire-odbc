# Releasing

1. Update `project(... VERSION ...)` in CMakeLists.txt, CHANGELOG.md and the expected
   driver version in the regression suite. DLL resource and MSI versions derive from CMake.
2. Run unit and live suites locally. Push main and require all four platform builds/tests green.
3. Tag that tested commit `vX.Y.Z` and push the tag. The workflow repeats every gate on
   the tagged commit. It publishes only after signing and verification succeed.

Windows signing uses the existing GizmoData EV certificate `ev-codesigning-cert` in
Azure Key Vault `kv-cs-gizmo-edge-prd`. The private key stays in the vault. The
`production` GitHub environment is restricted to `v*` tags; Azure trusts this repository's
immutable OIDC subject. Four environment secrets select the application/tenant/vault/certificate:
`AZURE_CLIENT_ID`, `AZURE_TENANT_ID`, `AZURE_KEY_VAULT_URI`, `AZURE_CERT_NAME`.
The signing and verification scripts match Hardcharger's release scripts.

The DLL is signed first; WiX embeds that DLL into the MSI; the MSI is then signed.
Verification proves that an unsigned DLL is refused, then requires Authenticode Valid,
an RFC 3161 timestamp, `signtool verify /pa`, and the same GizmoData signer for both.
The signed MSI is silently installed to check registration before publication. An
unsigned fallback is never published as a Windows release. The ZIP contains the signed
DLL and its Microsoft C++ runtime dependencies. Unix packages contain the tested library.
Every release includes SHA256SUMS and GitHub build-provenance attestations.

To refresh Homebrew after publishing, download SHA256SUMS and generate the tap formula
with `python scripts/homebrew-formula.py X.Y.Z /path/to/SHA256SUMS`. Commit it as
`Formula/chargewire-odbc.rb` in gizmodata/homebrew-tap after verifying the downloaded
package checksums. Manual CI runs can select Unix or Windows to reproduce a platform
failure; published releases always require every platform and signing gate.

Product wrappers must pin a published core release. The release workflow checks this before publication and includes signature-verification.txt with the Windows signature evidence.
