// pglinktest.cpp - Connection settings > Test, compiled into PsiTerm.app,
// PsiMail.app and PsiWeb.app (each .mmp: SUBPROJECT psitermssh). The test
// is psiglue.cpp's pg_link_test (only that part of the file is compiled
// here); this adds the EIKON side, so each app's dialog only reads its
// lines and calls PgLinkTestL.
#define PG_LINK_TEST_ONLY
#include "psiglue.cpp"

#include <eikenv.h>
#include <eikdialg.h>
#include <eiklabel.h>

// The result: one label per sentence, unused lines removed (a hidden line
// would still take its height)
class CPgLinkResultDialog : public CEikDialog
	{
public:
	CPgLinkResultDialog(const PgLinkTest& aTest, TInt aFirstLineId, const TDesC* aTitle = 0)
		: iTest(aTest), iFirstLineId(aFirstLineId), iTitle(aTitle) {}
private:
	void PreLayoutDynInitL();
	void SetSizeAndPositionL(const TSize& aSize);
	const PgLinkTest& iTest;
	TInt iFirstLineId;
	const TDesC* iTitle;               // another title than the resource's, or 0
	};

void CPgLinkResultDialog::PreLayoutDynInitL()
	{
	if (iTitle)
		SetTitleL(*iTitle);
	for (TInt i = 0; i < PG_LT_LINES; i++)
		{
		if (i < iTest.nlines)
			{
			TBuf<PG_LT_LINE> t;
			t.Copy(TPtrC8((const TUint8*)iTest.line[i]));
			SetLabelL(iFirstLineId + i, t);
			}
		else
			DeleteLine(iFirstLineId + i);
		}
	}

// Centred, and never wider or taller than the screen
void CPgLinkResultDialog::SetSizeAndPositionL(const TSize& aSize)
	{
	TSize screen = iEikonEnv->ScreenDevice()->SizeInPixels();
	TSize size(aSize.iWidth < screen.iWidth - 8 ? aSize.iWidth : screen.iWidth - 8,
		aSize.iHeight < screen.iHeight - 8 ? aSize.iHeight : screen.iHeight - 8);
	SetCornerAndSizeL(EHCenterVCenter, size);
	}

// The busy message, bottom left, shown at once: the test blocks this thread,
// so it is drawn and flushed here rather than after the usual delay
static void PgLinkBusy(void* aCtx, const char* aText)
	{
	CEikonEnv* env = (CEikonEnv*)aCtx;
	TBuf<80> t;
	t.Copy(TPtrC8((const TUint8*)aText));
	TRAPD(err, env->BusyMsgL(t, EHLeftVBottom, TTimeIntervalMicroSeconds32(0)));
	(void)err;
	env->WsSession().Flush();
	}

static void PgLinkRunL(int aBaudIndex, int aRtsCts, int aNetMode, const TDesC8& aPppStart,
	int aResultDialog, int aFirstLineId, TBool aConnect, const TDesC* aTitle)
	{
	CEikonEnv* env = CEikonEnv::Static();
	PgLinkTest* t = new(ELeave) PgLinkTest;
	CleanupStack::PushL(t);
	Mem::FillZ(t, sizeof(PgLinkTest));
	t->baud_index = aBaudIndex;
	t->rtscts = aRtsCts ? 1 : 0;
	t->net_mode = aNetMode ? 1 : 0;
	TInt n = aPppStart.Length();
	if (n > (TInt)sizeof(t->ppp_start) - 1)
		n = sizeof(t->ppp_start) - 1;
	Mem::Copy(t->ppp_start, aPppStart.Ptr(), n);
	t->ppp_start[n] = 0;
	t->progress = PgLinkBusy;
	t->ctx = env;
	t->dial = aConnect ? 1 : 0;         // Connect: dial at once, nothing to ask
	pg_link_test(t);
	env->BusyMsgCancel();
	if (t->need_dial)
		{
		// never dial unasked: statement, then the question
		if (CEikonEnv::QueryWinL(_L("The Psion's Internet connection is not up"), _L("Connect now to test it?")))
			{
			t->dial = 1;
			pg_link_test(t);
			env->BusyMsgCancel();
			}
		}
	CPgLinkResultDialog* d = new(ELeave) CPgLinkResultDialog(*t, aFirstLineId, aTitle);
	d->ExecuteLD(aResultDialog);
	CleanupStack::PopAndDestroy();       // t
	}

void PgLinkTestL(int aBaudIndex, int aRtsCts, int aNetMode, const TDesC8& aPppStart,
	int aResultDialog, int aFirstLineId)
	{
	PgLinkRunL(aBaudIndex, aRtsCts, aNetMode, aPppStart, aResultDialog, aFirstLineId, EFalse, 0);
	}

void PgLinkConnectL(int aBaudIndex, int aRtsCts, int aNetMode, const TDesC8& aPppStart,
	int aResultDialog, int aFirstLineId, const TDesC8& aTitle)
	{
	PgLinkRunL(aBaudIndex, aRtsCts, aNetMode, aPppStart, aResultDialog, aFirstLineId, ETrue, &aTitle);
	}
