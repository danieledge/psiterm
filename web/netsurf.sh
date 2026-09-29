#!/bin/bash
# Fetches NetSurf and its libraries at the versions PsiWeb was made with,
# applies the Psion patches (web/patches/) and builds everything once for
# this PC. That host build runs NetSurf's code generators (CSS property
# tables, HTML entities, the built-in font and images) and records each
# project's compile commands, which web/gen.py turns into rules for the
# Psion build.
#   web/netsurf.sh [DIR]     (default: build/netsurf)
# Needs: git, gcc, make, perl, flex, bison, gperf, pkg-config, zlib and
# libpng development files.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
NS=${1:-$HERE/../build/netsurf}
mkdir -p "$NS"
NS=$(cd "$NS" && pwd)
PREFIX=$NS/hinst
CMDS=$NS/cmds
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig

# project  commit
REPOS="
buildsystem    0005ae300283ff01c2e2b05e7376b3e55dea21f7
libwapcaplet   c7c128d3eb3223b216c974471f82e9337fbcf4ba
libparserutils 6b0cbf086ca8eb8fe74b69f0c9ecf274eb2397ca
libhubbub      6651b8cf87a4aa87bcdb2ff024a02659cd3f9402
libdom         f69781e1f062444b5af3f62d431d7d94018da53b
libcss         499f1c4601ad39942fd1b2204053a387bec9b989
libnsutils     0bd39060740b6163bd50875326654a722df97eb2
libnslog       bedff2146270a8a73cc265bab46ec39f9c170d07
libnsgif       22e99eb6818b1284d0f3ff1b7f46159e87221220
libnsbmp       ea063c9f46acb43e90208da14073332b505ef7e7
libnspsl       82815c2bc7fd70d1b6afccfa89a9a0f3fa73db8a
libnsfb        b701cdce7241c3747ccd78658a365db0983ebe24
nsgenbind      44c6736937ae17d4065d02959b82813b8f06a51e
netsurf        39da3c3a40af4566d86500ff3052dfdc7f9a0378
"
fetch() {   # name url commit
	if [ ! -d "$NS/$1/.git" ]; then
		git clone -q "$2" "$NS/$1"
	fi
	git -C "$NS/$1" checkout -q "$3"
}
echo "$REPOS" | while read -r name commit; do
	[ -n "$name" ] || continue
	echo "== $name"
	fetch "$name" "https://github.com/netsurf-browser/$name.git" "$commit"
done
echo "== utf8proc"
fetch libutf8proc https://github.com/JuliaStrings/utf8proc.git 4bfe012cb879a58a70715526e9db6a49486df571

echo "== patches"
for p in "$HERE"/patches/*-psion.diff; do
	proj=$(basename "$p" -psion.diff)
	git -C "$NS/$proj" checkout -q -- .
	git -C "$NS/$proj" apply "$p"
done

cat > "$NS/netsurf/Makefile.config" <<'EOF'
override NETSURF_USE_DUKTAPE := NO
override NETSURF_USE_CURL := NO
override NETSURF_USE_OPENSSL := NO
override NETSURF_USE_JPEG := NO
override NETSURF_USE_PNG := NO
override NETSURF_USE_WEBP := NO
override NETSURF_USE_RSVG := NO
override NETSURF_USE_NSSVG := NO
override NETSURF_USE_ROSPRITE := NO
override NETSURF_USE_HARU_PDF := NO
override NETSURF_USE_VIDEO := NO
override NETSURF_USE_UTF8PROC := YES
override NETSURF_FB_FRONTEND := ram
override NETSURF_FB_FONTLIB := internal
EOF

mkdir -p "$CMDS" "$PREFIX"
(cd "$NS/buildsystem" && make -s install PREFIX="$PREFIX" > /dev/null)
BS="NSSHARED=$PREFIX/share/netsurf-buildsystem"
for l in libwapcaplet libparserutils libhubbub libdom libcss libnsutils libnslog libnsgif libnsbmp libnspsl libnsfb nsgenbind; do
	echo "== host build: $l"
	(cd "$NS/$l" && make -s clean PREFIX="$PREFIX" $BS > /dev/null 2>&1 || true
	 make Q= PREFIX="$PREFIX" $BS 2>&1 | grep -E "^(cc|gcc) .* -c " > "$CMDS/$l.txt" || true
	 make -s install PREFIX="$PREFIX" $BS > /dev/null)
done
(cd "$NS/libutf8proc" && make -s install prefix="$PREFIX" > /dev/null)
echo "== host build: netsurf"
(cd "$NS/netsurf" && make -s clean TARGET=framebuffer > /dev/null 2>&1 || true
 make -j2 Q= TARGET=framebuffer PREFIX="$PREFIX" $BS 2>&1 | grep -E "^(cc|gcc) .* -c " > "$CMDS/netsurf.txt")
echo "NetSurf ready in $NS"
