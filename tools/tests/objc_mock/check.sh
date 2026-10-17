#!/bin/bash
# Syntax/type check of an iOS .m file on Linux against hand-written mock
# headers (Mock.h + UIKit/QuartzCore/CoreGraphics/CoreMotion stubs). Catches
# typos, block syntax and type slips only -- NOT wrong Apple API names; CI's
# xcrun fail-fast step is the real check.  Usage: check.sh FILE.m [max_lines]
set -u
d=$(cd "$(dirname "$0")" && pwd)
src=$(readlink -f "$1")
tmp=$(mktemp -d)
sed -e 's/^#import </#include </' "$src" > "$tmp/x.m"
clang-14 -fsyntax-only -fobjc-runtime=gnustep-2.0 -fobjc-arc -fblocks -Wall -Wextra \
    -Wno-unused-parameter -I"$d" -I"$(dirname "$src")" -include "$d/Mock.h" "$tmp/x.m" 2>&1 | head -"${2:-40}"
rc=${PIPESTATUS[0]}
rm -rf "$tmp"
exit $rc
