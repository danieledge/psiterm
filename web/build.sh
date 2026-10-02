#!/bin/bash
# Builds PsiWeb for the Psion Series 5mx and packages dist/PsiWeb.sis.
#   PSION_SDK=/path/to/psion_cpp_sdk_linux web/build.sh
#
# The browser engine, psiweb.exe, is Links 2 (web/links, from 0.62): the
# first run fetches and patches its sources (web/links/fetch.sh) into
# build/links, then web/links/epoc.mk builds it.
#
# NetSurf, the engine up to 0.61, is still in the tree (web/engine/fetch_psi.c,
# web/fb, web/patches, web/Makefile) and can be built instead:
#   PSIWEB_ENGINE=netsurf web/build.sh [host]
# ('host' also builds build/web-host/psiweb-host, NetSurf for a PC that
# renders into a 640x240 16-grey screenshot: see fb/pwhost.c.)
#
#   PSIWEB_SIS=path   where the .sis goes (default dist/PsiWeb.sis)
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
REL=$EPOCROOT/epoc32/release/marm/rel
ENGINE=${PSIWEB_ENGINE:-links}
SIS=${PSIWEB_SIS:-$TOP/dist/PsiWeb.sis}

if [ "$ENGINE" = links ]; then
	LK=$TOP/build/links
	[ -f "$LK/links-2.30/.psion-patched" ] || "$HERE/links/fetch.sh" "$LK"
	echo "== psiweb.exe (Links)"
	make -f "$HERE/links/epoc.mk" -j4 PSION_SDK="$PSION_SDK" exe > "$TOP/build/web-links.log" 2>&1 \
		|| { grep -E "error|undefined" "$TOP/build/web-links.log" | head -30; exit 1; }
	ENGINE_EXE=$LK/epoc/psiweb.exe
else
	NS=$TOP/build/netsurf
	[ -f "$NS/cmds/netsurf.txt" ] || "$HERE/netsurf.sh" "$NS"
	for t in epoc ${1:+$1}; do
		mkdir -p "$TOP/build/web-$t"
		python3 "$HERE/gen.py" "$NS/cmds" "$NS" > "$TOP/build/web-$t/rules.mk"
		echo "== psiweb (NetSurf, $t)"
		make -C "$HERE" -j2 TARGET=$t NS="$NS" PSION_SDK="$PSION_SDK" > "$TOP/build/web-$t.log" 2>&1 \
			|| { grep -E "error|undefined" "$TOP/build/web-$t.log" | head -30; exit 1; }
	done
	ENGINE_EXE=$REL/psiweb.exe
fi

echo "== pictures (PsiWeb.mbm)"
python3 "$HERE/tools/mkicons.py" "$TOP/build/web-icons" "$HERE/app/pwicons.h"
(cd "$TOP/build/web-icons" && WINEDEBUG=-all wine "$EPOCROOT/epoc32/tools/bmconv.exe" /q psiweb.mbm $(tr '\n' ' ' < files.txt) > /dev/null 2>&1)

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
rm -rf "$P"; mkdir -p "$P" "$(dirname "$SIS")"
cp "$REL/psiweb.app" "$REL/psiweb.rsc" "$HERE/pkg/psiweb.pkg" "$TOP/build/web-icons/psiweb.mbm" "$P/"
cp "$ENGINE_EXE" "$P/psiweb.exe"
install -m 644 "$REL/stdlib.sis" "$P/STDLIB.SIS"   # ESTLIB.DLL for psiweb.exe (not in the 5mx ROM)
python3 "$HERE/pkg/mkicon.py" "$P/psiweb.aif" > /dev/null
if [ "$ENGINE" = links ]; then
	{ echo "PsiWeb's browser engine is Links 2.30 (http://links.twibright.com/), GNU GPL v2,"
	  echo "with IJG libjpeg (IJG licence), libpng (libpng licence), zlib, and PsiTerm's"
	  echo "networking and TLS (MIT). Source: https://github.com/danieledge/psiterm"
	  echo "(web/ and web/links/fetch.sh, which fetches the exact Links sources used, and"
	  echo "web/links/patches, the changes made to them)."
	  echo; cat "$LK/links-2.30/COPYING"; } | sed 's/$/\r/' > "$P/COPYING.txt"
else
	{ echo "PsiWeb is NetSurf (https://www.netsurf-browser.org/), GNU GPL v2,"
	  echo "with PsiTerm's networking and TLS (MIT). Source: https://github.com/danieledge/psiterm"
	  echo "(web/ and web/netsurf.sh, which fetches the exact NetSurf sources used)."
	  echo; cat "$NS/netsurf/COPYING"; } | sed 's/$/\r/' > "$P/COPYING.txt"
fi
(cd "$P" && WINEDEBUG=-all wine "$EPOCROOT/epoc32/tools/makesis.exe" psiweb.pkg psiweb.sis > /dev/null)
mv "$P/psiweb.sis" "$SIS"
ls -la "$P/psiweb.exe" "$SIS"
