#!/bin/bash
# Builds PsiWeb (NetSurf for the Psion Series 5mx) and packages
# dist/PsiWeb.sis.
#   PSION_SDK=/path/to/psion_cpp_sdk_linux web/build.sh [host]
# 'host' also builds build/web-host/psiweb-host, the same browser for a PC
# that renders into a 640x240 16-grey screenshot (see fb/pwhost.c).
# First run fetches and prepares NetSurf (web/netsurf.sh) in build/netsurf.
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
NS=$TOP/build/netsurf
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
REL=$EPOCROOT/epoc32/release/marm/rel

[ -f "$NS/cmds/netsurf.txt" ] || "$HERE/netsurf.sh" "$NS"

for t in epoc ${1:+$1}; do
	mkdir -p "$TOP/build/web-$t"
	python3 "$HERE/gen.py" "$NS/cmds" "$NS" > "$TOP/build/web-$t/rules.mk"
	echo "== psiweb ($t)"
	make -C "$HERE" -j2 TARGET=$t NS="$NS" PSION_SDK="$PSION_SDK" > "$TOP/build/web-$t.log" 2>&1 \
		|| { grep -E "error|undefined" "$TOP/build/web-$t.log" | head -30; exit 1; }
done

echo "== PsiWeb.app"
(
export PATH=/usr/bin:/bin:$PSION_SDK/gcc-3.0-psion-98r2-9/bin:$EPOCROOT/epoc32/tools
export TMP=$EPOCROOT/tmp WINEDEBUG=-all
mkdir -p "$TMP" "$EPOCROOT/ptproj"
for l in psiweb:web/app psiwebinc:web psitermssh:ssh; do
	name=${l%%:*}; dir=$TOP/${l#*:}
	if [ -e "$EPOCROOT/ptproj/$name" ] && [ ! -L "$EPOCROOT/ptproj/$name" ]; then
		echo "$EPOCROOT/ptproj/$name exists and is not a link - move it away"; exit 1
	fi
	ln -sfn "$dir" "$EPOCROOT/ptproj/$name"
done
cd "$EPOCROOT/ptproj/psiweb"
makmake psiweb marm > "$TOP/build/psiweb-makmake.log" 2>&1
make -f psiweb.marm rel > "$TOP/build/psiweb-app.log" 2>&1 \
	|| { grep -i -B1 -A4 "error\|undefined" "$TOP/build/psiweb-app.log" | head -40; exit 1; }
)

echo "== package"
P=$TOP/build/web-pkg
rm -rf "$P"; mkdir -p "$P" "$TOP/dist"
cp "$REL/psiweb.app" "$REL/psiweb.rsc" "$REL/psiweb.exe" "$HERE/pkg/psiweb.pkg" "$P/"
python3 "$HERE/pkg/mkicon.py" "$P/psiweb.aif" > /dev/null
{ echo "PsiWeb is NetSurf (https://www.netsurf-browser.org/), GNU GPL v2,"
  echo "with PsiTerm's networking and TLS (MIT). Source: https://github.com/danieledge/psiterm"
  echo "(web/ and web/netsurf.sh, which fetches the exact NetSurf sources used)."
  echo; cat "$NS/netsurf/COPYING"; } | sed 's/$/\r/' > "$P/COPYING.txt"
(cd "$P" && WINEDEBUG=-all wine "$EPOCROOT/epoc32/tools/makesis.exe" psiweb.pkg "$TOP/dist/PsiWeb.sis" > /dev/null)
ls -la "$TOP/dist/PsiWeb.sis"
