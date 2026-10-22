@echo off
rem Builds dinput8.dll (x64) from an MSYS2 MinGW64 shell (g++ on PATH).
cd /d "%~dp0"
g++ -O2 -s -std=c++17 -shared -static -static-libgcc -static-libstdc++ -I third_party/openxr/include src/mgs1mc_vr.cpp src/dinput8.def -ld3d11 -ldxgi -lpdh -luser32 -lgdi32 -o dinput8.dll
if errorlevel 1 exit /b 1
echo built dinput8.dll
