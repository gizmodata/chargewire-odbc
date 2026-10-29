#!/usr/bin/env python3
"""Fail-closed local/CI live suites, with an isolated server (Hardcharger or chargewire-duckdb) and TLS."""
import argparse
import os
import pathlib
import re
import socket
import shutil
import subprocess
import sys
import tempfile
import time

p = argparse.ArgumentParser()
p.add_argument('--suite-root', default=str(pathlib.Path(__file__).resolve().parent.parent))
p.add_argument('--driver-name', default='Chargewire ODBC Driver')
p.add_argument('--driver-version', help='Product version; defaults to the caller CMake project version')
p.add_argument('--extra-suite', default='')
p.add_argument('--server', required=True)
p.add_argument('--server-kind', choices=['hardcharger', 'chargewire-duckdb'], default='hardcharger',
               help='Command-line dialect of --server; chargewire-duckdb is the SDK reference server')
p.add_argument('--driver', required=True)
p.add_argument('--build', default='build')
p.add_argument('--port', type=int, default=31337)
p.add_argument('--python-only', action='store_true', help='Re-test a packaged driver; C suites ran at build time')
a = p.parse_args()
suite_root = pathlib.Path(a.suite_root).resolve()
server = str(pathlib.Path(a.server).resolve())
driver = str(pathlib.Path(a.driver).resolve())
build = pathlib.Path(a.build).resolve()
env = os.environ.copy()
env['HARDCHARGER_PORT'] = str(a.port)
env['ODBC_EXPECTED_DRIVER_NAME'] = a.driver_name
env['ODBC_EXPECTED_DBMS_NAME'] = 'Hardcharger' if a.server_kind == 'hardcharger' else 'chargewire-duckdb'
version = a.driver_version
if not version:
    project = pathlib.Path('CMakeLists.txt')
    if not project.is_file():
        project = suite_root / 'CMakeLists.txt'
    match = re.search(r'project\([^ ]+ VERSION (\d+\.\d+\.\d+)', project.read_text())
    if not match:
        raise RuntimeError(f'Cannot determine the product version from {project}')
    version = match.group(1)
major, minor, patch = map(int, version.split('.'))
env['ODBC_EXPECTED_DRIVER_VERSION'] = f'{major:02d}.{minor:02d}.{patch:04d}'
env['ODBC_SUITE_ROOT'] = str(suite_root)
env.update(HARDCHARGER_DRIVER="{" + a.driver_name + "}" if os.name == "nt" else driver, HARDCHARGER_HOST='127.0.0.1', HARDCHARGER_CATALOG='test',
           HARDCHARGER_ADMIN_PASSWORD='hardcharger_password', CHARGEWIRE_DUCKDB_PASSWORD='hardcharger_password',
           PYTHONUNBUFFERED='1')
openssl = shutil.which('openssl')
if not openssl and os.name == 'nt':
    program_files = pathlib.Path(os.environ.get('ProgramFiles', 'C:/Program Files'))
    openssl = next((str(p) for p in [program_files / 'Git/usr/bin/openssl.exe',
                                    program_files / 'OpenSSL/bin/openssl.exe',
                                    program_files / 'OpenSSL-Win64/bin/openssl.exe'] if p.is_file()), None)
if not openssl:
    raise RuntimeError('OpenSSL is required to generate the TLS test certificates')
with tempfile.TemporaryDirectory(prefix='hardcharger-odbc-') as tmp:
    tmp = pathlib.Path(tmp)
    cert, key = tmp / 'server.crt', tmp / 'server.key'
    subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                    '-keyout', str(key), '-out', str(cert), '-subj', '/CN=localhost',
                    '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    client_cert, client_key = tmp / 'client.crt', tmp / 'client.key'
    subprocess.run([openssl, 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                    '-keyout', str(client_key), '-out', str(client_cert), '-subj', '/CN=hardcharger_user'],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for tls in (False, True):
        # All fixtures share the selected test port; refuse an unrelated server.
        with socket.socket() as probe:
            probe.settimeout(.2)
            if probe.connect_ex(('127.0.0.1', a.port)) == 0:
                raise RuntimeError(f'port {a.port} already has a listener')
        database = tmp / ('tls' if tls else 'plain') / 'test.duckdb'
        database.parent.mkdir()
        command = [server, '--host', '127.0.0.1', '--port', str(a.port), '--database', str(database)]
        if a.server_kind == 'hardcharger':
            command += ['--admin-username', 'hardcharger_user', '--init-sql-commands', 'INSTALL tpch; LOAD tpch;',
                        '--result-batch-rows', '2048', '--enable-instrumentation', '--no-banner']
        else:
            # The reference server has no init SQL; DuckDB autoloads tpch at the fixtures' CALL dbgen.
            command += ['--username', 'hardcharger_user']
        if tls:
            client_ca = '--mtls-ca-cert' if a.server_kind == 'hardcharger' else '--tls-client-ca'
            optional = '--no-mtls-require-client-cert' if a.server_kind == 'hardcharger' else '--mtls-require-client-cert=false'
            command += ['--tls-cert', str(cert), '--tls-key', str(key), client_ca, str(client_cert),
                        '--mtls-certificate-sign-in', optional, '--mtls-identity-source', 'subject_cn', '--mtls-admin-identities', 'hardcharger_user']
        env['HARDCHARGER_CLIENT_CERT'] = str(client_cert)
        env['HARDCHARGER_CLIENT_KEY'] = str(client_key)
        env['HARDCHARGER_USE_ENCRYPTION'] = 'true' if tls else 'false'
        env['HARDCHARGER_TRUSTED_CERTS'] = str(cert) if tls else ''
        # Exercise the ODBC manager's DSN lookup as well as connection strings.
        dsn = f'ChargewireOdbc-{tmp.name[-10:]}-{int(tls)}'  # ODBC limits DSN names to 32 characters.
        env['HARDCHARGER_TEST_DSN'] = dsn
        dsn_properties = {'Driver': driver, 'uri': f'chargewire://127.0.0.1:{a.port}' + ('' if tls else '?tls=false'),
                          'uid': 'hardcharger_user', 'pwd': 'hardcharger_password'}
        if tls:
            dsn_properties['trustedCerts'] = str(cert)
        if os.name == 'nt':
            import winreg
            dsn_key = 'Software\\ODBC\\ODBC.INI\\' + dsn
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, dsn_key) as registry:
                for name, value in dsn_properties.items():
                    winreg.SetValueEx(registry, name, 0, winreg.REG_SZ, value)
        else:
            env['ODBCINI'] = str(tmp / 'odbc.ini')
            pathlib.Path(env['ODBCINI']).write_text('[' + dsn + ']\n' + '\n'.join(f'{key}={value}' for key, value in dsn_properties.items()) + '\n')
        log = pathlib.Path(f'live-{ "tls" if tls else "plain" }.log')
        with log.open('w') as out:
            proc = subprocess.Popen(command, env=env, stdout=out, stderr=subprocess.STDOUT)
            try:
                for _ in range(100):
                    if proc.poll() is not None:
                        raise RuntimeError(f'server exited: {log.read_text()}')
                    try:
                        with socket.create_connection(('127.0.0.1', a.port), timeout=.2):
                            break
                    except OSError:
                        time.sleep(.2)
                else:
                    raise RuntimeError(f'server not ready: {log.read_text()}')
                if not tls and not a.python_only:
                    for suite in ('metadata', 'transactions'):
                        matches = list(build.rglob('test_' + suite + ('.exe' if os.name == 'nt' else '')))
                        if len(matches) != 1:
                            raise RuntimeError(f'expected one C {suite} executable: {matches}')
                        subprocess.run([str(matches[0])], env=env, check=True, timeout=120)
                suites = [suite_root / 'integration_tests' / name for name in ('test_pyodbc.py', 'test_chargewire.py', 'test_pre_rename_compat.py')]
                if a.extra_suite:
                    suites.append(pathlib.Path(a.extra_suite).resolve())
                for suite in suites:
                    subprocess.run([sys.executable, str(suite)], env=env, check=True, timeout=120)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
                if os.name == 'nt':
                    winreg.DeleteKey(winreg.HKEY_CURRENT_USER, dsn_key)
        print(f'{"TLS" if tls else "Plain HTTP"}: all live suites passed', flush=True)
