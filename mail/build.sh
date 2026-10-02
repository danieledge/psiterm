#!/bin/bash
# Builds PsiMail for the Psion Series 5mx and packages dist/PsiMail.sis.
#   PSION_SDK=/path/to/psion_cpp_sdk_linux mail/build.sh [host]
# 'host' also builds build/mail-host/psimail-host, the same engine for a PC
# (for the tests in mail/test).
set -e
: "${PSION_SDK:?set PSION_SDK to your psion_cpp_sdk_linux directory}"
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/.." && pwd)
export EPOCROOT=$PSION_SDK/epoc_cpp_sdk
REL=$EPOCROOT/epoc32/release/marm/rel
mkdir -p "$TOP/build"

for t in epoc ${1:+$1}; do
	echo "== psimail engine ($t)"
	make -C "$HERE" -j4 TARGET=$t PSION_SDK="$PSION_SDK" > "$TOP/build/mail-$t.log" 2>&1 \
		|| { grep -E "error|undefined" "$TOP/build/mail-$t.log" | grep -v error_to_string | head -30; exit 1; }
done

echo "== pictures (PsiMail.mbm)"
python3 "$HERE/tools/mkicons.py" "$TOP/build/mail-icons" "$HERE/ui/pmicons.h"
(cd "$TOP/build/mail-icons" && WINEDEBUG=-all wine "$EPOCROOT/epoc32/tools/bmconv.exe" /q psimail.mbm $(tr '\n' ' ' < files.txt) > /dev/null 2>&1)

echo "== PsiMail.app"
(
export PATH=/usr/bin:/bin:$PSION_SDK/gcc-3.0-psion-98r2-9/bin:$EPOCROOT/epoc32/tools
export TMP=$EPOCROOT/tmp WINEDEBUG=-all
mkdir -p "$TMP" "$EPOCROOT/ptproj"
for l in psimail:mail/app psimailinc:mail psitermssh:ssh psimailbtn:mail/button; do
	name=${l%%:*}; dir=$TOP/${l#*:}
	if [ -e "$EPOCROOT/ptproj/$name" ] && [ ! -L "$EPOCROOT/ptproj/$name" ]; then
		echo "$EPOCROOT/ptproj/$name exists and is not a link - move it away"; exit 1
	fi
	ln -sfn "$dir" "$EPOCROOT/ptproj/$name"
done
cd "$EPOCROOT/ptproj/psimail"
makmake psimail marm > "$TOP/build/psimail-makmake.log" 2>&1
make -f psimail.marm rel > "$TOP/build/psimail-app.log" 2>&1 \
	|| { grep -i -B1 -A4 "error\|undefined" "$TOP/build/psimail-app.log" | head -60; exit 1; }

# the Email icon below the screen (mail/button): pmbutton.exe takes the key,
# pmbutton.rdl (a boot hook) is built but not installed: see README
echo "== pmbutton.exe, pmbutton.rdl"
cd "$EPOCROOT/ptproj/psimailbtn"
for t in pmbutton pmbtnrec; do
	makmake $t marm > "$TOP/build/$t-makmake.log" 2>&1
	make -f $t.marm rel > "$TOP/build/$t.log" 2>&1 \
		|| { grep -i -B1 -A4 "error\|undefined" "$TOP/build/$t.log" | grep -v "def file" | head -40; exit 1; }
done
)

echo "== package"
P=$TOP/build/mail-pkg
rm -rf "$P"; mkdir -p "$P" "$TOP/dist"
cp "$REL/psimail.app" "$REL/psimail.rsc" "$REL/psimail.exe" "$REL/pmbutton.exe" "$HERE/pkg/psimail.pkg" "$TOP/build/mail-icons/psimail.mbm" "$P/"
# psimail.exe needs the EPOC C library (ESTLIB.DLL), which the 5mx ROM
# lacks: embed the SDK's redistributable stdlib.sis, as PsiTerm does
install -m 644 "$REL/stdlib.sis" "$P/STDLIB.SIS"
python3 "$HERE/pkg/mkicon.py" "$P/psimail.aif" > /dev/null
(cd "$P" && WINEDEBUG=-all wine "$EPOCROOT/epoc32/tools/makesis.exe" psimail.pkg "$TOP/dist/PsiMail.sis" > /dev/null)
ls -la "$TOP/dist/PsiMail.sis"
