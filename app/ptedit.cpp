// PTEDIT.CPP - editing a text file on the server (1.02, from Muxedo's editor)
//
// File > Files > Edit file on server... (Shift+Ctrl+O) picks the file in the
// file view; the editor then goes over the terminal, as the file view does,
// with its own menus, shortcuts and toolbar. One file at a time, as the
// Psion's own programs.
//
// The file comes over SFTP (Get, into Edit.tmp) and goes back the same way,
// safely (psissh's PSI_XOP_SAVE): if it has changed on the server since it
// was opened, PsiTerm asks before replacing it; it is written beside the file
// and renamed over it, so a dropped link never leaves half a file there.
//
// Text: UTF-8 on the server, the Psion's code page here; LF or CR LF lines
// (kept as they were, or changed with Tools > Line endings). A file with
// characters the Psion can't show opens read only, so saving never loses
// them; one with a NUL byte is not a text file. At most 64 KB.

#include "psiterm.h"
#include "ptxfer.h"
#include "pticons.h"
#include <eikgted.h>
#include <eikrted.h>
#include <txtrich.h>
#include <eikedwin.h>
#include <eikedwin.hrh>
#include <eikcfdlg.h>
#include <eikfontd.h>
#include <eikmfne.h>
#include <eikon.rsg>
#include <eiktbar.h>
#include <eikcmbut.h>
#include <eikmenub.h>
#include <eikmenup.h>
#include <eiktxtut.h>
#include <txtglobl.h>
#include <txtetext.h>
#include <txtfrmat.h>
#include <psiterm.rsg>

_LIT(KEditTmp, "C:\\System\\Apps\\PsiTerm\\Edit.tmp");
_LIT(KEditedFile, "C:\\System\\Apps\\PsiTerm\\Edited.dat");
const TInt KEditMax = 64 * 1024;
const TInt KRecentMax = 6;          // per server, on File > Recent

static TPtrC Upto(const TDesC& aText, TInt aMax)
	{
	return aText.Left(aText.Length() < aMax ? aText.Length() : aMax);
	}

static TPtrC8 Upto8(const TDesC8& aText, TInt aMax)
	{
	return aText.Left(aText.Length() < aMax ? aText.Length() : aMax);
	}

static void AppendUtf8(TDes8& aOut, TUint aCp)
	{
	if (aCp < 0x80)
		aOut.Append((TUint8)aCp);
	else if (aCp < 0x800)
		{
		aOut.Append((TUint8)(0xc0 | (aCp >> 6)));
		aOut.Append((TUint8)(0x80 | (aCp & 0x3f)));
		}
	else
		{
		aOut.Append((TUint8)(0xe0 | (aCp >> 12)));
		aOut.Append((TUint8)(0x80 | ((aCp >> 6) & 0x3f)));
		aOut.Append((TUint8)(0x80 | (aCp & 0x3f)));
		}
	}

// "Markdown", from the name's ending
static const TText* Language(const TDesC& aName)
	{
	struct TLang { const TText* iExt; const TText* iName; };
	static const TLang KLangs[] = {
		{ _S(".md"), _S("Markdown") }, { _S(".sh"), _S("Shell") }, { _S(".py"), _S("Python") },
		{ _S(".c"), _S("C") }, { _S(".h"), _S("C") }, { _S(".cpp"), _S("C++") }, { _S(".js"), _S("JavaScript") },
		{ _S(".ts"), _S("TypeScript") }, { _S(".json"), _S("JSON") }, { _S(".yaml"), _S("YAML") },
		{ _S(".yml"), _S("YAML") }, { _S(".conf"), _S("Config") }, { _S(".cfg"), _S("Config") },
		{ _S(".ini"), _S("Config") }, { _S(".toml"), _S("TOML") }, { _S(".html"), _S("HTML") },
		{ _S(".css"), _S("CSS") }, { _S(".txt"), _S("Text") }, { _S(".rs"), _S("Rust") },
		{ _S(".go"), _S("Go") }, { _S(".rb"), _S("Ruby") }, { _S(".pl"), _S("Perl") } };
	for (TUint i = 0; i < sizeof(KLangs) / sizeof(KLangs[0]); i++)
		{
		TPtrC e(KLangs[i].iExt);
		if (aName.Length() > e.Length() && aName.Right(e.Length()).CompareF(e) == 0)
			return KLangs[i].iName;
		}
	return _S("Text");
	}

// ============================================================================
// The view
// ============================================================================

class CPtEditView : public CCoeControl
	{
public:
	CPtEditView(CTermView& aTerm) : iTerm(aTerm) {}
	~CPtEditView();
	void ConstructL(const TRect& aRect);
	TBool LoadL(const TDesC8& aRemote);    // EFalse: it said why
	void ShowL();                           // (once on screen) the editor, with the text
	TBool SaveL(TBool aForce);
	void SaveCopyL();
	void RevertL();
	void GotoLineL();
	void CommandL(TInt aCommand);
	void InitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane);
	void SetViewRectL(const TRect& aRect);
	TBool Modified() const { return iModified; }
	const TDesC8& Remote() const { return iRemote; }
	TKeyResponse OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType);
private:
	// (the editor has a window of its own beside this one's, as Word's:
	// not a component, so keys and focus are passed on by hand)
	void FocusChanged(TDrawNow aDrawNow);
	TRect TextRect() const;
	void ShowTextL();
	void SizeChanged();
	void Draw(const TRect& aRect) const;
	void DrawBand(CWindowGc& aGc) const;
	void DrawStatus(CWindowGc& aGc) const;
	void StatusNow();
	void ApplyFontL();
	void LineCol(TInt& aLine, TInt& aCol, TInt& aLines) const;
	CTermView& iTerm;
	CEikGlobalTextEditor* iEd;
	TBool iColour;
	TInt iBandH, iStatusH;
	TBuf8<512> iRemote;       // the file on the server (UTF-8)
	TBuf<120> iName;          // its name, as Psion text
	TBuf<160> iFolder;        // where, as Psion text
	TUint iMtime;             // its modified time when opened (or last saved)
	TUint iSize;              // its size then
	TBool iCrLf;              // CR LF lines
	TBool iFinalNewline;      // it ended with a line end
	TBool iReadOnly;
	TBool iModified;
	TBool iWrap;
	TInt iFontStep;           // which of the typeface's sizes
	HBufC* iLoaded;           // the text fetched, until the editor has it
	// (Markdown: the formatted view, in an editor of its own over the source's)
	TBool iMarkdown;
	TBool iFormatted;
	CEikRichTextEditor* iRich;
	CArrayFixFlat<TInt>* iParaLine;   // formatted paragraph -> source line
	void RenderL();
	void ToSourceL();                 // the source, at the line the formatted view was on
	void MdStyle(TPtMdStyle& aStyle) const;
	CEikGlobalTextEditor* Active() const { return iFormatted && iRich ? (CEikGlobalTextEditor*)iRich : iEd; }
	};

CPtEditView::~CPtEditView()
	{
	delete iRich;
	delete iEd;
	delete iLoaded;
	delete iParaLine;
	}

void CPtEditView::ConstructL(const TRect& aRect)
	{
	CreateWindowL();
	iColour = iEikonEnv->ScreenDevice()->DisplayMode() >= EColor16;
	iWrap = ETrue;
	iFontStep = -1;
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	iBandH = f->HeightInPixels() + 6;
	iStatusH = small->HeightInPixels() + 4;
	SetRectL(aRect);
	}

// the text's part of the view (screen co-ordinates: the editor's window
// is a top-level one, as this view's is)
TRect CPtEditView::TextRect() const
	{
	TRect r = Rect();
	r.iTl.iY += iBandH;
	r.iBr.iY -= iStatusH;
	return r;
	}

// The editor itself, made as Psion Word makes its own (a window of its
// own; sized and activated before any text goes in)
void CPtEditView::ShowL()
	{
	if (!iEd)
		{
		iEd = new(ELeave) CEikGlobalTextEditor;
		iEd->ConstructL(NULL, 0, KEditMax, EEikEdwinOwnsWindow | EEikEdwinInclusiveSizeFixed
			| EEikEdwinNoAutoSelection | EEikEdwinAlwaysShowSelection, EEikFontControlAll, EEikNoSymbolFonts);
		iEd->SetAllowUndo(ETrue);
		iEd->SetRectL(TextRect());
		iEd->ActivateL();
		iEd->SetFocus(IsFocused());
		}
	ShowTextL();
	}

void CPtEditView::FocusChanged(TDrawNow /*aDrawNow*/)
	{
	if (iEd)
		iEd->SetFocus(IsFocused() && !(iFormatted && iRich));
	if (iRich)
		iRich->SetFocus(IsFocused() && iFormatted);
	}

// ---- Markdown, formatted (ptmd.cpp builds it) --------------------------------

void CPtEditView::MdStyle(TPtMdStyle& aStyle) const
	{
	TInt px = 0;
	iTerm.EditorFont(iFontStep, aStyle.iMono, px);
	aStyle.iDevice = iEikonEnv->ScreenDevice();
	aStyle.iBody.Copy(_L("Arial"));
	aStyle.iBodyPx = px + 1;
	aStyle.iMonoPx = px;
	if (iColour)
		{
		aStyle.iPaper = KRgbWhite;
		aStyle.iInk = KRgbBlack;
		aStyle.iSoft = TRgb(102, 102, 102);
		aStyle.iFaint = TRgb(153, 153, 153);
		aStyle.iHead = TRgb(0, 0, 102);
		aStyle.iLink = TRgb(0, 102, 204);
		aStyle.iRule = TRgb(204, 204, 204);
		aStyle.iCodeBack = TRgb(238, 238, 238);
		}
	else
		{
		aStyle.iPaper = KRgbWhite;
		aStyle.iInk = KRgbBlack;
		aStyle.iSoft = TRgb::Gray16(5);
		aStyle.iFaint = TRgb::Gray16(8);
		aStyle.iHead = KRgbBlack;
		aStyle.iLink = KRgbBlack;
		aStyle.iRule = TRgb::Gray16(10);
		aStyle.iCodeBack = TRgb::Gray16(14);
		}
	}

// The formatted view, from the source now in the editor; it shows the
// paragraph made from the source line the cursor is on
void CPtEditView::RenderL()
	{
	if (!iEd)
		return;
	if (!iParaLine)
		iParaLine = new(ELeave) CArrayFixFlat<TInt>(32);
	if (!iRich)
		{
		iRich = new(ELeave) CEikRichTextEditor;
		iRich->ConstructL(NULL, 0, 0, EEikEdwinOwnsWindow | EEikEdwinInclusiveSizeFixed
			| EEikEdwinNoAutoSelection | EEikEdwinReadOnly);
		iRich->SetRectL(TextRect());
		iRich->ActivateL();
		}
	HBufC* src = iEd->GetTextInHBufL();
	if (!src)
		src = HBufC::NewL(0);
	CleanupStack::PushL(src);
	TPtMdStyle st;
	MdStyle(st);
	PtRenderMarkdownL(*iRich->RichText(), *src, st, *iParaLine);
	CleanupStack::PopAndDestroy();
	iRich->HandleTextChangedL();
	iRich->SetWordWrapL(ETrue);
	// where the source's cursor is
	TInt pos = iEd->CursorPos();
	TInt srcLine = iEd->Text()->ParagraphNumberForPos(pos);
	TInt para = 0;
	for (TInt i = 0; i < iParaLine->Count(); i++)
		if ((*iParaLine)[i] <= srcLine)
			para = i;
	TInt len = 0;
	TInt at = iRich->RichText()->CharPosOfParagraph(len, para);
	iRich->SetCursorPosL(at < 0 ? 0 : at, EFalse);
	iFormatted = ETrue;
	iRich->MakeVisible(ETrue);
	iEd->MakeVisible(EFalse);
	FocusChanged(ENoDrawNow);
	iRich->DrawNow();
	}

void CPtEditView::ToSourceL()
	{
	if (!iFormatted || !iRich)
		{
		iFormatted = EFalse;
		return;
		}
	TInt pos = iRich->CursorPos();
	TInt para = iRich->RichText()->ParagraphNumberForPos(pos);
	TInt line = (iParaLine && para >= 0 && para < iParaLine->Count()) ? (*iParaLine)[para] : 0;
	iFormatted = EFalse;
	iRich->MakeVisible(EFalse);
	iEd->MakeVisible(ETrue);
	TInt len = 0;
	TInt at = iEd->Text()->CharPosOfParagraph(len, line);
	iEd->SetCursorPosL(at < 0 ? 0 : at, EFalse);
	FocusChanged(ENoDrawNow);
	iEd->DrawNow();
	}

// the fetched text into the editor
void CPtEditView::ShowTextL()
	{
	if (!iEd || !iLoaded)
		return;
	TPtrC t(*iLoaded);
	iEd->SetTextL(&t);
	delete iLoaded;
	iLoaded = NULL;
	ApplyFontL();
	iEd->SetWordWrapL(iWrap);
	iEd->SetReadOnly(iReadOnly);
	iEd->SetCursorPosL(0, EFalse);
	iModified = EFalse;
	iEd->DrawNow();
	if (iMarkdown && iFormatted)
		RenderL();                               // (1.02: Markdown opens formatted)
	if (iReadOnly)
		iEikonEnv->InfoMsg(_L("Opened read only - it has characters the Psion can't show"));
	}

void CPtEditView::SetViewRectL(const TRect& aRect)
	{
	SetRectL(aRect);
	DrawNow();
	}

void CPtEditView::SizeChanged()
	{
	if (iEd)
		TRAP_IGNORE(iEd->SetRectL(TextRect()));
	if (iRich)
		TRAP_IGNORE(iRich->SetRectL(TextRect()));
	}

// The terminal's typeface (PsiTerm.gdr), at the size nearest the terminal's
void CPtEditView::ApplyFontL()
	{
	TBuf<24> face;
	TInt px = 0;
	iTerm.EditorFont(iFontStep, face, px);
	TCharFormat cf;
	TCharFormatMask cm;
	cf.iFontSpec.iTypeface.iName = face;
	cf.iFontSpec.iHeight = iEikonEnv->ScreenDevice()->VerticalPixelsToTwips(px);
	cm.SetAttrib(EAttFontTypeface);
	cm.SetAttrib(EAttFontHeight);
	iEd->GlobalText()->ApplyCharFormatL(cf, cm, 0, iEd->TextLength());
	CParaFormat* pf = CParaFormat::NewLC();
	TParaFormatMask pm;
	pf->iLeftMarginInTwips = 0;
	pf->iSpaceBeforeInTwips = 0;           // (a line of a file is a paragraph here: no gaps)
	pf->iSpaceAfterInTwips = 0;
	// (and the line exactly the font's height: the standard leaves a gap
	// under each line of the terminal's fonts)
	pf->iLineSpacingInTwips = iEikonEnv->ScreenDevice()->VerticalPixelsToTwips(px);
	pf->iLineSpacingControl = CParaFormat::ELineSpacingExactlyInTwips;
	pm.SetAttrib(EAttLineSpacing);
	pm.SetAttrib(EAttLineSpacingControl);
	pm.SetAttrib(EAttLeftMargin);
	pm.SetAttrib(EAttSpaceBefore);
	pm.SetAttrib(EAttSpaceAfter);
	iEd->GlobalText()->ApplyParaFormatL(pf, pm, 0, iEd->TextLength());
	CleanupStack::PopAndDestroy();
	iEd->NotifyNewFormatL();
	}

// Fetches the file and puts it in the editor
TBool CPtEditView::LoadL(const TDesC8& aRemote)
	{
	iRemote = Upto8(aRemote, iRemote.MaxLength());
	TInt slash = iRemote.LocateReverse('/');
	PtUtf8ToText(slash >= 0 ? iRemote.Mid(slash + 1) : TPtrC8(iRemote), iName);
	PtUtf8ToText(slash > 0 ? iRemote.Left(slash) : TPtrC8(_L8("~")), iFolder);
	TBool md = (iName.Length() > 3 && iName.Right(3).CompareF(_L(".md")) == 0)
		|| (iName.Length() > 9 && iName.Right(9).CompareF(_L(".markdown")) == 0);
	if (md && !iMarkdown)
		iFormatted = ETrue;                      // (opened formatted the first time)
	iMarkdown = md;
	TInt r = PtRunXferL(iTerm, PSI_XOP_GET, iRemote, KEditTmp, _L("Opening file"), _L("Getting the file..."));
	if (r != PSI_XFER_OK)
		{
		PtXferFailed(r, iTerm);
		return EFalse;
		}
	PsiShared* s = iTerm.XferShared();
	iMtime = s ? s->xfer_mtime : 0;
	RFs& fs = iCoeEnv->FsSession();
	RFile f;
	User::LeaveIfError(f.Open(fs, KEditTmp, EFileRead));
	CleanupClosePushL(f);
	TInt size = 0;
	f.Size(size);
	if (size > KEditMax)
		{
		CleanupStack::PopAndDestroy();
		fs.Delete(KEditTmp);
		iEikonEnv->InfoMsg(_L("Too big to edit - PsiTerm edits files up to 64 KB"));
		return EFalse;
		}
	iSize = size;
	HBufC8* raw = HBufC8::NewLC(size + 1);
	TPtr8 d(raw->Des());
	User::LeaveIfError(f.Read(d, size));
	if (d.Locate(0) >= 0)
		{
		CleanupStack::PopAndDestroy(2);
		fs.Delete(KEditTmp);
		iEikonEnv->InfoMsg(_L("Not a text file"));
		return EFalse;
		}
	// UTF-8 -> the Psion's text; line ends -> paragraphs
	HBufC* text = HBufC::NewLC(size + 1);
	TPtr t(text->Des());
	TInt crlf = 0, lf = 0;
	iReadOnly = EFalse;
	TInt i = 0, n = d.Length();
	while (i < n)
		{
		TUint c = d[i++];
		if (c == '\r' && i < n && d[i] == '\n')
			{
			i++;
			crlf++;
			t.Append((TChar)CEditableText::EParagraphDelimiter);
			continue;
			}
		if (c == '\n')
			{
			lf++;
			t.Append((TChar)CEditableText::EParagraphDelimiter);
			continue;
			}
		if (c >= 0x80)
			{
			TInt more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : -1;
			TUint cp = more > 0 ? (c & (0x3f >> more)) : 0;
			TInt k = 0;
			while (more > 0 && k < more && i < n && (d[i] & 0xc0) == 0x80)
				{
				cp = (cp << 6) | (d[i++] & 0x3f);
				k++;
				}
			TInt m = (more > 0 && k == more) ? PsiMapToCodePage(cp) : -1;
			if (m < 0)
				{
				iReadOnly = ETrue;           // (saving would lose it)
				m = '?';
				}
			c = m;
			}
		else if (c < 0x20 && c != '\t')
			{
			iReadOnly = ETrue;               // (a control character the editor can't keep)
			c = '?';
			}
		t.Append((TChar)c);
		}
	iCrLf = crlf > lf;
	iFinalNewline = n > 0 && d[n - 1] == '\n';
	if (iFinalNewline && t.Length())
		t.SetLength(t.Length() - 1);          // (the last line end is not a paragraph of its own)
	CleanupStack::Pop();                      // text: kept until the editor has it
	CleanupStack::PopAndDestroy(2);           // raw, f
	delete iLoaded;
	iLoaded = text;
	fs.Delete(KEditTmp);
	iModified = EFalse;
	ShowTextL();                              // (Revert to saved: the editor is there)
	return ETrue;
	}

// The text, back to the server's form, into Edit.tmp (aFile: or that file)
static TInt WriteText(RFs& aFs, const TDesC& aFile, const TDesC& aText, TBool aCrLf, TBool aFinal)
	{
	HBufC8* out = HBufC8::New(aText.Length() * 3 + 4);
	if (!out)
		return KErrNoMemory;
	TPtr8 o(out->Des());
	for (TInt i = 0; i < aText.Length(); i++)
		{
		TUint c = aText[i];
		if (c == CEditableText::EParagraphDelimiter || c == CEditableText::ELineBreak)
			{
			if (aCrLf)
				o.Append('\r');
			o.Append('\n');
			}
		else if (c == CEditableText::ETabCharacter)
			o.Append('\t');
		else
			AppendUtf8(o, PsiCodePageToUnicode(c & 0xff));
		}
	if (aFinal && aText.Length())
		{
		if (aCrLf)
			o.Append('\r');
		o.Append('\n');
		}
	RFile f;
	TInt r = f.Replace(aFs, aFile, EFileWrite);
	if (r == KErrNone)
		{
		r = f.Write(o);
		if (r == KErrNone)
			r = f.Flush();
		f.Close();
		}
	delete out;
	return r;
	}

TBool CPtEditView::SaveL(TBool aForce)
	{
	if (iReadOnly)
		{
		iEikonEnv->InfoMsg(_L("Read only - File > Save copy on Psion keeps a copy"));
		return EFalse;
		}
	if (!iTerm.XferShared())
		{
		iEikonEnv->InfoMsg(_L("Not available - SSH is not connected"));
		return EFalse;
		}
	if (!iEd)
		return EFalse;
	HBufC* text = iEd->GetTextInHBufL();
	if (!text)
		text = HBufC::NewL(0);
	CleanupStack::PushL(text);
	TInt w = WriteText(iCoeEnv->FsSession(), KEditTmp, *text, iCrLf, iFinalNewline);
	CleanupStack::PopAndDestroy();
	if (w != KErrNone)
		{
		iEikonEnv->InfoMsg(_L("Could not save - the Psion's disk is full"));
		return EFalse;
		}
	for (;;)
		{
		PsiShared* s = iTerm.XferShared();
		if (!s)
			break;
		s->xfer_mtime = iMtime;
		s->xfer_expect = iSize;
		s->xfer_force = aForce;
		TInt r = PtRunXferL(iTerm, PSI_XOP_SAVE, iRemote, KEditTmp, _L("Saving"), _L("Saving the file..."));
		if (r == PSI_XFER_CHANGED && !aForce)
			{
			TBuf<160> q;
			q.Format(_L("%S has changed on the server since you opened it"), &iName);
			if (!iEikonEnv->QueryWinL(q, _L("Replace it with your version?")))
				break;
			aForce = ETrue;
			continue;
			}
		if (r != PSI_XFER_OK)
			{
			PtXferFailed(r, iTerm);
			break;
			}
		s = iTerm.XferShared();
		if (s)
			{
			iMtime = s->xfer_mtime;
			iSize = s->xfer_total;
			}
		iModified = EFalse;
		iCoeEnv->FsSession().Delete(KEditTmp);
		DrawNow();
		iEikonEnv->InfoMsg(_L("Saved"));
		return ETrue;
		}
	iCoeEnv->FsSession().Delete(KEditTmp);
	return EFalse;
	}

// File > Save copy on Psion...: the text as it would go to the server
void CPtEditView::SaveCopyL()
	{
	TFileName name(_L("C:\\Documents\\"));
	name.Append(Upto(iName, 80));
	CEikFileSaveAsDialog* dlg = new(ELeave) CEikFileSaveAsDialog(&name);
	if (!dlg->ExecuteLD(R_EIK_DIALOG_FILE_SAVEAS) || !iEd)
		return;
	HBufC* text = iEd->GetTextInHBufL();
	if (!text)
		text = HBufC::NewL(0);
	CleanupStack::PushL(text);
	TInt r = WriteText(iCoeEnv->FsSession(), name, *text, iCrLf, iFinalNewline);
	CleanupStack::PopAndDestroy();
	iEikonEnv->InfoMsg(r == KErrNone ? _L("Copy saved") : _L("Could not save the copy"));
	}

void CPtEditView::RevertL()
	{
	if (iModified && !iEikonEnv->QueryWinL(_L("Your changes will be lost"), _L("Revert to saved?")))
		return;
	TBuf8<512> remote(iRemote);
	if (LoadL(remote))
		DrawNow();
	}

// lines and columns, counted (the editor does not keep them)
void CPtEditView::LineCol(TInt& aLine, TInt& aCol, TInt& aLines) const
	{
	aLine = 1;
	aCol = 1;
	aLines = 1;
	if (!iEd)
		return;
	CPlainText* pt = iEd->Text();
	TInt len = pt->DocumentLength();
	TInt cur = iEd->CursorPos();
	TInt pos = 0;
	while (pos < len)
		{
		TPtrC seg = pt->Read(pos);
		TInt n = seg.Length();
		if (n <= 0)
			break;
		for (TInt i = 0; i < n && pos + i < len; i++)
			{
			if (seg[i] == CEditableText::EParagraphDelimiter)
				{
				aLines++;
				if (pos + i < cur)
					{
					aLine++;
					aCol = 1;
					}
				}
			else if (pos + i < cur)
				aCol++;
			}
		pos += n;
		}
	}

// View > Go to line... (Ctrl+L): "Line [ ] of 38"
class CPtGotoDialog : public CEikDialog
	{
public:
	CPtGotoDialog(TInt& aLine, TInt aLines) : iLine(aLine), iLines(aLines) {}
private:
	void PreLayoutDynInitL()
		{
		CEikNumberEditor* ne = (CEikNumberEditor*)Control(EPtDlgGotoLine);
		ne->SetMinimumAndMaximum(1, iLines);
		ne->SetNumber(iLine);
		TBuf<20> of;
		of.Format(_L("of %d"), iLines);
		SetLabelL(EPtDlgGotoOf, of);
		}
	TBool OkToExitL(TInt /*aButtonId*/)
		{
		iLine = ((CEikNumberEditor*)Control(EPtDlgGotoLine))->Number();
		return ETrue;
		}
	TInt& iLine;
	TInt iLines;
	};

void CPtEditView::GotoLineL()
	{
	TInt line, col, lines;
	LineCol(line, col, lines);
	CPtGotoDialog* dlg = new(ELeave) CPtGotoDialog(line, lines);
	if (!dlg->ExecuteLD(R_PT_GOTO_LINE_DIALOG))
		return;
	if (line < 1) line = 1;
	if (line > lines) line = lines;
	TInt len = 0;
	if (!iEd)
		return;
	TInt pos = iEd->Text()->CharPosOfParagraph(len, line - 1);
	if (pos < 0)
		pos = 0;
	iEd->SetCursorPosL(pos, EFalse);
	StatusNow();
	}

TKeyResponse CPtEditView::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	TUint code = aKeyEvent.iCode;
	TBool editKey = (code >= 0x20 && code < 0x100) || code == EKeyBackspace || code == EKeyDelete
		|| code == EKeyEnter || code == EKeyTab;
	if (iFormatted && iRich)
		{
		// the formatted view moves about; typing goes to the source, at
		// the line the cursor is on
		if (aType != EEventKey || !editKey || (aKeyEvent.iModifiers & EModifierCtrl))
			return iRich->OfferKeyEventL(aKeyEvent, aType);
		if (iReadOnly)
			return EKeyWasConsumed;
		ToSourceL();
		iEikonEnv->InfoMsg(_L("Editing the text - Ctrl+P formats it again"));
		}
	if (aType == EEventKey && !iReadOnly && !(aKeyEvent.iModifiers & EModifierCtrl))
		{
		TUint c = aKeyEvent.iCode;
		if (editKey)
			{
			if (!iModified)
				{
				iModified = ETrue;
				TRect band(Rect().iTl, TPoint(Rect().iBr.iX, Rect().iTl.iY + iBandH));
				DrawDeferred();
				}
			}
		}
	if (!iEd)
		return EKeyWasNotConsumed;
	TKeyResponse r = iEd->OfferKeyEventL(aKeyEvent, aType);
	if (aType == EEventKey)
		StatusNow();
	return r;
	}

void CPtEditView::StatusNow()
	{
	ActivateGc();
	DrawStatus(SystemGc());
	DeactivateGc();
	}

void CPtEditView::DrawBand(CWindowGc& aGc) const
	{
	TRect r = Rect();
	TRect band(r.iTl, TPoint(r.iBr.iX, r.iTl.iY + iBandH));
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(iColour ? TRgb(0, 0, 128) : TRgb::Gray16(4));
	aGc.DrawRect(band);
	const CFont* f = iEikonEnv->NormalFont();
	const CFont* small = iEikonEnv->AnnotationFont();
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	TBuf<40> state;
	if (iReadOnly)
		state.Copy(_L("Read only"));
	else if (iModified)
		state.Copy(_L("Modified"));
	TInt sw = state.Length() ? small->TextWidthInPixels(state) + 12 : 0;
	aGc.UseFont(f);
	aGc.SetPenColor(KRgbWhite);
	TInt by = band.iTl.iY + 3 + f->AscentInPixels();
	TBuf<120> name(iName);
	TextUtils::ClipToFit(name, *f, band.Width() / 2);
	aGc.DrawText(name, TPoint(band.iTl.iX + 6, by));
	TInt x = band.iTl.iX + 6 + f->TextWidthInPixels(name) + 10;
	aGc.UseFont(small);
	aGc.SetPenColor(iColour ? TRgb(190, 190, 255) : TRgb::Gray16(12));
	TBuf<200> where;
	TPtrC host(Upto(iTerm.Settings().iSshHost, 40));
	where.Format(_L("%S:%S"), &host, &iFolder);
	// (the start of a long folder is the part that goes)
	TInt room = band.iBr.iX - sw - x - 6;
	if (room > 20)
		{
		while (where.Length() > 4 && small->TextWidthInPixels(where) > room)
			where.Delete(0, 1);
		aGc.DrawText(where, TPoint(x, by));
		}
	if (state.Length())
		{
		aGc.SetPenColor(iColour && iModified ? TRgb(255, 210, 90) : KRgbWhite);
		aGc.DrawText(state, TPoint(band.iBr.iX - sw + 6, by));
		}
	aGc.DiscardFont();
	}

void CPtEditView::DrawStatus(CWindowGc& aGc) const
	{
	TRect r = Rect();
	TRect bar(r.iTl.iX, r.iBr.iY - iStatusH, r.iBr.iX, r.iBr.iY);
	aGc.SetPenStyle(CGraphicsContext::ENullPen);
	aGc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	aGc.SetBrushColor(KRgbWhite);
	aGc.DrawRect(bar);
	aGc.SetPenStyle(CGraphicsContext::ESolidPen);
	aGc.SetPenColor(TRgb::Gray16(8));
	aGc.DrawLine(bar.iTl, TPoint(bar.iBr.iX, bar.iTl.iY));
	TInt line, col, lines;
	LineCol(line, col, lines);
	TBuf<120> t;
	TPtrC lang(Language(iName));
	TPtrC ends(iCrLf ? _L("CR LF") : _L("LF"));
	if (iFormatted && iRich)
		t.Format(_L("Formatted - Ctrl+P shows the text    %d lines    %S    %S    UTF-8"), lines, &lang, &ends);
	else
		t.Format(_L("Ln %d, Col %d    %d lines    %S    %S    UTF-8"), line, col, lines, &lang, &ends);
	const CFont* small = iEikonEnv->AnnotationFont();
	aGc.UseFont(small);
	aGc.SetBrushStyle(CGraphicsContext::ENullBrush);
	aGc.SetPenColor(iColour ? TRgb(0, 0, 128) : TRgb::Gray16(5));
	aGc.DrawText(t, TPoint(bar.iTl.iX + 6, bar.iTl.iY + 2 + small->AscentInPixels()));
	aGc.DiscardFont();
	}

void CPtEditView::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	gc.SetPenStyle(CGraphicsContext::ENullPen);
	gc.SetBrushStyle(CGraphicsContext::ESolidBrush);
	gc.SetBrushColor(KRgbWhite);
	gc.DrawRect(Rect());
	DrawBand(gc);
	DrawStatus(gc);
	}

void CPtEditView::InitMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	if (aMenuId == R_PT_EDIT_FILE_MENU)
		{
		if (iReadOnly)
			aMenuPane->SetItemDimmed(EPtCmdEdSave, ETrue);
		if (!iModified)
			aMenuPane->SetItemDimmed(EPtCmdEdRevert, ETrue);
		}
	else if (aMenuId == R_PT_EDIT_EDIT_MENU)
		{
		if (iReadOnly)
			{
			aMenuPane->SetItemDimmed(EPtCmdEdUndo, ETrue);
			aMenuPane->SetItemDimmed(EPtCmdEdCut, ETrue);
			aMenuPane->SetItemDimmed(EPtCmdEdPaste, ETrue);
			aMenuPane->SetItemDimmed(EPtCmdEdReplace, ETrue);
			}
		}
	else if (aMenuId == R_PT_EDIT_VIEW_MENU)
		{
		aMenuPane->SetItemButtonState(EPtCmdEdWrap, iWrap ? EEikMenuItemSymbolOn : 0);
		aMenuPane->SetItemButtonState(EPtCmdEdFormatted, iFormatted ? EEikMenuItemSymbolOn : 0);
		if (!iMarkdown)
			aMenuPane->SetItemDimmed(EPtCmdEdFormatted, ETrue);
		}
	else if (aMenuId == R_PT_EDIT_ENDINGS_MENU)
		aMenuPane->SetItemButtonState(iCrLf ? EPtCmdEdCrLf : EPtCmdEdLf, EEikMenuItemSymbolOn);
	}

void CPtEditView::CommandL(TInt aCommand)
	{
	if (!iEd)
		return;
	// (editing commands work on the text: out of the formatted view first)
	if (iFormatted && (aCommand == EPtCmdEdUndo || aCommand == EPtCmdEdCut || aCommand == EPtCmdEdPaste
		|| aCommand == EPtCmdEdReplace || aCommand == EPtCmdEdGoto || aCommand == EPtCmdEdSelectAll))
		ToSourceL();
	switch (aCommand)
		{
	case EPtCmdEdSave:     SaveL(EFalse); break;
	case EPtCmdEdSaveCopy: SaveCopyL(); break;
	case EPtCmdEdRevert:   RevertL(); break;
	case EPtCmdEdUndo:
		iEd->UndoL();
		iModified = ETrue;
		DrawNow();
		break;
	case EPtCmdEdCut:
		iEd->ClipboardL(CEikEdwin::ECut);
		iModified = ETrue;
		DrawNow();
		break;
	case EPtCmdEdCopy:     Active()->ClipboardL(CEikEdwin::ECopy); break;
	case EPtCmdEdPaste:
		iEd->ClipboardL(CEikEdwin::EPaste);
		iModified = ETrue;
		DrawNow();
		break;
	case EPtCmdEdSelectAll: iEd->SelectAllL(); break;
	case EPtCmdEdFind:     Active()->RunFindDialogL(); break;
	case EPtCmdEdFindNext: Active()->RunFindAgainDialogL(); break;
	case EPtCmdEdFormatted:
		if (!iMarkdown)
			{
			iEikonEnv->InfoMsg(_L("Only Markdown (.md) files are formatted"));
			break;
			}
		if (iFormatted)
			ToSourceL();
		else
			RenderL();
		DrawNow();
		break;
	case EPtCmdEdReplace:
		iEd->RunReplaceDialogL();
		iModified = ETrue;
		DrawNow();
		break;
	case EPtCmdEdGoto:     GotoLineL(); break;
	case EPtCmdEdWrap:
		iWrap = !iWrap;
		iEd->SetWordWrapL(iWrap);
		iEd->DrawNow();
		break;
	case EPtCmdEdZoomIn:
	case EPtCmdEdZoomOut:
		{
		TInt n = iTerm.EditorFontSteps();
		if (iFontStep < 0)
			{
			TBuf<24> face;
			TInt px;
			iTerm.EditorFont(iFontStep, face, px);
			iFontStep = iTerm.EditorFontStep(px);
			}
		iFontStep += aCommand == EPtCmdEdZoomIn ? 1 : -1;
		if (iFontStep < 0) iFontStep = 0;
		if (iFontStep >= n) iFontStep = n - 1;
		ApplyFontL();
		iEd->DrawNow();
		if (iFormatted)
			RenderL();
		break;
		}
	case EPtCmdEdLf:
	case EPtCmdEdCrLf:
		if (iCrLf != (aCommand == EPtCmdEdCrLf))
			{
			iCrLf = aCommand == EPtCmdEdCrLf;
			iModified = ETrue;
			DrawNow();
			}
		break;
	default:
		break;
		}
	StatusNow();
	}

// ============================================================================
// Edited.dat: the files opened, newest first, per server (File > Recent)
// "server<TAB>path" lines, UTF-8
// ============================================================================

static void RecentLoadL(RFs& aFs, CDesC8ArrayFlat& aOut)
	{
	RFile f;
	if (f.Open(aFs, KEditedFile, EFileRead) != KErrNone)
		return;
	CleanupClosePushL(f);
	TInt size = 0;
	f.Size(size);
	if (size > 16 * 1024)
		size = 16 * 1024;
	HBufC8* b = HBufC8::NewLC(size);
	TPtr8 d(b->Des());
	f.Read(d, size);
	TInt start = 0;
	for (TInt i = 0; i <= d.Length(); i++)
		if (i == d.Length() || d[i] == '\n')
			{
			if (i > start)
				aOut.AppendL(d.Mid(start, i - start));
			start = i + 1;
			}
	CleanupStack::PopAndDestroy(2);
	}

static void RecentAddL(RFs& aFs, const TDesC8& aServer, const TDesC8& aPath)
	{
	CDesC8ArrayFlat* a = new(ELeave) CDesC8ArrayFlat(8);
	CleanupStack::PushL(a);
	RecentLoadL(aFs, *a);
	TBuf8<600> line(aServer);
	line.Append('\t');
	line.Append(Upto8(aPath, 512));
	for (TInt i = a->Count() - 1; i >= 0; i--)
		if ((*a)[i] == line)
			a->Delete(i);
	a->InsertL(0, line);
	// at most KRecentMax for a server, 30 in all
	TInt mine = 0;
	for (TInt i = 0; i < a->Count(); i++)
		{
		TPtrC8 l = (*a)[i];
		if (l.Length() > aServer.Length() && l.Left(aServer.Length()) == aServer && l[aServer.Length()] == '\t'
			&& ++mine > KRecentMax)
			{
			a->Delete(i);
			i--;
			}
		}
	while (a->Count() > 30)
		a->Delete(a->Count() - 1);
	HBufC8* out = HBufC8::NewLC(30 * 610);
	TPtr8 o(out->Des());
	for (TInt i = 0; i < a->Count(); i++)
		{
		o.Append((*a)[i]);
		o.Append('\n');
		}
	SafeWrite(aFs, KEditedFile, o);
	CleanupStack::PopAndDestroy(2);
	}

static void ServerKey(CTermView& aTerm, TDes8& aKey)
	{
	aKey.Zero();
	aKey.Append(Upto(aTerm.Settings().iSshUser, 30));
	aKey.Append('@');
	aKey.Append(Upto(aTerm.Settings().iSshHost, 40));
	aKey.AppendNum(aTerm.Settings().iSshPort);
	}

// this server's recent files, newest first
// (aPaths as TBuf8<512>*: a TDes8* would step by a bare descriptor's size)
static TInt RecentForServerL(CTermView& aTerm, TBuf8<512>* aPaths, TInt aMax)
	{
	CDesC8ArrayFlat* a = new(ELeave) CDesC8ArrayFlat(8);
	CleanupStack::PushL(a);
	RecentLoadL(CCoeEnv::Static()->FsSession(), *a);
	TBuf8<100> key;
	ServerKey(aTerm, key);
	TInt n = 0;
	for (TInt i = 0; i < a->Count() && n < aMax; i++)
		{
		TPtrC8 l = (*a)[i];
		if (l.Length() > key.Length() + 1 && l.Left(key.Length()) == key && l[key.Length()] == '\t')
			aPaths[n++] = Upto8(l.Mid(key.Length() + 1), aPaths[0].MaxLength());
		}
	CleanupStack::PopAndDestroy();
	return n;
	}

// ============================================================================
// The AppUi's side: the editor over the terminal, as the file view
// ============================================================================

void CPsiTermAppUi::OpenEditorL(const TDesC8& aRemote, TUint aSize)
	{
	if (aSize > (TUint)KEditMax)
		{
		iEikonEnv->InfoMsg(_L("Too big to edit - PsiTerm edits files up to 64 KB"));
		return;
		}
	if (iEdit)
		{
		if (!CloseEditorL())
			return;
		}
	CloseStart();
	CPtEditView* v = new(ELeave) CPtEditView(*iView);
	CleanupStack::PushL(v);
	v->ConstructL(TermRect(iView->Settings().iToolbar));
	if (!v->LoadL(aRemote))
		{
		CleanupStack::PopAndDestroy();
		return;
		}
	CleanupStack::Pop();
	iEdit = v;
	TBuf8<100> key;
	ServerKey(*iView, key);
	TRAP_IGNORE(RecentAddL(iCoeEnv->FsSession(), key, aRemote));
	if (!iEditBar && iToolBar)
		{
		CEikToolBar* bar = new(ELeave) CEikToolBar;
		CleanupStack::PushL(bar);
		bar->ConstructL(this, R_PT_EDIT_TOOLBAR, TRect(TPoint(0, 0), iEikonEnv->ScreenDevice()->SizeInPixels()));
		CleanupStack::Pop();
		iEditBar = bar;
		iEditBar->MakeVisible(EFalse);
		TRAP_IGNORE(
			ButtonPictureL(iEditBar, EPtCmdEdSave, EMbmToolSave);
			ButtonPictureL(iEditBar, EPtCmdEdFind, EMbmToolFind);
			ButtonPictureL(iEditBar, EPtCmdEdGoto, EMbmToolGoto);
			ButtonPictureL(iEditBar, EPtCmdEdClose, EMbmToolClose);
			);
		}
	AddToStackL(iEdit);
	iEditStacked = ETrue;
	UpdateStackedControlFlags(iView, ECoeStackFlagRefusesAllKeys | ECoeStackFlagRefusesFocus,
		ECoeStackFlagRefusesAllKeys | ECoeStackFlagRefusesFocus);
	iMenuBar->ChangeMenuBarL(R_PT_EDIT_HOTKEYS, R_PT_EDIT_MENUBAR, EFalse);
	iEdit->ActivateL();
	if (iToolBar && iEditBar && iView->Settings().iToolbar)
		{
		iToolBar->MakeVisible(EFalse);
		iEditBar->MakeVisible(ETrue);
		iEditBar->DrawNow();
		}
	iView->SetFocus(EFalse);
	iEdit->SetFocus(ETrue);
	iEdit->DrawNow();
	iEdit->ShowL();                          // the editor, now the view is on screen
	iEdit->DrawNow();
	}

// Closes the editor; asks first if there are changes not saved. EFalse:
// it stays (saving failed)
TBool CPsiTermAppUi::CloseEditorL()
	{
	if (!iEdit)
		return ETrue;
	if (iEdit->Modified())
		{
		if (iEikonEnv->QueryWinL(_L("The file has changes not saved on the server"), _L("Save before closing?")))
			{
			if (!iEdit->SaveL(EFalse))
				return EFalse;
			}
		}
	if (iEditStacked)
		RemoveFromStack(iEdit);
	iEditStacked = EFalse;
	delete iEdit;
	iEdit = NULL;
	UpdateStackedControlFlags(iView, 0, ECoeStackFlagRefusesAllKeys | ECoeStackFlagRefusesFocus);
	TRAP_IGNORE(iMenuBar->ChangeMenuBarL(R_PT_HOTKEYS, R_PT_MENUBAR, EFalse));
	if (iEditBar)
		iEditBar->MakeVisible(EFalse);
	if (iToolBar && iView->Settings().iToolbar)
		{
		iToolBar->MakeVisible(ETrue);
		iToolBar->DrawNow();
		}
	iView->SetFocus(ETrue);
	iView->DrawNow();
	return ETrue;
	}

// The editor's commands; EFalse if aCommand is not one
TBool CPsiTermAppUi::EditorCommandL(TInt aCommand)
	{
	if (aCommand == EPtCmdEditFile)
		{
		if (iEdit)
			{
			if (!CloseEditorL())
				return ETrue;
			}
		CloseStart();
		PtEditFileL(*iView);
		return ETrue;
		}
	if (aCommand >= EPtCmdEdRecent0 && aCommand < EPtCmdEdRecent0 + KRecentMax)
		{
		TBuf8<512> paths[KRecentMax];
		TInt n = RecentForServerL(*iView, paths, KRecentMax);
		TInt i = aCommand - EPtCmdEdRecent0;
		if (i < n)
			{
			if (!iView->XferShared())
				{
				iEikonEnv->InfoMsg(_L("Not available - SSH is not connected"));
				return ETrue;
				}
			OpenEditorL(paths[i], 0);
			}
		return ETrue;
		}
	if (aCommand < EPtCmdEdSave || aCommand > EPtCmdEdLast)
		return EFalse;
	if (aCommand == EPtCmdEdClose)
		{
		CloseEditorL();
		return ETrue;
		}
	if (iEdit)
		iEdit->CommandL(aCommand);
	return ETrue;
	}

// File > Recent: this server's files (the cascade, filled when it opens)
void CPsiTermAppUi::EditorRecentMenuL(CEikMenuPane* aMenuPane)
	{
	TBuf8<512> paths[KRecentMax];
	TInt n = RecentForServerL(*iView, paths, KRecentMax);
	// (1.04) the first in the resource's "(none yet)" (an item there, or ER5's
	// Right arrow would not open the cascade), the rest added
	for (TInt i = 0; i < n; i++)
		{
		CEikMenuPane::TItem::SData added;
		CEikMenuPane::TItem::SData& item = i == 0 ? aMenuPane->ItemData(EPtCmdEdRecent0) : added;
		item.iCommandId = EPtCmdEdRecent0 + i;
		item.iCascadeId = 0;
		item.iFlags = 0;
		TBuf<120> t;
		PtUtf8ToText(paths[i], t);
		// the name, then (shortened) where it is
		TInt slash = t.LocateReverse('/');
		item.iText = Upto(slash >= 0 ? t.Mid(slash + 1) : TPtrC(t), item.iText.MaxLength());
		item.iExtraText.Zero();
		if (slash > 0)
			{
			TPtrC dir(t.Left(slash));
			item.iExtraText = dir.Length() > 20 ? dir.Right(20) : dir;
			}
		if (i > 0)
			aMenuPane->AddMenuItemL(item);
		}
	}

void CPsiTermAppUi::EditorMenuPaneL(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	if (iEdit)
		iEdit->InitMenuPaneL(aMenuId, aMenuPane);
	}

void CPsiTermAppUi::EditorRectL(const TRect& aRect)
	{
	if (iEdit)
		iEdit->SetViewRectL(aRect);
	}

void CPsiTermAppUi::DeleteEditor()
	{
	if (iEdit)
		{
		if (iEditStacked)
			RemoveFromStack(iEdit);
		delete iEdit;
		iEdit = NULL;
		}
	delete iEditBar;
	iEditBar = NULL;
	}
