#!/usr/bin/env bash
# Production dispatcher + host task/queue stubs; never opens real audio/hardware.
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
test_build_dir="$(mktemp -d /tmp/qrp-rf-audio-tests.XXXXXX)"
cxx="${CXX:-c++}"
"$cxx" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g -pthread \
  -I"$repo_dir/tests/rf_audio_stubs" -I"$repo_dir/pal" -I"$repo_dir/main/rf_board" \
  "$repo_dir/tests/rf_audio_test.cpp" "$repo_dir/main/rf_board/rf_audio.cpp" \
  -o "$test_build_dir/rf_audio_test"
"$test_build_dir/rf_audio_test"
printf 'RF audio test binary retained at %s/rf_audio_test\n' "$test_build_dir"
