#!/bin/bash
# Build the desktop Qt5 target
# Usage: cd desktop && ./build.sh
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Temporarily restore CMakeLists.txt for cmake
cp CMakeLists.txt.desktop CMakeLists.txt
trap "rm -f CMakeLists.txt" EXIT

cmake -B build -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt@5 "$@"
cmake --build build
