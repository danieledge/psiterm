#!/bin/bash
# Runs the Links PsiWeb's ARM code (build/links/epoc/psiweb-emu.pe) in the
# emulator harness on a set of pages, counting instructions, with the 10 MB
# heap limit. For each page: open, wait until idle, screenshot, Page Down,
# screenshot. Writes NAME-1.png, NAME-2.png, NAME.log, NAME.json and
# summary.txt to OUTDIR (default build/links/emu-shots).
#
#   web/links/emu/run_pages.sh [OUTDIR] [NAME=URL ...]
#
# Build first: tools/docker/psibuild "make -f web/links/epoc.mk -j8 emu"
# Environment: JOBS (parallel runs, default 3), RUN_OPTS (more harness
# options, e.g. "--profile" or "--heap-limit 8000000"),
# NET=record|replay (default: live network): record saves each page's traffic
# to NETDIR/NAME/net.json (default NETDIR build/links/net); replay plays it
# back with the harness's virtual clock, so runs are repeatable and can be
# compared before and after a change.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
OUT=${1:-$TOP/build/links/emu-shots}
[ $# -gt 0 ] && shift
mkdir -p "$OUT/www"
OUT=$(cd "$OUT" && pwd)
JOBS=${JOBS:-3}
NET=${NET:-}
NETDIR=${NETDIR:-$TOP/build/links/net}
PORT=8765

PAGES="$*"
[ -n "$PAGES" ] || PAGES="
cern=http://info.cern.ch/
68k=http://68k.news/
npr=https://text.npr.org/
wikipedia=https://en.m.wikipedia.org/wiki/Psion
bbc=https://www.bbc.co.uk/
images+=http://127.0.0.1:$PORT/images.html
showpics=http://127.0.0.1:$PORT/images.html
"

# the local picture page (JPEG, PNG, GIF), served on 127.0.0.1 only
if [ "$NET" != replay ]; then
	python3 "$TOP/web/links/mktestpage.py" "$OUT/www" > /dev/null
	python3 -m http.server $PORT --bind 127.0.0.1 -d "$OUT/www" > "$OUT/httpd.log" 2>&1 &
	HTTPD=$!
	trap 'kill $HTTPD 2>/dev/null' EXIT
	sleep 1
fi

run_one() {   # name url
	local name=$1 url=$2 opts="--count --json $OUT/$1.json $RUN_OPTS"
	case $name in
	*+) name=${name%+}; opts="--count --images --json $OUT/$name.json $RUN_OPTS" ;;
	esac
	case $NET in
	record) opts="$opts --record $NETDIR/$name" ;;
	replay) opts="$opts --replay $NETDIR/$name" ;;
	esac
	if [ "$name" = showpics ]; then
		# pictures off, then the app's "Show pictures" (PW_CMD_IMAGES "1")
		cat > "$OUT/$name.txt" <<-EOF
		open $url
		idle 3000
		shot $OUT/$name-1.png
		cmd 12 1
		wait 400
		idle 3000
		shot $OUT/$name-2.png
		quit
		EOF
	else
		cat > "$OUT/$name.txt" <<-EOF
		open $url
		idle 3000
		shot $OUT/$name-1.png
		mark Page Down
		key 281
		wait 400
		idle 2000
		shot $OUT/$name-2.png
		quit
		EOF
	fi
	timeout 3600 python3 "$HERE/run_links.py" $opts "$OUT/$name.txt" > "$OUT/$name.log" 2>&1 || echo "rc=$?" >> "$OUT/$name.log"
}

pids=""
for p in $PAGES; do
	run_one "${p%%=*}" "${p#*=}" &
	pids="$pids $!"
	if [ $(echo $pids | wc -w) -ge "$JOBS" ]; then wait $pids; pids=""; fi
done
[ -z "$pids" ] || wait $pids

: > "$OUT/summary.txt"
for p in $PAGES; do
	name=${p%%=*}; name=${name%+}
	{ echo "$name  ${p#*=}"
	  grep -E "\[page\]|CRASH|rc=|exit " "$OUT/$name.log" | sed 's/^/    /'
	} >> "$OUT/summary.txt"
done
cat "$OUT/summary.txt"
