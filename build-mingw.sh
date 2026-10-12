#!/bin/sh
# Cross-compiles WaveSiphon.exe from Linux with MinGW-w64
# (Ubuntu/Debian: apt install g++-mingw-w64-x86-64-posix).
set -eu
cd "$(dirname "$0")"
CXX=${CXX:-x86_64-w64-mingw32-g++}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
mkdir -p build-mingw
"$WINDRES" -I src src/WaveSiphon.rc -O coff -o build-mingw/WaveSiphon.res
"$CXX" -std=c++17 -O2 -Wall -Wextra -municode -mwindows \
    -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 \
    src/main.cpp src/recorder.cpp src/wav_writer.cpp src/audio_apps.cpp build-mingw/WaveSiphon.res \
    -o build-mingw/WaveSiphon.exe \
    -static -s -lole32 -luuid -lcomctl32 -lshell32 -lversion -lavrt -luser32 -lgdi32
echo "built build-mingw/WaveSiphon.exe"
