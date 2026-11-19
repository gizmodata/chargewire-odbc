**Chargewire ODBC Driver 2.5.1**, built on published Chargewire C++ SDK 2.5.0.

- `SQLTables` treats its catalog argument as a search pattern under ODBC 3, as the ODBC specification requires. `SQLTables("%", NULL, NULL, "TABLE,VIEW")`, the call Power Query's Navigator makes in Power BI and Excel, returned no tables because the server matches catalog names exactly. It now lists every catalog's tables ([hardcharger-odbc-driver#6](https://github.com/gizmodata/hardcharger-odbc-driver/issues/6)).
- A catalog of only `%` lists every catalog. A pattern without wildcards is unescaped (`my\_lake` finds `my_lake`). Any other pattern (`la%`, `my_lake`) keeps the catalogs it matches. `_` matches one character, and matching is case-sensitive. Under ODBC 2 the catalog remains a plain name.

Release gates run unit and HTTP/TLS live suites on all four platforms, including read-only coverage. Windows DLLs and MSIs are EV-signed, signature-verified and installed-driver tested before publication.
