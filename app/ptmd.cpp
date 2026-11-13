// PTMD.CPP - Markdown, formatted, for the editor (1.02)
//
// A .md file opens formatted, as a modern editor's preview: headings in
// larger bold type, **bold**, *italic*, `code` on a rounded grey, code blocks
// on a shaded paragraph, lists with bullets, numbered lists, quotes with a
// bar at their left, rules, [links](...) underlined. Built into the editor's
// rich text from the source, which View > Formatted (Ctrl+P) goes back to
// for editing. Each formatted paragraph remembers the source line it came
// from, so the cursor goes to the same place.
//
// The common Markdown, line by line - not every corner of CommonMark: tables
// stay as written (in the fixed-width font), HTML is shown as text.

#include "psiterm.h"
#include <txtrich.h>
#include <txtfrmat.h>
#include <txtetext.h>
#include <gdi.h>

enum { KSpBold = 1, KSpItalic = 2, KSpCode = 4, KSpLink = 8, KSpFaint = 16 };

struct TMdSpan
	{
	TInt iStart, iLen;
	TUint iStyle;
	};

const TInt KMaxSpans = 48;

// Inline: **bold**, __bold__, *italic*, _italic_, `code`, [text](url),
// ![alt](url), \escapes. aOut gets the text as shown; aSpans the styled runs.
static TInt Inline(const TDesC& aIn, TDes& aOut, TMdSpan* aSpans)
	{
	TInt n = 0;
	TUint style = 0;
	TInt runStart = 0;
	aOut.Zero();
	TInt len = aIn.Length();
	// a run of the current style ends here
	#define CLOSE_RUN() if (style && aOut.Length() > runStart && n < KMaxSpans) \
		{ aSpans[n].iStart = runStart; aSpans[n].iLen = aOut.Length() - runStart; aSpans[n].iStyle = style; n++; }
	for (TInt i = 0; i < len && aOut.Length() < aOut.MaxLength() - 2; i++)
		{
		TText c = aIn[i];
		TText next = i + 1 < len ? aIn[i + 1] : 0;
		if (c == '\\' && next && next != ' ')
			{
			aOut.Append(next);
			i++;
			continue;
			}
		if (c == '`')
			{
			TInt close = aIn.Mid(i + 1).Locate('`');
			if (close >= 0)
				{
				CLOSE_RUN();
				TInt s = aOut.Length();
				TPtrC code(aIn.Mid(i + 1, close));
				aOut.Append(code.Left(code.Length() < aOut.MaxLength() - aOut.Length() - 2 ? code.Length() : aOut.MaxLength() - aOut.Length() - 2));
				if (n < KMaxSpans)
					{
					aSpans[n].iStart = s;
					aSpans[n].iLen = aOut.Length() - s;
					aSpans[n].iStyle = KSpCode;
					n++;
					}
				runStart = aOut.Length();
				i += close + 1;
				continue;
				}
			}
		if ((c == '*' || c == '_') && next == c)
			{
			// (a closing pair must follow for an opening one)
			if ((style & KSpBold) || aIn.Mid(i + 2).Find(c == '*' ? _L("**") : _L("__")) >= 0)
				{
				CLOSE_RUN();
				style ^= KSpBold;
				runStart = aOut.Length();
				i++;
				continue;
				}
			}
		if (c == '*' || c == '_')
			{
			TBool prevWord = i > 0 && TChar(aIn[i - 1]).IsAlphaDigit();
			TBool nextWord = next && TChar(next).IsAlphaDigit();
			// _ inside a word (snake_case) is just a letter
			TBool canOpen = !(style & KSpItalic) && next && next != ' ' && (c == '*' || !prevWord)
				&& aIn.Mid(i + 1).Locate(c) >= 0;
			TBool canClose = (style & KSpItalic) && i > 0 && aIn[i - 1] != ' ' && (c == '*' || !nextWord);
			if (canOpen || canClose)
				{
				CLOSE_RUN();
				style ^= KSpItalic;
				runStart = aOut.Length();
				continue;
				}
			}
		if ((c == '[' || (c == '!' && next == '[')))
			{
			TInt open = c == '!' ? i + 1 : i;
			TInt close = aIn.Mid(open).Locate(']');
			if (close > 0 && open + close + 1 < len && aIn[open + close + 1] == '(')
				{
				TInt paren = aIn.Mid(open + close + 1).Locate(')');
				if (paren > 0)
					{
					CLOSE_RUN();
					TInt s = aOut.Length();
					TPtrC text(aIn.Mid(open + 1, close - 1));
					if (c == '!')
						aOut.Append('[');
					aOut.Append(text.Left(text.Length() < aOut.MaxLength() - aOut.Length() - 4 ? text.Length() : 0));
					if (c == '!')
						aOut.Append(']');
					if (n < KMaxSpans)
						{
						aSpans[n].iStart = s;
						aSpans[n].iLen = aOut.Length() - s;
						aSpans[n].iStyle = (c == '!' ? KSpFaint : KSpLink) | style;
						n++;
						}
					runStart = aOut.Length();
					i = open + close + 1 + paren;
					continue;
					}
				}
			}
		aOut.Append(c);
		}
	CLOSE_RUN();
	#undef CLOSE_RUN
	return n;
	}

// ---- building the rich text ----------------------------------------------

class TMdBuilder
	{
public:
	TMdBuilder(CRichText& aRt, const TPtMdStyle& aStyle, CArrayFix<TInt>& aMap)
		: iRt(aRt), iStyle(aStyle), iMap(aMap), iPos(0) {}
	enum TKind { EBody, EHead1, EHead2, EHead3, EBullet, ENumber, EQuote, ECode, ERule, ETable };
	void ParaL(TKind aKind, const TDesC& aText, TInt aSrcLine, TInt aLevel = 0, TBool aFirstCode = EFalse,
		TBool aLastCode = EFalse);
private:
	void CharL(TInt aPos, TInt aLen, TUint aStyle, TInt aPx, TBool aBold, TRgb aInk);
	CRichText& iRt;
	const TPtMdStyle& iStyle;
	CArrayFix<TInt>& iMap;
	TInt iPos;
	};

void TMdBuilder::CharL(TInt aPos, TInt aLen, TUint aStyle, TInt aPx, TBool aBold, TRgb aInk)
	{
	if (aLen <= 0)
		return;
	TCharFormat cf;
	TCharFormatMask cm;
	TBool code = (aStyle & KSpCode) != 0;
	cf.iFontSpec.iTypeface.iName = code ? iStyle.iMono : iStyle.iBody;
	cf.iFontSpec.iHeight = iStyle.iDevice->VerticalPixelsToTwips(code ? iStyle.iMonoPx : aPx);
	cf.iFontSpec.iFontStyle.SetStrokeWeight((aBold || (aStyle & KSpBold)) ? EStrokeWeightBold : EStrokeWeightNormal);
	cf.iFontSpec.iFontStyle.SetPosture((aStyle & KSpItalic) ? EPostureItalic : EPostureUpright);
	cf.iFontPresentation.iTextColor = (aStyle & KSpLink) ? iStyle.iLink : (aStyle & KSpFaint) ? iStyle.iFaint : aInk;
	cf.iFontPresentation.iUnderline = (aStyle & KSpLink) ? EUnderlineOn : EUnderlineOff;
	cf.iFontPresentation.iHighlightStyle = code && !(aStyle & 0x100) ? TFontPresentation::EFontHighlightRounded
		: TFontPresentation::EFontHighlightNone;
	cf.iFontPresentation.iHighlightColor = iStyle.iCodeBack;
	cm.SetAttrib(EAttFontTypeface);
	cm.SetAttrib(EAttFontHeight);
	cm.SetAttrib(EAttFontStrokeWeight);
	cm.SetAttrib(EAttFontPosture);
	cm.SetAttrib(EAttColor);
	cm.SetAttrib(EAttFontUnderline);
	cm.SetAttrib(EAttFontHighlightStyle);
	cm.SetAttrib(EAttFontHighlightColor);
	iRt.ApplyCharFormatL(cf, cm, aPos, aLen);
	}

void TMdBuilder::ParaL(TKind aKind, const TDesC& aText, TInt aSrcLine, TInt aLevel, TBool aFirstCode, TBool aLastCode)
	{
	TBuf<512> shown;
	TMdSpan spans[KMaxSpans];
	TInt nspans = 0;
	if (aKind == ECode || aKind == ETable || aKind == ERule)
		shown.Copy(aText.Left(aText.Length() < shown.MaxLength() ? aText.Length() : shown.MaxLength()));
	else
		nspans = Inline(aText, shown, spans);
	if (aKind == ENumber)
		{
		// "3. " is kept from the source (the number is the writer's)
		}
	TInt start = iPos;
	if (shown.Length())
		iRt.InsertL(iPos, shown);
	iPos += shown.Length();
	iRt.InsertL(iPos, CEditableText::EParagraphDelimiter);
	iPos++;
	iMap.AppendL(aSrcLine);
	// the paragraph
	CParaFormat* pf = CParaFormat::NewLC();
	TParaFormatMask pm;
	TInt tw = iStyle.iDevice->HorizontalPixelsToTwips(1);
	TInt body = iStyle.iBodyPx;
	pf->iSpaceBeforeInTwips = 0;
	pf->iSpaceAfterInTwips = iStyle.iDevice->VerticalPixelsToTwips(body / 2);
	pf->iLeftMarginInTwips = tw * 4;
	pf->iRightMarginInTwips = tw * 4;
	pf->iIndentInTwips = 0;
	pf->iFillColor = iStyle.iPaper;
	TInt px = body;
	TBool bold = EFalse;
	TRgb ink = iStyle.iInk;
	switch (aKind)
		{
	case EHead1:
	case EHead2:
	case EHead3:
		{
		px = aKind == EHead1 ? body * 16 / 10 : aKind == EHead2 ? body * 13 / 10 : body * 11 / 10;
		bold = ETrue;
		pf->iSpaceBeforeInTwips = iStyle.iDevice->VerticalPixelsToTwips(body * 2 / 3);
		pf->iSpaceAfterInTwips = iStyle.iDevice->VerticalPixelsToTwips(body / 3);
		if (aKind != EHead3)
			{
			// (as GitHub's: a thin line under the larger headings)
			TParaBorder b;
			b.iLineStyle = TParaBorder::ESolid;
			b.iThickness = tw;
			b.iColor = iStyle.iRule;
			b.iAutoColor = EFalse;
			pf->SetParaBorderL(CParaFormat::EParaBorderBottom, b);
			pf->iBorderMarginInTwips = tw * 3;
			pm.SetAttrib(EAttBottomBorder);
			pm.SetAttrib(EAttBorderMargin);
			}
		if (aKind == EHead1)
			ink = iStyle.iHead;
		break;
		}
	case EBullet:
		{
		pf->iLeftMarginInTwips = tw * (6 + 14 * (aLevel + 1));
		pf->iSpaceAfterInTwips = iStyle.iDevice->VerticalPixelsToTwips(body / 6);
		TBullet* b = new(ELeave) TBullet;
		b->iCharacterCode = aLevel ? 0x96 : 0x95;     // (cp1252: a bullet; a dash below it)
		b->iHeightInTwips = iStyle.iDevice->VerticalPixelsToTwips(body);
		b->iTypeface.iName = iStyle.iBody;
		b->iHangingIndent = ETrue;
		b->iColor = iStyle.iSoft;
		pf->iBullet = b;
		pf->iIndentInTwips = -tw * 12;
		pm.SetAttrib(EAttBullet);
		break;
		}
	case ENumber:
		pf->iLeftMarginInTwips = tw * (6 + 16 * (aLevel + 1));
		pf->iIndentInTwips = -tw * 16;
		pf->iSpaceAfterInTwips = iStyle.iDevice->VerticalPixelsToTwips(body / 6);
		break;
	case EQuote:
		{
		TParaBorder b;
		b.iLineStyle = TParaBorder::ESolid;
		b.iThickness = tw * 3;
		b.iColor = iStyle.iRule;
		b.iAutoColor = EFalse;
		pf->SetParaBorderL(CParaFormat::EParaBorderLeft, b);
		pf->iBorderMarginInTwips = tw * 6;
		pf->iLeftMarginInTwips = tw * 14;
		pm.SetAttrib(EAttLeftBorder);
		pm.SetAttrib(EAttBorderMargin);
		ink = iStyle.iSoft;
		break;
		}
	case ECode:
	case ETable:
		pf->iFillColor = aKind == ECode ? iStyle.iCodeBack : iStyle.iPaper;
		pf->iLeftMarginInTwips = tw * 10;
		pf->iSpaceAfterInTwips = (aLastCode || aKind == ETable) ? iStyle.iDevice->VerticalPixelsToTwips(body / 2) : 0;
		pf->iSpaceBeforeInTwips = aFirstCode ? iStyle.iDevice->VerticalPixelsToTwips(2) : 0;
		break;
	case ERule:
		{
		TParaBorder b;
		b.iLineStyle = TParaBorder::ESolid;
		b.iThickness = tw;
		b.iColor = iStyle.iRule;
		b.iAutoColor = EFalse;
		pf->SetParaBorderL(CParaFormat::EParaBorderBottom, b);
		pm.SetAttrib(EAttBottomBorder);
		break;
		}
	default:
		break;
		}
	pm.SetAttrib(EAttSpaceBefore);
	pm.SetAttrib(EAttSpaceAfter);
	pm.SetAttrib(EAttLeftMargin);
	pm.SetAttrib(EAttRightMargin);
	pm.SetAttrib(EAttIndent);
	pm.SetAttrib(EAttFillColor);
	iRt.ApplyParaFormatL(pf, pm, start, iPos - start);
	CleanupStack::PopAndDestroy();
	// the text: the paragraph's own look, then the inline runs over it
	TInt len = shown.Length();
	if (aKind == ECode || aKind == ETable)
		CharL(start, len, KSpCode | 0x100, px, EFalse, ink);   // (0x100: no highlight - the paragraph is shaded)
	else
		{
		CharL(start, len, 0, px, bold, ink);
		for (TInt i = 0; i < nspans; i++)
			CharL(start + spans[i].iStart, spans[i].iLen, spans[i].iStyle, px, bold, ink);
		}
	}

// Leading spaces, for a list's level
static TInt Indent(const TDesC& aLine)
	{
	TInt i = 0;
	while (i < aLine.Length() && (aLine[i] == ' ' || aLine[i] == '\t'))
		i++;
	return i;
	}

static TBool IsRule(const TDesC& aLine)
	{
	TInt marks = 0;
	TText m = 0;
	for (TInt i = 0; i < aLine.Length(); i++)
		{
		TText c = aLine[i];
		if (c == ' ')
			continue;
		if ((c != '-' && c != '*' && c != '_') || (m && c != m))
			return EFalse;
		m = c;
		marks++;
		}
	return marks >= 3;
	}

void PtRenderMarkdownL(CRichText& aRt, const TDesC& aSrc, const TPtMdStyle& aStyle, CArrayFix<TInt>& aLineOfPara)
	{
	aRt.Reset();
	aLineOfPara.Reset();
	TMdBuilder b(aRt, aStyle, aLineOfPara);
	HBufC* para = HBufC::NewLC(1024);
	TPtr p(para->Des());
	TInt paraLine = 0;
	TBool inCode = EFalse, firstCode = EFalse;
	TInt line = 0;
	TInt i = 0, n = aSrc.Length();
	#define FLUSH() if (p.Length()) { b.ParaL(TMdBuilder::EBody, p, paraLine); p.Zero(); }
	while (i <= n)
		{
		TInt e = i;
		while (e < n && aSrc[e] != CEditableText::EParagraphDelimiter)
			e++;
		TPtrC l(aSrc.Mid(i, e - i));
		TInt ind = Indent(l);
		TPtrC t(l.Mid(ind));
		if (inCode)
			{
			if (t.Length() >= 3 && t.Left(3) == _L("```"))
				inCode = EFalse;
			else
				{
				// (the next line: is it the fence that ends this block?)
				TInt e2 = e + 1, s2 = e2;
				while (e2 < n && aSrc[e2] != CEditableText::EParagraphDelimiter)
					e2++;
				TPtrC nl(e < n ? aSrc.Mid(s2, e2 - s2) : TPtrC());
				TBool last = e >= n || (nl.Length() >= 3 && nl.Mid(Indent(nl)).Left(3) == _L("```"));
				b.ParaL(TMdBuilder::ECode, l, line, 0, firstCode, last);
				firstCode = EFalse;
				}
			}
		else if (t.Length() >= 3 && t.Left(3) == _L("```"))
			{
			FLUSH();
			inCode = ETrue;
			firstCode = ETrue;
			}
		else if (t.Length() == 0)
			{
			FLUSH();
			}
		else if (t[0] == '#')
			{
			TInt h = 0;
			while (h < t.Length() && t[h] == '#')
				h++;
			if (h <= 6 && (h == t.Length() || t[h] == ' '))
				{
				FLUSH();
				TPtrC text(t.Mid(h < t.Length() ? h + 1 : h));
				b.ParaL(h == 1 ? TMdBuilder::EHead1 : h == 2 ? TMdBuilder::EHead2 : TMdBuilder::EHead3, text, line);
				}
			else
				goto plain;
			}
		else if (IsRule(t))
			{
			FLUSH();
			b.ParaL(TMdBuilder::ERule, KNullDesC, line);
			}
		else if (t.Length() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ')
			{
			FLUSH();
			TPtrC text(t.Mid(2));
			// a task list: [ ] and [x] as boxes
			if (text.Length() >= 4 && text[0] == '[' && text[2] == ']' && text[3] == ' ')
				{
				TBuf<512> box;
				box.Append(text[1] == ' ' ? _L("[ ] ") : _L("[x] "));
				box.Append(text.Mid(4).Left(text.Length() - 4 < 500 ? text.Length() - 4 : 500));
				b.ParaL(TMdBuilder::EBullet, box, line, ind / 2);
				}
			else
				b.ParaL(TMdBuilder::EBullet, text, line, ind / 2);
			}
		else if (TChar(t[0]).IsDigit())
			{
			TInt d = 0;
			while (d < t.Length() && TChar(t[d]).IsDigit())
				d++;
			if (d < t.Length() - 1 && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ')
				{
				FLUSH();
				b.ParaL(TMdBuilder::ENumber, t, line, ind / 2);
				}
			else
				goto plain;
			}
		else if (t[0] == '>')
			{
			FLUSH();
			TPtrC text(t.Mid(t.Length() > 1 && t[1] == ' ' ? 2 : 1));
			b.ParaL(TMdBuilder::EQuote, text, line);
			}
		else if (t[0] == '|')
			{
			FLUSH();
			b.ParaL(TMdBuilder::ETable, l, line);
			}
		else
			{
		plain:
			// lines of a paragraph run together (Markdown's soft line ends)
			if (p.Length() == 0)
				paraLine = line;
			else if (p.Length() < p.MaxLength())
				p.Append(' ');
			TInt room = p.MaxLength() - p.Length();
			p.Append(t.Left(t.Length() < room ? t.Length() : room));
			// two spaces at the end: a line break kept
			if (l.Length() >= 2 && l.Right(2) == _L("  "))
				FLUSH();
			}
		line++;
		i = e + 1;
		}
	FLUSH();
	#undef FLUSH
	CleanupStack::PopAndDestroy();
	if (aRt.DocumentLength() > 0)
		aRt.DeleteL(aRt.DocumentLength() - 1, 1);   // (the last paragraph's own end is the document's)
	}
