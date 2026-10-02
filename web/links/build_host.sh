#!/bin/bash
# Builds build/links/host/links-psi: Links 2 with the "psi" graphics driver
# and web/fb/pwhost.c, a PC stand-in for the Psion that renders 640x240 in
# 16 greys and takes a script (see run_host.sh).
# Run inside the psion-build container:
#   tools/docker/psibuild web/links/build_host.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
LK=$TOP/build/links
SRC=$LK/links-2.30

[ -f "$SRC/.psion-patched" ] || "$HERE/fetch.sh"

# configure once, in text mode: it only finds out about the C library and
# OpenSSL; host/config2.h turns graphics on with the psi driver
if [ ! -f "$SRC/.psion-configured" ]; then
	echo "== configure links"
	(cd "$SRC" && ./configure --with-ssl --without-libevent --without-gpm --without-brotli \
		--without-zstd --without-bzip2 --without-lzma --without-lzip > "$LK/configure.log" 2>&1) \
		|| { tail -20 "$LK/configure.log"; exit 1; }
	touch "$SRC/.psion-configured"
fi

echo "== links-psi"
make -C "$HERE" -j"$(nproc)" LK="$LK" > "$LK/build-host.log" 2>&1 \
	|| { grep -E "error|undefined" "$LK/build-host.log" | head -30; exit 1; }
ls -la "$LK/host/links-psi"
