#!/usr/bin/env bash
# Compiles the actual adapted Si5351 driver against fake time and transport.
# Does not open a serial port, I2C device, or any RF hardware.
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
project_dir="$(cd "$test_dir/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/qrp-synth-tests.XXXXXX")"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I"$test_dir/si5351_stubs" -I"$project_dir/main/rf_board/include" \
    "$test_dir/si5351_radio_test.c" "$project_dir/main/rf_board/si5351.c" \
    -lm -o "$test_build_dir/si5351_radio_test"
"$test_build_dir/si5351_radio_test"
printf 'Synthesizer test binary retained at %s\n' "$test_build_dir/si5351_radio_test"
