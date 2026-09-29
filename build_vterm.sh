#!/bin/sh
# Builds libvterm for EPOC R5 as psivterm.lib (a static library PsiTerm links).
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
E=$PSION_SDK/epoc_cpp_sdk/epoc32
export PATH=$PATH:$PSION_SDK/gcc-3.0-psion-98r2-9/bin
F="-std=gnu99 -s -fomit-frame-pointer -O2 -c -nostdinc -mcpu=arm710 -mapcs-32 -mshort-load-bytes -msoft-float -fno-builtin -D__SYMBIAN32__ -D__PSISOFT32__ -D__GCC32__ -D__EPOC32__ -D__MARM__ -D__DLL__ -I $HERE/app/compat -I $HERE/libvterm/include -I $E/include/libc -I $E/include"
OUT=$HERE/build/vterm; rm -rf "$OUT"; mkdir -p "$OUT"
for f in "$HERE"/libvterm/src/*.c "$HERE"/app/compat/snprintf_shim.c; do
  arm-epoc-pe-gcc $F "$f" -o "$OUT/$(basename "$f" .c).o" 2>&1 | grep -v "warning\|^In file\|^ *from\|In function\|At top level" || true
done
arm-epoc-pe-ar rcs "$OUT/psivterm.lib" "$OUT"/*.o
cp "$OUT/psivterm.lib" "$E/release/marm/rel/psivterm.lib"
