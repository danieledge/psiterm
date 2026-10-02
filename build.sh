#!/bin/bash
# Builds PsiTerm for the Psion Series 5mx and packages dist/PsiTerm.sis.
#   PSION_SDK=/path/to/psion_cpp_sdk_linux ./build.sh
# Needs: the EPOC R5 C++ SDK for Linux with the gcc 3.0 Psion toolchain, and wine
# (for makmake/rcomp/makesis). See README.md.
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
export PATH=/usr/bin:/bin:$PSION_SDK/gcc-3.0-psion-98r2-9/bin:$EPOCROOT/epoc32/tools
export TMP=$EPOCROOT/tmp
export WINEDEBUG=-all
mkdir -p "$TMP" "$HERE/build"
REL=$EPOCROOT/epoc32/release/marm/rel

echo "== libvterm";  "$HERE/build_vterm.sh"
echo "== psissh";    make -C "$HERE/ssh" SDK="$PSION_SDK"
echo "== PsiTerm"
# makmake works on projects inside the SDK tree: link the app folder there
if [ -e "$EPOCROOT/ptproj/psiterm" ] && [ ! -L "$EPOCROOT/ptproj/psiterm" ]; then
  echo "$EPOCROOT/ptproj/psiterm exists and is not a link to this checkout - move it away"; exit 1
fi
mkdir -p "$EPOCROOT/ptproj"
ln -sfn "$HERE/app" "$EPOCROOT/ptproj/psiterm"
ln -sfn "$HERE/ssh" "$EPOCROOT/ptproj/psitermssh"   # Connection settings > Test (pglinktest.cpp)
( cd "$EPOCROOT/ptproj/psiterm" && makmake psiterm marm > "$HERE/build/makmake.log" 2>&1 \
  && make -f psiterm.marm rel > "$HERE/build/psiterm.log" 2>&1 ) \
  || { grep -i -B1 -A4 "error\|undefined" "$HERE/build/psiterm.log" | head -40; exit 1; }

echo "== pictures (PsiTerm.mbm)"
# the toolbar's pictures: tools/mkicons.py draws them, bmconv packs them
python3 "$HERE/tools/mkicons.py" "$HERE/build/icons" "$HERE/app/pticons.h"
( cd "$HERE/build/icons" && wine "$EPOCROOT/epoc32/tools/bmconv.exe" /q psiterm.mbm $(tr '\n' ' ' < files.txt) > /dev/null 2>&1 )

echo "== package"
cp "$REL/psiterm.app" "$REL/psiterm.rsc" "$REL/psissh.exe" "$HERE/build/icons/psiterm.mbm" "$HERE/pkg/"
# the EPOC C library (ESTLIB.DLL) is not in the 5mx ROM: embed the SDK's
# redistributable stdlib.sis so a fresh Psion gets it too
install -m 644 "$REL/stdlib.sis" "$HERE/pkg/STDLIB.SIS"   # (the SDK copy is read-only)
( cd "$HERE/pkg" && wine "$EPOCROOT/epoc32/tools/makesis.exe" psiterm.pkg "$HERE/dist/PsiTerm.sis" > /dev/null )
ls -la "$HERE/dist/PsiTerm.sis"
