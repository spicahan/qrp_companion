#!/usr/bin/env bash
# Production sequencing engine, injected peripherals; no physical devices.
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
test_build_dir="$(mktemp -d /tmp/qrp-rf-engine-tests.XXXXXX)"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$repo_dir/pal" -I"$repo_dir/main/rf_board" \
  "$repo_dir/tests/rf_engine_test.cpp" "$repo_dir/main/rf_board/rf_engine.cpp" \
  -o "$test_build_dir/rf_engine_test"
"$test_build_dir/rf_engine_test"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -I"$repo_dir/main/rf_board/include" \
  "$repo_dir/tests/rf_lpf_test.c" "$repo_dir/main/rf_board/lpf_classify.c" \
  -o "$test_build_dir/rf_lpf_test"
"$test_build_dir/rf_lpf_test"
printf 'RF engine test binary retained at %s\n' "$test_build_dir/rf_engine_test"
