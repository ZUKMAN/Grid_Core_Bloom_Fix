#!/bin/sh
# Cross build with llvm-mingw (https://github.com/mstorsjo/llvm-mingw).
# Output: build/dinput8.dll + build/Grid_Core_Bloom_Fix.ini
set -e
CXX=${CXX:-i686-w64-mingw32-clang++}
mkdir -p build
"$CXX" -std=c++17 -O2 -shared -static -s -Wall -Wextra -Wno-cast-function-type -Wno-missing-field-initializers \
    src/Grid_Core_Bloom_Fix.cpp src/dinput8.def -o build/dinput8.dll -Wl,--kill-at -luser32
cp src/Grid_Core_Bloom_Fix.ini build/
echo "build/dinput8.dll"
