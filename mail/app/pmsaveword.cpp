// PMSAVEWORD.CPP - File > Save as Word file (Shift+Ctrl+S): the open
// message as a Psion Word file
//
// A Word file (sysdoc docform/dfword.html) is a direct file store with the
// UIDs KDirectFileStoreLayoutUid, KUidAppDllDoc and Word's own (0x1000007F),
// whose root is a stream dictionary of the "document head streams": the
// application identifier (KUidAppIdentifierStream), Word's own UI settings
// (0x10000243), and the model's - styles, print setup, the plain text and
// the rich text markup - which CWordModel (wngmodel.h, the Word engine in
// wpeng.dll, the same one the Word program uses) writes with StoreL. So the
// message is put into a CWordModel's rich text and the model stored: Word
// opens the result as one of its own.
//
// What goes in is what the reader shows: the subject (larger, bold), From,
// To, Cc and Date with their names in bold, the attachments' names, an
// invitation's box, then the text with its bold, italic and underlining,
// headings, quotes and lists (their indents) as the reader has them.
// Pictures become "[Picture]": Word on the Psion takes pictures only as
// embedded objects of other programs (Sketch), not as plain bitmaps.

#include <eikenv.h>
#include <eikcfdlg.h>
#include <eikrted.h>
#include <txtrich.h>
#include <txtfmlyr.h>
#include <s32file.h>
#include <apparc.h>
#include <apaid.h>
#include <apadef.h>
#include <wngmodel.h>
#include <prnsetup.h>
#include <eikon.rsg>
#include "pmapp.h"

const TUid KUidPmWordApp = { 0x1000007F };            // KUidWordAppValue (eikdef.hrh, narrow)
const TUid KUidPmWordUiStream = { 0x10000243 };       // KUidWordAppUiStream (word.cpp)

// Word's own UI settings as a fresh document of the built-in Word has them
// (toolbar and toolband shown, the standard zoom); without the stream the
// Word program uses its defaults anyway
static const TUint8 KWordUi[14] = { 0x02, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x03, 0x00, 0x00 };

// the subject as a filename: no characters a filename can't have
static void FileNameFrom(TDes& aName, const TDesC& aSubject)
	{
	aName.Zero();
	for (TInt i = 0; i < aSubject.Length() && aName.Length() < 40; i++)
		{
		TText c = aSubject[i];
		if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || c < 32)
			c = ' ';
		aName.Append(c);
		}
	aName.Trim();
	while (aName.Length() && aName[aName.Length() - 1] == '.')
		aName.SetLength(aName.Length() - 1);
	if (!aName.Length())
		aName = _L("Message");
	}

// the reader's rich text into the Word model's: text, character formats
// (typeface, size, bold, italic, underline) and paragraph indents/spacing
static void CopyRichTextL(const CRichText& aFrom, CRichText& aTo)
	{
	TInt len = aFrom.DocumentLength();
	TInt pos = 0;
	CParaFormat* pf = CParaFormat::NewLC();
	while (pos < len)
		{
		// a paragraph
		TInt paraStart = pos;
		TInt dstStart = aTo.DocumentLength();
		TBool paraEnd = EFalse;
		while (pos < len && !paraEnd)
			{
			TPtrC view;
			TCharFormat cf;
			aFrom.GetChars(view, cf, pos);
			if (view.Length() == 0)
				{
				pos++;
				continue;
				}
			if (pos + view.Length() > len)
				view.Set(view.Left(len - pos));
			// up to the end of the paragraph, at most
			TInt para = view.Locate(CEditableText::EParagraphDelimiter);
			if (para >= 0)
				{
				view.Set(view.Left(para + 1));
				paraEnd = ETrue;
				}
			TInt at = aTo.DocumentLength();
			// pictures: words instead
			TInt pic;
			TPtrC rest = view;
			while ((pic = rest.Locate(CEditableText::EPictureCharacter)) >= 0)
				{
				if (pic) aTo.InsertL(aTo.DocumentLength(), rest.Left(pic));
				aTo.InsertL(aTo.DocumentLength(), _L("[Picture]"));
				rest.Set(rest.Mid(pic + 1));
				}
			if (rest.Length())
				aTo.InsertL(aTo.DocumentLength(), rest);
			TInt added = aTo.DocumentLength() - at;
			if (added > 0)
				{
				TCharFormatMask cm;
				cm.SetAttrib(EAttFontTypeface);
				cm.SetAttrib(EAttFontHeight);
				cm.SetAttrib(EAttFontPosture);
				cm.SetAttrib(EAttFontStrokeWeight);
				cm.SetAttrib(EAttFontUnderline);
				aTo.ApplyCharFormatL(cf, cm, at, added);
				}
			pos += view.Length();
			}
		// the paragraph's indent and spacing, as the reader has them
		aFrom.GetParagraphFormatL(pf, paraStart);
		TParaFormatMask pm;
		pm.SetAttrib(EAttLeftMargin);
		pm.SetAttrib(EAttSpaceBefore);
		pm.SetAttrib(EAttSpaceAfter);
		pm.SetAttrib(EAttBottomBorder);
		TInt dstLen = aTo.DocumentLength() - dstStart;
		if (dstLen > 0)
			aTo.ApplyParaFormatL(pf, pm, dstStart, dstLen);
		}
	CleanupStack::PopAndDestroy();           // pf
	}

// writes the Word file
static void WriteWordFileL(RFs& aFs, const TDesC& aPath, const CRichText& aText)
	{
	CWordModel* model = NULL;
	TRAPD(err, model = CWordModel::NewL(NULL, NULL, _L("Z:\\System\\Printers\\")));
	if (err != KErrNone || !model)
		User::Leave(err != KErrNone ? err : KErrNoMemory);
	CleanupStack::PushL(model);
	// the print setup is stored with the document, and storing it needs a
	// printer (CPrintSetup panics PRINT 1 without one): the Psion's own
	// default, as Word starts with, else the first driver there is
	// the print setup is stored with the document, and storing it needs a
	// printer device (CPrintSetup panics PRINT 1 without one - even
	// PrinterDevice() does), which CWordModel doesn't make: the Psion's
	// "General" driver if it has one, else the first
	CPrintSetup* ps = model->PrintSetup();
	if (!ps)
		User::Leave(KErrNotSupported);
	ps->AddPrinterDriverDirL(_L("\\System\\Printers\\"));
	CPrinterModelList* list = NULL;
	TRAPD(derr, list = ps->ModelNameListL(aFs));
	TInt models = derr == KErrNone && list ? list->ModelCount() : 0;
	if (models > 0)
		{
		TInt pick = 0;
		for (TInt i = 0; i < models; i++)
			{
			TPrinterModelName n = (*list)[i].iModelName;
			if (Clip(n, 7).CompareF(_L("General")) == 0) { pick = i; break; }
			}
		TRAP(derr, ps->CreatePrinterDeviceL(pick));
		}
	ps->FreeModelList();
	if (models <= 0 || derr != KErrNone)
		User::Leave(KErrNotSupported);
	CRichText* text = model->Text();
	if (!text)
		User::Leave(KErrNotSupported);
	text->Reset();
	CopyRichTextL(aText, *text);

	CFileStore* store = CDirectFileStore::ReplaceLC(aFs, aPath, EFileWrite);
	store->SetTypeL(TUidType(KDirectFileStoreLayoutUid, KUidAppDllDoc, KUidPmWordApp));
	CStreamDictionary* dict = CStreamDictionary::NewLC();
	// which program: Word (named as the Word program names itself)
	TApaAppIdentifier id(KUidPmWordApp, _L("Word.app"));
	RStoreWriteStream s;
	TStreamId sid = s.CreateLC(*store);
	id.ExternalizeL(s);
	s.CommitL();
	CleanupStack::PopAndDestroy();           // s
	dict->AssignL(KUidAppIdentifierStream, sid);
	// Word's UI settings
	sid = s.CreateLC(*store);
	s.WriteL(KWordUi, sizeof(KWordUi));
	s.CommitL();
	CleanupStack::PopAndDestroy();           // s
	dict->AssignL(KUidPmWordUiStream, sid);
	// the document: styles, print setup, text, markup
	model->StoreL(*store, *dict, NULL);
	// the dictionary is the root
	sid = s.CreateLC(*store);
	dict->ExternalizeL(s);
	s.CommitL();
	CleanupStack::PopAndDestroy();           // s
	store->SetRootL(sid);
	store->CommitL();
	CleanupStack::PopAndDestroy(3);          // dict, store, model
	}

void CPmView::SaveAsWordL()
	{
	if (iMode != EMessage || !iReader)
		{
		Toast(_L("Open a message to save it"));
		return;
		}
	if (iWaitingBody)
		{
		Toast(_L("Not available - the message isn't downloaded yet"));
		return;
		}
	TBuf<100> subject;
	const TPmRow* row = CurrentRow();
	if (row && row->iSubject.Length()) subject = Clip(row->iSubject, 100);
	else if (!MessageHeader(_L("Subject"), subject)) subject.Zero();
	TFileName name(_L("C:\\Documents\\"));
	TBuf<44> file;
	FileNameFrom(file, subject);
	name.Append(file);
	RFs& fs = iCoeEnv->FsSession();
	fs.MkDirAll(_L("C:\\Documents\\"));
	TBuf<40> title(_L("Save as Word file"));
	CEikFileSaveAsDialog* dlg = new(ELeave) CEikFileSaveAsDialog(&name, &title, NULL, EFalse);
	if (!dlg->ExecuteLD(R_EIK_DIALOG_FILE_SAVEAS))
		return;
	iEikonEnv->BusyMsgL(_L("Saving..."), EHLeftVBottom, TTimeIntervalMicroSeconds32(300000));
	TRAPD(err, WriteWordFileL(fs, name, *iReader->RichText()));
	iEikonEnv->BusyMsgCancel();
	if (err != KErrNone)
		{
		fs.Delete(name);                     // (not half a file)
		TBuf<120> t;
		if (err == KErrDiskFull) t = _L("No room left on the disk for the Word file");
		else if (err == KErrInUse || err == KErrAccessDenied) t = _L("The Word file is in use or read-only");
		else if (err == KErrNotSupported) t = _L("No printer drivers - Word files need one for their page setup");
		else t.Format(_L("The Word file could not be written (%d)"), err);
		Toast(t);
		return;
		}
	Toast(_L("Saved"));
	}
