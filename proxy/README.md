# PsiProxy

PsiProxy is an **optional** helper for PsiWeb, PsiMail and the other apps. You
do not need it: the apps work without it. It runs on a computer on your network
(Linux, or anything with Python 3.10 or later) and does the heavy work that a
36 MHz Psion cannot:

- it fetches pages for PsiWeb, doing the TLS itself, so the Psion can read
  modern `https://` sites;
- it slims each page for a 640x240, 16-grey browser with no CSS: articles are
  extracted into one clean column, home pages become a list of headlines, and
  site menus fold into one `[ Menu ]` link;
- it turns pictures (WebP, AVIF, JPEG, PNG, GIF) into small 16-grey GIFs;
- it streams downloads untouched, with `Range` support so updates can resume;
- it receives logs from the apps' **Send log to server** button (`/_putlog`).

Optionally, it can render JavaScript-only pages in a headless Chromium (see
[JavaScript rendering](#javascript-rendering)).

## Pointing the apps at it

On the Psion, open the app's **Connection settings**, go to **Network** and
turn on **Use proxy**. Enter the address of the computer running PsiProxy and
port **8081**. The setting is shared by all the apps.

PsiWeb sends every request, `https://` ones included, as a plain HTTP request
with an absolute address. The link between the Psion and PsiProxy is therefore
**plain HTTP**: keep PsiProxy on a network you trust.

## Installing

```bash
cd psiproxy                      # this folder, wherever you put it
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
# optional, for JavaScript rendering:
.venv/bin/pip install -r requirements-optional.txt
.venv/bin/python -m playwright install chromium
```

Each library is optional at run time. Without trafilatura and readability,
pages are slimmed whole. Without Pillow, pictures pass through unchanged.
Without Playwright, JavaScript rendering turns itself off.

## Running

```bash
.venv/bin/python psiproxyd.py            # the proxy (Ctrl+C stops it)
./psiproxy                               # the dashboard (needs textual)
```

By default it listens on **127.0.0.1:8081**, which only this computer can use.
To let a Psion on your LAN in, set `PSIPROXY_ADDR` to this computer's LAN
address (or `0.0.0.0`) and check `PSIPROXY_ALLOW` (below).

The dashboard shows what the proxy is doing and lets you flip its four
settings. The same settings are on the proxy's own page: open
`http://psiproxy/` (or the proxy's address and port) in PsiWeb.

### As a service

`psiproxy.service.example` is a systemd user unit. It expects the proxy in
`~/psiproxy` with its venv in `~/psiproxy/.venv`; edit the paths if not.

```bash
mkdir -p ~/.config/systemd/user
cp psiproxy.service.example ~/.config/systemd/user/psiproxy.service
systemctl --user daemon-reload
systemctl --user enable --now psiproxy.service
journalctl --user -u psiproxy.service -f
loginctl enable-linger "$USER"           # optional: start at boot without a login
```

## Settings

Settings come from the environment. They can also go in a file of
`KEY=VALUE` lines: `$PSIPROXY_CONFIG`, else `psiproxy.env` in this folder, else
`~/.config/psiproxy/psiproxy.env`. The environment wins over the file. Start
from `psiproxy.env.example`. The file is ignored by git; do not put secrets in
it.

| Variable | Default | What it does |
|---|---|---|
| `PSIPROXY_ADDR` | `127.0.0.1` | Address to listen on. |
| `PSIPROXY_PORT` | `8081` | Port to listen on (the apps' default). |
| `PSIPROXY_ALLOW` | see below | Clients allowed, as comma-separated CIDRs. Others get 403. |
| `PSIPROXY_ALLOW_PRIVATE` | `0` | `1` lets the proxy fetch private and local addresses. |
| `PSIPROXY_JS_RENDER` | `auto` | Headless-browser rendering: `auto` (only with `ALLOW_PRIVATE=1`), `1` or `0`. |
| `PSIPROXY_MAX_BODY` | `4194304` | Largest request body accepted, in bytes (413 beyond it). |
| `PSIPROXY_PUTLOG` | `1` | `0` turns off `/_putlog` (404). |
| `PSIPROXY_PUTLOG_TOKEN` | (none) | If set, `/_putlog` needs `?token=…` or an `X-PsiProxy-Token` header. |
| `PSIPROXY_LOG_DIR` | `~/psion-logs` | Where sent logs are saved. |
| `PSIPROXY_LOG_MAX_MB` | `50` | The oldest logs are deleted beyond this total. |
| `PSIPROXY_LOG_MAX_FILES` | `200` | ... or beyond this many files. |
| `PSIPROXY_PLUGINS` | `auto` | `auto` (the modules in the plug-in list), `none`, or a comma-separated list of modules. |
| `PSIPROXY_PLUGINS_FILE` | `plugins.local.json` | The local plug-in list (see Plug-ins). |
| `PSIPROXY_PLUGIN_PATH` | (none) | Extra folders to look for plug-ins in (`:`-separated). |
| `PSIPROXY_STATE_DIR` | `~/.config/psiproxy` | Saved toggles, the dashboard's control socket and the proxy's own log. |
| `PSIPROXY_CONFIG` | (none) | The settings file to read. |

`PSIPROXY_ALLOW`, when unset: if the proxy listens on a loopback address, only
loopback (`127.0.0.0/8`, `::1`). Otherwise loopback plus the private LAN ranges
`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`, link-local `169.254.0.0/16`
and `fe80::/10`, and IPv6 ULA `fc00::/7`. A VPN such as Tailscale
(`100.64.0.0/10`) is not included; add it if you need it. Set your own list to
narrow it, for example `PSIPROXY_ALLOW=192.168.1.40/32` for one Psion.

The four runtime toggles (Reading view, Collapse menus, JS rendering,
Interactive) are saved in `config.json` in the state folder.

## Security

Read this before you let anything but your own computer reach PsiProxy.

- **Never expose it to the internet.** It is an unauthenticated forward proxy:
  whoever can reach it can fetch web pages through your connection. Do not
  forward its port on your router. It listens on 127.0.0.1 unless you say
  otherwise.
- **Client allowlist.** Only addresses in `PSIPROXY_ALLOW` may use it (see
  above). Keep the list as tight as your network allows.
- **Private destinations are refused.** The proxy will not fetch addresses
  that are loopback, private (RFC 1918, IPv6 ULA, CGNAT `100.64.0.0/10`),
  link-local (including the `169.254.169.254` cloud metadata address),
  unspecified, multicast or otherwise not globally routable. The check is made
  after the name is looked up, the connection goes to the address that was
  checked, and every redirect is checked again. This stops a web page from
  using the proxy to reach your router or other machines. Set
  `PSIPROXY_ALLOW_PRIVATE=1` only if you want to browse your own LAN from the
  Psion. Requests addressed to the proxy itself (`localhost`, its own
  addresses, or the name `psiproxy`) show its settings page instead of being
  fetched.
- **JavaScript rendering is not covered by that check.** Headless Chromium
  makes its own connections. PsiProxy filters each request it can see, but
  redirects, WebSockets and the browser's own name lookups slip past. So with
  `PSIPROXY_JS_RENDER=auto` (the default) it is only available when private
  destinations are allowed anyway. `PSIPROXY_JS_RENDER=1` turns it on regardless;
  only do that on a network where the risk is acceptable.
- **Cookies and passwords pass through.** The proxy forwards the Psion's
  `Cookie`, `Authorization` and `Referer` headers to the site, and the link
  from the Psion to the proxy is plain HTTP. Anyone who can watch that part of
  your network can read them.
- **`/_putlog` writes files.** Each log is at most 2 MB, and the folder is kept
  to `PSIPROXY_LOG_MAX_MB` and `PSIPROXY_LOG_MAX_FILES`. Logs can contain host
  names, user and account names, server addresses and message subjects. Turn
  it off with `PSIPROXY_PUTLOG=0`, or require a token with
  `PSIPROXY_PUTLOG_TOKEN`. The apps do not send a token yet, so with a token
  set their **Send log to server** button is refused.
- **Its own log** (`psiproxy.log` in the state folder, and the journal) names
  the sites visited.
- **Request limits.** Request headers are limited to 64 KB (the request line
  to 8 KB) and bodies to `PSIPROXY_MAX_BODY`. A request that stalls for two
  minutes is dropped. Chunked request bodies are refused (411).
- **The dashboard's control socket** is a unix socket readable only by you.

## Plug-ins

PsiProxy can serve extra routes from optional plug-in modules. None is
built in, and PsiTerm, PsiMail and PsiWeb need none. A plug-in that is
missing or fails to load is logged and skipped; its routes answer 404.

To add one, put the module in `plugins.local/` beside `psiproxyd.py`, beside
`psiproxyd.py` itself, or in a folder on `PSIPROXY_PLUGIN_PATH`, and list it in
`plugins.local.json` beside `psiproxy_core.py` (or the file named by
`PSIPROXY_PLUGINS_FILE`). Both are local: `.gitignore` keeps them out of the
repository.

```json
[
  {"module": "hello", "route": "/_hello/", "local_only": true}
]
```

`route` is required. `"local_only": true` (the default) serves the route only
for requests to the proxy itself; `"anywhere": true` matches it anywhere in
the path. A module named in `PSIPROXY_PLUGINS` but not in the list must define
`PSIPROXY_ROUTE = "/_name/"` itself. The module needs

```python
def serve(session, method, path, query, body, client_close, http_response):
    session.t.send(http_response(200, "OK", b"hello\n", "text/plain", False,
                                 close=client_close))
    return not client_close          # keep the connection?
```

It may also define `set_logger(fn)` and `start()`, which run at start. Its
route answers only requests addressed to the proxy itself, unless it sets
`PSIPROXY_LOCAL_ONLY = False`. Plug-ins read their own settings and secrets
from the environment.

## JavaScript rendering

Some sites send an empty page and draw everything with JavaScript. When the
slimmed result looks like such a shell, PsiProxy can load the page in a warm
headless Chromium (Playwright), let it settle, and slim the result instead.
With **Interactive** on, the page stays open in a live session: buttons become
links and forms become simple forms that the proxy replays on the live page
(`/_act`). Sessions close after 10 minutes idle, and at most three are kept.

This needs `requirements-optional.txt`, the Chromium download, and
`PSIPROXY_JS_RENDER` to allow it (see [Security](#security)).

## Files

| File | What it is |
|---|---|
| `psiproxyd.py` | The proxy: TCP server, client allowlist, control socket, log |
| `psiproxy_core.py` | The engine: fetching, slimming, pictures, plug-ins, checks |
| `psiproxytui.py` | The dashboard (Textual) |
| `psiproxy` | Starts the dashboard |
| `psiproxy.env.example` | A settings file to copy |
| `psiproxy.service.example` | A systemd user unit to copy |
| `requirements.txt`, `requirements-optional.txt` | Python packages |

## Licence

PsiProxy is part of this repository and is under its MIT licence (see
`../LICENSE`). The packages it uses have their own licences: trafilatura,
readability-lxml and Playwright are Apache-2.0, lxml and lxml_html_clean are
BSD-3-Clause, Pillow is MIT-CMU (HPND), and Textual is MIT. Chromium, which
Playwright downloads, is under the BSD licence and others. None are included
here.
