#!/bin/bash
# Syntax-check C files the way a real iOS build sees them: define the Apple
# macros (NOT -DNFSU2_IOS=1), so each file derives NFSU2_IOS itself. Catches
# include-order bugs that command-line -DNFSU2_IOS checks hide (v24: main.c
# included <sys/stat.h> under #ifdef NFSU2_IOS before defining it). Apple-only
# system headers come from apple_stub/. Usage: apple_path_check.sh [files...]
set -u
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd)
src="$root/xboxrecomp/src"
gen=${NFSU2_GEN_DIR:-$root/../gen}
inc=(-I"$root/src" -I"$gen" -I"$root/xboxrecomp/include" -I"$src/kernel" -I"$src/platform"
     -I"$src/d3d" -I"$src/audio" -I"$src/apu" -I"$src/input" -I"$src/usb" -I"$src/nv2a"
     -I"$src/video" -I"$src/nv2a_gl" -I"$src" -I"$here/apple_stub"
     -include "$here/apple_stub/apple_extras.h")
files=("$@")
[ ${#files[@]} -eq 0 ] && files=("$root/src/main.c" "$root/src/nfsu2_state.c"
    "$src/kernel/xbox_xiso.c" "$src/kernel/kernel_file.c" "$src/kernel/kernel_path.c"
    "$src/input/xinput_ios.c" "$src/input/xinput_device.c")
rc=0
for f in "${files[@]}"; do
    out=$(gcc -std=gnu11 -fsyntax-only -Wall -Wno-comment -Werror=implicit-function-declaration \
          -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D__APPLE__ \
          -D__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__=160000 \
          "${inc[@]}" $(pkg-config --cflags sdl2 2>/dev/null) "$f" 2>&1 | grep -E "error" )
    if [ -n "$out" ]; then echo "FAIL $f"; echo "$out" | head -5; rc=1; else echo "ok   $f"; fi
done
exit $rc
