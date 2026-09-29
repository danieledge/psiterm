#!/usr/bin/env python3
"""Chunk-capable update server like the Mac's, with fault injection:
the 2nd request for chunk o=16384 is truncated, the 1st for o=32768 is corrupted."""
import http.server, zlib, urllib.parse, sys, os
ROOT = "/tmp/updsrv"
seen = {}
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a): pass
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        fn = os.path.join(ROOT, u.path.lstrip("/"))
        if not os.path.isfile(fn):
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        d = open(fn, "rb").read()
        q = urllib.parse.parse_qs(u.query)
        if "o" in q and os.environ.get("OLD") != "1":
            o, n = int(q["o"][0]), int(q["n"][0])
            c = d[o:o+n]; crc = zlib.crc32(c) & 0xffffffff
            seen[o] = seen.get(o, 0) + 1
            body = c
            if o == 16384 and seen[o] == 1: body = c[:5000]          # truncated
            if o == 32768 and seen[o] == 1: body = c[:100] + b"X" + c[101:]  # garbled
            self.send_response(200)
            self.send_header("Content-Length", str(len(c)))
            self.send_header("X-Total", str(len(d)))
            self.send_header("X-CRC32", "%08x" % crc)
            self.end_headers(); self.wfile.write(body)
        else:
            self.send_response(200); self.send_header("Content-Length", str(len(d))); self.end_headers(); self.wfile.write(d)
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
