// GLYPHS.CPP - mapping modern Unicode output onto what the Psion can draw
//
// The 5mx's fonts use Windows code page 1252. Anything outside that is
// either drawn by hand (box drawing, blocks, braille) or replaced with the
// closest ASCII look-alike.

#include <e32std.h>
#include "psiterm.h"

// Box drawing U+2500..U+257F: segment bits (generated from Unicode names)
static const TUint8 KBoxTable[128] =
	{
#include "boxtable.inc"
	};

// cp1252 0x80..0x9F -> Unicode (0 = undefined)
static const TUint16 KCp1252High[32] =
	{
	0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
	0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178
	};

struct TGlyphFallback
	{
	TUint16 iCode;
	TUint8 iByte;
	};

// Common symbols used by modern CLI tools (Claude Code, git, npm, htop...)
static const TGlyphFallback KFallbacks[] =
	{
	{ 0x25CF, 0x95 }, { 0x23FA, 0x95 }, { 0x25C9, 0x95 }, { 0x2B24, 0x95 },  // filled circles -> bullet
	{ 0x2219, 0xB7 }, { 0x22C5, 0xB7 }, { 0x2027, 0xB7 },                    // dots -> middle dot
	{ 0x25CB, 'o' }, { 0x25EF, 'o' }, { 0x25E6, 'o' }, { 0x25CC, 'o' },
	{ 0x25A0, '#' }, { 0x25A1, '#' }, { 0x25AA, 0xB7 }, { 0x25AB, 0xB7 },
	{ 0x25C6, '*' }, { 0x25C7, '*' }, { 0x2666, '*' },
	{ 0x2713, 'v' }, { 0x2714, 'v' }, { 0x221A, 'v' },
	{ 0x2717, 'x' }, { 0x2718, 'x' }, { 0x2715, 'x' }, { 0x2716, 'x' }, { 0x274C, 'x' },
	{ 0x2192, '>' }, { 0x21D2, '>' }, { 0x279C, '>' }, { 0x2794, '>' }, { 0x27A4, '>' },
	{ 0x2190, '<' }, { 0x21D0, '<' }, { 0x2191, '^' }, { 0x2193, 'v' }, { 0x2194, '-' },
	{ 0x21B5, '<' }, { 0x23CE, '<' }, { 0x232B, '<' }, { 0x21B3, 'L' },
	{ 0x276F, '>' }, { 0x203A, 0x9B }, { 0x25B6, '>' }, { 0x25BA, '>' }, { 0x25B8, '>' },
	{ 0x23F5, '>' }, { 0x25B7, '>' }, { 0x276E, '<' }, { 0x25C0, '<' }, { 0x25C2, '<' },
	{ 0x25B2, '^' }, { 0x25B4, '^' }, { 0x25BC, 'v' }, { 0x25BE, 'v' },
	{ 0x273B, '*' }, { 0x2736, '*' }, { 0x2733, '*' }, { 0x2722, '*' }, { 0x273D, '*' },
	{ 0x274B, '*' }, { 0x2217, '*' }, { 0x204E, '*' }, { 0x2731, '*' }, { 0x2738, '*' },
	{ 0x2739, '*' }, { 0x273A, '*' }, { 0x2605, '*' }, { 0x2606, '*' }, { 0x2728, '*' },
	{ 0x26A0, '!' }, { 0x2139, 'i' }, { 0x2261, '=' }, { 0x2248, '~' },
	{ 0x2264, '<' }, { 0x2265, '>' }, { 0x2260, '#' }, { 0x22EF, 0x85 }, { 0x2318, '#' },
	{ 0x2423, '_' }, { 0x2012, '-' }, { 0x2015, '-' }, { 0x2212, '-' }, { 0x2010, '-' },
	{ 0x2011, '-' }, { 0x2032, '\'' }, { 0x2033, '"' }, { 0x00A0, ' ' }, { 0x2003, ' ' },
	{ 0x2002, ' ' }, { 0x2009, ' ' }, { 0x200B, 0 }, { 0x2044, '/' }, { 0x2215, '/' },
	{ 0x2571, '/' }, { 0x2572, '\\' }, { 0x2573, 'X' }, { 0x00D7, 0xD7 }, { 0x2022, 0x95 },
	{ 0x26A1, '!' }, { 0x2699, '*' }, { 0x2630, '=' }, { 0x2261, '=' }
	};

TInt PsiMapToCodePage(TUint aCode)
	{
	if (aCode >= 0x20 && aCode < 0x7F)
		return aCode;
	if (aCode >= 0xA0 && aCode <= 0xFF)
		return aCode;
	TInt i;
	for (i = 0; i < 32; i++)
		{
		if (KCp1252High[i] == aCode && aCode != 0)
			return 0x80 + i;
		}
	const TInt n = sizeof(KFallbacks) / sizeof(KFallbacks[0]);
	for (i = 0; i < n; i++)
		{
		if (KFallbacks[i].iCode == aCode)
			return KFallbacks[i].iByte;
		}
	return -1;
	}

TUint PsiCodePageToUnicode(TUint aByte)
	{
	if (aByte >= 0x80 && aByte < 0xA0)
		{
		TUint u = KCp1252High[aByte - 0x80];
		return u ? u : '?';
		}
	return aByte;
	}

TInt PsiBoxSegments(TUint aCode)
	{
	if (aCode == 0x23BF)             // "⎿" used by Claude Code = corner up-right
		return KBoxUp | KBoxRight;
	if (aCode == 0x23BE)
		return KBoxDown | KBoxRight;
	if (aCode >= 0x2500 && aCode < 0x2580)
		{
		TInt v = KBoxTable[aCode - 0x2500];
		return v ? v : -1;
		}
	return -1;
	}
