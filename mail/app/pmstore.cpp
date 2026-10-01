// PMSTORE.CPP - where PsiMail keeps the mail on a disk
//
// The style guide (10.2.1): files that are not explicitly the user's go
// under \System, out of sight, so they are neither deleted by someone
// tidying up nor clutter the System screen. PsiMail's store (the folders,
// the messages kept to read offline, the outbox, the calendar's files,
// the certificate pins) is the program's own working data, so it lives in
//
//     <disk>:\System\Data\PsiMail\
//
// \System\Data\ is EPOC's folder for programs' data (Contacts.cdb is
// there). Not \System\Mail\: that is the Message Server's own store
// (KMsvDefaultFolder in msvstd.h) - it rebuilds its index there and can
// move the whole folder to another disk with the built-in Email program's
// messages, which must not take PsiMail's files with it.
//
// Attachments the user saves (Message > Attachments > Save) are the
// user's files, so they go where the user keeps files and can see them:
// <disk>:\Documents\Attachments\.
//
// Before 0.74 the store was <disk>:\PsiMail\. The first time PsiMail opens
// a disk that still has that and not the new one, the folder is renamed
// into place - one quick rename on the same disk, whatever is in it. If
// that fails (a file in use, the disk full or read-only) PsiMail goes on
// using the old folder, says so once, and writes why to Store.log in
// PsiMail's own folder; it tries again next time it opens.
//
// Removing PsiMail leaves the store: it is the user's mail (Help says so).

#include <eikenv.h>
#include "pmapp.h"

_LIT(KOldStore, "?:\\PsiMail");
_LIT(KNewStore, "?:\\System\\Data\\PsiMail");
_LIT(KNewParent, "?:\\System\\Data\\");
_LIT(KUserAttach, "?:\\Documents\\Attachments");
_LIT(KUserDocs, "?:\\Documents\\");
_LIT(KStoreLog, "Store.log");

static void OnDrive(TDes& aPath, const TDesC& aPattern, TChar aDrive)
	{
	aPath = aPattern;
	aPath[0] = (TText)aDrive;
	}

static TBool IsDir(RFs& aFs, const TDesC& aPath)
	{
	TEntry e;
	return aFs.Entry(aPath, e) == KErrNone && e.IsDir();
	}

// a line in Store.log, beside PsiMail.app (for when something went wrong)
static void StoreLog(RFs& aFs, const TDesC& aText)
	{
	TParse parse;
	parse.Set(CEikonEnv::Static()->EikAppUi()->Application()->AppFullName(), NULL, NULL);
	TFileName path(parse.DriveAndPath());
	path.Append(KStoreLog);
	RFile f;
	TInt r = f.Open(aFs, path, EFileWrite | EFileShareExclusive);
	if (r == KErrNotFound)
		r = f.Create(aFs, path, EFileWrite | EFileShareExclusive);
	if (r != KErrNone)
		return;
	TInt size = 0;
	f.Size(size);
	if (size < 8 * 1024)                     // (it only grows on trouble, but even so)
		{
		TBuf8<200> line;
		TTime now;
		now.HomeTime();
		TDateTime d = now.DateTime();
		line.Format(_L8("%04d-%02d-%02d %02d:%02d "), d.Year(), d.Month() + 1, d.Day() + 1, d.Hour(), d.Minute());
		TInt room = line.MaxLength() - line.Length() - 2;
		TPtrC t = aText.Left(aText.Length() < room ? aText.Length() : room);
		TBuf8<200> t8;
		t8.Copy(t);
		line.Append(t8);
		line.Append(_L8("\r\n"));
		TInt end = 0;
		f.Seek(ESeekEnd, end);
		f.Write(line);
		}
	f.Close();
	}

// The store's folder on a disk (ending with '\'), moving an old one there
// first if need be (see the top). aDrive is 'C' or 'D'.
void CPmView::StoreRoot(TChar aDrive, TDes& aRoot)
	{
	RFs& fs = iCoeEnv->FsSession();
	TBuf<40> oldDir, newDir;
	OnDrive(oldDir, KOldStore, aDrive);
	OnDrive(newDir, KNewStore, aDrive);
	TInt bit = aDrive == 'C' ? 1 : 2;
	if (!(iStoreTried & bit) && IsDir(fs, oldDir) && !IsDir(fs, newDir))
		{
		iStoreTried |= bit;
		CEikonEnv* env = CEikonEnv::Static();
		TRAPD(busy, env->BusyMsgL(_L("Moving the mail into the System folder..."), EHLeftVBottom, TTimeIntervalMicroSeconds32(0)));
		TBuf<40> parent;
		OnDrive(parent, KNewParent, aDrive);
		TInt r = fs.MkDirAll(parent);
		if (r == KErrAlreadyExists)
			r = KErrNone;
		if (r == KErrNone)
			r = fs.Rename(oldDir, newDir);
		if (busy == KErrNone)
			env->BusyMsgCancel();
		TBuf<160> log;
		if (r != KErrNone)
			{
			// keep using the old folder: nothing has moved
			log.Format(_L("could not move %S to %S (%d) - still using %S"), &oldDir, &newDir, r, &oldDir);
			StoreLog(fs, log);
			env->InfoMsg(_L("The mail could not be moved - it stays in \\PsiMail"));
			aRoot = oldDir;
			aRoot.Append('\\');
			return;
			}
		log.Format(_L("moved %S to %S"), &oldDir, &newDir);
		StoreLog(fs, log);
		// the attachments saved under the old store are the user's files:
		// out to \Documents\Attachments, if there isn't one already
		TBuf<60> oldAtt, userAtt, docs;
		oldAtt = newDir;
		oldAtt.Append(_L("\\Attachments"));
		OnDrive(userAtt, KUserAttach, aDrive);
		OnDrive(docs, KUserDocs, aDrive);
		if (IsDir(fs, oldAtt) && !IsDir(fs, userAtt))
			{
			TInt d = fs.MkDirAll(docs);
			if (d == KErrNone || d == KErrAlreadyExists)
				d = fs.Rename(oldAtt, userAtt);
			if (d != KErrNone)
				{
				log.Format(_L("saved attachments left in %S (%d)"), &oldAtt, d);
				StoreLog(fs, log);
				}
			}
		}
	if (!IsDir(fs, newDir) && IsDir(fs, oldDir))
		{
		// an earlier move failed this session: the old one, still
		aRoot = oldDir;
		aRoot.Append('\\');
		return;
		}
	aRoot = newDir;
	aRoot.Append('\\');
	}

// where Message > Attachments > Save puts them (ending with '\')
void CPmView::AttachRoot(TChar aDrive, TDes& aDir)
	{
	OnDrive(aDir, KUserAttach, aDrive);
	aDir.Append('\\');
	}
