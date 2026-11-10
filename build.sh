#!/usr/bin/env bash
# Build native/offline_quiet.dll with LLVM clang and the Visual Studio libraries.
# SDK headers: templates/addon/native/include in https://github.com/Swofty-Developments/SwoftyLauncher
set -euo pipefail
cd "$(dirname "$0")"
SDK="${1:?path to the SDK include dir: SwoftyLauncher/templates/addon/native/include}"
clang++ --target=x86_64-pc-windows-msvc -std=c++23 -O2 -Wall -Wextra -fms-runtime-lib=static -I"$SDK" -shared \
    -o offline_quiet.dll src/offline_quiet.cpp
rm -f offline_quiet.lib offline_quiet.exp
echo built native/offline_quiet.dll
