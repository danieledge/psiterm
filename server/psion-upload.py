#!/usr/bin/env python3
"""Receives one chunk of a PsiTerm upload on stdin (called by psion-update.sh
after it has read the request headers).
  argv: SHOTS_DIR QUERY CONTENT_LENGTH CRC32_HEX
Writes the chunk into SHOTS_DIR/.part-ID at offset o; when the last chunk
lands, renames it to upload-DATE.bin. Prints a full HTTP reply: 200 if the
chunk arrived intact, 400 otherwise (PsiTerm then sends it again).
Waits at most 6 s of silence for missing bytes rather than hanging."""
import sys, os, select, time, zlib, re, urllib.parse

def reply(code, text):
    body = text.encode()
    sys.stdout.buffer.write(b"HTTP/1.0 %d %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n"
                            % (code, b"OK" if code == 200 else b"Bad Request", len(body)) + body)
    sys.stdout.buffer.flush()
    print(text, file=sys.stderr)

shots, query, length, crc = sys.argv[1], sys.argv[2], int(sys.argv[3] or 0), sys.argv[4]
q = urllib.parse.parse_qs(query)
ident = q.get("id", [""])[0]
try:
    off, total = int(q["o"][0]), int(q["t"][0])
except Exception:
    reply(400, "bad query"); sys.exit()
if not re.fullmatch(r"[0-9a-f]{1,24}", ident) or not (0 < length <= 65536) or not (0 < total < 20000000):
    reply(400, "bad request"); sys.exit()

data = b""
fd = sys.stdin.fileno()
while len(data) < length:
    r, _, _ = select.select([fd], [], [], 6.0)
    if not r:
        break
    d = os.read(fd, length - len(data))
    if not d:
        break
    data += d
got_crc = "%08x" % (zlib.crc32(data) & 0xffffffff)
if len(data) != length or (crc and got_crc != crc.lower().zfill(8)):
    reply(400, "chunk at %d damaged: %d of %d bytes, crc %s want %s" % (off, len(data), length, got_crc, crc))
    sys.exit()

os.makedirs(shots, exist_ok=True)
part = os.path.join(shots, ".part-" + ident)
have = os.path.getsize(part) if os.path.exists(part) else 0
if off > have:
    reply(400, "chunk at %d but only %d bytes so far" % (off, have)); sys.exit()
with open(part, "r+b" if os.path.exists(part) else "wb") as f:
    f.seek(off); f.write(data); f.truncate(off + len(data))
if off + len(data) >= total:
    final = os.path.join(shots, "upload-%s.bin" % time.strftime("%Y%m%d-%H%M%S"))
    os.rename(part, final)
    reply(200, "saved %s (%d bytes)" % (final, total))
else:
    reply(200, "chunk at %d ok" % off)
