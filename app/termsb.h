// TERMSB.H - scrollback history and selection-to-text for PsiTerm.
// Plain C++ with no EPOC types so it can be unit-tested on a PC.
// Memory is supplied by the caller (a .app may not have writable statics).
#ifndef TERMSB_H
#define TERMSB_H

struct TSbCell
	{
	unsigned short iCh;       // Unicode code point (0 = empty)
	unsigned char iGrey;      // fg grey << 4 | bg grey
	unsigned char iFlags;     // KSbBold | KSbUnderline | KSbStrike | KSbWide
	};

enum { KSbBold = 1, KSbUnderline = 2, KSbStrike = 4, KSbWide = 8 };

struct TTermSb
	{
	TSbCell* iCells;          // iMaxLines * iMaxCols
	short* iCols;             // width of each stored line
	int iMaxLines;
	int iMaxCols;
	int iHead;                // slot the next line goes into
	int iCount;               // lines stored (<= iMaxLines)
	};

void SbInit(TTermSb* aSb, TSbCell* aCells, short* aCols, int aMaxLines, int aMaxCols);
void SbClear(TTermSb* aSb);
void SbPush(TTermSb* aSb, const TSbCell* aLine, int aCols);
// aBack = 1 is the most recent line pushed. Returns 0 if out of range.
const TSbCell* SbLine(const TTermSb* aSb, int aBack, int* aCols);

// Selection -> text. Lines are addressed by an absolute line number; the
// callback fills aChars with that line's code points and returns its width.
typedef int (*TSbLineFn)(void* aCtx, int aLine, unsigned int* aChars, int aMaxCols);
// Converts a code point to one output byte (0 = drop it).
typedef int (*TSbMapFn)(unsigned int aCh);
// (aLine0,aCol0)..(aLine1,aCol1) inclusive, in any order. Trailing spaces are
// trimmed from each line and lines are separated by aNewline. Returns the
// number of bytes written to aOut (never more than aOutMax).
int SbSelectionText(TSbLineFn aFn, void* aCtx, TSbMapFn aMap,
	int aLine0, int aCol0, int aLine1, int aCol1,
	unsigned char* aOut, int aOutMax, unsigned char aNewline);

// Normalises a selection so (aLine0,aCol0) comes first.
void SbOrder(int* aLine0, int* aCol0, int* aLine1, int* aCol1);
// Is (aLine,aCol) inside the (ordered) selection?
int SbInSelection(int aLine, int aCol, int aLine0, int aCol0, int aLine1, int aCol1);

#endif
