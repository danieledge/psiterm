#!/usr/bin/env python3
"""psiproxyd.py - the headless PsiProxy daemon.

A threaded TCP server that binds 127.0.0.1:8081 (override with PSIPROXY_ADDR /
PSIPROXY_PORT) and hands each accepted connection to a ProxySession over a
SocketTransport. A Psion reaches it over the LAN (Connection settings > Use
proxy = this box's address, port 8081) once PSIPROXY_ADDR is set to a LAN
address, through a WiRSa/Atom doing PPP, or any HTTP-proxy client. Clients
outside PSIPROXY_ALLOW are refused with 403.

It also:
  * serves the control/status page at http://<this-host>/ and handles
    /toggle?reading | /toggle?nav (adapted to work when addressed by IP:port);
  * exposes a local control channel for the TUI on a unix socket
    (~/.config/psiproxy/control.sock) speaking line-JSON;
  * logs to ~/.config/psiproxy/psiproxy.log (rotating).

It is an UNAUTHENTICATED LAN proxy by design - do not expose it to the WAN.
"""
import os, sys, json, socket, socketserver, threading, logging, logging.handlers
import ipaddress

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psiproxy_core as core

ADDR = os.environ.get("PSIPROXY_ADDR", "127.0.0.1").strip() or "127.0.0.1"
PORT = int(os.environ.get("PSIPROXY_PORT", "8081"))

# Who may use the proxy. Unset: loopback only when bound to loopback, else
# loopback + the private LAN ranges (RFC 1918, link-local, IPv6 ULA).
DEFAULT_ALLOW_LAN = ("127.0.0.0/8,::1/128,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16,"
                     "169.254.0.0/16,fc00::/7,fe80::/10")


def _is_loopback_bind(addr):
    try:
        return ipaddress.ip_address(addr).is_loopback
    except ValueError:
        return addr.lower() == "localhost"


def _parse_allow(text):
    nets = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        try:
            nets.append(ipaddress.ip_network(part, strict=False))
        except ValueError:
            sys.stderr.write("psiproxy: PSIPROXY_ALLOW: ignoring %r\n" % part)
    return nets


_allow_env = os.environ.get("PSIPROXY_ALLOW", "").strip()
if _allow_env:
    ALLOW = _parse_allow(_allow_env)
elif _is_loopback_bind(ADDR):
    ALLOW = _parse_allow("127.0.0.0/8,::1/128")
else:
    ALLOW = _parse_allow(DEFAULT_ALLOW_LAN)


def client_allowed(ip):
    try:
        a = ipaddress.ip_address(str(ip).split("%", 1)[0])
    except ValueError:
        return False
    if isinstance(a, ipaddress.IPv6Address) and a.ipv4_mapped:
        a = a.ipv4_mapped
    return any(a.version == n.version and a in n for n in ALLOW)
CONFIG_DIR = core.CONFIG_DIR
SOCK_PATH = os.path.join(CONFIG_DIR, "control.sock")
LOG_PATH = os.path.join(CONFIG_DIR, "psiproxy.log")

os.makedirs(CONFIG_DIR, exist_ok=True)

# -- logging: rotating file + stderr (journald) ------------------------------
logger = logging.getLogger("psiproxy")
logger.setLevel(logging.INFO)
logger.propagate = False
_fh = logging.handlers.RotatingFileHandler(LOG_PATH, maxBytes=1_000_000, backupCount=3)
_fh.setFormatter(logging.Formatter("%(asctime)s %(message)s", "%H:%M:%S"))
logger.addHandler(_fh)
_sh = logging.StreamHandler(sys.stderr)
_sh.setFormatter(logging.Formatter("%(message)s"))
logger.addHandler(_sh)


def log(msg):
    try:
        logger.info(msg)
    except Exception:
        pass


def discover_local_names():
    """Host strings (lowercase, no port) that identify this proxy, so a request
    to its own IP/hostname is served as the control page."""
    names = {"localhost", "127.0.0.1", "0.0.0.0", "::1", core.PROXY_HOST}
    try:
        hn = socket.gethostname()
        names.add(hn.lower())
        names.add(hn.split(".")[0].lower())
    except Exception:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None):
            names.add(str(info[4][0]).lower())
    except Exception:
        pass
    # the primary outbound LAN IP (the one PsiWeb will be pointed at)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        names.add(s.getsockname()[0].lower())
        s.close()
    except Exception:
        pass
    if ADDR not in ("0.0.0.0", "::"):
        names.add(ADDR.lower())
    return names


LOCAL_NAMES = discover_local_names()


# ===========================================================================
#  TCP proxy server
# ===========================================================================

class _NoStop:
    def is_set(self):
        return False


class ProxyHandler(socketserver.BaseRequestHandler):
    def handle(self):
        if not client_allowed(self.client_address[0]):
            log("refused client %s (not in PSIPROXY_ALLOW)" % (self.client_address[0],))
            try:
                self.request.sendall(core._http_response(
                    403, "Forbidden", b"This proxy does not serve your address\n",
                    "text/plain; charset=us-ascii", False, close=True))
            except Exception:
                pass
            return
        try:
            t = core.SocketTransport(self.request, _NoStop())
            core.ProxySession(t, log, local_names=LOCAL_NAMES).run()
        except Exception as e:
            # one bad connection must never crash the server
            log("conn error: %s" % e)

    def finish(self):
        try:
            self.request.close()
        except Exception:
            pass


class ThreadingTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True
    address_family = socket.AF_INET6 if ":" in ADDR else socket.AF_INET

    def handle_error(self, request, client_address):
        log("server: unhandled error on %s" % (client_address,))


# ===========================================================================
#  Local control channel (unix socket, line-JSON) for the TUI
# ===========================================================================

def control_command(req):
    """Handle one {"cmd":...} request; always return the current State snapshot
    (plus the listening address), so status and set share one reply shape."""
    cmd = (req.get("cmd") or "").lower()
    if cmd == "set":
        if isinstance(req.get("reading"), bool):
            core.STATE.set_flag("reading", req["reading"])
            log("control: reading view %s (tui)" % ("on" if req["reading"] else "off"))
        if isinstance(req.get("collapse_nav"), bool):
            core.STATE.set_flag("collapse_nav", req["collapse_nav"])
            log("control: collapse menus %s (tui)" % ("on" if req["collapse_nav"] else "off"))
        if isinstance(req.get("js_render"), bool):
            core.STATE.set_flag("js_render", req["js_render"])
            log("control: JS rendering %s (tui)" % ("on" if req["js_render"] else "off"))
        if isinstance(req.get("interactive"), bool):
            core.STATE.set_flag("interactive", req["interactive"])
            log("control: interactive %s (tui)" % ("on" if req["interactive"] else "off"))
    snap = core.STATE.snapshot()
    snap["listen"] = "%s:%d" % (ADDR, PORT)
    return snap


def handle_control_conn(conn):
    conn.settimeout(60)
    try:
        f = conn.makefile("rwb")
    except Exception:
        try:
            conn.close()
        except Exception:
            pass
        return
    try:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            try:
                req = json.loads(line.decode("utf-8", "replace"))
            except Exception:
                f.write(b'{"error":"bad json"}\n')
                f.flush()
                continue
            try:
                resp = control_command(req if isinstance(req, dict) else {})
            except Exception as e:
                resp = {"error": str(e)}
            f.write((json.dumps(resp) + "\n").encode("utf-8"))
            f.flush()
    except Exception:
        pass
    finally:
        try:
            f.close()
        except Exception:
            pass
        try:
            conn.close()
        except Exception:
            pass


def control_server():
    try:
        os.unlink(SOCK_PATH)
    except FileNotFoundError:
        pass
    except Exception:
        pass
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        srv.bind(SOCK_PATH)
    except OSError as e:
        log("control socket unavailable (%s); the dashboard will not connect" % e)
        return
    try:
        os.chmod(SOCK_PATH, 0o600)
    except Exception:
        pass
    srv.listen(16)
    log("control socket at %s" % SOCK_PATH)
    while True:
        try:
            conn, _ = srv.accept()
        except Exception as e:
            log("control: accept error: %s" % e)
            continue
        threading.Thread(target=handle_control_conn, args=(conn,), daemon=True).start()


# ===========================================================================
#  Main
# ===========================================================================

def main():
    core.STATE.load()
    log("PsiProxy starting on %s:%d (reading=%s, collapse_nav=%s)"
        % (ADDR, PORT, core.STATE.reading, core.STATE.collapse_nav))
    if core.CONFIG_FILE_USED:
        log("settings from %s" % core.CONFIG_FILE_USED)
    log("clients allowed: %s" % ", ".join(str(n) for n in ALLOW))
    log("private destinations: %s; JS rendering: %s; putlog: %s%s -> %s"
        % ("allowed" if core.ALLOW_PRIVATE else "refused",
           "available" if core.JS_RENDER_ALLOWED else "off",
           "on" if core.PUTLOG_ON else "off",
           " (token)" if core.PUTLOG_TOKEN else "", core.LOG_DIR))
    threading.Thread(target=control_server, daemon=True).start()
    # optional plug-ins: missing ones answer 404;
    # each one's start() runs here (one may pick up work left half done)
    loaded = core.load_plugins(log)
    log("plug-ins: %s" % (", ".join(loaded) or "none"))
    server = ThreadingTCPServer((ADDR, PORT), ProxyHandler)
    log("listening on http://%s:%d/" % (ADDR, PORT))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            server.server_close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
