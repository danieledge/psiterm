/* pmquote.cpp - a message's text without PsiMail's block codes, for
 * quoting in a reply or forwarding (see pmui.h for the codes). Portable
 * C++: no EIKON, no static data. */
#include "pmui.h"

/* The body without the block codes: for quoting in replies ("> ") and for
   forwarding. Lists keep their markers, quotes get their '>'s back. */
int pm_plain_text(const char* t, int len, char* out, int max, int quote)
	{
	int p = 0, k = 0;
	while (p < len && !(t[p] == '\n' && (p == 0 || t[p - 1] == '\n'))) p++;
	p++;
	while (p < len && k < max - 8)
		{
		int e = p;
		while (e < len && t[e] != '\n') e++;
		int q = p, depth = 0, skip = 0;
		const char* marker = 0;
		int ml = 0;
		if ((unsigned char)t[p] == 0x01 && p + 1 < e)
			{
			char kd = t[p + 1];
			q = p + 2;
			if (kd == 'h') q++;
			else if (kd == 'q') { depth = t[q] - '0'; q++; }
			else if (kd == 'l')
				{
				q++;
				marker = t + q;
				while (q < e && t[q] != 0x02) q++;
				ml = (int)(t + q - marker);
				if (q < e) q++;
				}
			else if (kd == 'u') skip = 1;
			else if (kd == 'r') { marker = "----"; ml = 4; }
			else if (kd == 'i')
				{
				/* "\x01i" alt "\x02" src: the words only */
				int a = q;
				while (a < e && t[a] != 0x02) a++;
				if (a == q) { marker = "picture"; ml = 7; }
				e = a;
				if (k < max - 2) out[k++] = '[';
				}
			while (q < e && t[q] == ' ' && kd != 'c') q++;
			}
		if (!skip)
			{
			if (quote && k < max - 2) { out[k++] = '>'; if (!depth) out[k++] = ' '; }
			for (int i = 0; i < depth && k < max - 2; i++) out[k++] = '>';
			if (depth && k < max - 1) out[k++] = ' ';
			for (int i = 0; i < ml && k < max - 2; i++) out[k++] = (unsigned char)marker[i] == 0x95 ? '*' : marker[i];
			if (ml && k < max - 1) out[k++] = ' ';
			for (int i = q; i < e && k < max - 2; i++)
				{
				unsigned char c = (unsigned char)t[i];
				if (c == 0x15)
					{
					i++;
					while (i < e && t[i] >= '0' && t[i] <= '9') i++;
					continue;                      /* (i now at \x16: skipped by the loop) */
					}
				if (c >= 0x11 && c <= 0x17) continue;
				if (c == 0x01 || c == 0x02) continue;
				out[k++] = (char)c;
				}
			if ((unsigned char)t[p] == 0x01 && p + 1 < e && t[p + 1] == 'i' && k < max - 2) out[k++] = ']';
			out[k++] = '\n';
			}
		while (e < len && t[e] != '\n') e++;       /* (an 'i' line was cut at its address) */
		p = e + 1;
		}
	return k;
	}
