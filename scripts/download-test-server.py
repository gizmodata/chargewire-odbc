#!/usr/bin/env python3
"""Fetch the released test server, or its successful main candidate before publication."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import zipfile

REPO = 'gizmodata/hardcharger'


def gh(*args):
    return subprocess.check_output(['gh', *args], text=True)


def candidate_run(runs, commit):
    for run in runs:
        if (run['head_sha'] == commit and run['head_branch'] == 'main'
                and run['event'] == 'push' and run['conclusion'] == 'success'
                and run['path'] == '.github/workflows/server.yml'):
            return run['id']
    raise RuntimeError('No successful server workflow on current Hardcharger main')


def verify_checksum(archive, checksums):
    expected = [line.split()[0] for line in checksums.splitlines()
                if len(line.split()) == 2 and line.split()[1].lstrip('*') == archive.name]
    with archive.open('rb') as stream:
        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
    if expected != [actual]:
        raise RuntimeError(f'Checksum mismatch or missing checksum: {archive.name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', required=True,
                        choices=['linux-amd64', 'linux-arm64', 'macos-arm64', 'windows-amd64'])
    parser.add_argument('--version', default='2.13.0')
    parser.add_argument('--dest', type=Path, default=Path('server'))
    args = parser.parse_args()
    args.dest.mkdir(parents=True, exist_ok=True)
    suffix = 'zip' if args.platform.startswith('windows-') else 'tar.gz'
    name = f'hardcharger-{args.platform}.{suffix}'
    release = subprocess.run(['gh', 'api', f'repos/{REPO}/releases/tags/v{args.version}'],
                             text=True, capture_output=True)
    if release.returncode == 0:
        info = json.loads(release.stdout)
        if info['draft'] or info['prerelease']:
            raise RuntimeError('Expected a published stable test-server release')
        gh('release', 'download', f'v{args.version}', '--repo', REPO, '--dir', str(args.dest),
           '--pattern', name, '--pattern', 'SHA256SUMS', '--pattern', 'versions.json')
        commit = None
        print(f'Test server: published Hardcharger v{args.version}', flush=True)
    else:
        if 'HTTP 404' not in release.stderr:
            raise RuntimeError(release.stderr)
        commit = json.loads(gh('api', f'repos/{REPO}/branches/main'))['commit']['sha']
        runs = json.loads(gh('api', f'repos/{REPO}/actions/workflows/server.yml/runs'
                            f'?head_sha={commit}&status=success&per_page=100'))['workflow_runs']
        run = candidate_run(runs, commit)
        for artifact in (f'hardcharger-{args.platform}', 'hardcharger-versions', 'hardcharger-SHA256SUMS'):
            gh('run', 'download', str(run), '--repo', REPO, '--name', artifact, '--dir', str(args.dest))
        print(f'Test server: unpublished main candidate {commit}, successful run {run}', flush=True)
    versions = json.loads((args.dest / 'versions.json').read_text())
    if versions['hardcharger']['version'] != args.version:
        raise RuntimeError('Test server version does not match the requested version')
    if commit is not None and versions['hardcharger']['commit'] != commit:
        raise RuntimeError('Candidate provenance does not match the successful main run')
    archive = args.dest / name
    verify_checksum(archive, (args.dest / 'SHA256SUMS').read_text())
    unpacked = args.dest / 'unpacked'
    if suffix == 'zip':
        with zipfile.ZipFile(archive) as bundle:
            bundle.extractall(unpacked)
    else:
        with tarfile.open(archive) as bundle:
            bundle.extractall(unpacked, filter='data')
    executable = 'hardcharger-server.exe' if suffix == 'zip' else 'hardcharger-server'
    binaries = list(unpacked.rglob(executable))
    if len(binaries) != 1:
        raise RuntimeError('Expected exactly one server executable')
    binary = binaries[0].resolve()
    said = subprocess.check_output([str(binary), '--version'], text=True).strip()
    if said != f'Hardcharger {args.version}':
        raise RuntimeError(f'Unexpected server version: {said}')
    print(f'Verified {said}: {binary}', flush=True)
    if os.environ.get('GITHUB_ENV'):
        with open(os.environ['GITHUB_ENV'], 'a', encoding='utf-8') as output:
            output.write(f'SERVER={binary}\n')


if __name__ == '__main__':
    main()
