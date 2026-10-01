// PMPRINT.CPP - File > Printing: Page setup, Print setup, Print preview and
// Print, with EIKON's own print dialogs, as Word and the built-in Email
// program have them.
//
// What is printed is the message as the reader shows it: the subject, the
// From / To / Cc / Date lines and the attachments' names, then the text
// with its formatting and its pictures (the rich text of the reader, laid
// out again for the page by FORM). The printer is chosen with the standard
// dialogs, so whatever printer drivers are installed (PsiWin's printing via
// the PC, a serial, parallel or infrared printer) can be used; with none
// installed, EIKON says so in its own words.
//
// The print setup starts as EIKON's default one (CEikonEnv::NewDefaultPrintSetupL)
// and the printer, page size and margins chosen are kept in Print.ini.

#include <eikenv.h>
#include <eikprtdg.h>
#include <eikprtpv.h>
#include <eikpprob.h>
#include <eikon.rsg>
#include <prnsetup.h>
#include <prninf.h>
#include <frmprint.h>
#include <frmpage.h>
#include <fldbltin.h>
#include <txtrich.h>
#include <s32file.h>
#include <eikrted.h>
#include "pmapp.h"

_LIT(KPrintIni, "C:\\System\\Apps\\PsiMail\\Print.ini");
const TUint32 KPrintIniMagic = 0x544e5250;      // 'PRNT'
const TInt KPreviewBands = 4;                   // bands per page in the preview (as Word's EIKON example)

class CPmPrinter : public CBase, public MPrintPreviewDialogObserver,
	public MFieldFileNameInfo, public MFieldNumPagesInfo
	{
public:
	~CPmPrinter();
	void PageSetupL();
	void PrintSetupL(CRichText* aText, const TDesC& aTitle);   // aText may be NULL
	void PreviewL(CRichText* aText, const TDesC& aTitle);
	void PrintL(CRichText* aText, const TDesC& aTitle);
private:
	// MPrintPreviewDialogObserver (ETrue = cancelled)
	TBool PageSetupChangedL(CPrintSetup* aPrintSetup, TInt& aNumPagesInDoc);
	TBool RunPrintRangeDialogL(CPrintSetup* aPrintSetup, TInt& aNumPagesInDoc);
	// a page header or footer's fields
	TInt UpdateFieldFileName(TPtr& aValueText) const;
	TInt UpdateFieldNumPages() const;
	void SetupL();
	void UseTextL(CRichText* aText, const TDesC& aTitle);
	TInt PaginateL();
	TBool RangeDialogL(TBool aPrintButton);
	void ProgressL();
	void Save();
	void SaveL(RWriteStream& aOut);
	void Restore();
	void RestoreL(RReadStream& aIn);
private:
	CPrintSetup* iSetup;
	CTextPageRegionPrinter* iPrinter;
	CArrayFixFlat<TInt>* iPages;
	TPrintParameters iParams;
	CRichText* iText;                // the reader's (not ours)
	TBuf<80> iTitle;
	};

CPmPrinter::~CPmPrinter()
	{
	delete iPrinter;
	delete iPages;
	delete iSetup;
	}

// EIKON's default print setup (the printer, A4...), then what was chosen before
void CPmPrinter::SetupL()
	{
	if (iSetup)
		return;
	iSetup = CEikonEnv::Static()->NewDefaultPrintSetupL();
	iSetup->Header()->SetFileNameInfo(*this);
	iSetup->Header()->SetNumPagesInfo(*this);
	iSetup->Footer()->SetFileNameInfo(*this);
	iSetup->Footer()->SetNumPagesInfo(*this);
	Restore();
	iPages = new(ELeave) CArrayFixFlat<TInt>(4);
	iParams.iNumCopies = 1;
	iParams.iFirstPage = 0;
	iParams.iLastPage = 0;
	}

void CPmPrinter::Restore()
	{
	RFs& fs = CEikonEnv::Static()->FsSession();
	RFileReadStream in;
	if (in.Open(fs, KPrintIni, EFileRead) != KErrNone)
		return;
	TRAPD(err, RestoreL(in));
	(void)err;
	in.Close();
	}

void CPmPrinter::RestoreL(RReadStream& aIn)
	{
	if (aIn.ReadUint32L() != KPrintIniMagic)
		User::Leave(KErrCorrupt);
	TUid model;
	model.iUid = aIn.ReadInt32L();
	TPageSpec spec;
	spec.InternalizeL(aIn);
	TPageMargins margins;
	margins.InternalizeL(aIn);
	if (model != iSetup->PrinterDevice()->Model().iUid)
		iSetup->CreatePrinterDeviceL(model, CEikonEnv::Static()->FsSession());   // (gone since: EIKON's default instead)
	if (spec.iPortraitPageSize.iWidth > 0 && spec.iPortraitPageSize.iHeight > 0)
		iSetup->PrinterDevice()->SelectPageSpecInTwips(spec);
	iSetup->iPageMarginsInTwips = margins;
	}

void CPmPrinter::Save()
	{
	if (!iSetup)
		return;
	RFs& fs = CEikonEnv::Static()->FsSession();
	fs.MkDirAll(KPrintIni);
	RFileWriteStream out;
	if (out.Replace(fs, KPrintIni, EFileWrite) != KErrNone)
		return;
	TRAPD(err, SaveL(out));
	(void)err;
	out.Close();
	}

void CPmPrinter::SaveL(RWriteStream& aOut)
	{
	aOut.WriteUint32L(KPrintIniMagic);
	aOut.WriteInt32L(iSetup->PrinterDevice()->Model().iUid.iUid);
	iSetup->PrinterDevice()->CurrentPageSpecInTwips().ExternalizeL(aOut);
	iSetup->iPageMarginsInTwips.ExternalizeL(aOut);
	aOut.CommitL();
	}

TInt CPmPrinter::UpdateFieldFileName(TPtr& aValueText) const
	{
	if (iTitle.Length() > aValueText.MaxLength())
		return iTitle.Length();
	aValueText = iTitle;
	return 0;
	}

TInt CPmPrinter::UpdateFieldNumPages() const
	{
	return iPages ? iPages->Count() : 1;
	}

// the text to print, laid out for the printer and the page
void CPmPrinter::UseTextL(CRichText* aText, const TDesC& aTitle)
	{
	iText = aText;
	iTitle.Copy(aTitle.Left(aTitle.Length() < iTitle.MaxLength() ? aTitle.Length() : iTitle.MaxLength()));
	if (!iPrinter)
		iPrinter = CTextPageRegionPrinter::NewL(iText, iSetup->PrinterDevice());
	iPrinter->SetDocument(iText);
	iPrinter->SetPrinterDevice(iSetup->PrinterDevice());
	iPrinter->SetPageList(iPages);
	iPrinter->SetFirstPageOfDoc(0);
	iPrinter->SetParagraphFillTextOnly(ETrue);
	}

// pages for the printer and page chosen: the number of them
TInt CPmPrinter::PaginateL()
	{
	CEikonEnv* env = CEikonEnv::Static();
	env->BusyMsgL(_L("Paginating..."), EHLeftVBottom, TTimeIntervalMicroSeconds32(500000));
	iPages->Reset();
	iPrinter->SetPrinterDevice(iSetup->PrinterDevice());
	iPrinter->SetPageMarginsInTwips(iSetup->iPageMarginsInTwips.iMargins);
	iPrinter->SetPageSpecInTwips(iSetup->PrinterDevice()->CurrentPageSpecInTwips());
	CTextPaginator* pg = CTextPaginator::NewL(iSetup->PrinterDevice(), iPages, CActive::EPriorityLow);
	CleanupStack::PushL(pg);
	pg->SetDocumentL(iText);
	pg->SetPageSpecInTwips(iSetup->PrinterDevice()->CurrentPageSpecInTwips());
	pg->SetPageMarginsInTwips(iSetup->iPageMarginsInTwips.iMargins);
	TInt pos = 0;
	pg->AppendTextL(pos);
	TInt n = pg->PaginationCompletedL();
	CleanupStack::PopAndDestroy();   // pg
	env->BusyMsgCancel();
	if (n < 1 || iPages->Count() < 1)
		{
		iPages->Reset();
		iPages->AppendL(iText->DocumentLength());
		}
	iParams.iFirstPage = 0;
	iParams.iLastPage = iPages->Count() - 1;
	return iPages->Count();
	}

// EIKON's Print (aPrintButton) or Print setup dialog: copies, pages, printer
TBool CPmPrinter::RangeDialogL(TBool aPrintButton)
	{
	TUid old = iSetup->PrinterDevice()->Model().iUid;
	TUid uid = old;
	CEikPrintRangeDialog* dlg = new(ELeave) CEikPrintRangeDialog(iParams, iSetup, uid, aPrintButton);
	dlg->iPageRange.iLowerLimit = 1;     // (as Word does: the dialog doesn't set these itself)
	dlg->iPageRange.iUpperLimit = iPages && iPages->Count() ? iPages->Count() : 1;
	TBool ok = dlg->ExecuteLD(R_EIK_DIALOG_PRINT_RANGE_SETUP);
	if (uid != old)
		{
		iSetup->CreatePrinterDeviceL(uid, CEikonEnv::Static()->FsSession());
		if (iPrinter)
			iPrinter->SetPrinterDevice(iSetup->PrinterDevice());
		}
	iSetup->FreeModelList();
	Save();
	return ok;
	}

void CPmPrinter::ProgressL()
	{
	iPrinter->SetPrintPreview(EFalse);
	CEikPrintProgressDialog* dlg = new(ELeave) CEikPrintProgressDialog(iSetup, iPrinter, iParams);
	dlg->ExecuteLD(R_EIK_DIALOG_PRINT_PROGRESS);
	iSetup->FreeModelList();
	}

void CPmPrinter::PageSetupL()
	{
	SetupL();
	CEikPageSetupDialog* dlg = new(ELeave) CEikPageSetupDialog(iSetup);
	if (dlg->ExecuteLD(R_EIK_DIALOG_PAGE_SPEC))
		Save();
	}

void CPmPrinter::PrintSetupL(CRichText* aText, const TDesC& aTitle)
	{
	SetupL();
	if (aText)
		{
		UseTextL(aText, aTitle);
		PaginateL();
		}
	RangeDialogL(EFalse);
	}

void CPmPrinter::PreviewL(CRichText* aText, const TDesC& aTitle)
	{
	SetupL();
	UseTextL(aText, aTitle);
	TInt pages = PaginateL();
	iPrinter->SetPrintPreview(ETrue);
	CEikPrintPreviewDialog* dlg = new(ELeave) CEikPrintPreviewDialog(*iSetup, *iPrinter, pages, this, KPreviewBands);
	if (dlg->ExecuteLD(R_EIK_DIALOG_PRINT_PREVIEW))
		ProgressL();                      // its Print button
	iPrinter->SetPrintPreview(EFalse);
	iSetup->FreeModelList();
	}

void CPmPrinter::PrintL(CRichText* aText, const TDesC& aTitle)
	{
	SetupL();
	UseTextL(aText, aTitle);
	PaginateL();
	TUid old = iSetup->PrinterDevice()->Model().iUid;
	if (!RangeDialogL(ETrue))
		return;
	if (iSetup->PrinterDevice()->Model().iUid != old)
		PaginateL();                      // another printer: other pages
	ProgressL();
	}

// the preview's Page setup button changed the page
TBool CPmPrinter::PageSetupChangedL(CPrintSetup* aPrintSetup, TInt& aNumPagesInDoc)
	{
	(void)aPrintSetup;
	aNumPagesInDoc = PaginateL();
	Save();
	return EFalse;
	}

// the preview's Print setup button
TBool CPmPrinter::RunPrintRangeDialogL(CPrintSetup* aPrintSetup, TInt& aNumPagesInDoc)
	{
	(void)aPrintSetup;
	TUid old = iSetup->PrinterDevice()->Model().iUid;
	RangeDialogL(EFalse);
	if (iSetup->PrinterDevice()->Model().iUid != old)
		aNumPagesInDoc = PaginateL();
	return EFalse;
	}

// ----- the commands ----------------------------------------------------------

void CPmAppUi::PrintCommandL(TInt aCommand)
	{
	if (!iPrinter)
		iPrinter = new(ELeave) CPmPrinter;
	if (aCommand == EPmCmdPageSetup)
		{
		iPrinter->PageSetupL();
		return;
		}
	TBuf<80> title;
	if (aCommand == EPmCmdPrintSetup)
		{
		// (the open message's pages, if one is open; the printer either way)
		CRichText* text = iView->Mode() == CPmView::EMessage ? iView->PrintTextL(title) : NULL;
		iPrinter->PrintSetupL(text, title);
		return;
		}
	CRichText* text = iView->PrintTextL(title);
	if (!text)
		return;
	if (aCommand == EPmCmdPrintPreview)
		iPrinter->PreviewL(text, title);
	else
		iPrinter->PrintL(text, title);
	}

// the message to print: the one open, or the one selected (opened first, as
// Reply does). NULL, having said why, if there is none or it isn't here yet
CRichText* CPmView::PrintTextL(TDes& aTitle)
	{
	if (iMode == EList && !iSidebar && CurrentRow())
		OpenCurrentL();
	if (iMode != EMessage || !CurrentRow())
		{
		Toast(_L("Nothing to print - select a message"));
		return NULL;
		}
	if (iWaitingBody || !iText)
		{
		Toast(_L("Not available until the message has downloaded"));
		return NULL;
		}
	Render();                                // (the reader is up to date)
	if (!iReader || iReaderUid != iMsgUid)
		{
		Toast(_L("Not available at this time"));
		return NULL;
		}
	const TDesC& s = CurrentRow()->iSubject;
	aTitle.Copy(s.Left(s.Length() < aTitle.MaxLength() ? s.Length() : aTitle.MaxLength()));
	return iReader->RichText();
	}

TBool CPmView::CanPrint() const
	{
	return (iMode == EList || iMode == EMessage) && CurrentRow() != NULL;
	}

// (for CPmAppUi's destructor, which pmprint.cpp alone knows how to delete)
void PmDeletePrinter(CPmPrinter* aPrinter)
	{
	delete aPrinter;
	}
