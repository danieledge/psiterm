/* psigrey.h - the grey calibration PsiTerm, PsiMail and PsiWeb share.
 *
 * The 5mx shows 16 greys (EGray16: 0 black .. 15 white). Pictures, web
 * pages and terminal colours arrive as 8-bit greys (0..255) and have to
 * become one of those 16. Done naively (level = v * 15 / 255) that assumes
 * the 16 levels look evenly spaced, which a passive STN panel with
 * frame-rate greys does not do: see docs/display.md.
 *
 * The calibration is one table: lv[k], how light level k looks on this
 * screen, as an 8-bit grey (lv[0] < lv[1] < ... < lv[15]). From it:
 *   - plain conversion (text, UI, terminal colours) takes the level whose
 *     lv is nearest (psigrey_nearest);
 *   - error diffusion (pictures) takes the nearest level and passes on
 *     v - lv[level] as the error.
 * With lv[k] = 17k (linear, the calibration off) both are exactly what the
 * apps did before.
 *
 * Stored in C:\System\Data\PsiGrey.ini as short text lines, so the C
 * engines (ESTLIB) and the C++ apps read it alike:
 *     PsiGrey 1
 *     calibrate 1          the table on (1) or plain linear levels (0)
 *     dither 1             pictures: 1 error diffusion, 0 ordered (4x4)
 *     gamma 100            the calibration screen's settings (x100) ...
 *     curve 35
 *     bright 0
 *     nudge 0 0 ... 0      (16 values)
 *     reading 1 1          Reading mode: contrast notches up, light on
 *     table 0 12 26 ...    ... and the table they make (16 values)
 * Written by Preferences > Screen greys in any of the three (to a temporary
 * file, then renamed over the old one). A missing file, or one that does
 * not parse, means the standard settings. Readers need only "calibrate",
 * "dither" and "table": the table is computed by the writer (with pow),
 * so the engines need no floating point.
 *
 * Header-only C, for C and C++: static functions, const data only (an
 * EIKON .app may not have writable static data).
 */
#ifndef PSIGREY_H
#define PSIGREY_H

#define PSIGREY_FILE "C:\\System\\Data\\PsiGrey.ini"
#define PSIGREY_VERSION 1

typedef struct
	{
	int on;                  /* the calibration table in use (else linear) */
	int dither;              /* pictures: 1 error diffusion, 0 ordered */
	int gamma;               /* x100: 50..250 */
	int curve;               /* S-curve strength 0..100 */
	int bright;              /* -64..64: positive is lighter */
	int nudge[16];           /* per level, -64..64: positive looks lighter (maps lighter) */
	int read_contrast;       /* Reading mode: contrast notches up (0..4) */
	int read_light;          /* Reading mode: keep the backlight on */
	unsigned char lv[16];    /* the table (as computed from the above) */
	} PsiGrey;

/* The standard table: gamma 1.00, curve 35, brightness 0, no nudges,
   computed by tools/mkgrey.py (and PsiTerm, with the same formula) */
#define PSIGREY_STD_GAMMA 100
#define PSIGREY_STD_CURVE 35
#define PSIGREY_STD_LV { 0, 12, 26, 42, 60, 78, 98, 118, 137, 157, 177, 195, 213, 229, 243, 255 }

/* (inline: no "defined but not used" warnings where a file uses only some) */
#ifdef __GNUC__
#define PSIGREY_FN static __inline__
#else
#define PSIGREY_FN static
#endif

PSIGREY_FN void psigrey_linear(unsigned char *lv)
	{
	int k;
	for (k = 0; k < 16; k++) lv[k] = (unsigned char)(k * 17);
	}

/* the table made usable: strictly increasing within 0..255, so every
   level is reachable, and black and white stay black and white */
PSIGREY_FN void psigrey_fix(unsigned char *lv)
	{
	int k, v[16];
	for (k = 0; k < 16; k++)
		{
		v[k] = lv[k];
		if (v[k] < k) v[k] = k;
		if (v[k] > 240 + k) v[k] = 240 + k;
		if (k && v[k] <= v[k - 1]) v[k] = v[k - 1] + 1;
		}
	for (k = 0; k < 16; k++) lv[k] = (unsigned char)v[k];
	}

PSIGREY_FN void psigrey_defaults(PsiGrey *g)
	{
	static const unsigned char std_lv[16] = PSIGREY_STD_LV;
	int k;
	g->on = 1;
	g->dither = 1;
	g->gamma = PSIGREY_STD_GAMMA;
	g->curve = PSIGREY_STD_CURVE;
	g->bright = 0;
	for (k = 0; k < 16; k++) { g->nudge[k] = 0; g->lv[k] = std_lv[k]; }
	g->read_contrast = 1;
	g->read_light = 1;
	}

/* the table to use: the calibration, or linear levels when it is off */
PSIGREY_FN void psigrey_levels(const PsiGrey *g, unsigned char *lv)
	{
	int k;
	if (!g->on) { psigrey_linear(lv); return; }
	for (k = 0; k < 16; k++) lv[k] = g->lv[k];
	psigrey_fix(lv);
	}

/* cal[v] = the level whose lv is nearest v (ties go to the darker) */
PSIGREY_FN void psigrey_nearest(const unsigned char *lv, unsigned char *cal)
	{
	int v, k = 0;
	for (v = 0; v < 256; v++)
		{
		while (k < 15 && (int)lv[k + 1] - v < v - (int)lv[k]) k++;
		cal[v] = (unsigned char)k;
		}
	}

PSIGREY_FN int psigrey_clamp(int v, int lo, int hi)
	{
	return v < lo ? lo : v > hi ? hi : v;
	}

/* reads up to n numbers from s into out; returns how many */
PSIGREY_FN int psigrey_nums(const char *s, const char *end, int *out, int n)
	{
	int got = 0;
	while (got < n)
		{
		int neg = 0, v = 0, digits = 0;
		while (s < end && (*s == ' ' || *s == '\t')) s++;
		if (s < end && *s == '-') { neg = 1; s++; }
		while (s < end && *s >= '0' && *s <= '9' && digits < 6) { v = v * 10 + (*s - '0'); s++; digits++; }
		if (!digits) break;
		while (s < end && *s >= '0' && *s <= '9') s++;
		out[got++] = neg ? -v : v;
		}
	return got;
	}

PSIGREY_FN int psigrey_word(const char *s, const char *end, const char *w)
	{
	while (*w) { if (s >= end || *s != *w) return 0; s++; w++; }
	return s >= end || *s == ' ' || *s == '\t';
	}

/* parses the file's text into *g (the standard settings first, then each
   line that is understood). 1 if it was a PsiGrey file. Bounded: never
   reads past buf + len, and every value is clamped. */
PSIGREY_FN int psigrey_parse(PsiGrey *g, const char *buf, int len)
	{
	const char *p = buf, *end = buf + (len > 0 ? len : 0);
	int ok = 0, n[16], k, c;
	psigrey_defaults(g);
	while (p < end)
		{
		const char *e = p, *a;
		while (e < end && *e != '\n' && *e != '\r') e++;
		a = p;
		while (a < e && *a != ' ' && *a != '\t') a++;
		if (psigrey_word(p, e, "PsiGrey")) ok = 1;
		else if (psigrey_word(p, e, "calibrate") && psigrey_nums(a, e, n, 1) == 1) g->on = n[0] ? 1 : 0;
		else if (psigrey_word(p, e, "dither") && psigrey_nums(a, e, n, 1) == 1) g->dither = n[0] ? 1 : 0;
		else if (psigrey_word(p, e, "gamma") && psigrey_nums(a, e, n, 1) == 1) g->gamma = psigrey_clamp(n[0], 50, 250);
		else if (psigrey_word(p, e, "curve") && psigrey_nums(a, e, n, 1) == 1) g->curve = psigrey_clamp(n[0], 0, 100);
		else if (psigrey_word(p, e, "bright") && psigrey_nums(a, e, n, 1) == 1) g->bright = psigrey_clamp(n[0], -64, 64);
		else if (psigrey_word(p, e, "reading") && (c = psigrey_nums(a, e, n, 2)) >= 1)
			{
			g->read_contrast = psigrey_clamp(n[0], 0, 4);
			if (c == 2) g->read_light = n[1] ? 1 : 0;
			}
		else if (psigrey_word(p, e, "nudge") && psigrey_nums(a, e, n, 16) == 16)
			for (k = 0; k < 16; k++) g->nudge[k] = psigrey_clamp(n[k], -64, 64);
		else if (psigrey_word(p, e, "table") && psigrey_nums(a, e, n, 16) == 16)
			{
			for (k = 0; k < 16; k++) g->lv[k] = (unsigned char)psigrey_clamp(n[k], 0, 255);
			psigrey_fix(g->lv);
			}
		p = e;
		while (p < end && (*p == '\n' || *p == '\r')) p++;
		}
	if (!ok) psigrey_defaults(g);
	return ok;
	}

PSIGREY_FN int psigrey_put(char *b, int at, int max, const char *s)
	{
	while (*s && at < max - 1) b[at++] = *s++;
	return at;
	}

PSIGREY_FN int psigrey_putn(char *b, int at, int max, int v)
	{
	char t[8];
	int i = 0;
	if (v < 0) { at = psigrey_put(b, at, max, "-"); v = -v; }
	do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 7);
	while (i > 0 && at < max - 1) b[at++] = t[--i];
	return at;
	}

/* the file's text (b gets a terminator; max 400 is plenty). Returns the length */
PSIGREY_FN int psigrey_format(const PsiGrey *g, char *b, int max)
	{
	int at = 0, k;
	if (max <= 0) return 0;
	at = psigrey_put(b, at, max, "PsiGrey 1\r\ncalibrate ");
	at = psigrey_putn(b, at, max, g->on);
	at = psigrey_put(b, at, max, "\r\ndither ");
	at = psigrey_putn(b, at, max, g->dither);
	at = psigrey_put(b, at, max, "\r\ngamma ");
	at = psigrey_putn(b, at, max, g->gamma);
	at = psigrey_put(b, at, max, "\r\ncurve ");
	at = psigrey_putn(b, at, max, g->curve);
	at = psigrey_put(b, at, max, "\r\nbright ");
	at = psigrey_putn(b, at, max, g->bright);
	at = psigrey_put(b, at, max, "\r\nnudge");
	for (k = 0; k < 16; k++) { at = psigrey_put(b, at, max, " "); at = psigrey_putn(b, at, max, g->nudge[k]); }
	at = psigrey_put(b, at, max, "\r\nreading ");
	at = psigrey_putn(b, at, max, g->read_contrast);
	at = psigrey_put(b, at, max, " ");
	at = psigrey_putn(b, at, max, g->read_light);
	at = psigrey_put(b, at, max, "\r\ntable");
	for (k = 0; k < 16; k++) { at = psigrey_put(b, at, max, " "); at = psigrey_putn(b, at, max, g->lv[k]); }
	at = psigrey_put(b, at, max, "\r\n");
	b[at] = 0;
	return at;
	}

/* The table from the settings (the writer's job: it needs pow, which the
   caller passes, e.g. a wrapper round Math::Pow). The same formula as
   tools/mkgrey.py:
     x = k / 15;  s = x + c (x^2 (3 - 2x) - x)   (an S-curve, c = curve / 100)
     lv = round(255 s^gamma) - bright + nudge[k]
   then psigrey_fix. A positive nudge or brightness lowers lv: the level
   is taken to look darker, so v maps to a lighter one. */
PSIGREY_FN void psigrey_compute(PsiGrey *g, double (*powfn)(double, double))
	{
	int k;
	for (k = 0; k < 16; k++)
		{
		double x = k / 15.0, c = g->curve / 100.0, s;
		int v;
		s = x + c * (x * x * (3.0 - 2.0 * x) - x);
		if (s < 0) s = 0;
		if (s > 1) s = 1;
		if (g->gamma != 100 && s > 0) s = powfn(s, g->gamma / 100.0);
		v = (int)(255.0 * s + 0.5) - g->bright - g->nudge[k];
		g->lv[k] = (unsigned char)psigrey_clamp(v, 0, 255);
		}
	psigrey_fix(g->lv);
	}

#endif
