#!/usr/bin/env python3
"""Assemble licenses for the static runtime dependencies into every binary package."""
from pathlib import Path
import os

root = Path(__file__).resolve().parent.parent
items = [
    ('Hardcharger ODBC project license', root / 'LICENSE'),
    ('Hardcharger ODBC project attribution', root / 'NOTICE'),
    ('Warp SQL', root / 'third_party/warp-sql/LICENSE'),
    ('Apache Arrow', root / '.deps/arrow-apache-arrow-26.0.0/LICENSE.txt'),
    ('Apache Arrow NOTICE', root / '.deps/arrow-apache-arrow-26.0.0/NOTICE.txt'),
    ('spdlog (including bundled fmt)', root / 'build/_deps/spdlog-src/LICENSE'),
]
if os.name == 'nt':
    items.extend((p.parent.name, p) for p in sorted((root / 'vcpkg_installed/x64-windows-static-md/share').glob('*/copyright')))
else:
    items.append(('Boost', root / '.deps/boost-1.92.0/LICENSE_1_0.txt'))
    openssl = [Path('/opt/homebrew/opt/openssl@3/LICENSE.txt'), Path('/usr/share/doc/libssl3t64/copyright'), Path('/usr/share/doc/libssl3/copyright')]
    rapidjson = [Path('/opt/homebrew/opt/rapidjson/license.txt'), Path('/opt/homebrew/opt/rapidjson/LICENSE'), Path('/opt/homebrew/opt/rapidjson/share/doc/rapidjson/license.txt'), Path('/usr/share/doc/rapidjson-dev/copyright')]
    for name, candidates in [('OpenSSL', openssl), ('RapidJSON', rapidjson)]:
        match = next((p for p in candidates if p.is_file()), None)
        if match is None:
            raise SystemExit(f'No license found for {name}: {candidates}')
        items.append((name, match))
text = ['Hardcharger ODBC Driver — third-party licenses\n']
for name, path in items:
    if not path.is_file():
        raise SystemExit(f'Missing license: {path}')
    text.extend(['\n' + '=' * 72 + '\n' + name + '\n' + '=' * 72 + '\n', path.read_text(errors='replace')])
(root / 'THIRD_PARTY_NOTICES.txt').write_text('\n'.join(text), encoding='utf-8')
print(f'Collected {len(items)} dependency license files.')
