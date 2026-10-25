#!/usr/bin/env python3
"""Build only Boost JSON, URL and Locale, statically."""
import argparse
import pathlib
import subprocess
import tarfile
import urllib.request

p = argparse.ArgumentParser()
p.add_argument('--prefix', required=True)
a = p.parse_args()
root = pathlib.Path('.deps').resolve()
root.mkdir(exist_ok=True)
source = root / 'boost-1.92.0'
if not source.exists():
    archive = root / 'boost.tar.xz'
    urllib.request.urlretrieve('https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz', archive)
    with tarfile.open(archive) as tar:
        tar.extractall(root, filter='data')
subprocess.run(['cmake', '-S', str(source), '-B', str(root / 'boost-build'), '-G', 'Ninja',
                '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(pathlib.Path(a.prefix).resolve()),
                '-DCMAKE_POSITION_INDEPENDENT_CODE=ON', '-DBUILD_SHARED_LIBS=OFF',
                '-DBOOST_INCLUDE_LIBRARIES=json;url;locale', '-DBOOST_LOCALE_ENABLE_ICU=OFF'], check=True)
subprocess.run(['cmake', '--build', str(root / 'boost-build'), '--parallel', '3'], check=True)
subprocess.run(['cmake', '--install', str(root / 'boost-build')], check=True)
