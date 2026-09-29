/* nsprintf.c - vsnprintf/snprintf for EPOC R5, whose estlib has neither.
 *
 * C99 behaviour: output is truncated to 'size' (always terminated when
 * size > 0) and the return value is the length the full output would have,
 * so snprintf(NULL, 0, ...) measures. Supports flags - + space # 0, width
 * and precision (including *), lengths hh h l ll z j t L, and conversions
 * d i u o x X c s p n % f F e E g G. */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

typedef struct { char *p; size_t size; size_t n; } out_t;

static void put(out_t *o, char c)
{
	if (o->n + 1 < o->size) o->p[o->n] = c;
	o->n++;
}

static void puts_n(out_t *o, const char *s, size_t len)
{
	while (len--) put(o, *s++);
}

static void pad(out_t *o, char c, int n)
{
	while (n-- > 0) put(o, c);
}

enum { F_MINUS = 1, F_PLUS = 2, F_SPACE = 4, F_HASH = 8, F_ZERO = 16 };

static void emit_num(out_t *o, unsigned long long v, int neg, int base, int upper,
		int flags, int width, int prec)
{
	char buf[72];
	const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int len = 0, zeros, total;
	char sign = 0;
	const char *prefix = "";
	int plen = 0;

	if (prec < 0) prec = 1; else flags &= ~F_ZERO;
	while (v) {
		/* base is 8, 10 or 16: a shift is cheaper than division by 16/8 */
		if (base == 16) { buf[len++] = dig[v & 15]; v >>= 4; }
		else if (base == 8) { buf[len++] = dig[v & 7]; v >>= 3; }
		else { buf[len++] = dig[v % 10]; v /= 10; }
	}
	if (neg) sign = '-';
	else if (flags & F_PLUS) sign = '+';
	else if (flags & F_SPACE) sign = ' ';
	if ((flags & F_HASH) && base == 16 && len) { prefix = upper ? "0X" : "0x"; plen = 2; }
	if ((flags & F_HASH) && base == 8 && prec <= len) prec = len + 1;
	zeros = prec > len ? prec - len : 0;
	total = len + zeros + plen + (sign ? 1 : 0);
	if (!(flags & F_MINUS) && !(flags & F_ZERO)) pad(o, ' ', width - total);
	if (sign) put(o, sign);
	puts_n(o, prefix, plen);
	if (!(flags & F_MINUS) && (flags & F_ZERO)) pad(o, '0', width - total);
	pad(o, '0', zeros);
	while (len) put(o, buf[--len]);
	if (flags & F_MINUS) pad(o, ' ', width - total);
}

/* %f / %e / %g, good to about 15 significant digits */
static void emit_float(out_t *o, double v, char conv, int flags, int width, int prec)
{
	char buf[400];
	int n = 0, neg = 0, exp10 = 0, i, total;
	int upper = (conv == 'E' || conv == 'G' || conv == 'F');
	char c = (char)(conv | 0x20);
	char sign = 0;

	if (prec < 0) prec = 6;
	if (v != v) { puts_n(o, upper ? "NAN" : "nan", 3); return; }
	if (v < 0) { neg = 1; v = -v; }
	if (v > 1e300) { if (neg) put(o, '-'); puts_n(o, upper ? "INF" : "inf", 3); return; }

	if (c == 'g') {
		double t = v;
		int e = 0;
		if (prec == 0) prec = 1;
		if (t != 0) {
			while (t >= 10) { t /= 10; e++; }
			while (t < 1) { t *= 10; e--; }
		}
		if (e < -4 || e >= prec) { c = 'e'; prec--; }
		else { c = 'f'; prec = prec - 1 - e; }
		if (!(flags & F_HASH)) flags |= 0x100;     /* strip trailing zeros */
	}
	if (c == 'e' && v != 0) {
		while (v >= 10) { v /= 10; exp10++; }
		while (v < 1) { v *= 10; exp10--; }
	}
	/* round at 'prec' decimals */
	{
		double r = 0.5;
		for (i = 0; i < prec; i++) r /= 10;
		v += r;
		if (c == 'e' && v >= 10) { v /= 10; exp10++; }
	}
	/* integer part */
	{
		char ib[330];
		int il = 0;
		double ip = 1;
		while (ip * 10 <= v) ip *= 10;
		for (; ip >= 1; ip /= 10) {
			int d = (int)(v / ip);
			if (d > 9) d = 9;
			ib[il++] = (char)('0' + d);
			v -= d * ip;
			if (il >= 320) break;
		}
		if (!il) ib[il++] = '0';
		memcpy(buf, ib, il);
		n = il;
	}
	if (prec > 0 || (flags & F_HASH)) buf[n++] = '.';
	for (i = 0; i < prec && n < 380; i++) {
		int d;
		v *= 10;
		d = (int)v;
		if (d > 9) d = 9;
		buf[n++] = (char)('0' + d);
		v -= d;
	}
	if (flags & 0x100) {
		if (memchr(buf, '.', n)) {
			while (n && buf[n - 1] == '0') n--;
			if (n && buf[n - 1] == '.') n--;
		}
	}
	if (c == 'e') {
		int e = exp10 < 0 ? -exp10 : exp10;
		buf[n++] = upper ? 'E' : 'e';
		buf[n++] = exp10 < 0 ? '-' : '+';
		if (e >= 100) buf[n++] = (char)('0' + e / 100);
		buf[n++] = (char)('0' + (e / 10) % 10);
		buf[n++] = (char)('0' + e % 10);
	}
	if (neg) sign = '-';
	else if (flags & F_PLUS) sign = '+';
	else if (flags & F_SPACE) sign = ' ';
	total = n + (sign ? 1 : 0);
	if (!(flags & F_MINUS) && !(flags & F_ZERO)) pad(o, ' ', width - total);
	if (sign) put(o, sign);
	if (!(flags & F_MINUS) && (flags & F_ZERO)) pad(o, '0', width - total);
	puts_n(o, buf, n);
	if (flags & F_MINUS) pad(o, ' ', width - total);
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
	out_t o;
	o.p = str; o.size = str ? size : 0; o.n = 0;

	while (*fmt) {
		int flags = 0, width = 0, prec = -1, lng = 0;
		char conv;
		if (*fmt != '%') { put(&o, *fmt++); continue; }
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-') flags |= F_MINUS;
			else if (*fmt == '+') flags |= F_PLUS;
			else if (*fmt == ' ') flags |= F_SPACE;
			else if (*fmt == '#') flags |= F_HASH;
			else if (*fmt == '0') flags |= F_ZERO;
			else break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			if (width < 0) { flags |= F_MINUS; width = -width; }
			fmt++;
		} else while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
			else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
		}
		/* length: 0 int, 1 long, 2 long long, -1 short, -2 char */
		for (;; fmt++) {
			if (*fmt == 'l') lng = lng == 1 ? 2 : 1;
			else if (*fmt == 'h') lng = lng == -1 ? -2 : -1;
			else if (*fmt == 'z' || *fmt == 't' || *fmt == 'I') lng = 1;
			else if (*fmt == 'j' || *fmt == 'q') lng = 2;
			else if (*fmt == 'L') lng = 3;
			else break;
		}
		conv = *fmt ? *fmt++ : 0;
		switch (conv) {
		case 'd': case 'i': {
			long long v;
			if (lng == 2) v = va_arg(ap, long long);
			else if (lng == 1) v = va_arg(ap, long);
			else v = va_arg(ap, int);
			if (lng == -1) v = (short)v;
			if (lng == -2) v = (signed char)v;
			if (v < 0) emit_num(&o, (unsigned long long)(-(v + 1)) + 1, 1, 10, 0, flags, width, prec);
			else emit_num(&o, (unsigned long long)v, 0, 10, 0, flags, width, prec);
			break;
		}
		case 'u': case 'x': case 'X': case 'o': {
			unsigned long long v;
			if (lng == 2) v = va_arg(ap, unsigned long long);
			else if (lng == 1) v = va_arg(ap, unsigned long);
			else v = va_arg(ap, unsigned int);
			if (lng == -1) v = (unsigned short)v;
			if (lng == -2) v = (unsigned char)v;
			emit_num(&o, v, 0, conv == 'u' ? 10 : conv == 'o' ? 8 : 16, conv == 'X',
					flags & ~(F_PLUS | F_SPACE), width, prec);
			break;
		}
		case 'p':
			emit_num(&o, (unsigned long)va_arg(ap, void *), 0, 16, 0, F_HASH, width, -1);
			break;
		case 'c':
			if (!(flags & F_MINUS)) pad(&o, ' ', width - 1);
			put(&o, (char)va_arg(ap, int));
			if (flags & F_MINUS) pad(&o, ' ', width - 1);
			break;
		case 's': {
			const char *s = va_arg(ap, const char *);
			size_t len;
			if (!s) s = "(null)";
			if (prec >= 0) {
				const char *e = memchr(s, 0, (size_t)prec);
				len = e ? (size_t)(e - s) : (size_t)prec;
			} else len = strlen(s);
			if (!(flags & F_MINUS)) pad(&o, ' ', width - (int)len);
			puts_n(&o, s, len);
			if (flags & F_MINUS) pad(&o, ' ', width - (int)len);
			break;
		}
		case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
			double v = lng == 3 ? (double)va_arg(ap, long double) : va_arg(ap, double);
			emit_float(&o, v, conv, flags, width, prec);
			break;
		}
		case 'n':
			*va_arg(ap, int *) = (int)o.n;
			break;
		case '%':
			put(&o, '%');
			break;
		default:
			put(&o, '%');
			if (conv) put(&o, conv);
			break;
		}
	}
	if (o.size > 0)
		o.p[o.n < o.size ? o.n : o.size - 1] = 0;
	return (int)o.n;
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;
	va_start(ap, fmt);
	n = vsnprintf(str, size, fmt, ap);
	va_end(ap);
	return n;
}
