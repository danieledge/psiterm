// TERMSB.CPP - see termsb.h
#include "termsb.h"

void SbInit(TTermSb* aSb, TSbCell* aCells, short* aCols, int aMaxLines, int aMaxCols)
	{
	aSb->iCells = aCells;
	aSb->iCols = aCols;
	aSb->iMaxLines = aCells ? aMaxLines : 0;
	aSb->iMaxCols = aMaxCols;
	aSb->iHead = 0;
	aSb->iCount = 0;
	}

void SbClear(TTermSb* aSb)
	{
	aSb->iHead = 0;
	aSb->iCount = 0;
	}

void SbPush(TTermSb* aSb, const TSbCell* aLine, int aCols)
	{
	if (aSb->iMaxLines <= 0)
		return;
	if (aCols > aSb->iMaxCols)
		aCols = aSb->iMaxCols;
	if (aCols < 0)
		aCols = 0;
	// drop trailing blank cells: most lines are short
	while (aCols > 0 && (aLine[aCols - 1].iCh == 0 || aLine[aCols - 1].iCh == ' ')
		&& (aLine[aCols - 1].iGrey & 0x0f) == 15 && !(aLine[aCols - 1].iFlags & (KSbUnderline | KSbStrike)))
		aCols--;
	TSbCell* dst = aSb->iCells + aSb->iHead * aSb->iMaxCols;
	for (int i = 0; i < aCols; i++)
		dst[i] = aLine[i];
	aSb->iCols[aSb->iHead] = (short)aCols;
	aSb->iHead = (aSb->iHead + 1) % aSb->iMaxLines;
	if (aSb->iCount < aSb->iMaxLines)
		aSb->iCount++;
	}

const TSbCell* SbLine(const TTermSb* aSb, int aBack, int* aCols)
	{
	if (aBack < 1 || aBack > aSb->iCount)
		{
		*aCols = 0;
		return 0;
		}
	int slot = aSb->iHead - aBack;
	if (slot < 0)
		slot += aSb->iMaxLines;
	*aCols = aSb->iCols[slot];
	return aSb->iCells + slot * aSb->iMaxCols;
	}

void SbOrder(int* aLine0, int* aCol0, int* aLine1, int* aCol1)
	{
	if (*aLine1 < *aLine0 || (*aLine1 == *aLine0 && *aCol1 < *aCol0))
		{
		int t = *aLine0; *aLine0 = *aLine1; *aLine1 = t;
		t = *aCol0; *aCol0 = *aCol1; *aCol1 = t;
		}
	}

int SbInSelection(int aLine, int aCol, int aLine0, int aCol0, int aLine1, int aCol1)
	{
	if (aLine < aLine0 || aLine > aLine1)
		return 0;
	if (aLine == aLine0 && aCol < aCol0)
		return 0;
	if (aLine == aLine1 && aCol > aCol1)
		return 0;
	return 1;
	}

int SbSelectionText(TSbLineFn aFn, void* aCtx, TSbMapFn aMap,
	int aLine0, int aCol0, int aLine1, int aCol1,
	unsigned char* aOut, int aOutMax, unsigned char aNewline)
	{
	unsigned int chars[256];
	int n = 0;
	SbOrder(&aLine0, &aCol0, &aLine1, &aCol1);
	for (int line = aLine0; line <= aLine1; line++)
		{
		int cols = aFn(aCtx, line, chars, 256);
		int from = (line == aLine0) ? aCol0 : 0;
		int to = (line == aLine1) ? aCol1 + 1 : cols;
		if (to > cols)
			to = cols;
		int start = n;
		for (int c = from; c < to && n < aOutMax; c++)
			{
			unsigned int ch = chars[c];
			if (ch == (unsigned int)-1)
				continue;                     // right half of a wide character
			int b = (ch == 0) ? ' ' : aMap(ch);
			if (b > 0)
				aOut[n++] = (unsigned char)b;
			}
		while (n > start && aOut[n - 1] == ' ')
			n--;                              // trim trailing blanks
		if (line != aLine1 && n < aOutMax)
			aOut[n++] = aNewline;
		}
	return n;
	}
