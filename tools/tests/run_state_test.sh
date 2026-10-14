#!/usr/bin/env bash
# Build and run the race-state reader test (src/nfsu2_state.c) on Linux.
# Usage: tools/tests/run_state_test.sh [build-dir]
set -eu

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
out="${1:-${STATETEST_DIR:-/tmp/nfsu2_statetest}}"
mkdir -p "$out"

gcc -std=gnu11 -O1 -g -Wall -Wextra -I"$root/src" \
    "$here/state_test.c" "$root/src/nfsu2_state.c" -o "$out/state_test"
"$out/state_test"
