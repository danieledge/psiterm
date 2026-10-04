#!/usr/bin/env python3
"""tools/emu/nethttp.py PORT - a tiny web server for PsiWeb's emulator test.

Serves two fixed pages from memory on 127.0.0.1 (never files from disk), and
logs every request, so the test can see that the page went through the
emulated modem. Used by tools/emu/net.sh.
"""
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PAGES = {
    '/': b"""<html><head><title>Emulator test page</title></head><body>
<h1>Hello from the host</h1>
<p>This page came from <b>tools/emu/nethttp.py</b>, through the emulator's
serial bridge and the host modem, to PsiWeb on an emulated Series 5mx.</p>
<ul><li>The modem dialled with ATDT host:port</li>
<li>The page was fetched with plain HTTP/1.0 or 1.1</li></ul>
<p><a href="/two">A second page</a></p>
<p>PSIEMU-HTTP-OK</p>
</body></html>
""",
    '/two': b"""<html><head><title>Second page</title></head><body>
<h1>Second page</h1><p>Links work too. <a href="/">Back to the first</a></p>
</body></html>
""",
}


class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    def do_GET(self):
        body = PAGES.get(self.path.split('?')[0])
        if body is None:
            self.send_response(404)
            body = b'<html><body><h1>Not found</h1></body></html>\n'
        else:
            self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=iso-8859-1')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write('nethttp: %s %s\n' % (self.client_address[0], fmt % args))
        sys.stderr.flush()


HTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
