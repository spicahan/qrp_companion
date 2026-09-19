#!/usr/bin/env bash
# Builds only pure native tests. Never opens a GUI, audio device, or serial port.
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
test_build_dir="$(mktemp -d /tmp/qrp-companion-tests.XXXXXX)"
cxx="${CXX:-c++}"
flags=(-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g)

"$cxx" "${flags[@]}" -I"$repo_dir/pal" -I"$repo_dir/core" \
  "$repo_dir/tests/radio_qmx_test.cpp" "$repo_dir/pal/radio_qmx.cpp" \
  "$repo_dir/core/cat.cpp" -o "$test_build_dir/radio_qmx_test"
"$test_build_dir/radio_qmx_test"

"$cxx" "${flags[@]}" -I"$repo_dir/core" -I"$repo_dir/desktop/fft_shim" \
  "$repo_dir/tests/dsp_input_offset_test.cpp" "$repo_dir/core/dsp.cpp" \
  "$repo_dir/core/nco.cpp" "$repo_dir/desktop/fft_shim/fft.cpp" \
  -o "$test_build_dir/dsp_input_offset_test"
"$test_build_dir/dsp_input_offset_test" 0
"$test_build_dir/dsp_input_offset_test" 12000

printf 'Native PAL/DSP regressions passed. Test binaries: %s\n' "$test_build_dir"
