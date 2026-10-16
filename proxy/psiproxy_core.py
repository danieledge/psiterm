#!/usr/bin/env python3
"""psiproxy_core.py - the web-slimming proxy engine for a Psion Series 5mx.

Ported from the macOS menu-bar modem (psimodem-tray.py): this is the proxy
engine only, with the serial / rumps / Bridge / tray code removed. It is
transport-agnostic - it reads and writes a byte stream - so the same code runs
over a TCP socket (psiproxyd.py) as it did over the serial line.

PsiWeb (Connection settings > Use proxy: this box's address, port 8081) sends every
request - https:// ones too - as a plain HTTP request with an absolute URI.
This engine fetches it (doing the TLS itself), un-gzips it, runs text/html
through the article / index pipeline (trafilatura + readability + an lxml tag
allowlist), transcodes images to small 16-grey GIFs Links can show, gzips the
slimmed output back, and keeps the connection up for many requests and sites.

Runtime feature flags (reading view, collapse menus) and the live stats live in
a single thread-safe `State` object (the module singleton `STATE`), which the
daemon and the TUI both read. reading / collapse_nav persist to
~/.config/psiproxy/config.json.
"""
import os, sys, time, json, socket, select, threading, subprocess, io, re, collections, queue
import gzip as _gzip, zlib, urllib.request, urllib.error, ssl
import http.client, ipaddress, hmac, importlib

HERE = os.path.dirname(os.path.abspath(__file__))


def _load_env_file():
    """Read KEY=VALUE settings from an optional config file into os.environ.
    The real environment always wins. The file is $PSIPROXY_CONFIG, else the
    first of <this dir>/psiproxy.env and ~/.config/psiproxy/psiproxy.env.
    Lines are KEY=VALUE; blank lines and # comments are ignored; values may be
    quoted. Keep secrets out of it: give them to the process's environment."""
    cands = [os.environ.get("PSIPROXY_CONFIG")] if os.environ.get("PSIPROXY_CONFIG") else [
        os.path.join(HERE, "psiproxy.env"),
        os.path.expanduser("~/.config/psiproxy/psiproxy.env")]
    for path in cands:
        try:
            with open(path, "r", encoding="utf-8") as f:
                lines = f.readlines()
        except OSError:
            continue
        for ln in lines:
            ln = ln.strip()
            if not ln or ln.startswith("#") or "=" not in ln:
                continue
            if ln.startswith("export "):
                ln = ln[7:].strip()
            k, _, v = ln.partition("=")
            k, v = k.strip(), v.strip()
            if len(v) >= 2 and v[0] == v[-1] and v[0] in "\"'":
                v = v[1:-1]
            if re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", k) and k not in os.environ:
                os.environ[k] = v
        return path
    return None


CONFIG_FILE_USED = _load_env_file()


def _env_int(name, default):
    try:
        return int(os.environ.get(name, "").strip() or default)
    except ValueError:
        return default


def _env_bool(name, default):
    v = os.environ.get(name, "").strip().lower()
    if not v:
        return default
    return v in ("1", "yes", "true", "on")


# -- security settings (see README.md, "Security") ----------------------------
# Fetch private/loopback/link-local destinations too (off: refused with 403).
ALLOW_PRIVATE = _env_bool("PSIPROXY_ALLOW_PRIVATE", False)
# Largest request body accepted from a client (413 beyond it).
MAX_REQ_BODY = _env_int("PSIPROXY_MAX_BODY", 4 * 1024 * 1024)
MAX_REQ_HEAD = 64 * 1024                 # request line + headers (431 beyond it)
MAX_REQ_LINE = 8 * 1024                  # the request line alone (414 beyond it)
REQ_STALL = 120                          # s a half-received request may stall
# /_putlog: on/off, optional shared token, where logs go and how much is kept
PUTLOG_ON = _env_bool("PSIPROXY_PUTLOG", True)
PUTLOG_TOKEN = os.environ.get("PSIPROXY_PUTLOG_TOKEN", "")
PUTLOG_MAX = 2 * 1024 * 1024             # one log (longer ones are truncated)
LOG_DIR = os.path.expanduser(os.environ.get("PSIPROXY_LOG_DIR", "~/psion-logs"))
LOG_DIR_MAX_BYTES = _env_int("PSIPROXY_LOG_MAX_MB", 50) * 1024 * 1024
LOG_DIR_MAX_FILES = _env_int("PSIPROXY_LOG_MAX_FILES", 200)
# Headless-browser rendering (Playwright) cannot be held to the private-
# destination rule reliably, so "auto" turns it on only with ALLOW_PRIVATE.
_JS = os.environ.get("PSIPROXY_JS_RENDER", "auto").strip().lower()
JS_RENDER_ALLOWED = ALLOW_PRIVATE if _JS in ("", "auto") else _JS in ("1", "yes", "true", "on")

# Pillow, for transcoding images to small 16-grey GIFs the Psion's Links can
# show (modern sites serve WebP/AVIF, which Links can't decode -> empty boxes).
# Optional: if it is not installed, images pass through unchanged.
try:
    from PIL import Image, ImageOps
except ImportError:
    Image = ImageOps = None

# trafilatura, to pull the main article out of a page (nav, ads, sidebars and
# footers removed) before simplifying. Optional, with a readability fallback;
# without either, pages are simplified whole. Never fatal.
trafilatura = None
_readability = None
try:
    import trafilatura
except ImportError:
    trafilatura = None
# readability-lxml: used alongside trafilatura for image-heavy articles
# (galleries/listicles) whose content images trafilatura drops; also the whole
# fallback extractor when trafilatura is absent.
try:
    from readability import Document as _readability   # readability-lxml
except Exception:
    _readability = None
try:
    import logging
    for _n in ("trafilatura", "trafilatura.core", "courlan", "htmldate"):
        logging.getLogger(_n).setLevel(logging.ERROR)
except Exception:
    pass

# NB: serial and rumps are deliberately NOT imported. This module is headless.

CONFIG_DIR = os.path.expanduser(os.environ.get("PSIPROXY_STATE_DIR", "~/.config/psiproxy"))
CONFIG_FILE = os.path.join(CONFIG_DIR, "config.json")
# the firmware's HtmlSimplifier binary (used only by the _index_page fallback);
# absent on this box, so _simplify falls back to raw HTML, which is fine.
PSISIMP = os.environ.get("PSISIMP", os.path.join(CONFIG_DIR, "psisimp"))

PROXY_HOST = "psiproxy"                   # the magic control-page name (back-compat)
PROXY_VERSION = "PsiProxy/1.0"
# The Psion's Links reads 8-bit Windows-1252, not UTF-8, so every HTML response
# is encoded to cp1252 and declared as such (see _html_bytes).
HTML_CTYPE = "text/html; charset=windows-1252"
# a browser-ish UA: many sites serve a lighter or saner page than they do to
# an unknown agent, and never the "unsupported browser" page
UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/124.0 Safari/537.36")
MAX_BODY = 4 * 1024 * 1024               # cap one fetched body (mirrors the Atom's bound)
PROXY_IMG_MAXW = 320                     # transcoded images: largest width, in pixels
PROXY_ARTICLE_MINCHARS = 800             # extracted text shorter than this -> treat the
                                         # page as an index/homepage and simplify it whole
PROXY_READER_WIDTH = 500                 # reading view: article column width (Links reads
                                         # <table width=N>; it has no CSS)
PROXY_ARTICLE_WIDTH = 560                # normal mode, article pages: column width (a little
                                         # wider than the reader, to fit images)
# Defaults for the two runtime flags (now held in State, overridden by config.json).
DEFAULT_READER = False                   # Reading view: strip to article (no pictures).
DEFAULT_COLLAPSE_NAV = True              # Collapse site menus to a single [ Menu ] link.
DEFAULT_JS_RENDER = True                 # Render JS-shell pages with headless Chromium.
DEFAULT_INTERACTIVE = True               # Stage 2: live interactive sessions for JS pages.

# -- server-side JS rendering (Stage 1: render-only) -------------------------
JS_RENDER_TIMEOUT = 12                   # hard cap (s) for one render
JS_SETTLE_MS = 800                       # extra settle after load/networkidle
JS_SHELL_MIN_VIS = 500                   # slimmed output shorter than this -> shell
JS_SHELL_SPA_BODYMAX = 1200              # empty-SPA raw body text below this -> shell
JS_SHELL_SPA_SCRIPTS = 8                 # ... with at least this many <script> tags
# a default locale/position so geo-aware pages render a sensible default
JS_GEO = {"latitude": 51.5074, "longitude": -0.1278}   # London
JS_LOCALE = "en-GB"

# -- Stage 2: live interactive sessions --------------------------------------
SESSION_IDLE = 600                       # evict a live session idle longer than this (s)
SESSION_CAP = 3                          # keep at most this many live sessions (LRU)
ACT_PATH = "/_act"                       # reserved path for replayed actions
# query keys the action handler reserves (everything else is a form field)
ACT_RESERVED = {"s", "e", "form", "kind", "v", "submit"}

# In-page instrumentation: tag the LIVE DOM non-destructively with data-psi-*
# attributes (adding attributes only; the real controls stay intact for replay).
# Python then rewrites a COPY of the HTML into proxy links/forms for the Psion.
JS_INSTRUMENT = r"""
() => {
  try {
    // clear any ids from a previous render so this render numbers everything
    // fresh (the DOM may have been rebuilt by JS, e.g. React) - avoids collisions
    document.querySelectorAll('[data-psi-id],[data-psi-act],[data-psi-fid]').forEach(e => {
      e.removeAttribute('data-psi-id'); e.removeAttribute('data-psi-act'); e.removeAttribute('data-psi-fid');
    });
    let n = 0;
    const id = (el) => { el.setAttribute('data-psi-id', String(++n)); return n; };
    const seen = (el) => el.hasAttribute('data-psi-id') || el.hasAttribute('data-psi-act');
    // clickable, non-navigational controls
    document.querySelectorAll('button,[role=button],summary,[onclick],input[type=button],input[type=submit]').forEach(el => {
      if (seen(el)) return;
      id(el); el.setAttribute('data-psi-act', 'click');
    });
    // anchors: real http(s) links stay navigational; the rest become click actions
    document.querySelectorAll('a').forEach(a => {
      if (seen(a)) return;
      const h = (a.getAttribute('href') || '').trim().toLowerCase();
      if (h.startsWith('http://') || h.startsWith('https://')) { a.setAttribute('data-psi-act','nav'); }
      else if (h.startsWith('mailto:') || h.startsWith('tel:')) { /* leave untouched */ }
      else { id(a); a.setAttribute('data-psi-act','click'); }
    });
    // forms
    document.querySelectorAll('form').forEach(f => {
      if (f.hasAttribute('data-psi-id')) return;
      const fid = id(f); f.setAttribute('data-psi-act','form');
      f.querySelectorAll('input,select,textarea').forEach(inp => inp.setAttribute('data-psi-fid', String(fid)));
    });
    // standalone text/search inputs not inside a form (common in SPAs: type + Enter)
    document.querySelectorAll('input[type=search],input[type=text],input:not([type])').forEach(inp => {
      if (inp.closest('form')) return;
      if (inp.hasAttribute('data-psi-id') || inp.hasAttribute('data-psi-fid')) return;
      id(inp); inp.setAttribute('data-psi-act','input');
    });
    return {n: n};
  } catch (e) { return {error: String(e)}; }
}
"""

# --- index vs article structural analysis (generic; no site-specific selectors) ---
PROXY_INDEX_LINKRATIO = 0.33             # link-text / visible-text above this (with many story
                                         # links) => index-like
PROXY_INDEX_LINKRATIO_HARD = 0.5         # link-text this dominant => index on its own
PROXY_INDEX_PROSE_FRAC = 0.15            # prose chars below this fraction of text => index-like
PROXY_INDEX_MIN_STORYLINKS = 15          # this many permalink-looking links => index-like ...
PROXY_INDEX_MAX_PROSE_P = 20             # ... when fewer than this many prose paragraphs
PROXY_HEADLINE_MINWORDS = 4              # a link with >= this many words is a headline, not nav
PROXY_ARIA_HIDDEN_MAXLEN = 400           # only delete aria-hidden subtrees shorter than this
# a11y "visually hidden" class idioms (the CSS-stripped source of Attribution/time noise)
PROXY_HIDDEN_CLASSES = [
    "visually-hidden", "visuallyhidden", "sr-only", "screen-reader-text",
    "screen-reader", "a11y-hidden", "hidden-visually", "u-visually-hidden",
    # the other word order, used by the BBC ("wr-hide-visually") and others
    "hide-visually", "visually-hide",
]
# nav/utility link wording (whole-text match, case-insensitive)
PROXY_NAV_LEXICON = (r"^(skip to (main )?content|accessibility( help)?|sign ?in|log ?in|"
                     r"register|subscribe|download the app|cookies?|consent|menu|search|"
                     r"home|back to top)$")
# pure boilerplate singletons: dropped entirely (not even shown in the menu)
PROXY_NAV_DROP = (r"^(skip to (main )?content|accessibility( help)?|cookies?( settings)?|"
                  r"consent|download the app|sign ?in|log ?in|register)$")
_HEADING_TAGS = {"h1", "h2", "h3"}       # a link inside one of these is always a headline


# ===========================================================================
#  Shared, thread-safe runtime state (flags + stats). One module singleton.
# ===========================================================================

class State:
    """Runtime feature flags and live stats, shared by the proxy worker threads,
    the control socket and the TUI. All access is under one lock. reading and
    collapse_nav persist to ~/.config/psiproxy/config.json."""
    def __init__(self):
        self._lock = threading.RLock()
        self.reading = DEFAULT_READER
        self.collapse_nav = DEFAULT_COLLAPSE_NAV
        self.js_render = DEFAULT_JS_RENDER
        self.interactive = DEFAULT_INTERACTIVE
        self.pages_ok = 0
        self.pages_failed = 0
        self.bytes_in = 0                 # upstream (decoded) bytes fetched
        self.bytes_out = 0               # bytes sent to the client (pre-gzip)
        self.renders = 0                 # pages re-fetched through the headless browser
        self.last_render_ms = 0          # how long the last render took
        self.started_at = time.time()
        self.last_host = None
        self.last_activity = None
        self.recent = collections.deque(maxlen=200)

    # -- persistence ------------------------------------------------------
    def load(self):
        try:
            with open(CONFIG_FILE) as f:
                d = json.load(f)
            with self._lock:
                if isinstance(d.get("reading"), bool):
                    self.reading = d["reading"]
                if isinstance(d.get("collapse_nav"), bool):
                    self.collapse_nav = d["collapse_nav"]
                if isinstance(d.get("js_render"), bool):
                    self.js_render = d["js_render"]
                if isinstance(d.get("interactive"), bool):
                    self.interactive = d["interactive"]
        except FileNotFoundError:
            pass
        except Exception:
            pass

    def save(self):
        try:
            os.makedirs(CONFIG_DIR, exist_ok=True)
            with self._lock:
                d = {"reading": self.reading, "collapse_nav": self.collapse_nav,
                     "js_render": self.js_render, "interactive": self.interactive}
            tmp = CONFIG_FILE + ".tmp"
            with open(tmp, "w") as f:
                json.dump(d, f)
            os.replace(tmp, CONFIG_FILE)
        except Exception:
            pass

    # -- flags ------------------------------------------------------------
    def set_flag(self, name, value):
        with self._lock:
            if name == "reading":
                self.reading = bool(value)
            elif name == "collapse_nav":
                self.collapse_nav = bool(value)
            elif name == "js_render":
                self.js_render = bool(value)
            elif name == "interactive":
                self.interactive = bool(value)
            else:
                return
        self.save()

    def toggle_flag(self, name):
        with self._lock:
            if name == "reading":
                self.reading = not self.reading
                new = self.reading
            elif name == "collapse_nav":
                self.collapse_nav = not self.collapse_nav
                new = self.collapse_nav
            elif name == "js_render":
                self.js_render = not self.js_render
                new = self.js_render
            elif name == "interactive":
                self.interactive = not self.interactive
                new = self.interactive
            else:
                return None
        self.save()
        return new

    def note_render(self, ms):
        with self._lock:
            self.renders += 1
            self.last_render_ms = int(ms)

    # -- stats ------------------------------------------------------------
    def record(self, method, host, status, bytes_in, bytes_out, ms, ok=True):
        with self._lock:
            self.last_host = host
            self.last_activity = time.time()
            self.bytes_in += max(0, bytes_in)
            self.bytes_out += max(0, bytes_out)
            if ok and status < 500:
                self.pages_ok += 1
            else:
                self.pages_failed += 1
            self.recent.appendleft({
                "t": self.last_activity, "method": method, "host": host,
                "status": status, "in": bytes_in, "out": bytes_out, "ms": ms,
            })

    def snapshot(self):
        with self._lock:
            total_in = self.bytes_in
            total_out = self.bytes_out
            ratio = (1.0 - (total_out / total_in)) if total_in > 0 else 0.0
            return {
                "reading": self.reading,
                "collapse_nav": self.collapse_nav,
                "js_render": self.js_render,
                "interactive": self.interactive,
                "pages_ok": self.pages_ok,
                "pages_failed": self.pages_failed,
                "renders": self.renders,
                "last_render_ms": self.last_render_ms,
                "bytes_in": total_in,
                "bytes_out": total_out,
                "saved_ratio": ratio,
                "started_at": self.started_at,
                "uptime": time.time() - self.started_at,
                "last_host": self.last_host,
                "last_activity": self.last_activity,
                "recent": list(self.recent),
            }


STATE = State()


# ===========================================================================
#  The proxy (transport-agnostic byte-stream engine)
# ===========================================================================

def _http_response(status, reason, body, content_type=None, gzip_body=False,
                   close=False, extra_headers=None):
    """Build a clean HTTP/1.1 response. Strips hop-by-hop framing; we set our
    own Content-Length / Content-Encoding / Connection."""
    body = body or b""
    enc_line = b""
    if gzip_body and body:
        try:
            body = _gzip.compress(body, 6)
            enc_line = b"Content-Encoding: gzip\r\n"
        except Exception:
            pass
    h = [b"HTTP/1.1 %d %s" % (status, (reason or "").encode("latin-1", "replace"))]
    if content_type:
        h.append(b"Content-Type: " + content_type.encode("latin-1", "replace"))
    h.append(b"Content-Length: %d" % len(body))
    if extra_headers:
        for k, v in extra_headers:
            try:
                h.append(("%s: %s" % (k, v)).encode("latin-1", "replace"))
            except Exception:
                pass
    h.append(b"Connection: " + (b"close" if close else b"keep-alive"))
    head = b"\r\n".join(h) + b"\r\n" + enc_line + b"\r\n"
    return head + body


def _error_body(what, detail=""):
    return _html_bytes("<html><head><title>Page not loaded</title></head><body>"
                       "<h2>Page not loaded</h2><p>%s</p><p>%s</p>"
                       "<p><small>%s web proxy</small></p></body></html>"
                       % (what, detail, PROXY_VERSION))


_META_CHARSET_RE = re.compile(rb"""(?is)<meta[^>]+charset\s*=\s*["']?\s*([a-z0-9_:.\-]+)""")


def _decode_html(body_bytes, content_type):
    """Decode a fetched HTML body to a Python str using the correct SOURCE
    charset: the HTTP Content-Type charset, else a <meta charset>/<meta
    http-equiv> in the HTML, else UTF-8. Always returns a str (errors replaced)."""
    enc = None
    if content_type:
        m = re.search(r"charset\s*=\s*([^\s;]+)", content_type, re.I)
        if m:
            enc = m.group(1).strip().strip('"\'').lower()
    if not enc:
        try:
            m = _META_CHARSET_RE.search(body_bytes[:4096])
            if m:
                enc = m.group(1).decode("ascii", "replace").strip().lower()
        except Exception:
            enc = None
    if not enc or enc in ("utf8",):
        enc = "utf-8"
    try:
        return body_bytes.decode(enc, "replace")
    except (LookupError, ValueError, TypeError):
        return body_bytes.decode("utf-8", "replace")


def _html_bytes(html):
    """Encode final HTML for the Psion's Links browser, which reads 8-bit
    Windows-1252. Most typographic characters (degrees, curly quotes, dashes,
    ellipsis, currency, (c)/(tm)) are single cp1252 bytes - exactly what avoids
    the UTF-8 mojibake. Anything outside cp1252 becomes a numeric HTML entity
    (xmlcharrefreplace) that Links can decode. Never raises."""
    if isinstance(html, bytes):
        try:
            html = html.decode("utf-8", "replace")
        except Exception:
            return html
    try:
        html = html.replace(" ", " ")   # nbsp -> plain space (ASCII nicety)
        return html.encode("cp1252", "xmlcharrefreplace")
    except Exception:
        try:
            return html.encode("ascii", "xmlcharrefreplace")
        except Exception:
            return html.encode("latin-1", "replace")


class SocketTransport:
    """The byte stream to a TCP client (PsiWeb over PPP, or a test client).
    recv() returns bytes, None when nothing is available yet, b'' on close."""
    def __init__(self, sock, stop_event):
        self.sock = sock
        self._stop = stop_event
        self.escaped = False
        sock.setblocking(False)

    def stopped(self):
        return self._stop.is_set()

    def recv(self):
        try:
            r, _, _ = select.select([self.sock], [], [], 0.2)
        except (OSError, ValueError):
            return b""
        if not r:
            return None
        try:
            d = self.sock.recv(4096)
        except BlockingIOError:
            return None
        except OSError:
            return b""
        return d  # b'' => client closed

    def send(self, data):
        # the socket is non-blocking: sendall() gives up (BlockingIOError)
        # as soon as the send buffer is full, which silently cut big
        # responses short for slow clients. Wait for room instead (up to
        # 120 s without progress).
        view = memoryview(data)
        idle = 0.0
        while view:
            if self._stop.is_set():
                return
            try:
                n = self.sock.send(view)
                view = view[n:]
                idle = 0.0
                continue
            except BlockingIOError:
                pass
            except OSError:
                return
            try:
                _, w, _ = select.select([], [self.sock], [], 1.0)
            except (OSError, ValueError):
                return
            if not w:
                idle += 1.0
                if idle >= 120:
                    return


def _content_length(header_text):
    for ln in header_text.split("\n"):
        k, _, v = ln.partition(":")
        if k.strip().lower() == "content-length":
            try:
                return max(0, int(v.strip()))
            except ValueError:
                return 0
    return 0


class ProxySession:
    """One keep-alive conversation with a PsiWeb client over any transport."""
    def __init__(self, transport, log, local_names=None):
        self.t = transport
        self.log = log
        # host strings (lowercase, no port) that identify THIS proxy, so a
        # request to the proxy's own IP:port is served as the control page.
        self.local_names = set(n.lower() for n in (local_names or ()))
        self.inbuf = bytearray()
        self.passthru = 0                    # images sent as-is (transcode fell back)
        self._req_authority = None           # host[:port] the current request came in on
        self._req_sid = None                 # psi_sess cookie on the current request
        self._emit_sid = None                # set a psi_sess cookie on this response

    def _is_control_host(self, hostpart):
        """True if hostpart names this proxy itself (control page), not a site."""
        if not hostpart:
            return True
        hp = hostpart.lower()
        return hp == PROXY_HOST or hp in self.local_names

    # -- reading requests -------------------------------------------------
    def _fill(self):
        d = self.t.recv()
        if d is None:
            return "none"
        if d == b"":
            return "closed"
        self.inbuf += d
        return "data"

    def read_request(self):
        """Return (head_bytes, body_bytes), an error tuple ("err", status,
        reason, text) for a request refused before its body is read, or None
        (closed/escape/stalled). The body is never read past MAX_REQ_BODY."""
        last = time.time()
        while not self.t.stopped():
            idx = self.inbuf.find(b"\r\n\r\n")
            sep = 4
            if idx < 0:
                j = self.inbuf.find(b"\n\n")
                if j >= 0:
                    idx, sep = j, 2
            if idx >= 0:
                if idx > MAX_REQ_HEAD:
                    return ("err", 431, "Request Header Fields Too Large",
                            "The request headers were too big for the proxy")
                head = bytes(self.inbuf[:idx])
                del self.inbuf[:idx + sep]
                htext = head.decode("latin-1", "replace")
                eol = htext.find("\n")
                if (eol if eol >= 0 else len(htext)) > MAX_REQ_LINE:
                    return ("err", 414, "URI Too Long",
                            "The address was too long for the proxy")
                if re.search(r"(?im)^transfer-encoding\s*:", htext):
                    return ("err", 411, "Length Required",
                            "The proxy needs a Content-Length, not a chunked body")
                need = _content_length(htext)
                if need > MAX_REQ_BODY:
                    return ("err", 413, "Payload Too Large",
                            "The request body was too big for the proxy")
                last = time.time()
                while len(self.inbuf) < need:
                    if self.t.stopped():
                        return None
                    r = self._fill()
                    if r == "closed":
                        return None
                    if r == "none":
                        if time.time() - last > REQ_STALL:
                            return None
                        time.sleep(0.01)
                    else:
                        last = time.time()
                body = bytes(self.inbuf[:need])
                del self.inbuf[:need]
                return head, body
            if len(self.inbuf) > MAX_REQ_HEAD:
                return ("err", 431, "Request Header Fields Too Large",
                        "The request headers were too big for the proxy")
            r = self._fill()
            if r == "closed":
                return None
            if r == "none":
                # idle between requests, or mid-head: wait for more
                if not self.inbuf:
                    last = time.time()
                    continue
                if time.time() - last > REQ_STALL:
                    return None
                time.sleep(0.01)
            else:
                last = time.time()
        return None

    def run(self):
        """Serve requests until the client closes, escapes or asks to close.
        Returns 'escape' for +++, else 'carrier'."""
        while not self.t.stopped():
            req = self.read_request()
            if req is None:
                return "escape" if getattr(self.t, "escaped", False) else "carrier"
            if req[0] == "err":
                _, code, reason, text = req
                self.log("proxy: request refused: %d %s" % (code, reason))
                self.t.send(_http_response(code, reason, _error_body(text),
                            HTML_CTYPE, False, close=True))
                return "carrier"
            head, body = req
            try:
                keep = self.handle(head, body)
            except Exception as e:
                self.log("proxy: handler error: %s" % e)
                try:
                    self.t.send(_http_response(502, "Bad Gateway",
                                _error_body("The proxy hit an internal error", str(e)),
                                HTML_CTYPE, False, close=True))
                except Exception:
                    pass
                return "carrier"
            if not keep:
                return "carrier"
        return "carrier"

    # -- handling one request --------------------------------------------
    def handle(self, head, body):
        text = head.decode("latin-1", "replace")
        lines = text.split("\n")
        reqline = lines[0].rstrip("\r")
        parts = reqline.split(" ")
        if len(parts) < 2:
            self.t.send(_http_response(400, "Bad Request",
                        _error_body("The request could not be understood"),
                        HTML_CTYPE, False, close=True))
            return False
        method = parts[0].upper()
        target = parts[1]
        version = parts[2] if len(parts) > 2 else "HTTP/1.0"
        client_close = not version.upper().startswith("HTTP/1.1")

        headers = {}
        for ln in lines[1:]:
            ln = ln.rstrip("\r")
            if ":" in ln:
                k, _, v = ln.partition(":")
                headers[k.strip().lower()] = v.strip()
        conn = headers.get("connection", "").lower() + " " + headers.get("proxy-connection", "").lower()
        if "close" in conn:
            client_close = True
        elif "keep-alive" in conn:
            client_close = False
        accepts_gzip = "gzip" in headers.get("accept-encoding", "").lower()
        accept = headers.get("accept", "").lower()
        self._req_sid = _parse_cookie(headers.get("cookie", ""), "psi_sess")
        self._emit_sid = None

        # Stage 2: a replayed interactive action (may arrive absolute or relative)
        apath, aquery = _target_path_query(target)
        if apath.rstrip("/").endswith(ACT_PATH):
            return self._do_interactive_act(aquery, headers, client_close, accepts_gzip)

        # Device logs: the apps POST their log here (a button) so it lands on the
        # box for debugging. Saved to ~/psion-logs/<app>-<timestamp>.log.
        if apath.rstrip("/").endswith("/_putlog"):
            return self._do_putlog(aquery, body, headers)

        # Optional plug-ins (see load_plugins): served only if the module
        # loaded at start; a route whose plug-in is absent answers 404.
        low = target.lower()
        to_us = target.startswith("/")
        if not to_us and (low.startswith("http://") or low.startswith("https://")):
            to_us = self._is_control_host(target.split("://", 1)[1].split("/", 1)[0].split(":", 1)[0].lower())
        for spec in PLUGIN_ROUTES:
            hit = (spec["prefix"] in apath) if spec["anywhere"] else apath.startswith(spec["prefix"])
            if not hit or (spec["local_only"] and not to_us):
                continue
            mod = PLUGINS.get(spec["module"])
            if mod is None:
                if not to_us:
                    continue        # a real site's own path: fetch it as normal
                self.t.send(_http_response(404, "Not Found",
                            b"Not available on this proxy\n",
                            "text/plain; charset=us-ascii", False, close=client_close))
                return not client_close
            if spec["set_logger"] and hasattr(mod, "set_logger"):
                mod.set_logger(self.log)
            return mod.serve(self, method, apath, aquery, body, client_close, _http_response)

        # CONNECT: PsiWeb must NOT tunnel; it sends https:// as a plain request
        if method == "CONNECT":
            self.t.send(_http_response(501, "Not Implemented",
                        _error_body("This proxy fetches https:// pages itself; "
                                    "ask for them as plain requests"),
                        HTML_CTYPE, False, close=True))
            return False

        # resolve the absolute URL and work out whether this is the control page
        low = target.lower()
        if low.startswith("http://") or low.startswith("https://"):
            url = target
            authority = url.split("://", 1)[1].split("/", 1)[0]
            hostpart = authority.split(":", 1)[0].lower()
            self._req_authority = authority
            # the proxy's own control page (addressed as "psiproxy" or by IP:port)
            if self._is_control_host(hostpart):
                from urllib.parse import urlsplit
                sp = urlsplit(url)
                return self._psiproxy(sp.path + ("?" + sp.query if sp.query else ""),
                                      client_close, authority)
        elif target.startswith("/"):
            host = headers.get("host", "")
            self._req_authority = host or None
            hostpart = host.split(":", 1)[0].lower()
            # a relative request to us (path /, /toggle...) is the control page
            if not host or self._is_control_host(hostpart):
                return self._psiproxy(target, client_close, host or None)
            url = "http://" + host + target
            hostpart = host.split(":", 1)[0].lower()
        else:
            self.t.send(_http_response(400, "Bad Request",
                        _error_body("The address is not one the proxy can fetch", target),
                        HTML_CTYPE, False, close=client_close))
            return not client_close

        is_image = ("image/" in accept and "text/html" not in accept)
        range_hdr = headers.get("range")

        t0 = time.time()
        try:
            resp, status, reason, final_url, ctype, up_hdrs = _open_upstream(
                method, url, headers, body, is_image, range_hdr, self.log)
        except Exception as e:
            blocked = _blocked_reason(e)
            if blocked:
                self.t.send(_http_response(403, "Forbidden",
                            _error_body("The proxy does not fetch local or private addresses",
                                        _esc_text(blocked)),
                            HTML_CTYPE, accepts_gzip, close=client_close))
                STATE.record(method, hostpart, 403, 0, 0,
                             int((time.time() - t0) * 1000), ok=False)
                self.log("proxy: %s %s %s -> 403 (%s)" % (method, hostpart, url[:80], blocked))
                return not client_close
            self.t.send(_http_response(502, "Bad Gateway",
                        _error_body("The server could not be reached", _esc_text(str(e))),
                        HTML_CTYPE, accepts_gzip, close=client_close))
            STATE.record(method, hostpart, 502, 0, 0,
                         int((time.time() - t0) * 1000), ok=False)
            self.log("proxy: %s %s %s -> 502 (%s)" % (method, hostpart, url[:80], e))
            return not client_close

        low_ct = (ctype or "").lower()
        is_html = ("text/html" in low_ct or "xhtml" in low_ct or "application/xml" in low_ct)
        is_img = low_ct.startswith("image/")
        # What gets the slimming / transcode treatment; everything else is a
        # transparent, streamed pass-through. A client Range request ALWAYS streams
        # verbatim (so the updater can resume), never slimmed or transcoded.
        slim_html = is_html and method != "HEAD" and not range_hdr
        transcode_img = is_img and method != "HEAD" and not range_hdr

        # ---- downloads / binaries / HEAD / Range: streamed pass-through --------
        if not slim_html and not transcode_img:
            sent, closed = self._stream_passthrough(resp, status, reason, up_hdrs,
                                                     method, client_close)
            ms = int((time.time() - t0) * 1000)
            self.log("proxy: %s %s %.80s -> %d passthru %s, %d bytes, %d ms"
                     % (method, hostpart, url, status,
                        ("range" if range_hdr else low_ct.split(";")[0] or "file"),
                        sent, ms))
            STATE.record(method, hostpart, status, 0, sent, ms, ok=(status < 500))
            return not closed

        # ---- buffered path: read (capped) + decompress, then slim / transcode --
        bodyb, truncated = _read_body_decoded(resp, up_hdrs, method)
        in_bytes = len(bodyb)                # upstream body, decoded
        if slim_html:
            out_ctype = HTML_CTYPE
            raw_html = _decode_html(bodyb, ctype)   # SOURCE charset -> Unicode
            # R1 un-hide cleanup first (removes CSS/ARIA-hidden noise that a
            # CSS-less browser would wrongly reveal), then route article vs index.
            try:
                cleaned = _unhide_clean(raw_html, self.log)
            except Exception:
                cleaned = raw_html
            try:
                slim = _render_html_page(cleaned, final_url, self.log)
            except Exception as e:
                self.log("proxy: render failed (%s); whole-page simplify" % e)
                slim = _index_page(raw_html.encode("utf-8", "replace"), final_url, self.log)
            # The static result looks like a JS shell: engage the browser.
            if (STATE.js_render and JS_RENDER_ALLOWED and method == "GET"
                    and _looks_like_js_shell(slim, raw_html, self.log)):
                if STATE.interactive:
                    # Stage 2: open/reuse a LIVE session, instrument + slim it
                    sid = self._req_sid or _new_sid()
                    t_r = time.time()
                    res = _render_js_session(sid, final_url, JS_RENDER_TIMEOUT, self.log)
                    rms = int((time.time() - t_r) * 1000)
                    islim = None
                    if res and res.get("html"):
                        islim = _interactive_slim(res["html"], sid,
                                                  res.get("url") or final_url, self.log)
                    if islim:
                        STATE.note_render(rms)
                        slim = islim
                        self._emit_sid = sid
                        self.log("interactive: %s session=%s %d ms, %d chars"
                                 % (hostpart, sid, rms, len(_visible_text(slim))))
                    else:
                        self.log("interactive: %s unavailable; kept static" % hostpart)
                else:
                    # Stage 1: render-only (keep the rendered result only if richer)
                    t_r = time.time()
                    rendered = _render_js(final_url, JS_RENDER_TIMEOUT, self.log)
                    rms = int((time.time() - t_r) * 1000)
                    if rendered:
                        STATE.note_render(rms)
                        slim2 = None
                        try:
                            slim2 = _render_html_page(_unhide_clean(rendered, self.log),
                                                      final_url, self.log)
                        except Exception as e:
                            self.log("proxy: post-render slim failed (%s)" % e)
                        before, after = len(_visible_text(slim)), len(_visible_text(slim2 or ""))
                        if slim2 and after >= before:
                            self.log("js-render: %s %d ms, %d -> %d chars (used rendered)"
                                     % (hostpart, rms, before, after))
                            slim = slim2
                        else:
                            self.log("js-render: %s %d ms, rendered not richer (%d vs %d); kept static"
                                     % (hostpart, rms, after, before))
                    else:
                        self.log("js-render: %s unavailable/failed; kept static" % hostpart)
            bodyb = _html_bytes(slim)               # OUTPUT -> Windows-1252
        else:   # transcode_img
            # transcode WebP/AVIF/etc. (and JPEG/PNG/GIF) to a small 16-grey GIF
            # Links can show; on any failure the original bytes go through
            gif = _transcode_image(bodyb, self.log) if bodyb else None
            if gif is not None:
                orig = len(bodyb)
                bodyb = gif
                out_ctype = "image/gif"
                self.log("proxy: image %.60s %s %d -> gif %d bytes"
                         % (url, low_ct, orig, len(bodyb)))
            else:
                self.passthru += 1           # Pillow missing/failed: original bytes
                out_ctype = ctype or "application/octet-stream"

        extra = []
        try:
            for k in ("Set-Cookie", "Cache-Control", "Content-Disposition",
                      "Last-Modified", "ETag", "Expires", "Location"):
                for v in (up_hdrs.get_all(k) or []):
                    extra.append((k, v))
        except Exception:
            pass
        if self._emit_sid:
            extra.append(("Set-Cookie", "psi_sess=%s; Path=/" % self._emit_sid))

        # only gzip the HTML we simplified (images etc. are already compressed)
        gzip_out = accepts_gzip and slim_html
        close = client_close or truncated
        self.t.send(_http_response(status, reason, bodyb, out_ctype, gzip_out,
                                   close, extra))
        kind = "html" if slim_html else "img"
        ms = int((time.time() - t0) * 1000)
        self.log("proxy: %s %s %.80s -> %d %s, %d->%d bytes, %d ms"
                 % (method, hostpart, url, status, kind, in_bytes, len(bodyb), ms))
        STATE.record(method, hostpart, status, in_bytes, len(bodyb), ms,
                     ok=(status < 500))
        return not close

    def _do_putlog(self, aquery, body, headers=None):
        """Save a log the device POSTed (the apps' "Send log to server" button).
        ?app=<name> picks the filename; the body is the log. -> PSIPROXY_LOG_DIR
        (default ~/psion-logs), pruned to PSIPROXY_LOG_MAX_MB/_MAX_FILES.
        Off with PSIPROXY_PUTLOG=0; with PSIPROXY_PUTLOG_TOKEN set, the request
        must carry ?token=<it> or an X-PsiProxy-Token header."""
        from urllib.parse import parse_qs
        q = parse_qs(aquery or "")
        if not PUTLOG_ON:
            self.t.send(_http_response(404, "Not Found", b"Not available on this proxy\n",
                        "text/plain; charset=us-ascii", False, close=True))
            return False
        if PUTLOG_TOKEN:
            given = (q.get("token", [""]) or [""])[0] or (headers or {}).get("x-psiproxy-token", "")
            if not hmac.compare_digest(given.encode("utf-8", "replace"),
                                       PUTLOG_TOKEN.encode("utf-8", "replace")):
                self.log("putlog: refused (no or wrong token)")
                self.t.send(_http_response(403, "Forbidden", b"Token needed\n",
                            "text/plain; charset=us-ascii", False, close=True))
                return False
        app = (q.get("app", ["log"]) or ["log"])[0] or "log"
        app = re.sub(r"[^A-Za-z0-9_.-]", "", app)[:40].lstrip(".") or "log"
        data = body or b""
        if len(data) > PUTLOG_MAX:
            data = data[:PUTLOG_MAX]
        try:
            d = LOG_DIR
            os.makedirs(d, mode=0o700, exist_ok=True)
            with _PUTLOG_LOCK:
                stem = "%s-%s" % (app, time.strftime("%Y%m%d-%H%M%S"))
                name, n = stem + ".log", 1
                while os.path.exists(os.path.join(d, name)):
                    n += 1
                    name = "%s-%d.log" % (stem, n)
                path = os.path.join(d, name)
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                with os.fdopen(fd, "wb") as f:
                    f.write(data)
                _prune_log_dir(d, keep=name, log=self.log)
            self.log("putlog: saved %d bytes from %s to %s" % (len(data), app, path))
            msg = ("Log saved: %s (%d bytes)\n" % (name, len(data))).encode("ascii", "replace")
            self.t.send(_http_response(200, "OK", msg,
                        "text/plain; charset=us-ascii", False, close=True))
        except Exception as e:
            self.log("putlog: failed: %s" % e)
            self.t.send(_http_response(500, "Error",
                        ("Could not save the log: %s\n" % e).encode("ascii", "replace"),
                        "text/plain; charset=us-ascii", False, close=True))
        return False

    def _psiproxy(self, target, close, authority=None):
        """The control panel. /toggle?reading and /toggle?nav flip a setting from
        PsiWeb itself; any other path shows the panel."""
        path, _, query = target.partition("?")
        if path.rstrip("/").endswith("/toggle") or path.strip("/") == "toggle":
            q = query.lower()
            if "reading" in q:
                new = STATE.toggle_flag("reading")
                self.log("proxy: reading view %s (web)" % ("on" if new else "off"))
            elif "nav" in q:
                new = STATE.toggle_flag("collapse_nav")
                self.log("proxy: collapse menus %s (web)" % ("on" if new else "off"))
            elif "act" in q:
                new = STATE.toggle_flag("interactive")
                self.log("proxy: interactive %s (web)" % ("on" if new else "off"))
            elif "js" in q:
                new = STATE.toggle_flag("js_render")
                self.log("proxy: JS rendering %s (web)" % ("on" if new else "off"))
        return self._control_page(close, authority)

    def _control_page(self, close, authority=None):
        auth = authority or self._req_authority or PROXY_HOST
        snap = STATE.snapshot()

        def row(label, on, key):
            return ('<li>%s: <b>%s</b> &middot; '
                    '<a href="http://%s/toggle?%s">%s</a></li>'
                    % (label, "ON" if on else "OFF", _esc_attr(auth), key,
                       "turn off" if on else "turn on"))
        body = ('<base href="http://%s/">\n'
                '<table width="%d" align="center"><tr><td>\n'
                '<h1>PsiProxy</h1>\n'
                '<ul>\n%s\n%s\n%s\n%s\n</ul>\n'
                '<hr><font size="2">Pages served: %d &middot; Failed: %d '
                '&middot; JS renders: %d &middot; Last site: %s</font>\n'
                '</td></tr></table>'
                % (_esc_attr(auth), PROXY_ARTICLE_WIDTH,
                   row("Reading view", snap["reading"], "reading"),
                   row("Collapse menus", snap["collapse_nav"], "nav"),
                   (row("JS rendering", snap["js_render"], "js") if JS_RENDER_ALLOWED
                    else "<li>JS rendering: <b>not enabled on this proxy</b></li>"),
                   row("Interactive", snap["interactive"], "act"),
                   snap["pages_ok"], snap["pages_failed"], snap["renders"],
                   _esc_text(snap["last_host"] or "-"))
                )
        self.t.send(_http_response(200, "OK", _html_bytes(body), HTML_CTYPE,
                                   False, close))
        return not close

    # -- Stage 2: replay an interactive action ---------------------------
    def _do_interactive_act(self, query, headers, client_close, accepts_gzip):
        """Handle GET /_act: replay a click or form submit on the caller's live
        session page, then return the re-rendered slim HTML. Falls back
        gracefully (fresh re-render of the Referer, or a notice) and never
        crashes."""
        from urllib.parse import parse_qs
        params = {}
        try:
            for k, v in parse_qs(query, keep_blank_values=True).items():
                params[k] = v[0] if v else ""
        except Exception:
            params = {}
        sid = params.get("s") or self._req_sid
        referer = headers.get("referer", "")
        t0 = time.time()
        res = _render_js_act(sid, params, JS_RENDER_TIMEOUT, self.log) if sid else None

        # session missing/expired: re-render the Referer fresh in a new session
        if (not res or res.get("expired") or not res.get("html")) and \
                referer.lower().startswith(("http://", "https://")):
            new_sid = _new_sid()
            self.log("act: session %s gone; fresh render of referer in %s" % (sid, new_sid))
            res = _render_js_session(new_sid, referer, JS_RENDER_TIMEOUT, self.log)
            if res and res.get("html"):
                sid = new_sid

        if not res or not res.get("html"):
            body = _html_bytes('<table width="%d" align="center"><tr><td>'
                               '<h2>Session expired</h2>'
                               '<p>Reload the page to carry on</p>'
                               '</td></tr></table>' % PROXY_ARTICLE_WIDTH)
            self.t.send(_http_response(200, "OK", body, HTML_CTYPE,
                                       accepts_gzip, close=client_close))
            return not client_close

        final_url = res.get("url") or referer or ""
        slim = _interactive_slim(res["html"], sid, final_url, self.log) or \
            '<table width="%d" align="center"><tr><td><p>Could not render</p>' \
            '</td></tr></table>' % PROXY_ARTICLE_WIDTH
        bodyb = _html_bytes(slim)
        ms = int((time.time() - t0) * 1000)
        STATE.note_render(ms)
        host = final_url.split("://", 1)[-1].split("/", 1)[0] if "://" in final_url else "-"
        STATE.record("ACT", host, 200, 0, len(bodyb), ms, ok=True)
        extra = [("Set-Cookie", "psi_sess=%s; Path=/" % sid)]
        self.t.send(_http_response(200, "OK", bodyb, HTML_CTYPE, accepts_gzip,
                                   client_close, extra))
        what = ("e=" + params["e"]) if params.get("e") else ("form=" + str(params.get("form")))
        self.log("act: s=%s %s -> %d chars, %d ms" % (sid, what, len(_visible_text(slim)), ms))
        return not client_close

    # -- transparent, Range-capable, streamed pass-through (downloads) ----
    def _stream_passthrough(self, resp, status, reason, hdrs, method, client_close):
        """Relay a non-slimmed response (binary, download, Range, HEAD) to the
        client byte-for-byte: status preserved (incl. 206), Content-Range /
        Accept-Ranges / Content-Length / Content-Encoding passed through, body
        chunk-streamed from upstream to the socket (no cap, no gzip, no re-encode).
        Returns (bytes_sent, connection_closed)."""
        ctype = hdrs.get("Content-Type") or "application/octet-stream"
        clen = hdrs.get("Content-Length")
        out = [b"HTTP/1.1 %d %s" % (status, (reason or "").encode("latin-1", "replace"))]
        out.append(b"Content-Type: " + ctype.encode("latin-1", "replace"))
        if clen is not None:
            out.append(("Content-Length: %s" % clen).encode("latin-1", "replace"))
        # preserve resume / caching / download headers verbatim
        for k in ("Content-Range", "Accept-Ranges", "Content-Encoding",
                  "Content-Disposition", "Last-Modified", "ETag", "Cache-Control",
                  "Expires", "Location", "Vary", "Set-Cookie"):
            try:
                for v in (hdrs.get_all(k) or []):
                    out.append(("%s: %s" % (k, v)).encode("latin-1", "replace"))
            except Exception:
                pass
        # without a Content-Length we must delimit the body by closing the socket
        close = client_close or (clen is None)
        out.append(b"Connection: " + (b"close" if close else b"keep-alive"))
        try:
            self.t.send(b"\r\n".join(out) + b"\r\n\r\n")
        except Exception:
            pass
        total = 0
        if method != "HEAD":
            while True:
                try:
                    chunk = resp.read(65536)
                except Exception:
                    break
                if not chunk:
                    break
                self.t.send(chunk)
                total += len(chunk)
        try:
            resp.close()
        except Exception:
            pass
        return total, close


_PUTLOG_LOCK = threading.Lock()


def _prune_log_dir(d, keep=None, log=None):
    """Delete the oldest *.log files in d until it holds at most
    LOG_DIR_MAX_FILES of them and LOG_DIR_MAX_BYTES in all (keep is spared)."""
    try:
        files = []
        for e in os.scandir(d):
            if e.is_file(follow_symlinks=False) and e.name.endswith(".log"):
                st = e.stat(follow_symlinks=False)
                files.append((st.st_mtime, e.name, st.st_size))
    except OSError:
        return
    files.sort()
    total = sum(f[2] for f in files)
    count = len(files)
    for mtime, name, size in files:
        if count <= LOG_DIR_MAX_FILES and total <= LOG_DIR_MAX_BYTES:
            break
        if name == keep:
            continue
        try:
            os.unlink(os.path.join(d, name))
            total -= size
            count -= 1
            if log:
                log("putlog: pruned %s" % name)
        except OSError:
            pass


# ===========================================================================
#  Optional plug-ins
# ===========================================================================
#
# A plug-in is a Python module in plugins.local/ beside this file, beside it,
# or on PSIPROXY_PLUGIN_PATH,
# with serve(session, method, path, query, body, client_close, http_response)
# returning "keep the connection". It may also have set_logger(fn) and
# start(). None is built in: which ones to load, and their routes, come from
# a local list (PSIPROXY_PLUGINS_FILE, default plugins.local.json beside this
# file, not part of the repository), a JSON list of
#   {"module": "name", "route": "/_x/", "anywhere": false,
#    "local_only": true, "set_logger": true}
# ("route" is required; "anywhere": the route may appear anywhere in the
# path; "local_only": only for requests to the proxy itself). A module named
# in PSIPROXY_PLUGINS but not in the list declares its own route with
# PSIPROXY_ROUTE = "/_x/" (and optionally PSIPROXY_LOCAL_ONLY = False).
#
# PSIPROXY_PLUGINS: "auto" (default) loads the list's modules and skips any
# that are missing; "none" loads none; else a comma-separated list of names.

PLUGINS = {}                             # module name -> loaded module
PLUGIN_ROUTES = []                       # routes answered (404 when not loaded)


def _plugin_list(log):
    """The local plug-in list (see above): {module: route spec}. A missing
    file is no list; a damaged one or a bad entry is logged and skipped."""
    path = os.path.expanduser(os.environ.get("PSIPROXY_PLUGINS_FILE", "").strip()
                              or os.path.join(HERE, "plugins.local.json"))
    try:
        with open(path) as f:
            items = json.load(f)
    except FileNotFoundError:
        return {}
    except (OSError, ValueError) as e:
        log("plugin: list %s not read (%s)" % (path, e))
        return {}
    specs = {}
    for it in items if isinstance(items, list) else []:
        name = it.get("module") if isinstance(it, dict) else None
        route = it.get("route") if isinstance(it, dict) else None
        if not (isinstance(name, str) and isinstance(route, str)
                and route.startswith("/_") and route.endswith("/")):
            log("plugin: bad entry in %s: %r" % (path, it))
            continue
        specs[name] = {"module": name, "prefix": route,
                       "anywhere": bool(it.get("anywhere", False)),
                       "local_only": bool(it.get("local_only", True)),
                       "set_logger": bool(it.get("set_logger", True))}
    return specs


def load_plugins(log):
    """Import the plug-ins once, at start. A missing or broken plug-in is
    logged and skipped: its routes answer 404. Starts each one that has
    start(). Returns the names loaded."""
    for p in os.environ.get("PSIPROXY_PLUGIN_PATH", "").split(os.pathsep):
        if p and p not in sys.path:
            sys.path.insert(0, os.path.expanduser(p))
    local = os.path.join(HERE, "plugins.local")    # (a local folder, not in the repository)
    if os.path.isdir(local) and local not in sys.path:
        sys.path.insert(0, local)
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    known = _plugin_list(log)
    PLUGIN_ROUTES[:] = list(known.values())
    want = os.environ.get("PSIPROXY_PLUGINS", "auto").strip()
    if want.lower() in ("", "auto"):
        names, quiet = list(known), True
    elif want.lower() == "none":
        names, quiet = [], True
    else:
        names, quiet = [n.strip() for n in want.split(",") if n.strip()], False
    for name in names:
        if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
            log("plugin: bad name %r" % name)
            continue
        try:
            mod = importlib.import_module(name)
        except ImportError as e:
            if not quiet or getattr(e, "name", None) != name:
                log("plugin: %s not loaded (%s)" % (name, e))
            continue
        except Exception as e:
            log("plugin: %s failed to load (%s)" % (name, e))
            continue
        if not callable(getattr(mod, "serve", None)):
            log("plugin: %s has no serve(); skipped" % name)
            continue
        if name not in known:
            route = getattr(mod, "PSIPROXY_ROUTE", None)
            if not (isinstance(route, str) and route.startswith("/_") and route.endswith("/")):
                log("plugin: %s has no PSIPROXY_ROUTE; skipped" % name)
                continue
            PLUGIN_ROUTES.append({"module": name, "prefix": route, "anywhere": False,
                                  "local_only": bool(getattr(mod, "PSIPROXY_LOCAL_ONLY", True)),
                                  "set_logger": True})
        PLUGINS[name] = mod
        try:
            if hasattr(mod, "set_logger"):
                mod.set_logger(log)
            if hasattr(mod, "start"):
                mod.start()
        except Exception as e:
            log("plugin: %s: start failed (%s)" % (name, e))
        log("plugin: %s loaded" % name)
    return sorted(PLUGINS)


# ===========================================================================
#  Upstream destination checks (no fetching of local or private addresses)
# ===========================================================================

class BlockedDestination(OSError):
    """An upstream address the proxy will not connect to."""


def _ip_refused(ip):
    """Why ip may not be fetched, or None if it may. Anything not globally
    routable is refused: loopback, RFC 1918, link-local (incl. cloud metadata),
    CGNAT 100.64/10, ULA, unspecified, multicast, reserved, documentation."""
    try:
        a = ipaddress.ip_address(ip.split("%", 1)[0])
    except ValueError:
        return "unparsable address"
    if isinstance(a, ipaddress.IPv6Address):
        for inner in (a.ipv4_mapped, a.sixtofour,
                      a.teredo[1] if a.teredo else None):
            if inner is not None:
                r = _ip_refused(str(inner))
                if r:
                    return r
    if a.is_multicast:
        return "multicast address"
    if not a.is_global:
        return "local or private address"
    return None


def _guarded_connect(host, port, timeout, source_address=None):
    """Resolve host, check every address, and connect to a checked address
    (the one connected to is the one checked, so DNS rebinding between the
    check and the connect cannot redirect it)."""
    infos = socket.getaddrinfo(host, port, 0, socket.SOCK_STREAM)
    blocked = None
    err = None
    for family, stype, proto, _, sa in infos:
        if not ALLOW_PRIVATE:
            why = _ip_refused(sa[0])
            if why:
                blocked = blocked or BlockedDestination(
                    "%s is %s (%s)" % (host, sa[0], why))
                continue
        s = None
        try:
            s = socket.socket(family, stype, proto)
            if timeout is not None and timeout is not socket._GLOBAL_DEFAULT_TIMEOUT:
                s.settimeout(timeout)
            if source_address:
                s.bind(source_address)
            s.connect(sa)
            return s
        except OSError as e:
            if s is not None:
                s.close()
            err = e
    raise err or blocked or OSError("no address for %s" % host)


class _GuardedHTTPConnection(http.client.HTTPConnection):
    def connect(self):
        self.sock = _guarded_connect(self.host, self.port, self.timeout, self.source_address)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)


class _GuardedHTTPSConnection(http.client.HTTPSConnection):
    def connect(self):
        sock = _guarded_connect(self.host, self.port, self.timeout, self.source_address)
        self.sock = self._context.wrap_socket(sock, server_hostname=self.host)


class _GuardedHTTPHandler(urllib.request.HTTPHandler):
    def http_open(self, req):
        return self.do_open(_GuardedHTTPConnection, req)


class _GuardedHTTPSHandler(urllib.request.HTTPSHandler):
    def https_open(self, req):
        return self.do_open(_GuardedHTTPSConnection, req, context=self._context)


def _build_opener():
    """urllib opener whose every connection, redirects included, goes
    through _guarded_connect. Environment proxies (http_proxy) are ignored."""
    ctx = ssl.create_default_context()
    return urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                       _GuardedHTTPHandler(),
                                       _GuardedHTTPSHandler(context=ctx))


def _blocked_reason(e):
    """The refusal text if exception e is (or wraps) a BlockedDestination."""
    for x in (e, getattr(e, "reason", None), getattr(e, "__cause__", None),
              getattr(e, "__context__", None)):
        if isinstance(x, BlockedDestination):
            return str(x)
    return None


def _url_host_refused(url):
    """For the headless browser's best-effort filter: why url's host may not be
    fetched, or None. Resolves the name (the browser resolves it again)."""
    from urllib.parse import urlsplit
    try:
        sp = urlsplit(url)
        if sp.scheme not in ("http", "https", "ws", "wss"):
            return None if sp.scheme in ("data", "blob", "about") else "scheme " + sp.scheme
        host = sp.hostname
        if not host:
            return "no host"
        for *_, sa in socket.getaddrinfo(host, sp.port or 443, 0, socket.SOCK_STREAM):
            why = _ip_refused(sa[0])
            if why:
                return why
    except Exception as e:
        return "lookup failed (%s)" % e
    return None


def _parse_cookie(cookie_header, name):
    """Return the value of one cookie from a Cookie header, or None."""
    if not cookie_header:
        return None
    for part in cookie_header.split(";"):
        k, _, v = part.strip().partition("=")
        if k.strip() == name:
            return v.strip() or None
    return None


def _target_path_query(target):
    """Split a request target into (path, query), for absolute or relative URIs."""
    low = target.lower()
    if low.startswith("http://") or low.startswith("https://"):
        try:
            from urllib.parse import urlsplit
            sp = urlsplit(target)
            return sp.path, sp.query
        except Exception:
            return "", ""
    if target.startswith("/"):
        p, _, q = target.partition("?")
        return p, q
    return "", ""


def _simplify(body_bytes, base_url, mode, log):
    """Pipe HTML through the psisimp binary (the firmware's HtmlSimplifier).
    mode is "keep" (pictures kept) or "text" (reading view). Falls back to the
    raw body if psisimp is missing or fails (as it is on this box)."""
    try:
        p = subprocess.run([PSISIMP, mode, base_url or ""],
                           input=body_bytes, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=60)
        if p.returncode == 0:
            return p.stdout
        log("proxy: psisimp exit %d; sent raw HTML" % p.returncode)
    except FileNotFoundError:
        log("proxy: psisimp not found at %s; sent raw HTML" % PSISIMP)
    except Exception as e:
        log("proxy: psisimp failed (%s); sent raw HTML" % e)
    return body_bytes


def _esc_text(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def _esc_attr(s):
    return s.replace("&", "&amp;").replace('"', "&quot;")


def _fmt_date(iso):
    """trafilatura dates are ISO YYYY-MM-DD; show '4 Oct 2026'. Raw on failure."""
    try:
        import datetime
        d = datetime.datetime.strptime(str(iso)[:10], "%Y-%m-%d")
        return "%d %s %d" % (d.day, d.strftime("%b"), d.year)
    except Exception:
        return _esc_text(str(iso))


def _build_article(body_bytes, final_url, keep_images, keep_links, width,
                   footer_label, log):
    """Build a clean single-column article page: a centred column (<table width=N>,
    which Links honours since it has no CSS), a plain big <h1> title (never a
    link), a <font size="2"> byline (author / date / source / reading time), the
    article body, and a source footer. keep_images and keep_links decide whether
    the body carries pictures and in-text links. Returns the HTML, or None to
    fall back (thin or failed extraction)."""
    if trafilatura is None and _readability is None:
        return None
    try:
        html = body_bytes.decode("utf-8", "replace")
    except Exception:
        return None
    meta = {}
    if trafilatura is not None:
        try:
            doc = trafilatura.bare_extraction(html, url=final_url, with_metadata=True)
            if hasattr(doc, "as_dict"):
                meta = doc.as_dict()
            elif isinstance(doc, dict):
                meta = doc
        except Exception as e:
            log("article: metadata failed (%s)" % e)

    # BODY. trafilatura is text-first and drops content images; for image-heavy
    # articles (galleries/listicles) readability keeps them in order. When images
    # are wanted, take whichever body has MORE images; otherwise keep trafilatura.
    traf_body = None
    if trafilatura is not None:
        try:
            traf_body = trafilatura.extract(
                html, url=final_url, output_format="html",
                include_links=keep_links, include_images=keep_images, include_tables=False,
                include_comments=False, include_formatting=True, with_metadata=False)
        except Exception as e:
            log("article: trafilatura extract failed (%s)" % e)
    body_html = traf_body
    chosen = "trafilatura"
    if keep_images and _readability is not None:
        read_body = None
        try:
            read_body = _readability(html).summary(html_partial=True)
        except Exception as e:
            log("article: readability failed (%s)" % e)
        if read_body:
            # trafilatura marks pictures as <graphic> (converted to <img> later),
            # so count both when comparing
            ti = len(re.findall(r"<(?:img|graphic)", traf_body or "", re.I))
            ri = len(re.findall(r"<img", read_body, re.I))
            rtext = len(re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", read_body)).strip())
            if ri > ti and ri > 0 and rtext > 200:
                body_html = _unhide_clean(read_body, log)   # R1 on the readability body too
                chosen = "readability"
            log("article: body extractor=%s (trafilatura imgs=%d, readability imgs=%d)"
                % (chosen, ti, ri))
    if not body_html and _readability is not None:
        try:
            body_html = _readability(html).summary(html_partial=True)
            chosen = "readability"
        except Exception:
            body_html = None
    if not body_html:
        log("article: extract failed; whole page")
        return None

    def g(k):
        v = meta.get(k) if isinstance(meta, dict) else None
        return v.strip() if isinstance(v, str) and v.strip() else None

    text = g("text")
    body_text = re.sub(r"\s+", " ", re.sub(r"<[^>]+>", " ", body_html)).strip()
    if chosen == "readability" or not text:
        text = body_text
    words = len(text.split())
    imgs_in_body = len(re.findall(r"<img", body_html, re.I))
    # a gallery/listicle can be word-light but is still a real article if it has
    # several pictures - don't demote it to the index pipeline (which drops images)
    if words < 120 and imgs_in_body < 3:
        log("article: thin (%d words, %d imgs); whole page" % (words, imgs_in_body))
        return None
    mins = max(1, round(words / 200))

    if keep_images:
        # trafilatura marks pictures as <graphic src alt/>; rename to <img> so
        # Links shows them (and our pipeline transcodes them to GIF)
        body_html = body_html.replace("<graphic", "<img").replace("</graphic>", "")
    # strip any stray document wrappers; emit the body as a fragment in the table
    body_html = re.sub(r"(?is)</?(?:html|head|body)[^>]*>", "", body_html).strip()
    # trafilatura often repeats the title as the body's first heading (sometimes
    # wrapped in <a>); drop it so the title shows once, plain, never underlined
    body_html = re.sub(r"(?is)^\s*<h1[^>]*>.*?</h1>\s*", "", body_html).strip()
    # FrogFind-style: reduce the body to a hard allowlist of the oldest tags
    body_html = _minimize_html(body_html, keep_images, keep_links, log)

    title = g("title")
    if not title and _readability is not None:
        try:
            title = (_readability(html).short_title() or "").strip() or None
        except Exception:
            title = None
    title = title or "Untitled"
    author = g("author")
    date = g("date")
    sitename = g("sitename") or g("hostname")
    parts = []
    if author:
        parts.append("By %s" % _esc_text(author))
    if date:
        parts.append(_fmt_date(date))
    if sitename:
        parts.append(_esc_text(sitename))
    parts.append("%d min read (%s words)" % (mins, format(words, ",")))
    byline = " &middot; ".join(parts)

    footer = "Source: %s" % _esc_text(final_url)
    if footer_label:
        footer += " &middot; " + footer_label
    out = ['<base href="%s">' % _esc_attr(final_url),
           '<table width="%d" align="center"><tr><td>' % width,
           '<h1>%s</h1>' % _esc_text(title)]      # title is plain text, never a link
    if byline:
        out.append('<font size="2">%s</font>' % byline)
    out.append('<hr>')
    out.append(body_html)
    out.append('<hr>')
    out.append('<font size="2">%s</font>' % footer)
    out.append('</td></tr></table>')
    log("article: %.50s (%d words, %d min, %s)"
        % (title, words, mins, "reader" if footer_label else "normal"))
    return "\n".join(out)


def _reader_page(body_bytes, final_url, log):
    """Reading view: the clean article skeleton with pictures off and in-text
    links flattened to plain text."""
    return _build_article(body_bytes, final_url, keep_images=False, keep_links=False,
                          width=PROXY_READER_WIDTH, footer_label="Reader view", log=log)


def _normal_article_page(body_bytes, final_url, log):
    """Normal mode, article pages: the same clean skeleton, but with pictures and
    in-text links kept (so the title is a heading, body links are underlined)."""
    return _build_article(body_bytes, final_url, keep_images=True, keep_links=True,
                          width=PROXY_ARTICLE_WIDTH, footer_label=None, log=log)


def _looks_like_index(url):
    """A site root or a one-word section index (/, /news, /sport): keep the whole
    page so all its headline links survive, rather than extracting one story."""
    try:
        from urllib.parse import urlsplit
        segs = [s for s in urlsplit(url).path.split("/") if s]
    except Exception:
        return False
    if not segs:
        return True
    return len(segs) == 1 and segs[0].isalpha() and len(segs[0]) <= 12


_EMPTY_BLOCK_RE = re.compile(
    r"<(div|p|span|li|ul|ol|section|article|h[1-6]|blockquote)>\s*</\1>", re.I)


def _tidy_html(data):
    """Light post-simplify cleanup so Links does not render big vertical gaps:
    drop empty block pairs and collapse runs of <br> and blank lines."""
    try:
        s = data.decode("utf-8", "replace")
    except Exception:
        return data
    for _ in range(4):
        new = _EMPTY_BLOCK_RE.sub("", s)
        if new == s:
            break
        s = new
    s = re.sub(r"(?:\s*<br\s*/?>\s*){3,}", "<br><br>", s, flags=re.I)
    s = re.sub(r"\n{3,}", "\n\n", s)
    return s.encode("utf-8", "replace")


# FrogFind-style hard allowlist: the oldest presentational tags Links renders
_MINIMIZE_ALLOWED = {"p", "br", "h1", "h2", "h3", "h4", "h5", "h6", "ul", "ol",
                     "li", "b", "i", "blockquote", "small", "hr"}

_LAZY_SRC_ATTRS = ("data-src", "data-original", "data-lazy-src", "data-lazy", "data-url")


def _img_placeholder(src):
    """True if an <img> src is missing, a data: URI, or an obvious placeholder."""
    s = (src or "").strip().lower()
    if not s or s.startswith("data:"):
        return True
    return ("1x1" in s or "blank." in s or "placeholder" in s or "spacer" in s
            or "transparent" in s)


def _best_srcset(ss):
    """Pick the best URL from a srcset: the largest-width candidate, or the last
    one when there are no widths. Robust to malformed input."""
    best = None
    best_w = -1
    last = None
    try:
        for part in ss.split(","):
            toks = part.split()
            if not toks:
                continue
            url = toks[0].strip()
            if not url:
                continue
            last = url
            w = -1
            if len(toks) > 1:
                d = toks[1].strip().lower()
                m = re.match(r"(\d+)w$", d)
                if m:
                    w = int(m.group(1))
                else:
                    m = re.match(r"([\d.]+)x$", d)
                    if m:
                        w = int(float(m.group(1)) * 1000)   # density -> pseudo-width
            if w > best_w:
                best_w = w
                best = url
    except Exception:
        return last
    return best or last


def _resolve_img_srcs(root):
    """Fill each <img>'s src with a real http(s) URL from srcset / lazy data-*
    attrs when the current src is missing or a placeholder."""
    for img in root.iter("img"):
        if not isinstance(img.tag, str):
            continue
        try:
            if _img_placeholder(img.get("src")):
                cand = None
                for k in _LAZY_SRC_ATTRS:
                    v = (img.get(k) or "").strip()
                    if v.lower().startswith(("http://", "https://")):
                        cand = v
                        break
                if not cand:
                    ss = img.get("srcset") or img.get("data-srcset")
                    if ss:
                        cand = _best_srcset(ss)
                if cand:
                    img.set("src", cand)
        except Exception:
            continue


_DROP_HEAD_TAGS = {"h1", "h2", "h3", "h4"}
# tags that count as real content under a heading
_SECTION_CONTENT_TAGS = {"a", "li", "p", "img", "ul", "ol", "blockquote", "table",
                         "pre", "dl", "figure", "form", "input", "select", "textarea",
                         "button"}


def _drop_empty_headings(root):
    """Remove headings whose section has no real content - i.e. no link, list,
    paragraph, image or non-whitespace text between the heading and the next
    heading (or the end). JS picker/scaffold pages (e.g. BBC Weather) emit a wall
    of section labels whose content is painted by JS and so arrives empty; those
    bold non-clickable titles bury the few real links. Conservative: a heading is
    dropped only when its section is genuinely empty. Operates on the lxml tree,
    in document order, so nesting is handled. Never raises."""
    try:
        import bisect
        nodes = [el for el in root.iter() if isinstance(el.tag, str)]
        pos = {id(el): i for i, el in enumerate(nodes)}
        size = {id(el): sum(1 for _ in el.iter()) for el in nodes}   # subtree length (pre-order is contiguous)
        head_pos = sorted(pos[id(el)] for el in nodes if el.tag.lower() in _DROP_HEAD_TAGS)
        if not head_pos:
            return
        to_drop = []
        for el in nodes:
            if el.tag.lower() not in _DROP_HEAD_TAGS:
                continue
            p = pos[id(el)]
            start = p + size[id(el)]                 # first node after this heading's own subtree
            idx = bisect.bisect_right(head_pos, p)
            nxt = head_pos[idx] if idx < len(head_pos) else len(nodes)
            meaningful = bool((el.tail or "").strip())
            for el2 in nodes[start:nxt]:
                if el2.tag.lower() in _SECTION_CONTENT_TAGS or (el2.text or "").strip() \
                        or (el2.tail or "").strip():
                    meaningful = True
                    break
            if not meaningful:
                to_drop.append(el)
        for el in to_drop:
            parent = el.getparent()
            if parent is not None:
                parent.remove(el)
    except Exception:
        return


# extra tags/attrs kept only on the interactive path, so the proxy forms and
# /_act controls survive the slimmer
_INTERACTIVE_TAGS = {"form", "input", "select", "option", "textarea", "label", "a"}
_INTERACTIVE_KEEP_ATTRS = {
    "form": ("action", "method"),
    "input": ("type", "name", "value", "checked", "placeholder"),
    "select": ("name",),
    "option": ("value", "selected"),
    "textarea": ("name",),
}


def _minimize_html(body_html, keep_images, keep_links, log, interactive=False):
    """Reduce extracted article body to a hard tag allowlist (FrogFind's trick):
    demote strong/em to b/i, unwrap every other tag keeping its text, and strip
    all attributes but href on <a> and src/alt on <img>. With interactive=True
    the allowlist also keeps form/input/select/option/textarea/label and their
    functional attributes, so the proxy forms and /_act links are not stripped.
    Returns minimized HTML, or the input unchanged on any failure."""
    if not body_html or not body_html.strip():
        return body_html
    try:
        import lxml.html as LH
        from lxml import etree
    except Exception as e:
        log("minimize: lxml missing (%s); body left as-is" % e)
        return body_html
    try:
        allowed = set(_MINIMIZE_ALLOWED)
        if keep_links:
            allowed.add("a")
        if keep_images:
            allowed.add("img")
        if interactive:
            allowed |= _INTERACTIVE_TAGS
        root = LH.fragment_fromstring(body_html, create_parent="div")
        etree.strip_tags(root, etree.Comment)
        # demote the newer inline tags to the oldest ones
        for el in root.iter():
            if isinstance(el.tag, str):
                t = el.tag.lower()
                if t == "strong":
                    el.tag = "b"
                elif t == "em":
                    el.tag = "i"
        # unwrap every tag not in the allowlist (its text/children are kept)
        present = set(el.tag.lower() for el in root.iter() if isinstance(el.tag, str))
        drop = set(t for t in present if t not in allowed)
        # strip_tags never deletes the element it is called on, so the wrapper
        # <div> survives even though "div" is in the drop set; inner divs unwrap
        if not keep_images:
            drop.add("img")
        if not keep_links:
            drop.add("a")
        if drop:
            etree.strip_tags(root, *drop)
        if keep_images:
            _resolve_img_srcs(root)             # srcset / lazy data-* -> a real src
        # strip every attribute but href on <a> and src/alt on <img>
        for el in root.iter():
            if not isinstance(el.tag, str):
                continue
            t = el.tag.lower()
            keep = ("href", "name") if t == "a" else ("src", "alt") if t == "img" else ()
            if interactive and t in _INTERACTIVE_KEEP_ATTRS:
                keep = _INTERACTIVE_KEEP_ATTRS[t]
            for name in list(el.attrib):
                if name.lower() not in keep:
                    del el.attrib[name]
        # drop section-label headings that have no content under them (generic;
        # safe for articles, whose headings do have content)
        _drop_empty_headings(root)
        # serialize the wrapper's inner HTML (drop the wrapper div itself)
        parts = [root.text or ""]
        for child in root:
            parts.append(etree.tostring(child, encoding="unicode", method="html"))
        out = "".join(parts)
        return _tidy_html(out.encode("utf-8", "replace")).decode("utf-8", "replace")
    except Exception as e:
        log("minimize: failed (%s); body left as-is" % e)
        return body_html


# Standalone metadata-noise fragments common on news indexes (BBC etc.). Each is
# removed only when it is the WHOLE list-item or a whole standalone text node.
_INDEX_NOISE = [
    r"Attribution[A-Za-z]*",
    r"Posted",
    r"\d+\s*(?:second|minute|hour|day|week|month|year)s?\s+ago(?:\s*\d+\s*(?:s|sec|min|hr|hrs|h|d|w|mo|yr))?",
    r"\d+\s*(?:sec|min|hr|hrs|mo|yr)\b",
]
# separators that may sit between stacked noise fragments
_NOISE_SEP = r"(?:\s|&middot;|&nbsp;|[·,|/–-])*"


def _strip_index_noise(html):
    """Remove per-headline metadata cruft (Attribution / Posted / relative time)
    from an index page, but only where the fragment is the whole item or a whole
    standalone text node, so headlines and links are never touched."""
    try:
        noise = "(?:%s)" % "|".join(_INDEX_NOISE)
        combo = r"%s(?:%s%s)*" % (noise, _NOISE_SEP, noise)
        # whole <li>/<p> that is only noise (one or more stacked fragments)
        html = re.sub(r"(?is)<(li|p)>\s*%s\s*</\1>" % combo, "", html)
        # standalone text nodes between tags that are only noise
        pat = re.compile(r"(?i)(>)\s*%s\s*(<)" % combo)
        for _ in range(3):
            new = pat.sub(r"\1\2", html)
            if new == html:
                break
            html = new
        return html
    except Exception:
        return html


def _index_page(body_bytes, final_url, log):
    """Tidy an index/homepage (the non-article fallback): simplify the whole page,
    reduce it to the tag allowlist, strip per-item metadata noise, and wrap it in
    the same narrow centred column as articles. Keeps the headline links."""
    simplified = _simplify(body_bytes, final_url, "keep", log)
    body = _minimize_html(simplified.decode("utf-8", "replace"), True, True, log)
    body = _strip_index_noise(body)
    body = _tidy_html(body.encode("utf-8", "replace")).decode("utf-8", "replace")
    out = ['<base href="%s">' % _esc_attr(final_url),
           '<table width="%d" align="center"><tr><td>' % PROXY_ARTICLE_WIDTH,
           body,
           '</td></tr></table>']
    return "\n".join(out)


def _unhide_clean(html, log):
    """R1: delete nodes the page hid with CSS/ARIA (which a CSS-less browser would
    wrongly reveal - the generic source of Attribution/Posted/time noise), and
    drop script/style/noscript/svg/iframe/form. Returns cleaned HTML, or the
    input unchanged on any failure. Applied before trafilatura/simplify."""
    if not html or not html.strip():
        return html
    try:
        import lxml.html as LH
        from lxml import etree
    except Exception as e:
        log("unhide: lxml missing (%s)" % e)
        return html
    try:
        root = LH.fromstring(html)
    except Exception:
        return html
    try:
        etree.strip_elements(root, "script", "style", "noscript", "svg", "iframe",
                             "form", with_tail=False)
        classpat = re.compile(
            r"(?i)(?:^|[^a-z])(" +
            "|".join(re.escape(c) for c in PROXY_HIDDEN_CLASSES) +
            r")(?:[^a-z]|$)")
        remove = []
        for el in root.iter():
            if not isinstance(el.tag, str):
                continue
            cls = el.get("class") or ""
            hidden = False
            if cls and classpat.search(cls):
                hidden = True
            elif (el.get("aria-hidden") or "").lower() == "true":
                if len(el.text_content() or "") < PROXY_ARIA_HIDDEN_MAXLEN:
                    hidden = True
            else:
                style = (el.get("style") or "").lower().replace(" ", "")
                if style and ("display:none" in style or "visibility:hidden" in style
                              or ("position:absolute" in style and "width:1px" in style
                                  and "height:1px" in style)
                              or "clip:rect(0" in style or "clip-path:inset(50%" in style):
                    hidden = True
            if hidden:
                remove.append(el)
        for el in remove:
            p = el.getparent()
            if p is not None:
                p.remove(el)
        # R1b: collapse responsive/fragmented duplicate text using aria-label.
        # When an element gives a short aria-label (the one true accessible name)
        # and its visible text is just that label duplicated by media-query
        # variants a CSS-less browser reveals twice (e.g. BBC's "Wednesday 7th
        # October" + "Wed 7th", "Light RainLight Rain"), replace the subtree with
        # the label. Guards keep it from eating containers (the hour-by-hour <ol>)
        # or richer text that merely starts with the label.
        def _norm(s):
            return re.sub(r"[^a-z0-9]", "", (s or "").lower())
        for el in list(root.iter()):
            if not isinstance(el.tag, str):
                continue
            label = (el.get("aria-label") or "").strip()
            if not label or len(label) > 80:
                continue
            lc = _norm(label)
            if not lc or len(lc) < 3:
                continue
            vc = _norm(el.text_content() or "")
            if lc != vc and lc in vc and len(vc) <= int(len(lc) * 2.2):
                for c in list(el):
                    el.remove(c)
                el.text = label
        return LH.tostring(root, encoding="unicode")
    except Exception as e:
        log("unhide: failed (%s); html left as-is" % e)
        return html


def _is_story_href(href):
    """Generic 'looks like a permalink' test (no site-specific paths)."""
    try:
        from urllib.parse import urlsplit
        p = urlsplit(href or "").path
    except Exception:
        return False
    if not p or p == "/":
        return False
    segs = [s for s in p.split("/") if s]
    if not segs:
        return False
    if re.search(r"/(article|story|stories|news|av|video|videos|watch|p)/", p, re.I):
        return True
    if re.search(r"/\d{4}/", p):            # a year in the path (dated permalinks)
        return True
    last = segs[-1]
    if len(segs) >= 2 and (len(last) >= 12 or "-" in last or re.search(r"\d", last)):
        return True
    return False


def _classify_index(html, log):
    """True if the page is an index/nav list (route to the index pipeline), False
    if it is a single article (route to trafilatura). Structural, not site-based."""
    try:
        import lxml.html as LH
        root = LH.fromstring(html)
    except Exception:
        return False
    try:
        total = len(re.sub(r"\s+", " ", root.text_content() or "").strip())
        if total <= 0:
            return False
        linktext = 0
        story = 0
        for a in root.iter("a"):
            linktext += len(re.sub(r"\s+", " ", a.text_content() or "").strip())
            if _is_story_href(a.get("href") or ""):
                story += 1
        ratio = linktext / total
        prose_chars = 0
        prose_p = 0
        for p in root.iter("p"):
            t = re.sub(r"\s+", " ", p.text_content() or "").strip()
            if len(t) > 60:
                lt = sum(len((a.text_content() or "").strip()) for a in p.iter("a"))
                if lt < 0.5 * len(t):
                    prose_chars += len(t)
                    prose_p += 1
        # link text dominating is the clean discriminator (an index is mostly
        # link text; an article is mostly prose). Summary-heavy indexes (BBC) keep
        # many prose paragraphs, so prose count alone cannot be trusted.
        is_index = (ratio > PROXY_INDEX_LINKRATIO_HARD
                    or (ratio > PROXY_INDEX_LINKRATIO and story > PROXY_INDEX_MIN_STORYLINKS)
                    or (ratio > PROXY_INDEX_LINKRATIO and prose_chars < PROXY_INDEX_PROSE_FRAC * total)
                    or (story > PROXY_INDEX_MIN_STORYLINKS and prose_p < PROXY_INDEX_MAX_PROSE_P))
        log("classify: linkratio=%.2f prose=%d/%d story=%d prose_p=%d -> %s"
            % (ratio, prose_chars, total, story, prose_p, "index" if is_index else "article"))
        return is_index
    except Exception as e:
        log("classify: failed (%s); article" % e)
        return False


def _index_page2(html, final_url, log):
    """Custom DOM index pipeline: dedup links, split nav from headlines, build a
    sectioned headline list, drop images, collapse the menu (if enabled), and wrap
    in the narrow column. Keeps hrefs (trafilatura would drop them)."""
    import lxml.html as LH
    root = LH.fromstring(html)
    try:
        root.make_links_absolute(final_url)
    except Exception:
        pass

    def norm(a):
        h = (a.get("href") or "").strip()
        if not h or h.startswith("#") or h.lower().startswith(("javascript:", "mailto:", "tel:")):
            return ""
        return h.split("#")[0].rstrip("/").lower()

    def atext(a):
        return re.sub(r"\s+", " ", a.text_content() or "").strip()

    def in_heading(a):
        p = a.getparent()
        while p is not None:
            if isinstance(p.tag, str) and p.tag.lower() in _HEADING_TAGS:
                return True
            p = p.getparent()
        return False

    anchors = [a for a in root.iter("a") if isinstance(a.tag, str)]
    href_count = {}
    for a in anchors:
        h = norm(a)
        if h:
            href_count[h] = href_count.get(h, 0) + 1

    navdrop = re.compile(PROXY_NAV_DROP, re.I)
    navlex = re.compile(PROXY_NAV_LEXICON, re.I)
    decision = {}
    for a in anchors:
        t = atext(a)
        h = norm(a)
        if not t or not h:
            decision[id(a)] = "drop"
        elif navdrop.match(t):
            decision[id(a)] = "drop"
        elif len(t.split()) >= PROXY_HEADLINE_MINWORDS or in_heading(a):
            decision[id(a)] = "content"
        else:
            decision[id(a)] = "nav"

    collapse_nav = STATE.collapse_nav
    body = []
    ul_open = [False]
    pending_nav = []
    nav_bottom = []
    seen_content = set()
    seen_nav = set()

    def close_ul():
        if ul_open[0]:
            body.append("</ul>")
            ul_open[0] = False

    def flush_nav_inline():
        if pending_nav:
            close_ul()
            strip = " &middot; ".join('<a href="%s">%s</a>' % (_esc_attr(h), _esc_text(t))
                                      for t, h in pending_nav)
            body.append("<p>%s</p>" % strip)
            del pending_nav[:]

    for el in root.iter():
        if not isinstance(el.tag, str):
            continue
        tag = el.tag.lower()
        if tag in _HEADING_TAGS:
            if any(decision.get(id(x)) == "content" for x in el.iter("a")):
                continue                        # heading wraps a headline link: emitted as the link
            txt = re.sub(r"\s+", " ", el.text_content() or "").strip()
            if txt and len(txt.split()) <= 8:
                flush_nav_inline()
                close_ul()
                body.append("<h2>%s</h2>" % _esc_text(txt))
        elif tag == "a":
            d = decision.get(id(el))
            t = atext(el)
            h = norm(el)
            if not t or not h:
                continue
            href = el.get("href") or h
            if d == "content":
                if h in seen_content:
                    continue
                seen_content.add(h)
                flush_nav_inline()
                if not ul_open[0]:
                    body.append("<ul>")
                    ul_open[0] = True
                body.append('<li><a href="%s">%s</a></li>' % (_esc_attr(href), _esc_text(t)))
            elif d == "nav":
                if h in seen_nav:
                    continue
                seen_nav.add(h)
                if collapse_nav:
                    nav_bottom.append((t, href))
                else:
                    pending_nav.append((t, href))
    flush_nav_inline()
    close_ul()
    body_html = "\n".join(body)

    top = ""
    bottom = ""
    if collapse_nav and nav_bottom:
        top = '<p><a href="#psi-nav">[ Menu ]</a></p>'
        strip = " &middot; ".join('<a href="%s">%s</a>' % (_esc_attr(h), _esc_text(t))
                                  for t, h in nav_bottom)
        bottom = '<hr><a name="psi-nav"></a><h2>Menu</h2><p>%s</p>' % strip

    assembled = "\n".join(x for x in (top, body_html, bottom) if x)
    assembled = _strip_index_noise(assembled)                       # R7 safety net
    assembled = _minimize_html(assembled, False, True, log)         # allowlist; no images, keep links
    assembled = _tidy_html(assembled.encode("utf-8", "replace")).decode("utf-8", "replace")
    log("index2: %d headlines, %d nav links" % (len(seen_content), len(seen_nav)))
    out = ['<base href="%s">' % _esc_attr(final_url),
           '<table width="%d" align="center"><tr><td>' % PROXY_ARTICLE_WIDTH,
           assembled,
           '</td></tr></table>']
    return "\n".join(out)


def _safe_index2(cleaned, final_url, log):
    try:
        r = _index_page2(cleaned, final_url, log)
        if r:
            return r
    except Exception as e:
        log("index2: failed (%s); old index path" % e)
    return _index_page(cleaned.encode("utf-8", "replace"), final_url, log)


def _render_html_page(cleaned, final_url, log):
    """Route cleaned HTML: reading view -> reader; else classify index vs article.
    Article extraction that comes back thin falls back to the index pipeline."""
    enc = cleaned.encode("utf-8", "replace")
    if STATE.reading:
        page = _reader_page(enc, final_url, log)
        return page if page is not None else _safe_index2(cleaned, final_url, log)
    if _classify_index(cleaned, log):
        return _safe_index2(cleaned, final_url, log)
    page = _normal_article_page(enc, final_url, log)
    return page if page is not None else _safe_index2(cleaned, final_url, log)


def _transcode_image(body_bytes, log):
    """Decode any image, make it a small 16-grey GIF for Links. Returns the
    GIF bytes, or None to fall back to the original (Pillow missing, animated,
    unsupported, corrupt...)."""
    if Image is None:
        return None
    try:
        im = Image.open(io.BytesIO(body_bytes))
        try:
            im.seek(0)                       # multi-frame (animated GIF/WebP): first frame
        except Exception:
            pass
        im = im.convert("L")                 # greyscale
        if im.width > PROXY_IMG_MAXW:
            h = max(1, int(im.height * PROXY_IMG_MAXW / im.width))
            im = im.resize((PROXY_IMG_MAXW, h), Image.LANCZOS)
        im = ImageOps.posterize(im, 4)       # 16 grey levels
        buf = io.BytesIO()
        im.save(buf, "GIF")
        return buf.getvalue()
    except Exception as e:
        log("proxy: image not transcoded (%s); sent original" % e)
        return None


# ===========================================================================
#  Server-side JavaScript rendering (Stage 1: render-only)
#
#  Some pages arrive as a JS shell with little real content (an empty SPA
#  container, or a scaffold whose body is painted by client-side JS). For those
#  we re-fetch the page with a headless Chromium (Playwright), wait for it to
#  settle, and feed the post-JS DOM back into the normal slimming pipeline.
#
#  Playwright's sync API must be driven from a single thread, so one long-lived
#  worker thread owns the browser and processes render jobs off a queue; the
#  proxy worker threads hand it a URL and block for the result. The browser and
#  one context stay warm and are relaunched on crash. If Playwright or Chromium
#  is unavailable, rendering is simply disabled (the proxy never breaks).
# ===========================================================================

_BODY_RE = re.compile(r"(?is)<body[^>]*>(.*)</body>")
_SCRIPT_STYLE_RE = re.compile(r"(?is)<(script|style|noscript|template)[^>]*>.*?</\1>")
_TAG_RE = re.compile(r"<[^>]+>")
_WS_RE = re.compile(r"\s+")
_EMPTY_ROOT_RE = re.compile(
    r"(?is)<(div|main|section)[^>]*\bid=[\"'](root|app|__next|__nuxt|svelte)[\"'][^>]*>\s*</\1>")
_SCRIPT_COUNT_RE = re.compile(r"(?i)<script\b")


def _visible_text(html):
    """Visible text of an HTML string: drop script/style, strip tags, squeeze ws."""
    if not html:
        return ""
    s = _SCRIPT_STYLE_RE.sub(" ", html)
    s = _TAG_RE.sub(" ", s)
    return _WS_RE.sub(" ", s).strip()


def _looks_like_js_shell(slim_html, raw_html, log):
    """True if the page we are about to serve looks like a JS shell that rendering
    could fill in. Generic (no site-specific rules):
      1. the SLIMMED output we'd serve has < ~500 chars of visible text, or
      2. the raw page is an empty-SPA shell (empty #root/#app/#__next, little
         body text, many <script> tags), or
      3. orphan headings with almost no link/paragraph content.
    Deliberately conservative so content-rich indexes and articles are NOT
    flagged (they must stay on the fast path)."""
    try:
        out_vis = _visible_text(slim_html)
        if len(out_vis) < JS_SHELL_MIN_VIS:
            log("js-shell: slim output thin (%d chars) -> render" % len(out_vis))
            return True
        # raw-page signals
        m = _BODY_RE.search(raw_html or "")
        body = m.group(1) if m else (raw_html or "")
        raw_vis = _visible_text(body)
        scripts = len(_SCRIPT_COUNT_RE.findall(raw_html or ""))
        if (_EMPTY_ROOT_RE.search(raw_html or "")
                and len(raw_vis) < JS_SHELL_SPA_BODYMAX and scripts >= JS_SHELL_SPA_SCRIPTS):
            log("js-shell: empty SPA root (body %d chars, %d scripts) -> render"
                % (len(raw_vis), scripts))
            return True
        # orphan headings, little else
        heads = len(re.findall(r"(?i)<h[1-3]\b", slim_html))
        links = len(re.findall(r"(?i)<a\b", slim_html))
        if heads >= 3 and links < 3 and len(out_vis) < 600:
            log("js-shell: orphan headings (%d) few links (%d) -> render" % (heads, links))
            return True
    except Exception as e:
        log("js-shell: detect failed (%s); no render" % e)
    return False


class RenderEngine:
    """Owns one warm headless-Chromium browser on a dedicated thread. It serves
    three job kinds, all on that one thread (Playwright's sync API is single-
    threaded):
      * "render"  - Stage 1: render a URL to post-JS HTML on a throwaway page.
      * "session" - Stage 2: render a URL on a LIVE page kept in a session (its
                    own context), holding JS state for later actions.
      * "act"     - Stage 2: replay a click / form submit on a session's live
                    page, then re-render it.
    Any result of None means unavailable/failed, so callers fall back."""

    def __init__(self):
        self._q = queue.Queue()
        self._thread = None
        self._start_lock = threading.Lock()
        self.available = None            # None=unknown, True/False once probed
        self._log = lambda *_: None
        self._pw = None
        self._browser = None
        self._stateless_ctx = None
        self.sessions = {}               # sid -> {context, page, url, created, last}

    def start(self, log):
        with self._start_lock:
            self._log = log
            if self._thread is not None and self._thread.is_alive():
                return
            self._thread = threading.Thread(target=self._worker, daemon=True,
                                            name="psiproxy-render")
            self._thread.start()

    # -- public API (enqueue + block for the worker) ----------------------
    def _submit(self, job, timeout):
        if self.available is False:
            return None
        if self._thread is None or not self._thread.is_alive():
            self.start(self._log)
        holder = queue.Queue(1)
        job["holder"] = holder
        self._q.put(job)
        try:
            return holder.get(timeout=timeout + 6)
        except queue.Empty:
            self._log("js-render: timed out waiting for worker")
            return None

    def render(self, url, timeout=JS_RENDER_TIMEOUT):
        return self._submit({"type": "render", "url": url, "timeout": timeout}, timeout)

    def session_render(self, sid, url, timeout=JS_RENDER_TIMEOUT):
        return self._submit({"type": "session", "sid": sid, "url": url,
                             "timeout": timeout}, timeout)

    def session_act(self, sid, params, timeout=JS_RENDER_TIMEOUT):
        return self._submit({"type": "act", "sid": sid, "params": params,
                             "timeout": timeout}, timeout)

    # -- the one thread that touches Playwright ---------------------------
    def _launch(self):
        self._browser = self._pw.chromium.launch(
            headless=True, args=["--no-sandbox", "--disable-dev-shm-usage", "--disable-gpu"])
        self._stateless_ctx = self._new_context()

    def _new_context(self):
        c = self._browser.new_context(locale=JS_LOCALE, geolocation=JS_GEO,
                                      permissions=["geolocation"])
        try:
            c.add_cookies([
                {"name": "ckns_explicit", "value": "1", "domain": ".bbc.co.uk", "path": "/"},
                {"name": "ckns_policy", "value": "111", "domain": ".bbc.co.uk", "path": "/"},
            ])
        except Exception:
            pass
        return c

    def _relaunch(self):
        for sid in list(self.sessions):
            self._close_session(sid)
        for obj in (self._stateless_ctx, self._browser):
            try:
                if obj:
                    obj.close()
            except Exception:
                pass
        self._stateless_ctx = self._browser = None
        self._launch()

    def _worker(self):
        try:
            from playwright.sync_api import sync_playwright
        except Exception as e:
            self.available = False
            self._log("js-render: Playwright not installed (%s); rendering disabled" % e)
            self._drain_none()
            return
        try:
            self._pw = sync_playwright().start()
            self._launch()
            self.available = True
            self._log("js-render: headless Chromium ready")
        except Exception as e:
            self.available = False
            self._log("js-render: Chromium launch failed (%s); disabled" % e)
            self._drain_none()
            return

        while True:
            job = self._q.get()
            if job is None:
                break
            holder = job.get("holder")
            result = None
            try:
                if not self._browser.is_connected():
                    raise RuntimeError("browser disconnected")
                typ = job["type"]
                if typ == "render":
                    result = self._render_one(self._stateless_ctx, job["url"], job["timeout"])
                elif typ == "session":
                    result = self._do_session(job["sid"], job["url"], job["timeout"])
                elif typ == "act":
                    result = self._do_act(job["sid"], job["params"], job["timeout"])
            except Exception as e:
                self._log("js-render: worker error (%s); relaunching browser" % e)
                try:
                    self._relaunch()
                except Exception as e2:
                    self.available = False
                    self._log("js-render: relaunch failed (%s); disabled" % e2)
                    if holder:
                        holder.put(None)
                    self._drain_none()
                    break
                result = None
            if holder:
                holder.put(result)

    # -- rendering primitives --------------------------------------------
    def _block_heavy(self, page):
        def _route(route):
            try:
                if route.request.resource_type in ("image", "media", "font"):
                    return route.abort()
                # best effort only (redirect hops, WebSockets and the browser's
                # own DNS are not covered): see PSIPROXY_JS_RENDER in README.md
                if not ALLOW_PRIVATE:
                    why = _url_host_refused(route.request.url)
                    if why:
                        self._log("js-render: refused %.80s (%s)" % (route.request.url, why))
                        return route.abort()
            except Exception:
                pass
            return route.continue_()
        try:
            page.route("**/*", _route)
        except Exception:
            pass

    def _settle(self, page, timeout):
        try:
            page.wait_for_load_state("networkidle", timeout=min(7000, int(timeout * 1000)))
        except Exception:
            pass
        try:
            page.wait_for_timeout(JS_SETTLE_MS)
        except Exception:
            pass

    def _render_one(self, context, url, timeout):
        page = context.new_page()
        try:
            self._block_heavy(page)
            page.goto(url, timeout=int(timeout * 1000), wait_until="load")
            self._settle(page, timeout)
            return page.content()
        finally:
            try:
                page.close()
            except Exception:
                pass

    # -- sessions ---------------------------------------------------------
    def _evict(self):
        now = time.time()
        for sid in [s for s, v in list(self.sessions.items()) if now - v["last"] > SESSION_IDLE]:
            self._log("session: evict idle %s" % sid)
            self._close_session(sid)
        while len(self.sessions) > SESSION_CAP:
            sid = min(self.sessions, key=lambda s: self.sessions[s]["last"])
            self._log("session: evict LRU %s" % sid)
            self._close_session(sid)

    def _close_session(self, sid):
        v = self.sessions.pop(sid, None)
        if not v:
            return
        for obj in (v.get("page"), v.get("context")):
            try:
                if obj:
                    obj.close()
            except Exception:
                pass

    def _do_session(self, sid, url, timeout):
        self._evict()
        sess = self.sessions.get(sid)
        if sess is None:
            ctx = self._new_context()
            page = ctx.new_page()
            self._block_heavy(page)
            sess = {"context": ctx, "page": page, "url": None,
                    "created": time.time(), "last": time.time()}
            self.sessions[sid] = sess
            self._log("session: new %s (%d live)" % (sid, len(self.sessions)))
        sess["last"] = time.time()
        page = sess["page"]
        if url and sess.get("url") != url:
            page.goto(url, timeout=int(timeout * 1000), wait_until="load")
            self._settle(page, timeout)
            sess["url"] = page.url
        return self._instrument(page)

    def _do_act(self, sid, params, timeout):
        self._evict()
        sess = self.sessions.get(sid)
        if sess is None:
            return {"expired": True}
        sess["last"] = time.time()
        page = sess["page"]
        eid = params.get("e")
        formid = params.get("form")
        act_tmo = min(4000, int(timeout * 1000))
        try:
            if formid:
                self._replay_form(page, formid, params, timeout)
            elif eid:
                loc = page.locator('[data-psi-id="%s"]' % _css_num(eid)).first
                # dispatch a click event rather than a real mouse click, so controls
                # hidden-until-hover (e.g. todomvc's destroy button) still fire
                try:
                    loc.dispatch_event("click", timeout=act_tmo)
                except Exception:
                    loc.click(timeout=act_tmo, force=True)
        except Exception as e:
            self._log("act: action failed (%s); re-rendering current page" % e)
        self._settle(page, timeout)
        sess["url"] = page.url
        return self._instrument(page)

    def _replay_form(self, page, formid, params, timeout):
        fsel = '[data-psi-id="%s"]' % _css_num(formid)
        kind = params.get("kind", "form")
        tmo = min(4000, int(timeout * 1000))
        if kind == "input":
            # a standalone text/search box: type the value and press Enter
            loc = page.locator(fsel).first
            loc.fill(params.get("v", ""), timeout=tmo)
            loc.press("Enter")
            return
        # a real <form>: set each submitted field by name, then submit
        for k, v in params.items():
            if k in ACT_RESERVED:
                continue
            try:
                field = page.locator('%s [name="%s"]' % (fsel, _css_str(k))).first
                tag = (field.evaluate("e => e.tagName.toLowerCase()"))
                if tag == "select":
                    field.select_option(value=v, timeout=tmo)
                else:
                    ftype = (field.evaluate("e => (e.getAttribute('type')||'').toLowerCase()"))
                    if ftype in ("checkbox", "radio"):
                        field.check(timeout=tmo)
                    else:
                        field.fill(v, timeout=tmo)
            except Exception as e:
                self._log("act: set field %s failed (%s)" % (k, e))
        try:
            page.locator(fsel).first.evaluate(
                "f => { if (f.requestSubmit) f.requestSubmit(); else f.submit(); }")
        except Exception:
            try:
                page.locator('%s [type=submit]' % fsel).first.click(timeout=tmo)
            except Exception as e:
                self._log("act: submit failed (%s)" % e)

    def _instrument(self, page):
        """Tag the live DOM and capture its HTML (with the data-psi-* attrs)."""
        try:
            page.evaluate(JS_INSTRUMENT)
        except Exception as e:
            self._log("act: instrument failed (%s)" % e)
        return {"html": page.content(), "url": page.url}

    def _drain_none(self):
        """Answer any queued/future jobs with None so callers don't block."""
        try:
            while True:
                job = self._q.get_nowait()
                if job is None:
                    continue
                h = job.get("holder") if isinstance(job, dict) else None
                if h:
                    try:
                        h.put(None)
                    except Exception:
                        pass
        except queue.Empty:
            pass


_RENDER = RenderEngine()


def _css_num(v):
    """A digits-only id for a CSS attribute selector (defends the selector)."""
    s = str(v)
    return s if s.isdigit() else re.sub(r"[^0-9]", "", s) or "0"


def _css_str(v):
    """Escape a value for use inside a double-quoted CSS attribute selector."""
    return str(v).replace("\\", "\\\\").replace('"', '\\"')


def _render_js(url, timeout=JS_RENDER_TIMEOUT, log=None):
    """Render url with the warm headless browser and return post-JS HTML, or None
    (unavailable / failed / timed out). Never raises."""
    lg = log or (lambda *_: None)
    if not JS_RENDER_ALLOWED:
        return None
    try:
        _RENDER.start(lg)
        return _RENDER.render(url, timeout)
    except Exception as e:
        lg("js-render: %s" % e)
        return None


def _render_js_session(sid, url, timeout=JS_RENDER_TIMEOUT, log=None):
    """Render url on the session's live page; returns {"html","url"} or None."""
    lg = log or (lambda *_: None)
    if not JS_RENDER_ALLOWED:
        return None
    try:
        _RENDER.start(lg)
        return _RENDER.session_render(sid, url, timeout)
    except Exception as e:
        lg("js-render: session %s" % e)
        return None


def _render_js_act(sid, params, timeout=JS_RENDER_TIMEOUT, log=None):
    """Replay an action on the session's live page; returns {"html","url"},
    {"expired":True}, or None."""
    lg = log or (lambda *_: None)
    if not JS_RENDER_ALLOWED:
        return None
    try:
        _RENDER.start(lg)
        return _RENDER.session_act(sid, params, timeout)
    except Exception as e:
        lg("js-render: act %s" % e)
        return None


def _new_sid():
    """A short random session id."""
    import secrets
    return secrets.token_hex(8)


def _control_label(el):
    """Readable label for an instrumented click control."""
    for getter in (lambda: re.sub(r"\s+", " ", el.text_content() or "").strip(),
                   lambda: (el.get("aria-label") or "").strip(),
                   lambda: (el.get("value") or "").strip(),
                   lambda: (el.get("title") or "").strip(),
                   lambda: (el.get("alt") or "").strip()):
        try:
            v = getter()
            if v:
                return v[:80]
        except Exception:
            pass
    return ""


def _field_label(inp, name):
    for g in (inp.get("aria-label"), inp.get("placeholder"), inp.get("title"), name):
        if g and str(g).strip():
            return str(g).strip()[:40]
    return name


def _mk(tag, **attrs):
    from lxml import etree
    el = etree.Element(tag)
    for k, v in attrs.items():
        if v is not None:
            el.set(k, str(v))
    return el


def _hidden(name, value):
    return _mk("input", type="hidden", name=name, value=value or "")


def _textlabel(text):
    from lxml import etree
    s = etree.Element("label")
    s.text = text
    return s


def _rewrite_click(el, sid):
    """Replace a non-navigational control with a proxy /_act link."""
    eid = el.get("data-psi-id")
    label = _control_label(el) or ("[action %s]" % eid)
    a = _mk("a", href="%s?s=%s&e=%s" % (ACT_PATH, sid, eid))
    a.text = label
    a.tail = el.tail
    p = el.getparent()
    if p is not None:
        p.replace(el, a)


def _rewrite_form(f, sid, fid):
    """Rebuild a real <form> as a GET proxy form posting to /_act."""
    nf = _mk("form", action=ACT_PATH, method="get")
    nf.append(_hidden("s", sid))
    nf.append(_hidden("form", fid))
    kept = 0
    for inp in f.iter():
        if not isinstance(inp.tag, str):
            continue
        t = inp.tag.lower()
        if t not in ("input", "select", "textarea"):
            continue
        name = inp.get("name")
        if not name:
            continue
        itype = (inp.get("type") or "text").lower()
        if itype == "hidden":
            nf.append(_hidden(name, inp.get("value") or ""))
            continue
        if itype in ("submit", "button", "image", "reset", "file", "password"):
            continue   # never proxy passwords/file pickers; submit added below
        lbl = _field_label(inp, name)
        if t == "select":
            s = _mk("select", name=name)
            for opt in inp.iter("option"):
                o = _mk("option", value=opt.get("value"))
                if opt.get("selected") is not None:
                    o.set("selected", "selected")
                o.text = re.sub(r"\s+", " ", opt.text_content() or "").strip()[:60]
                s.append(o)
            nf.append(_textlabel(lbl + ": "))
            nf.append(s)
        elif itype in ("checkbox", "radio"):
            ni = _mk("input", type=itype, name=name, value=inp.get("value"))
            if inp.get("checked") is not None:
                ni.set("checked", "checked")
            nf.append(ni)
            nf.append(_textlabel(" " + lbl))
        else:
            nf.append(_textlabel(lbl + ": "))
            nf.append(_mk("input", type="text", name=name, value=inp.get("value")))
        nf.append(_mk("br"))
        kept += 1
    nf.append(_mk("input", type="submit", value="Submit"))
    nf.tail = f.tail
    p = f.getparent()
    if p is not None:
        p.replace(f, nf)
    return kept


def _rewrite_standalone_input(inp, sid):
    """Wrap a standalone text/search box (not in a <form>) as a proxy form that
    replays as 'type the value and press Enter'."""
    eid = inp.get("data-psi-id")
    nf = _mk("form", action=ACT_PATH, method="get")
    nf.append(_hidden("s", sid))
    nf.append(_hidden("form", eid))
    nf.append(_hidden("kind", "input"))
    lbl = (inp.get("placeholder") or inp.get("aria-label") or inp.get("name") or "Search")
    nf.append(_textlabel(str(lbl)[:40] + ": "))
    nf.append(_mk("input", type="text", name="v"))
    nf.append(_mk("input", type="submit", value="Go"))
    nf.tail = inp.tail
    p = inp.getparent()
    if p is not None:
        p.replace(inp, nf)


def _interactive_slim(instr_html, sid, final_url, log):
    """Turn the instrumented live-page HTML into a slim, interactive page for the
    Psion: navigational links stay; non-nav controls become /_act links; forms and
    standalone inputs become proxy forms. Returns the HTML, or None to fall back."""
    try:
        import lxml.html as LH
    except Exception:
        return None
    try:
        cleaned = _unhide_clean(instr_html, log)
        root = LH.fromstring(cleaned)
    except Exception as e:
        log("interactive: parse failed (%s)" % e)
        return None
    try:
        root.make_links_absolute(final_url)
    except Exception:
        pass
    acts = forms = 0
    try:
        for f in list(root.iter("form")):
            if isinstance(f.tag, str) and f.get("data-psi-act") == "form" and f.get("data-psi-id"):
                _rewrite_form(f, sid, f.get("data-psi-id"))
                forms += 1
        for inp in list(root.iter("input")):
            if inp.get("data-psi-act") == "input" and inp.get("data-psi-id"):
                _rewrite_standalone_input(inp, sid)
                forms += 1
        for el in list(root.iter()):
            if isinstance(el.tag, str) and el.get("data-psi-act") == "click" and el.get("data-psi-id"):
                _rewrite_click(el, sid)
                acts += 1
    except Exception as e:
        log("interactive: rewrite failed (%s)" % e)
        return None
    try:
        html = LH.tostring(root, encoding="unicode")
    except Exception:
        return None
    body = _minimize_html(html, keep_images=True, keep_links=True, log=log, interactive=True)
    body = _tidy_html(body.encode("utf-8", "replace")).decode("utf-8", "replace")
    log("interactive: %d actions, %d forms" % (acts, forms))
    out = ['<base href="%s">' % _esc_attr(final_url),
           '<table width="%d" align="center"><tr><td>' % PROXY_ARTICLE_WIDTH,
           '<font size="2">PsiProxy live view &middot; %s</font><hr>' % _esc_text(final_url[:80]),
           body,
           '</td></tr></table>']
    return "\n".join(out)


def _open_upstream(method, url, client_headers, body, is_image, range_header, log):
    """Open the upstream connection (TLS, redirects) WITHOUT reading the body, so
    the caller can either stream it (downloads/Range) or buffer+decompress it
    (HTML/images). A client Range is forwarded verbatim; gzip is only requested
    when there is no Range (so byte ranges stay transparent). Returns
    (resp, status, reason, final_url, content_type, headers)."""
    req_headers = {
        "User-Agent": UA,
        "Accept": ("image/jpeg,image/png,image/gif" if is_image
                   else "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"),
    }
    if not range_header:
        req_headers["Accept-Encoding"] = "gzip"
    for k in ("cookie", "referer", "authorization", "accept-language",
              "if-modified-since", "if-none-match", "content-type"):
        if k in client_headers:
            req_headers[k.title()] = client_headers[k]
    if range_header:
        req_headers["Range"] = range_header

    data = body if (body and method not in ("GET", "HEAD")) else None
    req = urllib.request.Request(url, data=data, method=method, headers=req_headers)
    # every connection (redirect hops included) is checked by _guarded_connect
    opener = _build_opener()
    try:
        resp = opener.open(req, timeout=30)
    except urllib.error.HTTPError as e:
        resp = e  # still a readable response (4xx/5xx/416)

    status = getattr(resp, "status", None) or getattr(resp, "code", 200) or 200
    reason = getattr(resp, "reason", "OK") or "OK"
    final_url = resp.geturl()
    hdrs = resp.headers
    ctype = hdrs.get("Content-Type") or ""
    return resp, status, reason, final_url, ctype, hdrs


def _read_body_decoded(resp, hdrs, method, cap=MAX_BODY):
    """Read an upstream body into memory (capped) and un-gzip/deflate it. Used by
    the HTML/image path only (streaming downloads never call this). Returns
    (body_bytes, truncated)."""
    raw = resp.read(cap + 1) if method != "HEAD" else b""
    truncated = len(raw) > cap
    if truncated:
        raw = raw[:cap]
    ce = (hdrs.get("Content-Encoding") or "").lower()
    bodyb = raw
    if "gzip" in ce:
        try:
            bodyb = _gzip.decompress(raw)
        except Exception:
            pass
    elif "deflate" in ce:
        try:
            bodyb = zlib.decompress(raw)
        except zlib.error:
            try:
                bodyb = zlib.decompress(raw, -15)
            except Exception:
                pass
    return bodyb, truncated


def _fetch(method, url, client_headers, body, is_image, log):
    """Fetch upstream fully (open + read + un-gzip), the buffered form. Returns
    (status, reason, final_url, content_type, body_bytes, headers, truncated)."""
    resp, status, reason, final_url, ctype, hdrs = _open_upstream(
        method, url, client_headers, body, is_image, None, log)
    bodyb, truncated = _read_body_decoded(resp, hdrs, method)
    return status, reason, final_url, ctype, bodyb, hdrs, truncated
