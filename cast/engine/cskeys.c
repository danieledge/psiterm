/* cskeys.c - a typed character (INPUT kind 6) as key presses on the
 * Psion Series 5mx's UK keyboard.
 *
 * ER5 can only be given raw scan codes (UserSvr::AddEvent), and which key
 * makes "@" or "-" is the keyboard's business, so the viewer sends the
 * character and the Psion turns it into a matrix key and the modifier held
 * with it. The table is the one tools/emu/net.py types with, checked key by
 * key against the 5mx ROM (1.05): "@" is Fn+4 and "~" Shift+' on this ROM,
 * the other way round from some published maps.
 *
 * Characters without a key here (accented letters, which the 5mx makes
 * with its Special character dialog, "`", "|", the euro) are dropped:
 * cs_char_keys says 0.
 */
#include "cs.h"

#define SHIFT  0x12        /* EStdKeyLeftShift */
#define FN     0x18        /* EStdKeyLeftFunc */

typedef struct { unsigned char ch, mods, scan; } KeyOf;

static const KeyOf KEYS[] = {
	{ ',', 0, 0x79 }, { '.', 0, 0x7a }, { '\'', 0, 0x7e },
	{ '!', SHIFT, '1' }, { '"', SHIFT, '2' }, { 0xa3, SHIFT, '3' },   /* the pound sign */
	{ '$', SHIFT, '4' }, { '%', SHIFT, '5' }, { '^', SHIFT, '6' }, { '&', SHIFT, '7' },
	{ '*', SHIFT, '8' }, { '(', SHIFT, '9' }, { ')', SHIFT, '0' },
	{ '~', SHIFT, 0x7e }, { '?', SHIFT, 0x7a }, { '/', SHIFT, 0x79 },
	{ '_', FN, '1' }, { '#', FN, '2' }, { '\\', FN, '3' }, { '@', FN, '4' },
	{ '<', FN, '5' }, { '>', FN, '6' }, { '[', FN, '7' }, { ']', FN, '8' },
	{ '{', FN, '9' }, { '}', FN, '0' },
	{ ':', FN, 0x7e }, { ';', FN, 'L' }, { '=', FN, 'P' }, { '-', FN, 'O' }, { '+', FN, 'I' },
	{ 0, 0, 0 }
};

int cs_char_keys(int aChar, int *aMods, int *aScan)
{
	int i;
	*aMods = 0;
	*aScan = 0;
	if (aChar >= 'a' && aChar <= 'z') {
		*aScan = aChar - 32;
		return 1;
	}
	if (aChar >= 'A' && aChar <= 'Z') {
		*aMods = SHIFT;
		*aScan = aChar;
		return 1;
	}
	if (aChar >= '0' && aChar <= '9') {
		*aScan = aChar;
		return 1;
	}
	switch (aChar) {
	case ' ':  *aScan = 0x05; return 1;          /* EStdKeySpace */
	case '\r':
	case '\n': *aScan = 0x03; return 1;          /* EStdKeyEnter */
	case '\t': *aScan = 0x02; return 1;          /* EStdKeyTab */
	case 8:    *aScan = 0x01; return 1;          /* EStdKeyBackspace: the Psion's Del */
	case 27:   *aScan = 0x04; return 1;          /* EStdKeyEscape */
	}
	for (i = 0; KEYS[i].ch; i++)
		if (KEYS[i].ch == aChar) {
			*aMods = KEYS[i].mods;
			*aScan = KEYS[i].scan;
			return 1;
		}
	return 0;
}
