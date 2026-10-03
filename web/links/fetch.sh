#!/bin/bash
# Fetches the sources for Links 2 in PsiWeb at pinned versions, checks them,
# unpacks them into build/links and applies the Psion patches.
#   web/links/fetch.sh [DIR]      (default: build/links)
#
#   links-2.30      http://links.twibright.com/          GPL v2
#   jpeg-9f         https://www.ijg.org/ (IJG libjpeg)   IJG licence
#   libpng-1.6.43   https://github.com/pnggroup/libpng   libpng licence
# zlib comes from ssh/zlib in this repository.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
LK=${1:-$HERE/../../build/links}
mkdir -p "$LK"
LK=$(cd "$LK" && pwd)

# file  sha256  url
SOURCES="
links-2.30.tar.bz2     c4631c6b5a11527cdc3cb7872fc23b7f2b25c2b021d596be410dadb40315f166 http://links.twibright.com/download/links-2.30.tar.bz2
jpegsrc.v9f.tar.gz     04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b https://www.ijg.org/files/jpegsrc.v9f.tar.gz
libpng-1.6.43.tar.gz   fecc95b46cf05e8e3fc8a414750e0ba5aad00d89e9fdf175e94ff041caf1a03a https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.43.tar.gz
"

get() {   # file sha url
	if [ ! -f "$LK/$1" ]; then
		echo "== fetch $1"
		if command -v curl > /dev/null; then curl -fsSL -o "$LK/$1.part" "$3"
		else python3 -c "import sys,urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])" "$3" "$LK/$1.part"; fi
		mv "$LK/$1.part" "$LK/$1"
	fi
	echo "$2  $LK/$1" | sha256sum -c --quiet - || { echo "$1: checksum mismatch"; exit 1; }
}

echo "$SOURCES" | while read -r f s u; do
	if [ -n "$f" ]; then get "$f" "$s" "$u"; fi
done

cd "$LK"
[ -d jpeg-9f ] || tar xzf jpegsrc.v9f.tar.gz
[ -d libpng-1.6.43 ] || tar xzf libpng-1.6.43.tar.gz
if [ ! -f links-2.30/.psion-patched ]; then
	[ -d links-2.30 ] && { echo "$LK/links-2.30 exists without our patches: move it away"; exit 1; }
	tar xjf links-2.30.tar.bz2
	for p in "$HERE"/patches/*.diff; do
		echo "== patch $(basename "$p")"
		patch -d links-2.30 -p1 -s < "$p"
		basename "$p" >> links-2.30/.psion-patches
	done
	touch links-2.30/.psion-patched
fi
echo "sources ready in $LK"
