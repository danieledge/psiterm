// PMINVITE.CPP - invitations (and the contact card's box) in the reader
//
// The engine sums up a message's invitation in <uid>.inv and its contact
// cards in <uid>.vcd (../engine/invite.h). The reader shows them at the top,
// under the header, with the choices as links (Tab to one, Enter, or tap):
//
//   Invitation: Design review
//   When: Wed 7 Oct 2026, 10:00-11:30      Where: Room 2
//   Organiser: Alice Smith <alice@example.com>
//   Accept    Tentative    Decline
//
// and the same choices are on Edit > Invitation (Accept / Tentative /
// Decline / Remove from Agenda), as every pen action has a menu command.
//
// Accept and Tentative put the event in the Agenda (CPmCalSync::AddToAgendaL,
// as Event > Create new event does), from where calendar sync sends it to
// the server; each choice sends the organiser an iTIP reply (an outbox
// message with Itip headers, which ../engine/compose.c turns into a
// text/calendar METHOD:REPLY part) through the outbox as any message goes.
// A cancellation offers to remove the event from the Agenda. What was
// answered is kept in <uid>.inr, so the box says so next time.
//
// The contact card's box and Edit > Add to Contacts > Contact card are in
// pmvcard.cpp; the menu hooks for both, and for Save as Word document
// (pmsaveword.cpp), are at the end of this file.

#include <eikenv.h>
#include <eikmenup.h>
#include <txtrich.h>
#include <txtfmlyr.h>
#include <agmmodel.h>
#include <agclient.h>
#include <agmsiter.h>
#include <agmentry.h>
#include "pmapp.h"
#include "pmcontacts.h"

// ---------------------------------------------------------------- helpers

// a key's value in a summary file ("key TAB value" lines)
TPtrC PmKeyValue(const TDesC& aText, const TDesC& aKey)
	{
	TPtrC rest(aText);
	while (rest.Length())
		{
		TInt nl = rest.Locate('\n');
		TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
		if (line.Length() && line[line.Length() - 1] == '\r')
			line.Set(line.Left(line.Length() - 1));
		TInt tab = line.Locate('\t');
		if (tab == aKey.Length() && line.Left(tab).Compare(aKey) == 0)
			return line.Mid(tab + 1);
		}
	return TPtrC();
	}

static TInt Num(const TDesC& aText, TInt aPos, TInt aLen)
	{
	TInt v = 0;
	for (TInt i = aPos; i < aPos + aLen && i < aText.Length(); i++)
		{
		TInt c = aText[i];
		if (c < '0' || c > '9') return v;
		v = v * 10 + c - '0';
		}
	return v;
	}

// "YYYYMMDDHHMM" -> TTime (null if it isn't one)
static TTime LocalTime(const TDesC& aText)
	{
	if (aText.Length() < 8)
		return Time::NullTTime();
	TInt y = Num(aText, 0, 4), m = Num(aText, 4, 2), d = Num(aText, 6, 2);
	TInt hh = Num(aText, 8, 2), mm = Num(aText, 10, 2);
	if (y < 1900 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31 || hh > 23 || mm > 59)
		return Time::NullTTime();
	return TTime(TDateTime(y, TMonth(m - 1), d - 1, hh, mm, 0, 0));
	}

static TInt ToNum(const TDesC& aText)
	{
	TLex l(aText);
	TInt v = 0;
	l.Val(v);
	return v;
	}

// "Wed 7 Oct 2026" / "10:00" as the Psion shows them
static void AppendDate(TDes& aOut, const TTime& aTime)
	{
	static const TText* const KDays[] = { _S("Mon"), _S("Tue"), _S("Wed"), _S("Thu"), _S("Fri"), _S("Sat"), _S("Sun") };
	static const TText* const KMonths[] = { _S("Jan"), _S("Feb"), _S("Mar"), _S("Apr"), _S("May"), _S("Jun"),
		_S("Jul"), _S("Aug"), _S("Sep"), _S("Oct"), _S("Nov"), _S("Dec") };
	TDateTime d = aTime.DateTime();
	aOut.Append(TPtrC(KDays[aTime.DayNoInWeek()]));
	aOut.AppendFormat(_L(" %d "), d.Day() + 1);
	aOut.Append(TPtrC(KMonths[d.Month()]));
	aOut.AppendFormat(_L(" %d"), d.Year());
	}

static void AppendClock(TDes& aOut, const TTime& aTime)
	{
	TBuf<20> t;
	TRAPD(err, aTime.FormatL(t, _L("%-B%:0%J%:1%T%+B")));
	if (err != KErrNone)
		{
		TDateTime d = aTime.DateTime();
		t.Format(_L("%02d:%02d"), d.Hour(), d.Minute());
		}
	aOut.Append(t);
	}

// when: "Wed 7 Oct 2026, 10:00-11:30", "Thu 15 Oct 2026 (all day)"
void PmWhenText(TDes& aOut, const TTime& aStart, const TTime& aEnd, TBool aAllDay)
	{
	AppendDate(aOut, aStart);
	TDateTime s = aStart.DateTime(), e = aEnd.DateTime();
	TBool sameDay = s.Year() == e.Year() && s.Month() == e.Month() && s.Day() == e.Day();
	if (aAllDay)
		{
		if (!sameDay)
			{
			aOut.Append(_L(" to "));
			AppendDate(aOut, aEnd);
			}
		aOut.Append(_L(" (all day)"));
		return;
		}
	aOut.Append(_L(", "));
	AppendClock(aOut, aStart);
	if (aEnd > aStart)
		{
		aOut.Append('-');
		if (!sameDay)
			{
			AppendDate(aOut, aEnd);
			aOut.Append(' ');
			}
		AppendClock(aOut, aEnd);
		}
	}

static void AddStyle(CArrayFixFlat<TInt>& aStyles, TInt aPos, TInt aLen, TInt aKind)
	{
	if (aLen <= 0)
		return;
	TRAPD(err, { aStyles.AppendL(aPos); aStyles.AppendL(aLen); aStyles.AppendL(aKind); });
	(void)err;
	}

// ---------------------------------------------------------------- the files

// <uid>.inv / .vcd / .inr of the open message, read afresh
void CPmView::LoadInviteL()
	{
	delete iInvText;
	iInvText = NULL;
	delete iCardText;
	iCardText = NULL;
	iInvAnswer.Zero();
	if (iMode != EMessage || iWaitingBody || !iText)
		return;
	TBuf<150> path;
	MsgPath(iMsgUid, _L("inv"), path);
	ReadFileL(path, iInvText, 8 * 1024);
	if (iInvText && iInvText->Left(8).Compare(_L("#PSIINV1")) != 0)
		{
		delete iInvText;
		iInvText = NULL;
		}
	MsgPath(iMsgUid, _L("vcd"), path);
	ReadFileL(path, iCardText, 32 * 1024);
	if (iCardText && iCardText->Left(8).Compare(_L("#PSIVCD1")) != 0)
		{
		delete iCardText;
		iCardText = NULL;
		}
	MsgPath(iMsgUid, _L("inr"), path);
	HBufC* answer = NULL;
	ReadFileL(path, answer, 64);
	if (answer)
		{
		TPtrC a = *answer;
		TInt nl = a.Locate('\n');
		iInvAnswer.Copy(Clip(nl >= 0 ? a.Left(nl) : a, iInvAnswer.MaxLength()));
		delete answer;
		}
	}

TBool CPmView::HasInvite() const
	{
	return iMode == EMessage && iInvText != NULL;
	}

TBool CPmView::InviteIs(const TDesC& aMethod) const
	{
	return HasInvite() && PmKeyValue(*iInvText, _L("method")).Compare(aMethod) == 0;
	}

TPtrC CPmView::InviteField(const TDesC& aKey) const
	{
	return iInvText ? PmKeyValue(*iInvText, aKey) : TPtrC();
	}

// ---------------------------------------------------------------- the box

// The invitation's (or card's) box at the top of the reader: text into
// aText, styles as (position, length, kind) triples: 0 bold, 1 underlined
// (a link), 2 bigger, 3 italic, 4 a rule below
void CPmView::InviteBannerL(TDes& aText, CArrayFixFlat<TInt>& aStyles)
	{
	TRAPD(err, LoadInviteL());
	(void)err;
	const TChar KPara(CEditableText::EParagraphDelimiter);
	if (iInvText)
		{
		TPtrC method = InviteField(_L("method"));
		TPtrC summary = InviteField(_L("summary"));
		TInt p0 = aText.Length();
		TBuf<160> head;
		if (method.Compare(_L("cancel")) == 0)
			head = _L("Cancelled: ");
		else if (method.Compare(_L("reply")) == 0)
			{
			// someone answering an invitation of ours
			TPtrC who = InviteField(_L("replyname"));
			if (!who.Length()) who.Set(InviteField(_L("replyaddr")));
			TPtrC st = InviteField(_L("replystatus"));
			head.Append(Clip(who, 60));
			head.Append(st.Compare(_L("accepted")) == 0 ? _L(" has accepted: ") :
				st.Compare(_L("declined")) == 0 ? _L(" has declined: ") :
				st.Compare(_L("tentative")) == 0 ? _L(" might come: ") : _L(" has replied: "));
			}
		else if (method.Compare(_L("request")) == 0)
			head = _L("Invitation: ");
		else
			head = _L("Event: ");
		aText.Append(head);
		aText.Append(Clip(summary, 120));
		AddStyle(aStyles, p0, aText.Length() - p0, 0);
		AddStyle(aStyles, p0, aText.Length() - p0, 2);
		aText.Append(KPara);

		TTime start = LocalTime(InviteField(_L("start")));
		TTime end = LocalTime(InviteField(_L("end")));
		TBool allDay = ToNum(InviteField(_L("allday"))) != 0;
		if (start != Time::NullTTime())
			{
			p0 = aText.Length();
			aText.Append(_L("When: "));
			AddStyle(aStyles, p0, 6, 0);
			TBuf<80> w;
			PmWhenText(w, start, end == Time::NullTTime() ? start : end, allDay);
			aText.Append(w);
			if (ToNum(InviteField(_L("repeats"))))
				aText.Append(_L(" (repeats)"));
			if (!ToNum(InviteField(_L("tzok"))))
				aText.Append(_L(" (time zone not known)"));
			aText.Append(KPara);
			}
		TPtrC where = InviteField(_L("location"));
		if (where.Length())
			{
			p0 = aText.Length();
			aText.Append(_L("Where: "));
			AddStyle(aStyles, p0, 7, 0);
			aText.Append(Clip(where, 120));
			aText.Append(KPara);
			}
		TPtrC orgAddr = InviteField(_L("orgaddr"));
		if (orgAddr.Length() && method.Compare(_L("reply")) != 0)
			{
			p0 = aText.Length();
			aText.Append(_L("Organiser: "));
			AddStyle(aStyles, p0, 11, 0);
			TPtrC orgName = InviteField(_L("orgname"));
			if (orgName.Length())
				{
				aText.Append(Clip(orgName, 60));
				aText.Append(_L(" <"));
				aText.Append(Clip(orgAddr, 80));
				aText.Append('>');
				}
			else
				aText.Append(Clip(orgAddr, 80));
			aText.Append(KPara);
			}
		// the choices
		TInt last = aText.Length();
		if (method.Compare(_L("request")) == 0 || method.Compare(_L("publish")) == 0)
			{
			static const TText* const KChoice[] = { _S("Accept"), _S("Tentative"), _S("Decline") };
			TBool request = method.Compare(_L("request")) == 0;
			for (TInt c = 0; c < (request ? 3 : 1); c++)
				{
				if (c) aText.Append(_L("     "));
				TPmLinkRange lr;
				lr.iPos = aText.Length();
				aText.Append(request ? TPtrC(KChoice[c]) : TPtrC(_L("Add to Agenda")));
				lr.iLen = aText.Length() - lr.iPos;
				lr.iLink = KPmLinkAccept - c;
				iLinks->AppendL(lr);
				AddStyle(aStyles, lr.iPos, lr.iLen, 1);
				AddStyle(aStyles, lr.iPos, lr.iLen, 0);
				}
			if (iInvAnswer.Length())
				{
				p0 = aText.Length();
				aText.Append(_L("     "));
				aText.Append(iInvAnswer.CompareF(_L("accepted")) == 0 ? _L("You accepted") :
					iInvAnswer.CompareF(_L("tentative")) == 0 ? _L("You said tentative") :
					iInvAnswer.CompareF(_L("declined")) == 0 ? _L("You declined") : _L("In the Agenda"));
				AddStyle(aStyles, p0, aText.Length() - p0, 3);
				}
			aText.Append(KPara);
			}
		else if (method.Compare(_L("cancel")) == 0)
			{
			TPmLinkRange lr;
			lr.iPos = aText.Length();
			aText.Append(_L("Remove from Agenda"));
			lr.iLen = aText.Length() - lr.iPos;
			lr.iLink = KPmLinkRemove;
			iLinks->AppendL(lr);
			AddStyle(aStyles, lr.iPos, lr.iLen, 1);
			AddStyle(aStyles, lr.iPos, lr.iLen, 0);
			aText.Append(KPara);
			}
		else
			last = -1;
		if (last < 0)
			last = aText.Length() - 1;
		AddStyle(aStyles, last, aText.Length() - last, 4);
		}
	if (iCardText)
		CardBannerL(aText, aStyles);
	}

// a link in the box: Tab and Enter, or a tap
void CPmView::InviteLinkL(TInt aLink)
	{
	if (aLink == KPmLinkAddCard)
		AddCardsL();
	else if (aLink == KPmLinkRemove)
		RemoveInvitedL();
	else if (aLink <= KPmLinkAccept && aLink > KPmLinkAccept - 3)
		AnswerInviteL(KPmLinkAccept - aLink);
	}

// ---------------------------------------------------------------- the Agenda

// entries in the Agenda with this name that start then (all-day: that
// day): how many; removed if aRemove
static TInt AgendaMatchL(const TDesC& aFile, const TDesC& aTitle, const TTime& aStart, TBool aAllDay, TBool aRemove)
	{
	RFs fs;
	User::LeaveIfError(fs.Connect());
	TEntry entry;
	TInt r = fs.Entry(aFile, entry);
	fs.Close();
	if (r != KErrNone)
		User::Leave(KErrNotFound);
	CEikonEnv* env = CEikonEnv::Static();
	env->BusyMsgL(_L("Looking in the Agenda..."), EHLeftVBottom, TTimeIntervalMicroSeconds32(300000));
	CParaFormatLayer* para = CParaFormatLayer::NewL();
	CleanupStack::PushL(para);
	CCharFormatLayer* chr = CCharFormatLayer::NewL();
	CleanupStack::PushL(chr);
	RAgendaServ* serv = RAgendaServ::NewL();
	CleanupStack::PushL(serv);
	User::LeaveIfError(serv->Connect());
	CleanupClosePushL(*serv);
	CAgnEntryModel* model = CAgnEntryModel::NewL();
	CleanupStack::PushL(model);
	model->SetServer(serv);
	model->OpenL(aFile, TTimeIntervalMinutes(9 * 60), TTimeIntervalMinutes(9 * 60), TTimeIntervalMinutes(9 * 60));
	serv->WaitUntilLoaded();
	CArrayFixFlat<TAgnEntryId>* found = new(ELeave) CArrayFixFlat<TAgnEntryId>(4);
	CleanupStack::PushL(found);
	CAgnSyncIter* it = CAgnSyncIter::NewL(serv);
	CleanupStack::PushL(it);
	TDateTime sd = aStart.DateTime();
	TTime day(TDateTime(sd.Year(), sd.Month(), sd.Day(), 0, 0, 0, 0));
	TBuf<200> title;
	for (it->First(); it->Available(); it->Next())
		{
		if (it->HasBeenDeleted()) continue;
		CAgnEntry::TType type = it->Type();
		if (type != CAgnEntry::EAppt && type != CAgnEntry::EEvent) continue;
		CAgnEntry* e = NULL;
		TRAPD(err, e = model->FetchEntryL(it->EntryId()));
		if (err == KErrNoMemory) User::Leave(err);
		if (err != KErrNone || !e) continue;
		CleanupStack::PushL(e);
		TBool when;
		if (e->Type() == CAgnEntry::EAppt)
			when = !aAllDay && e->CastToAppt()->StartDateTime() == aStart;
		else
			{
			TDateTime id = e->InstanceStartDate().DateTime();
			when = aAllDay && TTime(TDateTime(id.Year(), id.Month(), id.Day(), 0, 0, 0, 0)) == day;
			}
		if (when)
			{
			CRichText* rt = e->RichTextL();
			title.Zero();
			if (rt)
				{
				TInt n = rt->DocumentLength();
				rt->Extract(title, 0, n < title.MaxLength() ? n : title.MaxLength());
				TInt para = title.Locate(CEditableText::EParagraphDelimiter);
				if (para >= 0) title.SetLength(para);
				}
			title.Trim();
			if (title.CompareF(Clip(aTitle, title.MaxLength())) == 0)
				found->AppendL(it->EntryId());
			}
		CleanupStack::PopAndDestroy();       // e
		}
	CleanupStack::PopAndDestroy();           // it
	TInt n = found->Count();
	if (aRemove)
		for (TInt i = 0; i < n; i++)
			model->DeleteEntryL((*found)[i]);
	CleanupStack::PopAndDestroy();           // found
	CleanupStack::PopAndDestroy();           // model
	serv->CloseAgenda();
	CleanupStack::PopAndDestroy();           // serv->Close()
	CleanupStack::Pop();                     // serv
	delete serv;
	CleanupStack::PopAndDestroy(2);          // chr, para
	env->BusyMsgCancel();
	return n;
	}

// the server already has it (calendar sync fetched it into events.txt):
// e.g. Fastmail puts invitations sent to you straight into your calendar
TBool CPmView::ServerHasEventL(const TDesC& aTitle, const TDesC& aStart)
	{
	if (!iCal->iCal.enabled)
		return EFalse;
	TFileName path;
	StoreDir(path);
	path.Append(_L("cal\\events.txt"));
	HBufC* ev = NULL;
	ReadFileL(path, ev, 256 * 1024);
	if (!ev)
		return EFalse;
	TBool found = EFalse;
	TPtrC rest = *ev;
	while (rest.Length() && !found)
		{
		TInt nl = rest.Locate('\n');
		TPtrC line = nl >= 0 ? rest.Left(nl) : rest;
		rest.Set(nl >= 0 ? rest.Mid(nl + 1) : TPtrC());
		// calid href etag recurid flags start end alarm summary location
		TPtrC f[10];
		TPtrC l = line;
		for (TInt k = 0; k < 10; k++)
			{
			TInt tab = l.Locate('\t');
			f[k].Set(tab >= 0 ? l.Left(tab) : l);
			l.Set(tab >= 0 ? l.Mid(tab + 1) : TPtrC());
			}
		if (f[5].Compare(Clip(aStart, 12)) == 0 && f[8].CompareF(Clip(aTitle, 200)) == 0)
			found = ETrue;
		}
	delete ev;
	return found;
	}

static void AgendaTrouble(CPmView& aView, TInt aErr)
	{
	TBuf<80> t;
	if (aErr == KErrNotFound) t = _L("No Agenda file where Calendar settings say");
	else if (aErr == KErrInUse || aErr == KErrLocked) t = _L("The Agenda file is in use - try again");
	else t.Format(_L("The Agenda could not be changed (%d)"), aErr);
	aView.Toast(t);
	}

// ---------------------------------------------------------------- answering

void CPmView::RememberAnswerL(const TDesC& aAnswer)
	{
	TBuf<150> path;
	MsgPath(iMsgUid, _L("inr"), path);
	RFile f;
	if (f.Replace(iCoeEnv->FsSession(), path, EFileWrite) == KErrNone)
		{
		TBuf8<32> a;
		a.Copy(Clip(aAnswer, 30));
		a.Append('\n');
		f.Write(a);
		f.Close();
		}
	iInvAnswer.Copy(Clip(aAnswer, iInvAnswer.MaxLength()));
	}

// aChoice 0 accept, 1 tentative, 2 decline (a "publish": 0 adds it, no reply)
void CPmView::AnswerInviteL(TInt aChoice)
	{
	if (!HasInvite() || !(InviteIs(_L("request")) || InviteIs(_L("publish"))))
		{
		Toast(_L("This message has no invitation to answer"));
		return;
		}
	TBool request = InviteIs(_L("request"));
	if (!request && aChoice != 0)
		{
		Toast(_L("Nothing to answer - this event was only sent to you"));
		return;
		}
	TBuf<200> summary;
	summary.Copy(Clip(InviteField(_L("summary")), 200));
	TBuf<120> where;
	where.Copy(Clip(InviteField(_L("location")), 120));
	TBuf<16> startText;
	startText.Copy(Clip(InviteField(_L("start")), 12));
	TTime start = LocalTime(startText);
	TTime end = LocalTime(InviteField(_L("end")));
	TBool allDay = ToNum(InviteField(_L("allday"))) != 0;
	if (start == Time::NullTTime())
		{
		Toast(_L("The invitation has no date PsiMail can read"));
		return;
		}
	if (end == Time::NullTTime() || end < start)
		end = start;
	TBuf<160> note;
	// Accept / Tentative: into the Agenda, unless it is there already
	if (aChoice != 2)
		{
		TInt have = 0;
		TRAPD(err, have = AgendaMatchL(iCal->iAgendaFile, summary, start, allDay, EFalse));
		if (err != KErrNone)
			{
			AgendaTrouble(*this, err);
			return;
			}
		if (have)
			note = _L("Already in the Agenda");
		else
			{
			TBool onServer = EFalse;
			TRAP(err, onServer = ServerHasEventL(summary, startText));
			if (onServer)
				note = _L("Already in your calendar - the next sync puts it in the Agenda");
			else
				{
				TInt alarm = ToNum(InviteField(_L("alarm")));
				if (!InviteField(_L("alarm")).Length() || alarm < -1)
					alarm = -1;
				if (alarm < 0 && !allDay)
					alarm = 15;                  // (as Create new event has it)
				TRAP(err, CPmCalSync::AddToAgendaL(iCal->iAgendaFile, summary, where, start, end, allDay, alarm));
				if (err != KErrNone)
					{
					AgendaTrouble(*this, err);
					return;
					}
				note = _L("Added to the Agenda");
				}
			}
		}
	else
		{
		// Decline: out of the Agenda too, if an earlier Accept put it there
		TInt have = 0;
		TRAPD(err, have = AgendaMatchL(iCal->iAgendaFile, summary, start, allDay, EFalse));
		if (err == KErrNone && have &&
			iEikonEnv->QueryWinL(_L("This event is in the Agenda"), _L("Remove it?")))
			{
			TRAP(err, AgendaMatchL(iCal->iAgendaFile, summary, start, allDay, ETrue));
			if (err == KErrNone)
				note = _L("Removed from the Agenda");
			}
		}
	// the reply to the organiser
	static const TText* const KStatus[] = { _S("ACCEPTED"), _S("TENTATIVE"), _S("DECLINED") };
	TPtrC status(KStatus[aChoice]);
	TBuf<120> me;
	const PmAccount& a = iSettings->iAccounts[iSettings->iAcct];
	TPtrC8 email((const TUint8*)a.email);
	me.Copy(Clip(InviteField(_L("me")), 120));
	if (!me.Length())
		me.Copy(email.Left(email.Length() < 120 ? email.Length() : 120));
	TPtrC orgAddr = InviteField(_L("orgaddr"));
	TBool reply = request && orgAddr.Length() && orgAddr.CompareF(me) != 0;
	if (reply)
		{
		TRAPD(err, SendInviteReplyL(status, aChoice, me));
		if (err != KErrNone)
			{
			TBuf<80> t;
			t.Format(_L("The reply could not be written (%d)"), err);
			Toast(t);
			reply = EFalse;
			}
		}
	TRAPD(rem, RememberAnswerL(request ? status : TPtrC(_L("added"))));
	(void)rem;
	// what happened, in one infoprint
	TBuf<200> msg;
	if (reply)
		{
		msg = aChoice == 0 ? _L("Accepted") : aChoice == 1 ? _L("Tentative") : _L("Declined");
		msg.Append(iSettings->iOffline ? _L(" - the reply goes at the next check for mail") : _L(" - sending the reply"));
		}
	if (note.Length())
		{
		if (msg.Length()) msg.Append(_L(" - "));
		msg.Append(note);
		}
	if (!msg.Length())
		msg = _L("Done");
	iReaderUid = 0;                              // the box says what was answered
	Render();
	Toast(Clip(msg, 120));
	// to the calendar server through the Agenda (as Create new event)
	if (aChoice != 2 && iCal->iCal.enabled && note.Compare(_L("Added to the Agenda")) == 0 && !iSettings->iOffline)
		CalendarSyncL();
	}

// an outbox message to the organiser with the Itip headers that
// ../engine/compose.c turns into the iTIP REPLY
void CPmView::SendInviteReplyL(const TDesC& aStatus, TInt aChoice, const TDesC& aMe)
	{
	RFs& fs = iCoeEnv->FsSession();
	TBuf<120> dir;
	OutboxDir(dir);
	fs.MkDirAll(dir);
	TInt no = 1;
	{
	TBuf<130> spec(dir);
	spec.Append(_L("*.*"));
	CDir* list = NULL;
	if (fs.GetDir(spec, KEntryAttNormal, ESortByName, list) == KErrNone && list)
		{
		for (TInt i = 0; i < list->Count(); i++)
			{
			TInt n = ToNum((*list)[i].iName);
			if (n >= no) no = n + 1;
			}
		delete list;
		}
	}
	TFileName tmp(dir), path(dir);
	tmp.AppendFormat(_L("%04d.tmp"), no);
	path.AppendFormat(_L("%04d.txt"), no);
	const PmAccount& a = iSettings->iAccounts[iSettings->iAcct];
	TBuf<64> myName;
	TPtrC8 fn((const TUint8*)a.fullname);
	myName.Copy(fn.Left(fn.Length() < 64 ? fn.Length() : 64));
	TPtrC summary = InviteField(_L("summary"));

	HBufC* buf = HBufC::NewLC(2400);
	TPtr h = buf->Des();
	h.Append(_L("#PSIMAIL1\nTo: "));
	TPtrC orgName = InviteField(_L("orgname"));
	if (orgName.Length())
		{
		CPmContacts::AppendAddress(h, Clip(orgName, 60), Clip(InviteField(_L("orgaddr")), 120));
		}
	else
		h.Append(Clip(InviteField(_L("orgaddr")), 120));
	h.Append(_L("\nSubject: "));
	h.Append(aChoice == 0 ? _L("Accepted: ") : aChoice == 1 ? _L("Tentative: ") : _L("Declined: "));
	h.Append(Clip(summary, 150));
	h.Append(_L("\nItip: "));
	h.Append(aStatus);
	h.Append(_L("\nItip-Uid: "));
	h.Append(Clip(InviteField(_L("uid")), 255));
	h.Append(_L("\nItip-Seq: "));
	h.Append(Clip(InviteField(_L("seq")), 10));
	if (InviteField(_L("recur")).Length())
		{
		h.Append(_L("\nItip-Recur: "));
		h.Append(Clip(InviteField(_L("recur")), 199));
		}
	h.Append(_L("\nItip-Organizer: "));
	h.Append(Clip(InviteField(_L("orgline")), 299));
	h.Append(_L("\nItip-Attendee: "));
	h.Append(Clip(aMe, 119));
	if (myName.Length())
		{
		h.Append(_L("\nItip-Name: "));
		h.Append(myName);
		}
	h.Append(_L("\nItip-Summary: "));
	h.Append(Clip(summary, 199));
	if (InviteField(_L("utcstart")).Length())
		{
		h.Append(_L("\nItip-Start: "));
		h.Append(Clip(InviteField(_L("utcstart")), 20));
		h.Append(_L("\nItip-End: "));
		h.Append(Clip(InviteField(_L("utcend")), 20));
		}
	h.Append(_L("\n\n"));
	h.Append(myName.Length() ? TPtrC(myName) : TPtrC(aMe));
	h.Append(aChoice == 0 ? _L(" has accepted this invitation.\n") :
		aChoice == 1 ? _L(" has tentatively accepted this invitation.\n") : _L(" has declined this invitation.\n"));
	TTime start = LocalTime(InviteField(_L("start")));
	if (start != Time::NullTTime())
		{
		TBuf<100> w;
		TTime end = LocalTime(InviteField(_L("end")));
		PmWhenText(w, start, end == Time::NullTTime() ? start : end, ToNum(InviteField(_L("allday"))) != 0);
		h.Append('\n');
		h.Append(Clip(summary, 150));
		h.Append(_L("\n"));
		h.Append(w);
		h.Append('\n');
		}
	RFile f;
	User::LeaveIfError(f.Replace(fs, tmp, EFileWrite));
	TInt r = f.Write(h);
	TInt r2 = f.Flush();
	f.Close();
	if (r == KErrNone) r = r2;
	if (r == KErrNone) r = fs.Replace(tmp, path);
	if (r != KErrNone)
		{
		fs.Delete(tmp);
		User::Leave(r);
		}
	CleanupStack::PopAndDestroy();           // buf
	if (!iSettings->iOffline)
		Cmd(PM_CMD_SEND, KNullDesC8, 0, KNullDesC8);
	}

// a cancellation: the event out of the Agenda
void CPmView::RemoveInvitedL()
	{
	if (!HasInvite())
		{
		Toast(_L("This message has no invitation"));
		return;
		}
	TBuf<200> summary;
	summary.Copy(Clip(InviteField(_L("summary")), 200));
	TTime start = LocalTime(InviteField(_L("start")));
	TBool allDay = ToNum(InviteField(_L("allday"))) != 0;
	if (start == Time::NullTTime())
		{
		Toast(_L("The invitation has no date PsiMail can read"));
		return;
		}
	TInt have = 0;
	TRAPD(err, have = AgendaMatchL(iCal->iAgendaFile, summary, start, allDay, EFalse));
	if (err != KErrNone)
		{
		AgendaTrouble(*this, err);
		return;
		}
	if (!have)
		{
		Toast(_L("Not in the Agenda"));
		return;
		}
	TBuf<120> what;
	what.Append('"');
	what.Append(Clip(summary, 60));
	what.Append(_L("\" is in the Agenda"));
	if (!iEikonEnv->QueryWinL(what, _L("Remove it?")))
		return;
	TRAP(err, AgendaMatchL(iCal->iAgendaFile, summary, start, allDay, ETrue));
	if (err != KErrNone)
		{
		AgendaTrouble(*this, err);
		return;
		}
	TRAP(err, RememberAnswerL(_L("removed")));
	iReaderUid = 0;
	Render();
	Toast(_L("Removed from the Agenda"));
	// (calendar sync sends the deletion if the entry came from the server)
	if (iCal->iCal.enabled && !iSettings->iOffline)
		CalendarSyncL();
	}

// ---------------------------------------------------------------- menus

void CPmAppUi::DynInitMail2L(TInt aMenuId, CEikMenuPane* aMenuPane)
	{
	CPmView::TMode m = iView->Mode();
	TBool msg = (m == CPmView::EList || m == CPmView::EMessage) && iView->CurrentRow() != NULL;
	if (aMenuId == R_PM_FILE_MENU)
		aMenuPane->SetItemDimmed(EPmCmdSaveWord, m != CPmView::EMessage);
	else if (aMenuId == R_PM_CONTACTS_MENU)
		{
		aMenuPane->SetItemDimmed(EPmCmdAddSender, !msg);
		aMenuPane->SetItemDimmed(EPmCmdAddCard, !iView->HasCard());
		}
	else if (aMenuId == R_PM_INVITE_MENU)
		{
		TBool answer = iView->InviteIs(_L("request"));
		TBool publish = iView->InviteIs(_L("publish"));
		aMenuPane->SetItemDimmed(EPmCmdAccept, !answer && !publish);
		aMenuPane->SetItemDimmed(EPmCmdTentative, !answer);
		aMenuPane->SetItemDimmed(EPmCmdDecline, !answer);
		aMenuPane->SetItemDimmed(EPmCmdRemoveEvent, !iView->HasInvite() || iView->InviteIs(_L("reply")));
		}
	}

// the commands of this round (invitations, cards, Word): ETrue if handled
TBool CPmAppUi::HandleMail2CommandL(TInt aCommand)
	{
	switch (aCommand)
		{
	case EPmCmdAccept:
	case EPmCmdTentative:
	case EPmCmdDecline:
		iView->AnswerInviteL(aCommand == EPmCmdAccept ? 0 : aCommand == EPmCmdTentative ? 1 : 2);
		return ETrue;
	case EPmCmdRemoveEvent:
		iView->RemoveInvitedL();
		return ETrue;
	case EPmCmdAddCard:
		iView->AddCardsL();
		return ETrue;
	case EPmCmdSaveWord:
		iView->SaveAsWordL();
		return ETrue;
	case EPmCmdContactsMenu:
	case EPmCmdInviteMenu:
		return ETrue;                            // (cascades)
	default:
		return EFalse;
		}
	}
