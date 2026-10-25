#!/usr/bin/env python3
"""Generate the tap formula from an already downloaded release's SHA256SUMS."""
import argparse
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument('--product', choices=['warp', 'hardcharger'], default='warp')
p.add_argument('version')
p.add_argument('checksums', type=Path)
a = p.parse_args()
hashes = {}
for line in a.checksums.read_text().splitlines():
    digest, name = line.split(maxsplit=1)
    hashes[name.lstrip('./* ')] = digest
repo = 'hardcharger-odbc-driver' if a.product == 'hardcharger' else 'warp-sql-odbc'
library = 'hardcharger-odbc' if a.product == 'hardcharger' else 'warpsql-odbc'
base = f'https://github.com/gizmodata/{repo}/releases/download/v{a.version}'

def asset(platform):
    name = f'{library}-{platform}.tar.gz'
    return f'      url "{base}/{name}"\n      sha256 "{hashes[name]}"'

formula = ('''# typed: false
# frozen_string_literal: true

class HardchargerOdbc < Formula
  desc "Hardcharger ODBC driver using native Warp SQL and Arrow streaming"
  homepage "https://github.com/gizmodata/hardcharger-odbc-driver"
  version "''' + a.version + '''"
  license "Apache-2.0"
  depends_on "unixodbc"

  on_macos do
    on_arm do
''' + asset('macos-arm64') + '''
    end
  end
  on_linux do
    on_intel do
''' + asset('linux-amd64') + '''
    end
    on_arm do
''' + asset('linux-arm64') + '''
    end
  end

  def install
    lib.install Dir["lib/*"]
    pkgshare.install Dir["share/hardcharger-odbc/*"]
  end

  def post_install
    ini = etc/"odbcinst.ini"
    name = "Hardcharger ODBC Driver"
    path = lib/(OS.mac? ? "libhardcharger-odbc.dylib" : "libhardcharger-odbc.so")
    text = ini.exist? ? ini.read : ""
    if text.match?(/^\\[#{Regexp.escape(name)}\\]$/)
      # Replace only this driver's section, preserving other drivers.
      text.gsub!(/^\\[#{Regexp.escape(name)}\\]\\n[^\\[]*/, "[#{name}]\\nDriver = #{path}\\n\\n")
    else
      text += "\\n[#{name}]\\nDriver = #{path}\\n"
    end
    ini.atomic_write(text)
  end

  def caveats
    <<~EOS
      Driver registered as Hardcharger ODBC Driver. Add a DSN to ~/.odbc.ini:

        [Hardcharger]
        Driver = Hardcharger ODBC Driver
        host = localhost
        port = 31337
        uid = your-username
        pwd = your-password
        useEncryption = true

      Linux packages require glibc 2.39 or later (Ubuntu 24.04 or equivalent).
    EOS
  end

  test do
    path = lib/(OS.mac? ? "libhardcharger-odbc.dylib" : "libhardcharger-odbc.so")
    assert_path_exists path
    assert_match "Hardcharger ODBC Driver", shell_output("#{Formula[\"unixodbc\"].opt_bin}/odbcinst -q -d")
  end
end
''')

if a.product == 'warp':
    formula = formula.replace('hardcharger-odbc-driver', 'warp-sql-odbc').replace('HardchargerOdbc', 'WarpsqlOdbc').replace('Hardcharger ODBC', 'Warp SQL ODBC').replace('hardcharger-odbc', 'warpsql-odbc').replace('[Hardcharger]', '[Warp SQL]')
print(formula)
