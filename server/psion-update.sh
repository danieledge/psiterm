#!/bin/bash
# Tiny HTTP/1.0 server for PsiTerm (run by socat, one process per connection)
#   GET  /version.txt, /PsiTerm.sis(.sig)  -> from $PSION_UPDATE_DIR
#        (and PsiWeb-/PsiMail-version.txt, PsiWeb.sis(.sig), PsiMail.sis(.sig))
#   GET  /<file>.sis?o=OFFSET&n=LENGTH     -> one chunk, with X-CRC32 header
#   POST /upload                           -> saved in $PSION_SHOTS_DIR
# After sending, the connection is held open until the Psion hangs up
# (max 20 s): closing at once makes the modem drop its unsent buffer.
# Settings (environment variables, with these defaults):
#   PSION_UPDATE_DIR  folder with version.txt, PsiTerm.sis, PsiTerm.sis.sig
#   PSION_SHOTS_DIR   where uploaded screenshots are saved
#   PSION_LOG         log file
# Run it under socat, e.g.
#   socat TCP-LISTEN:8686,reuseaddr,fork,range=192.168.0.0/16 EXEC:/path/to/psion-update.sh
HERE=$(cd "$(dirname "$0")" && pwd)
DIR="${PSION_UPDATE_DIR:-$HOME/PsionDownloads/update}"
SHOTS="${PSION_SHOTS_DIR:-$HOME/PsionDownloads/screenshots}"
LOG="${PSION_LOG:-$HERE/update.log}"
if [ -x /opt/homebrew/bin/python3 ]; then PY=/opt/homebrew/bin/python3; else PY=$(command -v python3); fi
fsize() { wc -c < "$1" | tr -d ' '; }
read -r method path proto
len=0
crc=""
while IFS= read -r h; do
  h=${h%$'\r'}
  [ -z "$h" ] && break
  case "$h" in
    [Cc]ontent-[Ll]ength:*) len=${h#*:}; len=${len// /} ;;
    [Xx]-[Cc][Rr][Cc]32:*) crc=${h#*:}; crc=${crc// /} ;;
  esac
done
path=${path%$'\r'}
log() { echo "$(date '+%F %T') $method $path -> $1" >> "$LOG"; }
linger() { /usr/bin/perl -e 'alarm 20; 1 while <STDIN>' 2>/dev/null; }

# chunked upload (PsiTerm 0.17+): POST /upload?id=..&o=..&t=.. with X-CRC32
case "$path" in /upload\?*)
  if [ "$method" = "POST" ]; then
    exec 3>&1
    res=$("$PY" "$HERE/psion-upload.py" "$SHOTS" "${path#*\?}" "$len" "$crc" 2>&1 >&3)
    log "$res"
    linger
    exit 0
  fi ;;
esac

if [ "$method" = "POST" ] && [ "$path" = "/upload" ] && [ "$len" -gt 0 ] && [ "$len" -lt 20000000 ]; then
  mkdir -p "$SHOTS"
  out="$SHOTS/upload-$(date '+%Y%m%d-%H%M%S').bin"
  head -c "$len" > "$out"
  got=$(fsize "$out")
  if [ "$got" = "$len" ]; then
    printf 'HTTP/1.0 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'
    log "saved $out ($got bytes)"
  else
    printf 'HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'
    log "short upload $got of $len"
  fi
  linger
  exit 0
fi

query=""
case "$path" in *\?*) query=${path#*\?}; path=${path%%\?*} ;; esac
f="${path#/}"
case "$f" in
  version.txt|PsiTerm.sis|PsiTerm.sis.sig) ;;
  PsiWeb-version.txt|PsiWeb.sis|PsiWeb.sis.sig) ;;
  PsiMail-version.txt|PsiMail.sis|PsiMail.sis.sig) ;;
  *) f="" ;;
esac
if [ "$method" = "GET" ] && [ -n "$f" ] && [ -f "$DIR/$f" ]; then
  if [ -n "$query" ]; then
    "$PY" - "$DIR/$f" "$query" <<'PY'
import sys, zlib, urllib.parse
q = urllib.parse.parse_qs(sys.argv[2])
o = int(q.get("o", ["0"])[0]); n = int(q.get("n", ["0"])[0])
with open(sys.argv[1], "rb") as fh:
    total = fh.seek(0, 2)
    fh.seek(o); d = fh.read(max(0, n))
out = sys.stdout.buffer
out.write(("HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
           "Content-Length: %d\r\nX-Total: %d\r\nX-CRC32: %08x\r\nConnection: close\r\n\r\n"
           % (len(d), total, zlib.crc32(d) & 0xffffffff)).encode())
out.write(d); out.flush()
PY
    log "$f chunk $query"
  else
    flen=$(fsize "$DIR/$f")
    printf 'HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %s\r\nConnection: close\r\n\r\n' "$flen"
    # paced (~5 KB/s) - an old PsiTerm writing to the card at full 115200
    # speed drops bytes; chunked downloads (0.16+) don't need this
    "$PY" -c '
import sys, time
d = open(sys.argv[1], "rb").read(); o = sys.stdout.buffer
for i in range(0, len(d), 512):
    o.write(d[i:i+512]); o.flush(); time.sleep(0.1)
' "$DIR/$f"
    log "$f (paced)"
  fi
else
  printf 'HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'
  log 404
fi
linger
