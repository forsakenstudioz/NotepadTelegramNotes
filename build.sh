#!/bin/sh
# Builds TelegramNotes.dll (64-bit) with MinGW-w64.
#   Ubuntu/Debian:  sudo apt-get install g++-mingw-w64-x86-64
set -e
cd "$(dirname "$0")"
mkdir -p dist/TelegramNotes
x86_64-w64-mingw32-g++ -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -shared \
  -static -static-libgcc -static-libstdc++ -s \
  -o dist/TelegramNotes/TelegramNotes.dll src/TelegramNotes.cpp \
  -lwinhttp -lshell32 -lole32 -luser32 -luuid
cd dist && rm -f TelegramNotes-x64.zip && zip -qr TelegramNotes-x64.zip TelegramNotes
echo "Built dist/TelegramNotes/TelegramNotes.dll and dist/TelegramNotes-x64.zip"
