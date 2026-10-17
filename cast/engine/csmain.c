/* csmain.c - psicast.exe's work: link A of cast/docs/PROTOCOL.md, the
 * Psion's end.
 *
 * Connect to the Atom (192.168.7.1:5961, through psiglue: a socket over
 * Psion Internet, or ATDT192.168.7.1:5961 on the modem route), say HELLO,
 * then capture the screen, encode what changed (cast/codec) and send it as
 * UPDATEs, at most 'window' of them unacknowledged. While the window is full
 * nothing is queued: the next capture after an ACK is encoded against the
 * last frame sent, so the changes merge and the newest picture always goes.
 *
 * Capturing adapts: up to the preferred rate (10 a second as standard)
 * while the screen changes, 20 a second for a moment after the viewer's
 * own input, not at all with no viewers or while paused. When the screen
 * has been still for 2 s it is only looked at: a quarter of the frame
 * buffer's rows, a different quarter each time, at the preferred rate (at
 * most 10 a second), and captured at once when they differ - so a key
 * pressed on the Psion itself after a pause shows within about 0.1 s, not
 * up to the second the old once-a-second capture took. (ER5 has no way to
 * ask how long since the last key or tap: no User::InactivityTime.) With
 * the window server's copy (no frame buffer) it captures once every 4 s
 * instead. While the Psion is off nothing runs. A capture whose frame
 * buffer has not changed at all costs one pass of word compares
 * (csepoc.cpp) and never reaches the encoder.
 *
 * Latency (PROTOCOL.md): an Atom that asks for it in WELCOME gets each
 * UPDATE stamped with the time its frame was captured, and STATUS carries
 * this clock and answers the PING it replies to, so the Atom can show the
 * viewers how long a picture takes from the Psion's screen to the Atom.
 *
 * Everything from the Atom is untrusted: framing, lengths and CRCs are
 * checked, input events are validated and rate-limited, and injected only
 * while Preferences allow control.
 */
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "cs.h"
#include "pccodec.h"
#include "zlib.h"
#include "../../web/engine/pwnet.h"

extern PsiShared *pg_shared(void);
extern int  pg_wait(int ms, int want_net, int want_kbd);
extern int  pg_kbd_avail(void);
extern int  pg_kbd_read(void *buf, int max);
extern int  pg_quit_requested(void);
extern int  pg_net_nodelay(void);

#define ATOM_HOST        "192.168.7.1"
#define ATOM_PORT        5961
#define PROTO_VERSION    1
#define LEVEL            3          /* deflate: CPU against the link's speed */

#define MAX_IN_PAYLOAD   4096       /* the most we take from the Atom in one message */
#define BODY_MAX         (65536 - 4)   /* an UPDATE's payload is seq + body, at most 65536 */
#define WELCOME_MS       10000
#define LIVENESS_MS      20000      /* nothing from the Atom for this long: the link is dead */
#define STATUS_MS        4000       /* at least every 5 s (PROTOCOL.md) */
#define STILL_AFTER_MS   2000       /* no change for this long: only look (cs_capture_peek) */
#define PEEK_PARTS       4          /* ...at a quarter of the rows each time */
#define PEEK_MIN_MS      100        /* ...at the preferred rate, at most 10 a second */
#define INPUT_FAST_MS    1500       /* after the viewer's input: capture at 20 a second */
#define FAST_MS          50
#define SLOW_FAST_MS     1000       /* the same with the slow capture (no frame buffer) */
#define SLOW_STILL_MS    4000
#define POWER_EVERY_MS   30000
#define BACKOFF_MIN_MS   2000
#define BACKOFF_MAX_MS   30000
#define APP_CHECK_MS     2000
#define ACK_LOST_MS      8000       /* window full this long: the ACK was lost */

/* input: at most this many events at once, then INPUT_RATE a second */
#define INPUT_BURST      64
#define INPUT_RATE       100

/* message types */
#define T_HELLO   0x01
#define T_UPDATE  0x02
#define T_STATUS  0x03
#define T_BYE     0x04
#define T_DEVINFO 0x05
#define T_WELCOME 0x81
#define T_ACK     0x82
#define T_KEYREQ  0x83
#define T_VIEWERS 0x84
#define T_INPUT   0x85
#define T_PING    0x86

/* (word-aligned: the encoder reads frames a word at a time) */
static unsigned long g_frame_w[CS_STRIDE * CS_H / 4];
#define g_frame ((unsigned char *)g_frame_w)
static unsigned char g_tx[12 + BODY_MAX + 4];
static unsigned char g_rx[12 + MAX_IN_PAYLOAD + 1024];
static int g_rx_len;
static pc_enc *g_enc;

static CsShared *S(void) { return cs_shared(); }

/* ---- what the app sees ------------------------------------------------------ */

static void set_msg(const char *fmt, ...)
{
	char b[sizeof(S()->msg)];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (strcmp(b, S()->msg)) {
		memcpy(S()->msg, b, sizeof(b));
		S()->eng_seq++;
		if (b[0])
			cs_log("cast: %s", b);
	}
}

static void set_phase(int p)
{
	if (S()->phase != p) {
		S()->phase = p;
		S()->eng_seq++;
	}
}

static void set_viewers(int n, int c)
{
	if (S()->viewers != n || S()->viewers_control != c) {
		S()->viewers = n;
		S()->viewers_control = c;
		S()->eng_seq++;
	}
}

/* ---- little-endian fields ---------------------------------------------------- */

static void put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char *p, unsigned long v)
{
	p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static unsigned get16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned long get32(const unsigned char *p)
{
	return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* ---- the session ------------------------------------------------------------- */

typedef struct
	{
	unsigned long seq_sent, seq_acked;
	int window;
	int welcomed;
	int need_key;
	unsigned long t_hello, last_rx, last_status, next_cap, still_since, fast_until;
	unsigned long last_power, last_app, last_ack;
	int interval;
	int capture_fails;
	int timed;
	int body_max;                         /* the most an UPDATE body may be (WELCOME) */
	int ping_acks;                        /* the Atom's PINGs carry last_seq */
	int stamp;                            /* the Atom asked for capture times (WELCOME) */
	int peeking, peek_part, peeks_timed;  /* still: looking at part of the screen */
	/* the settings as last acted on */
	int control, paused;
	unsigned int app_seen;
	/* the viewer's input */
	int pen_down, pen_x, pen_y;
	unsigned char keys_down[32];          /* a bit per scan code (0..255) */
	unsigned long tokens_at;
	int tokens;
	unsigned long in_dropped;
	/* rates: since stat_at */
	unsigned long stat_at, stat_bytes, stat_updates, stat_busy;
	unsigned long errors;
	char why[96];                         /* why the session ended */
	} Session;

static Session g_s;

static int send_raw(const unsigned char *b, int n)
{
	if (pwn_write(b, n) != 0) {
		snprintf(g_s.why, sizeof(g_s.why), "the connection to the Atom was lost");
		return -1;
	}
	g_s.stat_bytes += n;
	return 0;
}

/* a small message (up to 128 bytes of payload) */
static int send_msg(int type, int flags, const unsigned char *pl, int n)
{
	unsigned char b[8 + 128 + 4];
	unsigned long crc;
	if (n < 0 || n > 128)
		return -1;
	b[0] = 'P'; b[1] = 'C'; b[2] = (unsigned char)type; b[3] = (unsigned char)flags;
	put32(b + 4, n);
	if (n)
		memcpy(b + 8, pl, n);
	crc = crc32(0L, b, 8 + n);
	put32(b + 8 + n, crc);
	return send_raw(b, 12 + n);
}

static void stats_update(unsigned long now)
{
	unsigned long dt = now - g_s.stat_at;
	CsShared *s = S();
	if (dt < 1000)
		return;
	s->bytes_sec = g_s.stat_bytes < 4000000UL ? g_s.stat_bytes * 1000UL / dt : g_s.stat_bytes / dt * 1000UL;
	s->ups_x10 = g_s.stat_updates * 10000UL / dt;
	s->cpu_x10 = g_s.stat_busy < 4000000UL ? g_s.stat_busy * 1000UL / dt : 1000;
	if (s->cpu_x10 > 1000)
		s->cpu_x10 = 1000;
	s->eng_seq++;
	g_s.stat_at = now;
	g_s.stat_bytes = g_s.stat_updates = g_s.stat_busy = 0;
}

/* aToken: the PING this answers (0: none). After PROTOCOL.md's 12 bytes,
   this clock and that token, so the Atom can work out the clock offset */
static int send_status(unsigned long now, unsigned long aToken)
{
	unsigned char p[20];
	int pct, mains, low;
	unsigned long ram = cs_free_ram();
	stats_update(now);
	cs_power(&pct, &mains, &low);
	p[0] = (unsigned char)pct;
	p[1] = (unsigned char)mains;
	put32(p + 2, ram);
	put16(p + 6, S()->ups_x10 > 65535 ? 65535 : S()->ups_x10);
	put16(p + 8, S()->cpu_x10);
	p[10] = (unsigned char)(S()->allow_control ? 1 : 0);
	p[11] = (unsigned char)(S()->paused ? 1 : 0);
	put32(p + 12, cs_ms());
	put32(p + 16, aToken);
	g_s.last_status = now;
	return send_msg(T_STATUS, 0, p, 20);
}

static int send_hello(void)
{
	unsigned char p[12 + 40];
	const char *name = cs_device_name();
	int n = (int)strlen(name);
	if (n > 40)
		n = 40;
	put16(p, PROTO_VERSION);
	put16(p + 2, CS_W);
	put16(p + 4, CS_H);
	p[6] = 0;                                /* format: the model in PROTOCOL.md */
	p[7] = 0;
	/* bit0 input, bit1 control allowed now, bit2 can stamp its updates */
	put32(p + 8, 1UL | (S()->allow_control ? 2UL : 0UL) | 4UL);
	memcpy(p + 12, name, n);
	return send_msg(T_HELLO, 0, p, 12 + n);
}

/* the digitiser's size, so the viewer can draw the icons round the screen */
static int send_devinfo(void)
{
	unsigned char p[4];
	int w, h;
	cs_digitiser(&w, &h);
	put16(p, w);
	put16(p + 2, h);
	return send_msg(T_DEVINFO, 0, p, 4);
}

/* encodes the frame just captured (at aCapMs) and sends it. 1 sent, 0
   nothing to send (or it could not be encoded: the next is a keyframe), -1
   link lost. Stamped (flags bit1, if the Atom asked): seq, then the capture
   time, then the body */
static int send_update(unsigned long aCapMs)
{
	int len = 0, key = g_s.need_key, r;
	int hdr = g_s.stamp ? 16 : 12;
	unsigned long crc;
	r = pc_enc_frame(g_enc, g_frame, CS_STRIDE, key, g_tx + hdr, g_s.body_max, &len);
	if (r == PC_ERR_ARG && !key) {
		key = 1;                            /* (the stream needs a fresh start) */
		r = pc_enc_frame(g_enc, g_frame, CS_STRIDE, 1, g_tx + hdr, g_s.body_max, &len);
	}
	if (r == 1)
		return 0;
	if (r != PC_OK || len < 0 || len > g_s.body_max) {
		cs_log("cast: the update could not be encoded (%d)", r);
		g_s.need_key = 1;
		return 0;
	}
	g_s.seq_sent++;
	g_tx[0] = 'P'; g_tx[1] = 'C'; g_tx[2] = T_UPDATE;
	g_tx[3] = (unsigned char)((key ? 1 : 0) | (g_s.stamp ? 2 : 0));
	put32(g_tx + 4, hdr - 8 + len);
	put32(g_tx + 8, g_s.seq_sent);
	if (g_s.stamp)
		put32(g_tx + 12, aCapMs);
	crc = crc32(0L, g_tx, hdr + len);
	put32(g_tx + hdr + len, crc);
	if (key) {
		pc_stats st;
		pc_enc_stats(g_enc, &st);
		cs_log("cast: keyframe %lu, %d bytes (%d of ops: %d fills, %d raw, %d RLE%s)", g_s.seq_sent, len,
			st.in_bytes, st.fills, st.raws, st.rles, st.pending ? ", more to come" : "");
	}
	g_s.need_key = 0;
	if (g_s.seq_sent - g_s.seq_acked == 1)
		g_s.last_ack = cs_ms();                 /* (the oldest unacknowledged went now) */
	if (send_raw(g_tx, hdr + len + 4) != 0)
		return -1;
	g_s.stat_updates++;
	S()->updates++;
	return 1;
}

/* ---- the viewer's input ------------------------------------------------------ */

static int key_is_down(int sc) { return (g_s.keys_down[sc >> 3] >> (sc & 7)) & 1; }
static void key_mark(int sc, int down)
{
	if (down) g_s.keys_down[sc >> 3] |= (unsigned char)(1 << (sc & 7));
	else g_s.keys_down[sc >> 3] &= (unsigned char)~(1 << (sc & 7));
}

/* lets go of everything the viewer holds: the pen, and keys (modifiers too) */
static void release_all(void)
{
	int sc;
	if (g_s.pen_down) {
		cs_inject_pen(3, g_s.pen_x, g_s.pen_y);
		g_s.pen_down = 0;
	}
	for (sc = 0; sc < 256; sc++)
		if (key_is_down(sc)) {
			cs_inject_key(0, sc);
			key_mark(sc, 0);
		}
}

static int take_token(unsigned long now)
{
	unsigned long dt = now - g_s.tokens_at;
	if (dt >= 10) {
		long add = (long)(dt * INPUT_RATE / 1000UL);
		if (add > INPUT_BURST) add = INPUT_BURST;
		g_s.tokens += (int)add;
		if (g_s.tokens > INPUT_BURST) g_s.tokens = INPUT_BURST;
		g_s.tokens_at = now;
	}
	if (g_s.tokens <= 0)
		return 0;
	g_s.tokens--;
	return 1;
}

/* a scan code the viewer may press: anything on the 5mx's keyboard but Off
   (switching the Psion off would end the very session that asked) */
static int scan_ok(int sc)
{
	return sc > 0 && sc <= 0xa1 && sc != 0xa0;
}

static void input_event(const unsigned char *e, unsigned long now)
{
	int kind = e[0];
	int a = (int)get16(e + 2), b = (int)get16(e + 4);
	int x = (short)a, y = (short)b;          /* (signed: the silkscreen is off the LCD) */
	int up = kind == 3 || kind == 5;
	if (!g_s.control || g_s.paused) {
		g_s.in_dropped++;
		return;
	}
	if (!up && !take_token(now)) {          /* (an up always passes: nothing stays held) */
		g_s.in_dropped++;
		return;
	}
	switch (kind) {
	case 1: case 2: case 3:
		if (x < -80 || x >= CS_W + 80 || y < -40 || y >= CS_H + 60) {
			g_s.in_dropped++;
			return;
		}
		if (kind == 1) {
			if (g_s.pen_down)
				cs_inject_pen(3, g_s.pen_x, g_s.pen_y);
			g_s.pen_down = 1;
		} else if (!g_s.pen_down) {
			g_s.in_dropped++;
			return;
		}
		cs_inject_pen(kind, x, y);
		g_s.pen_x = x;
		g_s.pen_y = y;
		if (kind == 3)
			g_s.pen_down = 0;
		break;
	case 4:
		if (!scan_ok(a)) { g_s.in_dropped++; return; }
		cs_inject_key(1, a);
		key_mark(a, 1);
		break;
	case 5:
		if (!scan_ok(a) || !key_is_down(a)) { g_s.in_dropped++; return; }
		cs_inject_key(0, a);
		key_mark(a, 0);
		break;
	case 6: {
		int mods, sc;
		if (a > 255 || !cs_char_keys(a, &mods, &sc)) {
			g_s.in_dropped++;
			return;
		}
		if (mods && !key_is_down(mods))
			cs_inject_key(1, mods);
		cs_inject_key(1, sc);
		cs_inject_key(0, sc);
		if (mods && !key_is_down(mods))
			cs_inject_key(0, mods);
		break;
	}
	default:
		g_s.in_dropped++;
		return;
	}
	S()->inputs++;
	g_s.fast_until = now + INPUT_FAST_MS;
	g_s.peeking = 0;
	if ((long)(g_s.next_cap - (now + 30)) > 0)
		g_s.next_cap = now + 30;             /* show what it did, soon */
}

/* ---- from the Atom ----------------------------------------------------------- */

/* every update up to seq has been taken - applied or discarded - by the
   Atom (an ACK, or a PING's last_seq: both cumulative) */
static void ack(unsigned long seq, unsigned long now)
{
	if (seq > g_s.seq_acked && seq <= g_s.seq_sent) {
		int was_full = (int)(g_s.seq_sent - g_s.seq_acked) >= g_s.window;
		g_s.last_ack = now;
		g_s.seq_acked = seq;
		if (was_full) {
			g_s.next_cap = now;          /* the newest picture, now */
			g_s.peeking = 0;
		}
	}
}

static void handle(int type, const unsigned char *p, int n, unsigned long now)
{
	switch (type) {
	case T_WELCOME: {
		int i, k = 0;
		char url[sizeof(S()->viewer_url)];
		if (n < 8)
			return;
		g_s.window = (int)get16(p + 2);
		if (g_s.window < 1) g_s.window = 1;
		if (g_s.window > 8) g_s.window = 8;
		/* the largest UPDATE payload the Atom takes (0: 65536); the
		   encoder splits a change to fit, but needs PC_ENC_MIN_OUT */
		/* (bit1 of its capabilities: stamp each UPDATE with the capture
		   time, 4 bytes more of the payload) */
		g_s.stamp = (get32(p + 4) & 2) != 0;
		g_s.body_max = BODY_MAX - (g_s.stamp ? 4 : 0);
		if (n >= 12) {
			unsigned long most = get32(p + 8), over = g_s.stamp ? 8UL : 4UL;
			if (most != 0 && most < over + g_s.body_max)
				g_s.body_max = most < over + PC_ENC_MIN_OUT ? PC_ENC_MIN_OUT : (int)(most - over);
		}
		/* then, optionally, the viewer's address (UTF-8; printable ASCII
		   kept, e.g. "http://192.168.1.40/screen") */
		for (i = 12; i < n && k < (int)sizeof(url) - 1; i++)
			if (p[i] > ' ' && p[i] < 127)
				url[k++] = (char)p[i];
		url[k] = 0;
		if (strcmp(url, S()->viewer_url)) {
			memcpy(S()->viewer_url, url, sizeof(url));
			S()->eng_seq++;
		}
		if (!g_s.welcomed)
			cs_log("cast: WELCOME version %u, window %d, capabilities %lx, updates up to %d bytes%s%s%s", get16(p),
				g_s.window, get32(p + 4), g_s.body_max + (g_s.stamp ? 8 : 4), g_s.stamp ? ", stamped" : "",
				k ? ", viewer at " : "", url);
		g_s.welcomed = 1;
		g_s.next_cap = now;
		g_s.peeking = 0;
		set_phase(CS_PHASE_SHARING);
		set_msg("");
		break;
	}
	case T_ACK:
		if (n >= 4)
			ack(get32(p), now);
		break;
	case T_KEYREQ:
		g_s.need_key = 1;
		g_s.next_cap = now;
		break;
	case T_VIEWERS:
		if (n < 2)
			return;
		if (S()->viewers <= 0 && p[0] > 0) {
			g_s.next_cap = now;
			g_s.peeking = 0;
		}
		if (S()->viewers != p[0])
			cs_log("cast: %d viewer%s, %d with control", p[0], p[0] == 1 ? "" : "s", p[1]);
		set_viewers(p[0], p[1] > p[0] ? p[0] : p[1]);
		break;
	case T_INPUT: {
		int i;
		for (i = 0; i + 8 <= n; i += 8)
			input_event(p + i, now);
		break;
	}
	case T_PING:
		/* (a newer Atom: token, then the last UPDATE it applied - an ACK
		   that a lost ACK cannot stall; an older one sends the token only) */
		if (n >= 8) {
			unsigned long last = get32(p + 4);
			g_s.ping_acks = 1;
			if (last > g_s.seq_acked && last <= g_s.seq_sent)
				cs_log("cast: PING says %lu applied (its ACK did not come)", last);
			ack(last, now);
		}
		send_status(now, n >= 4 ? get32(p) : 0);
		break;
	default:
		break;                                /* (unknown: a newer Atom; ignored) */
	}
}

/* takes every whole message out of g_rx; a bad one costs a byte (resync) */
static void parse(unsigned long now)
{
	int pos = 0;
	while (g_rx_len - pos >= 12) {
		unsigned char *m = g_rx + pos;
		unsigned long len, crc;
		if (m[0] != 'P' || m[1] != 'C') {
			pos++;
			if (g_s.welcomed) g_s.errors++;
			continue;
		}
		len = get32(m + 4);
		if (len > MAX_IN_PAYLOAD) {
			pos++;
			g_s.errors++;
			continue;
		}
		if ((unsigned long)(g_rx_len - pos) < 12 + len)
			break;                            /* the rest has still to come */
		crc = crc32(0L, m, 8 + (unsigned)len);
		if (crc != get32(m + 8 + len)) {
			pos++;
			g_s.errors++;
			continue;
		}
		g_s.last_rx = now;
		handle(m[2], m + 8, (int)len, now);
		pos += 12 + (int)len;
	}
	if (pos > 0) {
		memmove(g_rx, g_rx + pos, g_rx_len - pos);
		g_rx_len -= pos;
	}
}

/* ---- the loop ---------------------------------------------------------------- */

static int quit_now(void)
{
	return S()->quitting || pg_quit_requested();
}

/* 1 if the engine should end: asked to, PsiCast gone, or a flat battery */
static int should_stop(unsigned long now, unsigned long *aLastPower, unsigned long *aLastApp)
{
	if (quit_now())
		return 1;
	if (now - *aLastApp >= APP_CHECK_MS) {
		*aLastApp = now;
		if (!S()->detached && cs_app_gone()) {
			cs_log("cast: PsiCast has gone without stopping: stopping too");
			S()->stop_reason = CS_STOP_APP_GONE;
			return 1;
		}
	}
	if (now - *aLastPower >= POWER_EVERY_MS || *aLastPower == 0) {
		int pct, mains, low;
		*aLastPower = now ? now : 1;
		cs_power(&pct, &mains, &low);
		if (S()->battery_stop && low && mains != 1) {
			cs_log("cast: the battery is low (%d%%): stopping", pct);
			S()->stop_reason = CS_STOP_BATTERY;
			return 1;
		}
	}
	return 0;
}

static void drain_app(void)
{
	unsigned char junk[32];
	while (pg_kbd_avail() > 0)
		pg_kbd_read(junk, sizeof(junk));
}

/* the settings changed (the app pokes us): act on what matters at once */
static int settings(unsigned long now)
{
	CsShared *s = S();
	int changed = 0;
	if (s->app_seq == g_s.app_seen)
		return 0;
	g_s.app_seen = s->app_seq;
	if (!!s->allow_control != g_s.control) {
		g_s.control = !!s->allow_control;
		cs_log("cast: control from the viewer %s", g_s.control ? "allowed" : "not allowed");
		changed = 1;
	}
	if (!!s->paused != g_s.paused) {
		g_s.paused = !!s->paused;
		cs_log("cast: %s", g_s.paused ? "paused" : "going on");
		changed = 1;
		if (!g_s.paused) {
			g_s.next_cap = now;
			g_s.peeking = 0;
		}
	}
	if (!g_s.control || g_s.paused)
		release_all();
	return changed && g_s.welcomed ? send_status(now, 0) : 0;
}

/* one connection to the Atom: 0 asked to stop, -1 the link was lost
   (g_s.why says why) */
static int session(void)
{
	unsigned long now = cs_ms();
	int r;
	memset(&g_s, 0, sizeof(g_s));
	g_rx_len = 0;
	g_s.window = 2;
	g_s.body_max = BODY_MAX;
	g_s.need_key = 1;
	g_s.t_hello = g_s.last_rx = g_s.still_since = g_s.stat_at = g_s.tokens_at = now;
	g_s.last_status = now;
	g_s.last_app = now;
	g_s.last_power = now;
	g_s.tokens = INPUT_BURST;
	g_s.app_seen = S()->app_seq;
	g_s.control = !!S()->allow_control;
	g_s.paused = !!S()->paused;
	g_s.interval = PEEK_MIN_MS;
	S()->updates = 0;
	set_viewers(-1, 0);
	set_msg("Waiting for the Atom to answer");
	if (send_hello() != 0 || send_devinfo() != 0)
		return -1;
	for (;;) {
		int wait, m;
		long until;
		now = cs_ms();
		cs_beat();
		if (should_stop(now, &g_s.last_power, &g_s.last_app)) {
			release_all();
			send_msg(T_BYE, 0, 0, 0);
			return 0;
		}
		if (settings(now) < 0)
			return -1;
		if (!g_s.welcomed && now - g_s.t_hello > WELCOME_MS) {
			snprintf(g_s.why, sizeof(g_s.why), "the Atom did not answer - it needs its PsiCast firmware");
			return -1;
		}
		/* (only once nothing is waiting to be read: a Psion too busy to
		   look for a while has not lost the Atom) */
		if (now - g_s.last_rx > LIVENESS_MS && !(pg_wait(20, 1, 0) & 1)) {
			snprintf(g_s.why, sizeof(g_s.why), "no word from the Atom for 20 s");
			return -1;
		}
		if (now - g_s.last_status >= STATUS_MS && send_status(now, 0) != 0)
			return -1;

		/* capture, and send if there is room in the window */
		if (g_s.welcomed && !g_s.paused && S()->viewers != 0 && (long)(now - g_s.next_cap) >= 0) {
			int fast = 1000 / (S()->max_rate >= 1 && S()->max_rate <= 20 ? S()->max_rate : 10);
			if ((long)(g_s.fast_until - now) > 0 && fast > FAST_MS)
				fast = FAST_MS;
			if (cs_capture_slow() && fast < SLOW_FAST_MS)
				fast = SLOW_FAST_MS;            /* (the window server's copy: ~0.8 s of CPU each) */
			if ((int)(g_s.seq_sent - g_s.seq_acked) < g_s.window) {
				unsigned long t0 = cs_ms();
				int cr = 1, pk = 1;
				/* still: look at a quarter of the rows first, and capture
				   only if they have changed (the four quarters in turn, so
				   any change is seen within four looks) */
				if (g_s.peeking && !g_s.need_key && !pc_enc_pending(g_enc)) {
					pk = cs_capture_peek(g_s.peek_part, PEEK_PARTS);
					g_s.peek_part = (g_s.peek_part + 1) % PEEK_PARTS;
					if (g_s.peeks_timed < 4 && pk >= 0) {
						g_s.peeks_timed++;
						cs_log("cast: a look at the screen: %s, %lu ms", pk ? "changed" : "still", cs_ms() - t0);
					}
				}
				if (pk != 0)
					cr = cs_capture(g_frame);
				if (pk == 0) {
					/* (nothing seen: look again soon) */
				} else if (cr >= 0) {
					g_s.capture_fails = 0;
					/* (raw frame buffer unchanged: nothing to encode, unless a
					   keyframe is due or the last change did not all fit; else
					   pc_enc_frame itself says when nothing changed) */
					if (cr == 0 || g_s.need_key || pc_enc_pending(g_enc)) {
						r = send_update(t0);
						if (r < 0)
							return -1;
						if (r > 0)
							g_s.still_since = now;
						if (r > 0 && pc_enc_pending(g_enc))
							g_s.fast_until = now + 200;   /* (the rest of a big change: soon) */
					}
					if (g_s.timed < 8) {           /* what a capture costs, for the log */
						g_s.timed++;
						cs_log("cast: capture %s, %lu ms in all", cr ? "unchanged" : "read", cs_ms() - t0);
					}
				} else if (++g_s.capture_fails == 20) {
					cs_log("cast: the screen could not be read 20 times running");
				}
				g_s.stat_busy += cs_ms() - t0;
				/* still for 2 s: look rather than capture (cs_capture_peek;
				   the window server's copy cannot be looked at cheaply, so
				   that is captured every SLOW_STILL_MS) */
				if (now - g_s.still_since >= STILL_AFTER_MS && (long)(g_s.fast_until - now) <= 0 && pk != 0) {
					g_s.peeking = !cs_capture_slow();
					g_s.interval = g_s.peeking ? (fast > PEEK_MIN_MS ? fast : PEEK_MIN_MS) : SLOW_STILL_MS;
				} else if (pk != 0) {
					g_s.peeking = 0;
					g_s.interval = fast;
				}
			}
			else if (!g_s.ping_acks && now - g_s.last_ack > ACK_LOST_MS) {
				/* (an older Atom, whose PING does not say what it has applied:
				   window full and no ACK for a while - on a reliable link
				   that is an ACK lost to line noise - an UPDATE lost the
				   same way brings a KEYREQ - so take it as acknowledged) */
				cs_log("cast: no ACK for %lu..%lu in %d s: taken as acknowledged",
					g_s.seq_acked + 1, g_s.seq_sent, ACK_LOST_MS / 1000);
				g_s.seq_acked = g_s.seq_sent;
				g_s.last_ack = now;
			}
			/* (window full: the ACK brings next_cap forward) */
			g_s.next_cap = now + g_s.interval;
		}

		/* wait for the Atom, the app, the next capture or the next STATUS */
		until = (long)(g_s.last_status + STATUS_MS - now);
		if (g_s.welcomed && !g_s.paused && S()->viewers != 0) {
			long c = (long)(g_s.next_cap - now);
			if (c < until) until = c;
		}
		if (until > 1000 && !g_s.welcomed) until = 1000;
		if (until > 2000) until = 2000;
		wait = until < 0 ? 0 : (int)until;
		m = pg_wait(wait, 1, 1);
		if (m & 2)
			drain_app();
		if (m & 1) {
			int room = (int)sizeof(g_rx) - g_rx_len;
			int n = pwn_read(g_rx + g_rx_len, room, 0);
			if (n == 0 || (n < 0 && n != PWN_TIMEOUT && n != PWN_CANCEL)) {
				snprintf(g_s.why, sizeof(g_s.why), "the Atom closed the connection");
				return -1;
			}
			if (n > 0) {
				g_rx_len += n;
				parse(cs_ms());
			}
		}
	}
}

/* waits up to aMs for nothing but a stop (backing off between tries) */
static int pause_or_stop(int aMs)
{
	unsigned long start = cs_ms(), lp = start, la = start;
	for (;;) {
		unsigned long now = cs_ms();
		long left = (long)(start + aMs - now);
		cs_beat();
		if (should_stop(now, &lp, &la))
			return 1;
		if (left <= 0)
			return 0;
		if (pg_wait(left > 2000 ? 2000 : (int)left, 0, 1) & 2)
			drain_app();
	}
}

void cs_loop(void)
{
	char why[96];
	int backoff = BACKOFF_MIN_MS;
	CsShared *s = S();
	set_phase(CS_PHASE_STARTING);
	g_enc = pc_enc_new(CS_W, CS_H, LEVEL);
	if (!g_enc) {
		cs_log("cast: not enough memory for the encoder");
		s->stop_reason = CS_STOP_MEMORY;
		set_phase(CS_PHASE_STOPPED);
		return;
	}
	if (cs_capture_init() != 0) {
		s->stop_reason = CS_STOP_CAPTURE;
		set_phase(CS_PHASE_STOPPED);
		pc_enc_free(g_enc);
		return;
	}
	if (s->capture[0] == 0) {
		snprintf(s->capture, sizeof(s->capture), "%s", cs_capture_kind());
		s->eng_seq++;
	}
	cs_log("cast: the screen is read from the %s", cs_capture_kind());
	for (;;) {
		unsigned long t0;
		int r;
		if (quit_now())
			break;
		set_phase(CS_PHASE_CONNECTING);
		set_msg("Connecting to the Atom");
		why[0] = 0;
		if (pwn_connect(ATOM_HOST, ATOM_PORT, 0, why, sizeof(why)) != 0) {
			if (quit_now())
				break;
			set_phase(CS_PHASE_WAITING);
			set_msg("Atom not found - %s", why[0] ? why : "no connection");
			if (pause_or_stop(backoff))
				break;
			backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
			continue;
		}
		cs_log("cast: connected to the Atom");
		/* Psion Internet: no Nagle, so a small update or a STATUS is not
		   held back until the Atom (lwIP: delayed ACKs) acknowledges the
		   last segment - up to 250 ms a time */
		if (!pwn_modem()) {
			int nd = pg_net_nodelay();
			if (nd != 0)
				cs_log("cast: TCP no-delay not set (%d)", nd);
		}
		t0 = cs_ms();
		r = session();
		release_all();
		set_viewers(-1, 0);
		pwn_close(1);
		if (g_s.errors)
			cs_log("cast: %lu bad bytes or messages from the Atom (resynchronised)", g_s.errors);
		if (r == 0)
			break;
		if (cs_ms() - t0 > 30000)
			backoff = BACKOFF_MIN_MS;
		set_phase(CS_PHASE_WAITING);
		set_msg("%s", g_s.why[0] ? g_s.why : "the connection to the Atom was lost");
		if (pause_or_stop(backoff))
			break;
		backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
	}
	if (g_s.in_dropped)
		cs_log("cast: %lu input events not injected", g_s.in_dropped);
	pwn_release_now();
	pc_enc_free(g_enc);
	g_enc = 0;
	set_phase(CS_PHASE_STOPPED);
}
