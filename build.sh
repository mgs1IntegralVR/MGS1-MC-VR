#!/bin/sh
# Builds dinput8.dll (x64) with MinGW-w64. Usage: ./build.sh
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-g++ -O2 -s -std=c++17 -shared -static -static-libgcc -static-libstdc++ \
  -I third_party/openxr/include src/mgs1mc_vr.cpp src/dinput8.def \
  -ld3d11 -ldxgi -lpdh -luser32 -lgdi32 -o dinput8.dll
echo "built dinput8.dll"
