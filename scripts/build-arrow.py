#!/usr/bin/env python3
"""Build pinned, static Arrow IPC + compute. No Flight, gRPC or protobuf."""
import argparse
import pathlib
import subprocess
import tarfile
import urllib.request

p = argparse.ArgumentParser()
p.add_argument('--prefix', required=True)
p.add_argument('--toolchain')
p.add_argument('--triplet', default='x64-windows-static-md')
a = p.parse_args()
root = pathlib.Path('.deps').resolve()
root.mkdir(exist_ok=True)
source = root / 'arrow-apache-arrow-26.0.0'
if not source.exists():
    archive = root / 'arrow.tar.gz'
    urllib.request.urlretrieve('https://github.com/apache/arrow/archive/refs/tags/apache-arrow-26.0.0.tar.gz', archive)
    with tarfile.open(archive) as tar:
        tar.extractall(root, filter='data')
cmd = ['cmake', '-S', str(source / 'cpp'), '-B', str(root / 'arrow-build'), '-G', 'Ninja',
       '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(pathlib.Path(a.prefix).resolve()),
       '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5',
       '-DARROW_BUILD_STATIC=ON', '-DARROW_BUILD_SHARED=OFF', '-DARROW_DEPENDENCY_SOURCE=BUNDLED',
       '-DARROW_IPC=ON', '-DARROW_COMPUTE=ON', '-DARROW_WITH_UTF8PROC=OFF', '-DARROW_WITH_RE2=OFF',
       '-DARROW_WITH_ZSTD=ON', '-DARROW_WITH_LZ4=ON', '-DARROW_FLIGHT=OFF', '-DARROW_PARQUET=OFF',
       '-DARROW_CSV=OFF', '-DARROW_JSON=OFF', '-DARROW_FILESYSTEM=OFF', '-DARROW_DATASET=OFF',
       '-DARROW_ACERO=OFF', '-DARROW_JEMALLOC=OFF', '-DARROW_MIMALLOC=OFF', '-DARROW_BUILD_TESTS=OFF',
       '-DARROW_SIMD_LEVEL=NONE', '-DARROW_RUNTIME_SIMD_LEVEL=NONE', '-DCMAKE_INSTALL_LIBDIR=lib']
if a.toolchain:
    cmd += ['-DCMAKE_TOOLCHAIN_FILE=' + a.toolchain, '-DVCPKG_TARGET_TRIPLET=' + a.triplet, '-DVCPKG_MANIFEST_MODE=OFF']
subprocess.run(cmd, check=True)
subprocess.run(['cmake', '--build', str(root / 'arrow-build'), '--parallel', '3'], check=True)
subprocess.run(['cmake', '--install', str(root / 'arrow-build')], check=True)
