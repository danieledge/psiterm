#!/bin/bash
# Builds the experimental kernel driver PSIKERN.LDD and its test app
# PSIKT.APP, and PsiKernTest.sis for a 5mx, into build/kernel-pkg/. Never
# put these in dist/ or a release.
#   tools/docker/psibuild "experimental/kernel/build.sh"
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
REL=$EPOCROOT/epoc32/release/marm/rel
export PATH=/usr/bin:/bin:$PSION_SDK/gcc-3.0-psion-98r2-9/bin:$EPOCROOT/epoc32/tools
export TMP=$EPOCROOT/tmp WINEDEBUG=-all
mkdir -p "$TMP" "$EPOCROOT/ptproj" "$TOP/build"

echo "== psiekern.lib (EKERN imports with the 5mx ROM's ordinals)"
# (in a scratch folder with TMP=.: with the SDK's TMP, dlltool names the
# archive members "tmp\\d11..." and the library comes out unusable)
D=$TOP/build/kernel-implib; rm -rf "$D"; mkdir -p "$D"
( cd "$D" && TMP=. arm-pe-dlltool --as=arm-pe-as --output-lib "$REL/psiekern.lib" \
	--def "$HERE/ekern_rom.def" --dllname "EKERN[100000ba].EXE" )

for l in psikernldd:ldd psikerntest:test; do
	name=${l%%:*}; dir=$HERE/${l#*:}
	if [ -e "$EPOCROOT/ptproj/$name" ] && [ ! -L "$EPOCROOT/ptproj/$name" ]; then
		echo "$EPOCROOT/ptproj/$name exists and is not a link - move it away"; exit 1
	fi
	ln -sfn "$dir" "$EPOCROOT/ptproj/$name"
done
# (makmake resolves "../psikern.h" from ptproj/<name>/, so the shared header
# must be reachable as ptproj/psikern.h too)
ln -sfn "$HERE/psikern.h" "$EPOCROOT/ptproj/psikern.h"

echo "== PSIKERN.LDD"
( cd "$EPOCROOT/ptproj/psikernldd"
  makmake psikern marm > "$TOP/build/kernel-ldd-makmake.log" 2>&1
  make -f psikern.marm rel > "$TOP/build/kernel-ldd.log" 2>&1 \
	|| { grep -i -B1 -A4 "error\|undefined" "$TOP/build/kernel-ldd.log" | head -40; exit 1; } )

echo "== PSIKT.APP"
( cd "$EPOCROOT/ptproj/psikerntest"
  makmake psikt marm > "$TOP/build/kernel-test-makmake.log" 2>&1
  make -f psikt.marm rel > "$TOP/build/kernel-test.log" 2>&1 \
	|| { grep -i -B1 -A4 "error\|undefined" "$TOP/build/kernel-test.log" | head -40; exit 1; } )

P=$TOP/build/kernel-pkg
rm -rf "$P"; mkdir -p "$P"
cp "$REL/psikern.ldd" "$REL/psikt.app" "$REL/psikt.rsc" "$HERE/psikt.pkg" "$P/"
# (for a 5mx: a local file only, never in dist/ or a release)
( cd "$P" && wine "$EPOCROOT/epoc32/tools/makesis.exe" psikt.pkg PsiKernTest.sis > /dev/null )
ls -la "$P"
