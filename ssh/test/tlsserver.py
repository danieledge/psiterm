#!/usr/bin/env python3
"""HTTPS (TLS 1.3 only) file server with Range support, like
raw.githubusercontent.com, for testing psissh's updater.
   tlsserver.py PORT ROOT"""
import http.server, ssl, sys, os, subprocess
PORT, ROOT = int(sys.argv[1]), sys.argv[2]
crt, key = "/tmp/tlstest.crt", "/tmp/tlstest.key"
if not os.path.exists(crt):
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=localhost",
                    "-days", "30", "-keyout", key, "-out", crt], check=True, capture_output=True)
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, fmt, *a): sys.stderr.write("tls: " + (fmt % a) + "\n")
    def do_GET(self):
        fn = os.path.join(ROOT, self.path.split("?")[0].lstrip("/"))   # query ignored, like GitHub
        if not os.path.isfile(fn):
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        d = open(fn, "rb").read()
        rng = self.headers.get("Range")
        if rng and rng.startswith("bytes="):
            a, b = rng[6:].split("-"); a = int(a); b = min(int(b), len(d) - 1)
            body = d[a:b + 1]
            self.send_response(206); self.send_header("Content-Range", "bytes %d-%d/%d" % (a, b, len(d)))
        else:
            body = d; self.send_response(200)
        self.send_header("Content-Length", str(len(body))); self.send_header("Connection", "close")
        self.end_headers(); self.wfile.write(body)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(crt, key)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()
