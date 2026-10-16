#!/usr/bin/env python3
"""psiproxytui.py - a live dashboard for the PsiProxy daemon.

A Textual TUI that talks to the daemon's local control socket
(~/.config/psiproxy/control.sock, line-JSON). It shows uptime, the listening
address, pages ok/failed, bytes in->out and the saved ratio, two toggles
(Reading view / Collapse menus) you can flip with 'r' / 'n' or by clicking, and
a scrolling table of recent requests. It refreshes about once a second.

Run it with the launcher:  ./psiproxy  (in this folder)
"""
import os, sys, json, socket, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import psiproxy_core as core

SOCK_PATH = os.path.join(core.CONFIG_DIR, "control.sock")

from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Header, Footer, Static, DataTable, Switch, Label


class ControlClient:
    """Keeps one connection to the daemon's unix control socket, reconnecting on
    any error. request() returns the reply dict, or None when unreachable."""
    def __init__(self, path):
        self.path = path
        self.sock = None
        self.f = None

    def _connect(self):
        self.close()
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(3.0)
        s.connect(self.path)
        self.sock = s
        self.f = s.makefile("rwb")

    def request(self, req):
        for attempt in (1, 2):
            try:
                if self.f is None:
                    self._connect()
                self.f.write((json.dumps(req) + "\n").encode("utf-8"))
                self.f.flush()
                line = self.f.readline()
                if not line:
                    raise OSError("closed")
                return json.loads(line.decode("utf-8", "replace"))
            except Exception:
                self.close()
                if attempt == 2:
                    return None
        return None

    def close(self):
        for h in (self.f, self.sock):
            try:
                if h:
                    h.close()
            except Exception:
                pass
        self.f = self.sock = None


def _human_bytes(n):
    n = float(n or 0)
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return ("%d %s" % (int(n), unit)) if unit == "B" else ("%.1f %s" % (n, unit))
        n /= 1024.0


def _human_uptime(secs):
    secs = int(secs or 0)
    d, secs = divmod(secs, 86400)
    h, secs = divmod(secs, 3600)
    m, s = divmod(secs, 60)
    if d:
        return "%dd %dh %dm" % (d, h, m)
    if h:
        return "%dh %dm %ds" % (h, m, s)
    if m:
        return "%dm %ds" % (m, s)
    return "%ds" % s


class PsiProxyTUI(App):
    CSS = """
    Screen { layout: vertical; }
    #stats { padding: 1 2; height: auto; background: $panel; }
    #toggles { height: auto; padding: 0 2 1 2; background: $panel; }
    .tog { height: 3; width: 34; content-align: left middle; }
    .tog Label { width: 20; content-align: left middle; }
    #table { height: 1fr; }
    DataTable { height: 1fr; }
    """

    BINDINGS = [
        ("r", "toggle_reading", "Reading view"),
        ("n", "toggle_nav", "Collapse menus"),
        ("j", "toggle_js", "JS rendering"),
        ("i", "toggle_interactive", "Interactive"),
        ("q", "quit", "Quit"),
    ]

    TITLE = "PsiProxy"
    SUB_TITLE = "Psion web-slimming proxy"

    def __init__(self, **kw):
        super().__init__(**kw)
        self.client = ControlClient(SOCK_PATH)
        self.snap = {}
        self._guard = False

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        yield Static("Connecting to the daemon…", id="stats")
        with Horizontal(id="toggles"):
            with Horizontal(classes="tog"):
                yield Label("Reading view")
                yield Switch(id="sw_reading")
            with Horizontal(classes="tog"):
                yield Label("Collapse menus")
                yield Switch(id="sw_nav")
            with Horizontal(classes="tog"):
                yield Label("JS rendering")
                yield Switch(id="sw_js")
            with Horizontal(classes="tog"):
                yield Label("Interactive")
                yield Switch(id="sw_act")
        with Vertical(id="table"):
            yield DataTable(id="requests")
        yield Footer()

    def on_mount(self) -> None:
        table = self.query_one("#requests", DataTable)
        table.add_columns("Time", "Host", "Status", "In", "Out", "ms")
        table.cursor_type = "row"
        table.zebra_stripes = True
        self.poll()
        self.set_interval(1.0, self.poll)

    # -- polling ----------------------------------------------------------
    def poll(self) -> None:
        snap = self.client.request({"cmd": "status"})
        if snap is None:
            self.query_one("#stats", Static).update(
                "[b red]Daemon not reachable[/]  (is psiproxy.service running?)")
            return
        self.snap = snap
        self._render_stats(snap)
        self._render_toggles(snap)
        self._render_table(snap)

    def _render_stats(self, snap) -> None:
        ratio = snap.get("saved_ratio", 0.0) or 0.0
        txt = (
            "[b]Listening[/] {listen}   "
            "[b]Uptime[/] {up}\n"
            "[b]Pages[/] [green]{ok}[/] ok · [red]{fail}[/] failed   "
            "[b]JS renders[/] [magenta]{renders}[/]{lastms}   "
            "[b]Last[/] {last}\n"
            "[b]Traffic[/] {bin} in → {bout} out   "
            "[b]Saved[/] [cyan]{ratio:.0f}%[/]"
        ).format(
            listen=snap.get("listen", "?"),
            up=_human_uptime(snap.get("uptime", 0)),
            ok=snap.get("pages_ok", 0),
            fail=snap.get("pages_failed", 0),
            renders=snap.get("renders", 0),
            lastms=(" (%dms)" % snap["last_render_ms"]) if snap.get("last_render_ms") else "",
            last=snap.get("last_host") or "-",
            bin=_human_bytes(snap.get("bytes_in", 0)),
            bout=_human_bytes(snap.get("bytes_out", 0)),
            ratio=ratio * 100.0,
        )
        self.query_one("#stats", Static).update(txt)

    def _render_toggles(self, snap) -> None:
        self._guard = True
        try:
            self.query_one("#sw_reading", Switch).value = bool(snap.get("reading"))
            self.query_one("#sw_nav", Switch).value = bool(snap.get("collapse_nav"))
            self.query_one("#sw_js", Switch).value = bool(snap.get("js_render"))
            self.query_one("#sw_act", Switch).value = bool(snap.get("interactive"))
        finally:
            self._guard = False

    def _render_table(self, snap) -> None:
        table = self.query_one("#requests", DataTable)
        table.clear()
        for r in snap.get("recent", [])[:200]:
            ts = time.strftime("%H:%M:%S", time.localtime(r.get("t", 0)))
            status = r.get("status", 0)
            status_str = ("[red]%s[/]" % status) if status >= 400 else ("[green]%s[/]" % status)
            table.add_row(
                ts,
                (r.get("host") or "-")[:30],
                status_str,
                _human_bytes(r.get("in", 0)),
                _human_bytes(r.get("out", 0)),
                str(r.get("ms", 0)),
            )

    # -- actions / events -------------------------------------------------
    def _send_set(self, name, value) -> None:
        snap = self.client.request({"cmd": "set", name: bool(value)})
        if snap is not None:
            self.snap = snap
            self._render_stats(snap)
            self._render_toggles(snap)

    def action_toggle_reading(self) -> None:
        self._send_set("reading", not bool(self.snap.get("reading")))

    def action_toggle_nav(self) -> None:
        self._send_set("collapse_nav", not bool(self.snap.get("collapse_nav")))

    def action_toggle_js(self) -> None:
        self._send_set("js_render", not bool(self.snap.get("js_render")))

    def action_toggle_interactive(self) -> None:
        self._send_set("interactive", not bool(self.snap.get("interactive")))

    def on_switch_changed(self, event: Switch.Changed) -> None:
        if self._guard:
            return
        sid = event.switch.id
        if sid == "sw_reading":
            self._send_set("reading", event.value)
        elif sid == "sw_nav":
            self._send_set("collapse_nav", event.value)
        elif sid == "sw_js":
            self._send_set("js_render", event.value)
        elif sid == "sw_act":
            self._send_set("interactive", event.value)


def main():
    PsiProxyTUI().run()


if __name__ == "__main__":
    main()
