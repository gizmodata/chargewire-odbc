@echo off
rem Run from an MSVC x64 developer shell after vcpkg/Arrow setup; see docs/BUILDING.md.
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release %*
if errorlevel 1 exit /b %errorlevel%
cmake --build build --parallel 3
if errorlevel 1 exit /b %errorlevel%
ctest --test-dir build --output-on-failure
