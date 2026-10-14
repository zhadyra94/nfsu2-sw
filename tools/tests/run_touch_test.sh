#!/usr/bin/env bash
# Build and run the iOS touch-layer behaviour test on Linux.
# Usage: tools/tests/run_touch_test.sh [build-dir]
# Needs gcc and SDL2 development files (pkg-config sdl2). No window, no GPU.
set -eu

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
src="$root/xboxrecomp/src"
out="${1:-${TOUCHTEST_DIR:-/tmp/nfsu2_touchtest}}"
mkdir -p "$out"

# Compile exactly like the iOS build sees the file: NFSU2_IOS defined, the
# input and platform headers on the path (xinput_xbox.h pulls platform/).
cflags=(-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter
        -DNFSU2_IOS=1 -DNOMINMAX -DWIN32_LEAN_AND_MEAN
        -I"$src/input" -I"$src" -I"$root/xboxrecomp/include"
        $(pkg-config --cflags sdl2))

gcc "${cflags[@]}" "$here/touch_test.c" "$src/input/xinput_ios.c" \
    -o "$out/touch_test" $(pkg-config --libs sdl2) -lm

# A clean environment: the layer reads these at first use.
unset RECOMP_TOUCH NFSU2_TOUCH_LAYOUT NFSU2_TOUCH_SCALE
export SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy

set +e
"$out/touch_test" 2>"$out/touch_test.stderr"
rc=$?
set -e
[ "$rc" -ne 0 ] && echo "(stderr of the run: $out/touch_test.stderr)"
exit "$rc"
