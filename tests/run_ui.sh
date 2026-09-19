#!/usr/bin/env bash
# No hardware, display server, audio device, or serial port is used.
# Optional argument: directory to receive actual-app framebuffer PPM snapshots.
set -euo pipefail
test_dir="$(cd "$(dirname "$0")" && pwd)"
project_dir="$(cd "$test_dir/.." && pwd)"
test_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/qrp-ui-tests.XXXXXX")"
snapshot_dir="${1:-}"

"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror \
    -Wno-unused-parameter -Wno-missing-field-initializers \
    -fsanitize=address,undefined \
    -I"$project_dir/pal" -I"$project_dir/core" \
    "$test_dir/ui_radio_test.cpp" \
    "$project_dir/core/ui_state.cpp" "$project_dir/core/ui_widget.cpp" \
    -o "$test_build_dir/ui_radio_test"

"$test_build_dir/ui_radio_test" --snapshots "$snapshot_dir"
"$test_build_dir/ui_radio_test" --rotated --snapshots "$snapshot_dir"
"$test_build_dir/ui_radio_test" --qmx --snapshots "$snapshot_dir"
"$test_build_dir/ui_radio_test" --qmx --rotated --snapshots "$snapshot_dir"
printf 'UI test binary retained at %s\n' "$test_build_dir/ui_radio_test"
