#!/usr/bin/env bash
# Build tod_scale.asi (32-bit Windows DLL).
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-i686-w64-mingw32-gcc}
OUT=tod_scale.asi

"$CC" -O2 -shared -static -static-libgcc \
      -o "$OUT" tod_scale.c \
      -lkernel32 -luser32

echo "built: $(pwd)/$OUT"
file "$OUT"
