#!/bin/bash
# Builds PsiTerm for the Psion Series 5mx and packages dist/PsiTerm.sis.
#   PSION_SDK=/path/to/psion_cpp_sdk_linux ./build.sh
# Needs: the EPOC R5 C++ SDK for Linux with the gcc 3.0 Psion toolchain, and wine
# (for makmake/rcomp/makesis), and psion-core as the core/ submodule (the
# shared connection layer, TLS, zlib, the tools). See README.md; or run it in
# the build container: core/tools/docker/psibuild "bash build.sh".
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
CORE=${PSION_CORE:-$HERE/core}
[ -f "$CORE/ssh/psiglue.cpp" ] || { echo "core/ is empty: git submodule update --init"; exit 1; }
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
export PATH=/usr/bin:/bin:$PSION_SDK/gcc-3.0-psion-98r2-9/bin:$EPOCROOT/epoc32/tools
export TMP=$EPOCROOT/tmp
export WINEDEBUG=-all
mkdir -p "$TMP" "$HERE/build"
REL=$EPOCROOT/epoc32/release/marm/rel

echo "== libvterm";  "$HERE/build_vterm.sh"
echo "== psissh";    make -C "$HERE/ssh" SDK="$PSION_SDK" CORE="$(realpath --relative-to="$HERE/ssh" "$CORE")"
echo "== psihold";   make -f "$CORE/ssh/psihold.mk" PSION_SDK="$PSION_SDK" TOP="$HERE" > "$HERE/build/psihold.log" 2>&1 \
  || { grep -E "error|undefined" "$HERE/build/psihold.log" | head -20; exit 1; }
echo "== PsiTerm"
# makmake works on projects inside the SDK tree: link the app folder there
if [ -e "$EPOCROOT/ptproj/psiterm" ] && [ ! -L "$EPOCROOT/ptproj/psiterm" ]; then
  echo "$EPOCROOT/ptproj/psiterm exists and is not a link to this checkout - move it away"; exit 1
fi
mkdir -p "$EPOCROOT/ptproj"
ln -sfn "$HERE/app" "$EPOCROOT/ptproj/psiterm"
ln -sfn "$CORE/ssh" "$EPOCROOT/ptproj/psitermssh"   # the shared sources (pglinktest.cpp, psigreyui.cpp) and headers
( cd "$EPOCROOT/ptproj/psiterm" && makmake psiterm marm > "$HERE/build/makmake.log" 2>&1 \
  && make -f psiterm.marm rel > "$HERE/build/psiterm.log" 2>&1 ) \
  || { grep -i -B1 -A4 "error\|undefined" "$HERE/build/psiterm.log" | head -40; exit 1; }

echo "== pictures (PsiTerm.mbm)"
# the toolbar's pictures: core/tools/mkicons.py draws them, bmconv packs them
python3 "$CORE/tools/mkicons.py" "$HERE/build/icons" "$HERE/app/pticons.h"
( cd "$HERE/build/icons" && wine "$EPOCROOT/epoc32/tools/bmconv.exe" /q psiterm.mbm $(tr '\n' ' ' < files.txt) > /dev/null 2>&1 )

echo "== package"
cp "$REL/psiterm.app" "$REL/psiterm.rsc" "$REL/psissh.exe" "$HERE/build/icons/psiterm.mbm" "$HERE/build/psihold/psihold.exe" "$HERE/pkg/"
# the Terminus font and its licence (core/pkg; git-ignored here)
cp "$CORE/pkg/psiterm.gdr" "$CORE/pkg/terminus-licence.txt" "$HERE/pkg/"
# the EPOC C library (ESTLIB.DLL) is not in the 5mx ROM: embed the SDK's
# redistributable stdlib.sis so a fresh Psion gets it too
install -m 644 "$REL/stdlib.sis" "$HERE/pkg/STDLIB.SIS"   # (the SDK copy is read-only)
( cd "$HERE/pkg" && wine "$EPOCROOT/epoc32/tools/makesis.exe" psiterm.pkg "$HERE/dist/PsiTerm.sis" > /dev/null )
ls -la "$HERE/dist/PsiTerm.sis"
