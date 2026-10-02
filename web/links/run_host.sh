#!/bin/bash
# Builds links-psi (Links 2 with the psi driver) and renders a set of pages
# at 640x240 in 16 greys, as the Psion would show them, with the heap peak
# for each page. Run on the PC from the repository root:
#
#   web/links/run_host.sh [OUTDIR] [NAME=URL ...]
#
# With no pages given it runs the standard set (see PAGES below), plus a
# local page with JPEG, PNG and GIF pictures served on 127.0.0.1. Each page
# runs in a fresh process: open, wait until idle, screenshot, Page Down,
# screenshot. With no pages given there is also "nav": a pen tap on a link,
# then Back and Forward. OUTDIR (default build/links/shots) gets NAME-1.png, NAME-2.png
# and summary.txt.
#
# Environment:
#   LINKS_OPTS       extra Links options
#   PSI_HEAP_LIMIT   refuse allocations above this many bytes (e.g. 10485760)
#   NOBUILD=1        skip the build
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
RUN=$TOP/build/links/run
OUT=${1:-$TOP/build/links/shots}
[ $# -gt 0 ] && shift
mkdir -p "$OUT" "$RUN/home" "$RUN/www"
OUT=$(cd "$OUT" && pwd)
cd "$TOP"

PAGES="$*"
[ -n "$PAGES" ] || PAGES="
cern=http://info.cern.ch/
68k=http://68k.news/
npr=http://text.npr.org/
wikipedia=https://en.m.wikipedia.org/wiki/Psion
bbc=https://www.bbc.co.uk/
images=http://127.0.0.1:8765/images.html
"

# Links' own settings, chosen for a 640x240 screen and a 10 MB heap
OPTS="-async-dns 0 -html-user-font-size 14 -menu-font-size 12 \
-memory-cache-size 262144 -image-cache-size 262144 -font-cache-size 262144 -format-cache-size 1 \
-html-g-background-color 0xffffff -menu-background-color 0xffffff -menu-foreground-color 0x000000 \
-dither-images 0 -dither-letters 0 $LINKS_OPTS"

[ -n "$NOBUILD" ] || tools/docker/psibuild web/links/build_host.sh

# a local page with pictures in each format
python3 "$HERE/mktestpage.py" "$RUN/www"

for p in $PAGES; do
	name=${p%%=*}; url=${p#*=}
	cat > "$RUN/$name.txt" <<-EOF
	open $url
	idle 2500
	shot $name-1.pgm
	key 281
	wait 400
	idle 1000
	shot $name-2.pgm
	quit
	EOF
	rm -f "$RUN/$name"-*.pgm
done
# navigation: tap a link with the pen, then Back and Forward (PW_CMD_*)
if [ -z "$*" ]; then
	PAGES="$PAGES nav=http://68k.news/"
	cat > "$RUN/nav.txt" <<-EOF
	open http://68k.news/
	idle 2500
	pen 300 222
	wait 400
	idle 2500
	shot nav-1.pgm
	cmd 2
	wait 400
	idle 1500
	shot nav-2.pgm
	cmd 3
	wait 400
	idle 1500
	shot nav-3.pgm
	quit
	EOF
	rm -f "$RUN"/nav-*.pgm
fi

# all pages in one container; each page is its own links-psi process
tools/docker/psibuild "cd '$RUN' && (python3 -m http.server 8765 --bind 127.0.0.1 -d www > httpd.log 2>&1 &) && sleep 1;
for p in $(echo $PAGES | tr '\n' ' '); do
	n=\${p%%=*}
	echo \"== \$n\"
	HOME='$RUN/home' PW_SCRIPT=\$n.txt PSI_HEAP_LIMIT='$PSI_HEAP_LIMIT' timeout 120 ../host/links-psi $OPTS > \$n.log 2>&1; echo \"rc=\$?\" >> \$n.log
done"

: > "$OUT/summary.txt"
for p in $PAGES; do
	name=${p%%=*}; url=${p#*=}
	for f in "$RUN/$name"-*.pgm; do
		[ -f "$f" ] && python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$f" "$OUT/$(basename "${f%.pgm}").png"
	done
	{ echo "$name  $url"
	  grep -E "^\[mem\]|^\[fatal\]|^rc=|ERROR" "$RUN/$name.log" | sed 's/^/    /'
	} >> "$OUT/summary.txt"
done
cat "$OUT/summary.txt"
echo "screenshots in $OUT"
