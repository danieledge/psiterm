// pmhelp.cpp - Tools > Help on PsiMail (Shift+Ctrl+H): the help topics, in
// an EIKON dialog with a topic list and a read-only text viewer.
//
// The built-in programs' help lives in .hlp files made by the SDK's aleppo
// tool and shown by the Help program. Aleppo's converter (almain.exe) is an
// EPOC WINS executable that doesn't run under wine, and a .hlp is a Data
// database with the Help program's own streams in it, so PsiMail keeps its
// help here, in the program, where it works on every machine. Each topic is
// a title and paragraphs; a line beginning "*" is a bullet.
#include "pmapp.h"
#include <eikchlst.h>
#include <eikrted.h>
#include <txtrich.h>
#include <frmtview.h>
#include <frmtlay.h>
#include <barsread.h>

struct TPmHelpTopic { const char* iTitle; const char* iText; };

// The wording follows the EIKON style guide: the user's words, key names as
// the keyboard prints them (Ctrl, Shift, Fn, Tab, Enter, Esc), menu commands
// named as they appear (without their "..."), and no jargon that a glossary
// would not allow ("program", "folder", "memory disk").
static const TPmHelpTopic KHelpTopics[] =
	{
	{ "Getting started",
	  "PsiMail reads and sends email through your provider's IMAP and SMTP servers, over a WiFi modem "
	  "or the Psion's own Internet connection.\n"
	  "\n"
	  "To add an account, use Tools > Accounts > Add. Fastmail's servers are filled in to begin with "
	  "(imap.fastmail.com 993 and smtp.fastmail.com 465, both TLS); for another provider, type its "
	  "servers and ports.\n"
	  "\n"
	  "Most providers want an app password rather than your normal one. On fastmail.com choose "
	  "Settings > Privacy & Security > Manage app passwords > New app password, with access to Mail "
	  "(and Calendars, for the calendar). Gmail and Outlook have the same under their security settings, "
	  "once two-step verification is on.\n"
	  "\n"
	  "Then press Shift+Ctrl+C (File > Check mail) to fetch your folders and the Inbox. The first time "
	  "PsiMail talks to a server it shows the server's key and asks whether to trust it.\n"
	  "\n"
	  "PsiMail opens where you closed it - the folder, the Outbox or the calendar - with your accounts, "
	  "folders and preferences as you left them." },

	{ "Reading messages",
	  "The folders are on the left and the messages of the open folder on the right; unread messages "
	  "are in bold. Move with the arrow keys, open with Enter, and go back with Esc. Tab moves between "
	  "the folder list and the messages. A tap selects a message and a second tap opens it.\n"
	  "\n"
	  "A message's text is downloaded when you open it (up to 64 KB - Message > Get whole message "
	  "fetches the rest). Left and Right step to the previous and next message. Tab moves between the "
	  "links and attachments in a message and Enter opens one; Message > View as web page "
	  "shows the message in PsiWeb.\n"
	  "\n"
	  "Message > Attachments > Open opens an attached file in its own program (Word, Sketch and so on); "
	  "Message > Attachments > Save (Ctrl+S) saves it in the Attachments folder in Documents, on the "
	  "disk the mail is on. Delete moves a "
	  "message to the Trash; Edit > Archive puts it in the Archive folder; Message > Unread and Flagged "
	  "mark it.\n"
	  "\n"
	  "Edit > Find (Ctrl+F) looks for words in the open folder on the server. Tap a column heading, or "
	  "use View > Sort, to change the order. View > Zoom in and Zoom out (Ctrl+M, Shift+Ctrl+M) go "
	  "round the three sizes." },

	{ "Pictures in messages",
	  "Pictures that come with a message - in the message's own HTML, or attached to it - are shown in "
	  "the message, shrunk to fit the screen. Pictures up to 300 KB are fetched with the message; a "
	  "bigger one is shown as its name and size, and a tap on it fetches it. Pictures on the web are "
	  "never fetched: they appear as their words.\n"
	  "\n"
	  "Tools > Preferences > Show pictures chooses what is shown: Yes, Only attached files (nothing "
	  "from inside the HTML), or No. Whatever the setting, an attached picture can still be saved or "
	  "opened with Message > Attachments.\n"
	  "\n"
	  "While a picture is being fetched or decoded its frame says so; if it can't be shown, the frame "
	  "says why. Working offline, pictures not yet fetched wait until you go online." },

	{ "Writing messages",
	  "Message > Create new email (Ctrl+N) opens a new message; Ctrl+R replies, Shift+Ctrl+R replies to "
	  "everyone and Ctrl+W forwards. Type addresses separated by commas.\n"
	  "\n"
	  "In the text, Ctrl+B, Ctrl+I and Ctrl+U make bold, italic and underlined text, which is sent as "
	  "HTML with a plain copy for older programs. The Attachments button adds files from the Psion (up "
	  "to 8). When you forward a message that has attachments, PsiMail asks whether to forward them too.\n"
	  "\n"
	  "Send puts the message in the Outbox and sends it straight away when you are online. Save as "
	  "draft keeps it in the Outbox to finish later: open the Outbox (Ctrl+B) and press Enter on it. A "
	  "copy of each message you send is saved in the Sent folder unless the account settings say "
	  "otherwise.\n"
	  "\n"
	  "A signature, if you set one in the account settings, goes at the end of every new message." },

	{ "Addresses from Contacts",
	  "PsiMail uses the addresses in the Psion's Contacts program.\n"
	  "\n"
	  "In an address line (To, CC or BCC), type the start of a name or an address and press Tab: one "
	  "match is filled in, and with several PsiMail shows them to choose from. The Contacts button "
	  "(Ctrl+L) opens the same list: type to narrow it, press Space or tap to mark several people, and "
	  "press Enter to add them.\n"
	  "\n"
	  "Edit > Add to Contacts > Sender adds the sender of the selected message to Contacts - as a new "
	  "entry, or as another address for the entry with the same name." },

	{ "Invitations and contact cards",
	  "An invitation to a meeting (a calendar file sent by Google Calendar, Outlook, Fastmail and the "
	  "like) is shown at the top of the message: what, when, where and who sent it, with Accept, "
	  "Tentative and Decline under it. Tab to one and press Enter, or tap it; Edit > Invitation has the "
	  "same commands.\n"
	  "\n"
	  "Accept and Tentative put the event in the Agenda (the file in Tools > Calendar settings), with "
	  "an alarm; with calendar sync on it then goes to your calendar on the server too. Each answer is "
	  "sent to the organiser, through the Outbox like any message, so their calendar shows it. Decline "
	  "takes an accepted event out of the Agenda again, if you say so. An event that repeats is put in "
	  "the Agenda for its first time only.\n"
	  "\n"
	  "When an event is cancelled, Remove from Agenda (in the message, or Edit > Invitation) takes it "
	  "out. An event sent only for your information has Add to Agenda instead.\n"
	  "\n"
	  "A contact card (a .vcf file) in a message is shown the same way, with Add to Contacts (also "
	  "Edit > Add to Contacts > Contact card): the name, company, job title, phone numbers, email "
	  "addresses, address and web page go into the Contacts program's own fields. A card whose email "
	  "address is in Contacts already is left as it is.\n"
	  "\n"
	  "To send your own card, press the Attachments button when writing a message and choose Add my "
	  "contact card: your name and address go with the message as a .vcf file." },

	{ "Saving a message as a Word file",
	  "File > Save as Word file (Shift+Ctrl+S) saves the open message as a file the Psion's Word "
	  "program opens: the subject, From, To, Cc and Date, then the text with its bold, italic and "
	  "underlining, headings, lists and quotes as PsiMail shows them. It goes in the Documents folder "
	  "to begin with, named after the subject. Pictures are shown in the file as [Picture]." },

	{ "Folders",
	  "Every folder on the server is in the list on the left, with the number of unread messages after "
	  "its name. Standard folders - Inbox, Sent, Drafts, Trash, Junk and Archive - have their own "
	  "pictures.\n"
	  "\n"
	  "File > Folder > Create new makes a folder, at the top level or inside another one. File > Folder "
	  "> Rename and Delete act on the highlighted folder (or the open one). The Inbox and the standard "
	  "folders can't be renamed or deleted, and a folder that holds other folders must be emptied of "
	  "them first. Deleting a folder deletes the messages in it too, so PsiMail asks first.\n"
	  "\n"
	  "Edit > Move to folder (Ctrl+X) moves the selected message. View > Go to (Ctrl+G) opens any "
	  "folder, and Ctrl+I goes straight to the Inbox.\n"
	  "\n"
	  "File > Folder > Check this folder (Ctrl+Y) fetches new messages for the open folder alone; Get "
	  "older messages (Shift+Ctrl+G) brings in the next 50 older ones. Check mail fetches every folder "
	  "and sends what is waiting in the Outbox." },

	{ "Download ahead and offline",
	  "PsiMail keeps the newest messages of each folder (50, unless the account's Limits page says "
	  "otherwise) on the memory disk (the CF card) or the internal disk, so they can be read without a "
	  "connection. After Check mail it also downloads the text of the newest messages ahead, so they "
	  "open at once; the number is set in Tools > Preferences (Download ahead), and 0 turns it off.\n"
	  "\n"
	  "File > Work offline (Shift+Ctrl+W) stops PsiMail connecting. Anything you do offline - deleting, "
	  "moving, flagging, sending - is kept and done the next time you check mail. Making, renaming and "
	  "deleting folders needs the server, so PsiMail asks to go online for those.\n"
	  "\n"
	  "What PsiMail is doing is shown at the bottom left of the screen. Esc, or File > Stop, "
	  "stops a download that is running; File > Disconnect (Ctrl+U) hangs up." },

	{ "Where the mail is kept",
	  "PsiMail keeps your mail in the System folder, out of the way of your own files: "
	  "\\System\\Data\\PsiMail on the memory disk (the CF card), or on the internal disk if there is no "
	  "card or Tools > Preferences > Keep mail on says so. View > Status information shows which.\n"
	  "\n"
	  "Before version 0.74 the mail was in a PsiMail folder at the top of the disk. The first time a "
	  "newer PsiMail opens, it moves that folder into the System folder - one quick step - and the "
	  "attachments you had saved go to the Attachments folder in Documents. If the move can't be done "
	  "(the disk is full or a file is in use, say) PsiMail says so and goes on using the old folder, "
	  "and tries again next time.\n"
	  "\n"
	  "Removing PsiMail removes the program and its settings, but not your mail: that stays in "
	  "\\System\\Data\\PsiMail until you delete that folder yourself." },

	{ "The calendar",
	  "PsiMail can keep the Psion's Agenda in step with a CalDAV calendar such as Fastmail's. Turn it on "
	  "in Tools > Calendar settings: the server (caldav.fastmail.com, or another server's address), the "
	  "password if it differs from the mail one, your time zone and the Agenda file.\n"
	  "\n"
	  "Event > Sync calendar (Shift+Ctrl+Y, from anywhere) fetches changes and sends new Agenda entries; with Sync with "
	  "the Agenda on, Check mail does it too. View > Go to > Calendar (Shift+Ctrl+N) shows the week; "
	  "View > Switch view (Ctrl+Q) changes between the week and the month. Event > Create new event "
	  "(Ctrl+N) adds an entry to the Agenda, which is then sent to the server; Event > Details shows "
	  "the selected event.\n"
	  "\n"
	  "Repeating entries made on the Psion stay on the Psion; to-do lists and anniversaries are not "
	  "synced." },

	{ "Connections",
	  "Tools > Connection settings chooses how PsiMail reaches the Internet, the same way as PsiTerm.\n"
	  "\n"
	  "Test (Ctrl+T) tries the settings shown, before OK: whether the modem answers and at what "
	  "speed, and whether CTS allows RTS/CTS flow control; for Psion Internet, whether the "
	  "connection is up and names can be looked up (it asks before dialling).\n"
	  "\n"
	  "Modem: a serial WiFi modem (such as a WiRSa or a WiFi232) on the Psion's serial port. PsiMail "
	  "dials the server with ATDT host:port. Set the baud rate to the modem's; 115200 with RTS/CTS "
	  "flow control is fastest if the cable carries those lines.\n"
	  "\n"
	  "Psion Internet: the Psion's own dial-up (PPP) connection, set up in the Control panel's Internet "
	  "and Modems settings. PsiMail starts it when it needs to and the Psion's connection dialogs "
	  "appear; File > Disconnect hangs it up.\n"
	  "\n"
	  "Everything goes over TLS 1.3 (or STARTTLS, if the account says so); the server's certificate is "
	  "checked against the key you trusted the first time.\n"
	  "\n"
	  "While PsiMail works it says so at the bottom left - Checking mail, Sending, Getting message - "
	  "and what came of it at the top right. To see every step (dialling, logging in, each message "
	  "fetched), which helps when a connection won't come up, set Tools > Preferences > New mail > "
	  "Show detailed progress to Yes." },

	{ "The Email icon",
	  "The Email icon below the screen can open PsiMail instead of the built-in Email program. PsiMail "
	  "asks once, the first time it opens after it is installed; after that, Tools > Preferences > "
	  "Email icon opens chooses PsiMail or Built-in Email.\n"
	  "\n"
	  "With PsiMail chosen, a tap on the Email icon brings PsiMail to the front, or opens it if it is "
	  "closed. A small helper program, pmbutton, waits in the background for the tap, taking very "
	  "little memory and doing nothing until then; the other icons work as they always do. After the "
	  "Psion has been reset, open PsiMail once and the Email icon is PsiMail's again.\n"
	  "\n"
	  "Choose Built-in Email, or remove PsiMail, and the Email icon opens the built-in Email program "
	  "again. PsiMail's connections and the built-in Email program are not changed either way." },

	{ "Updating PsiMail",
	  "Tools > Update PsiMail looks for a newer PsiMail on GitHub (the published releases, or the test "
	  "builds) or on a local server on your own network, downloads it in pieces to the memory disk, "
	  "checks its signature and offers to install it. PsiMail closes while the installer runs; start it "
	  "again from the Extras bar afterwards.\n"
	  "\n"
	  "Tools > About PsiMail (Shift+Ctrl+A) shows the version you have." },

	{ "Keyboard shortcuts",
	  "Up/Down, Fn+Up/Fn+Down (Pg Up/Pg Dn), Fn+Left/Fn+Right (Home/End): move and scroll\n"
	  "Enter or Right: open; Esc or Left: back\n"
	  "Left/Right in a message: previous / next message\n"
	  "Tab: between the folders and the messages; in a message, the next link; in an address, Contacts\n"
	  "Del: delete (to the Trash)\n"
	  "\n"
	  "Shift+Ctrl+C: check mail (send & receive)\n"
	  "Ctrl+Y / Shift+Ctrl+G: check this folder / get older messages\n"
	  "Ctrl+U / Esc / Shift+Ctrl+W: disconnect / stop / work offline\n"
	  "Ctrl+N / Ctrl+R / Shift+Ctrl+R / Ctrl+W: create new / reply / reply to all / forward\n"
	  "Ctrl+D / Ctrl+X / Shift+Ctrl+E: delete / move to folder / archive\n"
	  "Shift+Ctrl+U / Shift+Ctrl+F: unread / flagged\n"
	  "Ctrl+S: save an attachment\n"
	  "Ctrl+I / Ctrl+G / Ctrl+B / Ctrl+F: Inbox / go to folder / Outbox / find\n"
	  "Ctrl+M / Shift+Ctrl+M: zoom in / out\n"
	  "Ctrl+T / Shift+Ctrl+T / Shift+Ctrl+L: toolbar / title bar / folder list\n"
	  "Shift+Ctrl+Q / Shift+Ctrl+B: status information / sort\n"
	  "Shift+Ctrl+N / Ctrl+Q: the calendar / switch view (week or month)\n"
	  "Shift+Ctrl+Y / Shift+Ctrl+D: sync the calendar / go to today\n"
	  "Ctrl+K / Shift+Ctrl+H / Shift+Ctrl+A / Ctrl+E: preferences / help / about / close" }
	};

static const TInt KHelpCount = sizeof(KHelpTopics) / sizeof(KHelpTopics[0]);

// ----- the text, with its scroll bar (0.75) ------------------------------------
//
// The help's text is a read-only rich text editor with a scroll bar beside
// it, as EIKON's edwins in dialogs have. The edwin's own scroll bar frame
// didn't keep its thumb up to date for a read-only text (and sat outside the
// line unless told otherwise), so, as in the reader, the bar is drawn here
// (PmDrawScrollBar) and the text scrolled with its view: Up and Down a line,
// Pg Up and Pg Dn a page, Home and End, and the pen on the bar. Up at the
// top goes back to the Topic line. The line's resource is the editor's
// (RTXTED): its width is the text's, the bar comes on top.

const TInt KHelpBarW = 23;                 // EIKON's scroll bar width

CPmHelpText::~CPmHelpText()
	{
	delete iEditor;
	}

void CPmHelpText::ConstructFromResourceL(TResourceReader& aReader)
	{
	iEditor = new(ELeave) CEikRichTextEditor(TEikBorder(TEikBorder::ENone));
	if (DrawableWindow())
		iEditor->SetContainerWindowL(*this);
	iEditor->ConstructFromResourceL(aReader);
	}

void CPmHelpText::SetContainerWindowL(const CCoeControl& aContainer)
	{
	CCoeControl::SetContainerWindowL(aContainer);
	if (iEditor)
		iEditor->SetContainerWindowL(*this);
	}

TSize CPmHelpText::MinimumSize()
	{
	TSize s = iEditor->MinimumSize();
	return TSize(s.iWidth + KHelpBarW + 2, s.iHeight + 2);
	}

void CPmHelpText::SizeChangedL()
	{
	Layout();
	}

void CPmHelpText::PositionChanged()
	{
	Layout();
	}

void CPmHelpText::Layout()
	{
	if (!iEditor)
		return;
	TRect r = Rect();
	r.Shrink(1, 1);
	iBar = TRect(r.iBr.iX - KHelpBarW, r.iTl.iY, r.iBr.iX, r.iBr.iY);
	TRAPD(err, iEditor->SetRectL(TRect(r.iTl, TPoint(iBar.iTl.iX, r.iBr.iY))));
	(void)err;
	}

TInt CPmHelpText::CountComponentControls() const
	{
	return iEditor ? 1 : 0;
	}

CCoeControl* CPmHelpText::ComponentControl(TInt /*aIndex*/) const
	{
	return iEditor;
	}

void CPmHelpText::Model(TInt& aTotal, TInt& aShown, TInt& aAbove) const
	{
	CTextLayout* lay = iEditor->TextLayout();
	aShown = iEditor->Rect().Height();
	aTotal = lay ? lay->FormattedHeightInPixels() : aShown;
	if (aTotal < aShown) aTotal = aShown;
	aAbove = iAbove;
	if (aAbove > aTotal - aShown) aAbove = aTotal - aShown;
	if (aAbove < 0) aAbove = 0;
	}

void CPmHelpText::Draw(const TRect& /*aRect*/) const
	{
	CWindowGc& gc = SystemGc();
	// the edit line's edge, as the dialog's other lines have it
	gc.SetPenStyle(CGraphicsContext::ESolidPen);
	gc.SetPenColor(KRgbBlack);
	gc.SetBrushStyle(CGraphicsContext::ENullBrush);
	gc.DrawRect(Rect());
	DrawBar(gc);
	}

void CPmHelpText::DrawBar(CWindowGc& aGc) const
	{
	TInt total, shown, above;
	Model(total, shown, above);
	PmDrawScrollBar(aGc, iBar, total, shown, above, iPress);
	}

void CPmHelpText::UpdateBar()
	{
	if (!IsReadyToDraw())
		return;
	ActivateGc();
	DrawBar(SystemGc());
	DeactivateGc();
	}

// a new topic: at its top, without a cursor (it is for reading)
void CPmHelpText::TextChangedL()
	{
	iAbove = 0;
	iEditor->TextView()->SetCursorVisibilityL(TCursor::EFCursorInvisible, TCursor::EFCursorInvisible);
	UpdateBar();
	}

// the end of the text is in view (no scrolling on into blank lines)
TBool CPmHelpText::AtEnd() const
	{
	TInt total, shown, above;
	Model(total, shown, above);
	return iAbove >= total - shown;
	}

// scrolls the text: the pixels it moved (0 at the top or the end)
TInt CPmHelpText::ScrollL(TInt aMovement)
	{
	TCursorPosition::TMovementType m = (TCursorPosition::TMovementType)aMovement;
	TInt px = iEditor->TextView()->ScrollDisplayL(m);
	if (px < 0) px = -px;
	TBool down = m == TCursorPosition::EFLineDown || m == TCursorPosition::EFPageDown;
	iAbove += down ? px : -px;
	if (iAbove < 0) iAbove = 0;
	iEditor->TextView()->SetCursorVisibilityL(TCursor::EFCursorInvisible, TCursor::EFCursorInvisible);
	UpdateBar();
	return px;
	}

TKeyResponse CPmHelpText::OfferKeyEventL(const TKeyEvent& aKeyEvent, TEventCode aType)
	{
	if (aType != EEventKey)
		return EKeyWasNotConsumed;
	switch (aKeyEvent.iCode)
		{
	case EKeyUpArrow:
		if (iAbove <= 0 || ScrollL(TCursorPosition::EFLineUp) == 0)
			{
			iAbove = 0;
			UpdateBar();
			if (iDialog)
				iDialog->TopicLineL();       // at the top: back to the Topic line
			return EKeyWasConsumed;
			}
		return EKeyWasConsumed;
	case EKeyDownArrow:
		if (!AtEnd())
			ScrollL(TCursorPosition::EFLineDown);
		return EKeyWasConsumed;
	case EKeyPageUp:
		ScrollL(TCursorPosition::EFPageUp);
		return EKeyWasConsumed;
	case EKeyPageDown:
		if (!AtEnd())
			ScrollL(TCursorPosition::EFPageDown);
		return EKeyWasConsumed;
	case EKeyHome:
		while (iAbove > 0 && ScrollL(TCursorPosition::EFPageUp) > 0) {}
		iAbove = 0;
		UpdateBar();
		return EKeyWasConsumed;
	case EKeyEnd:
		{
		for (TInt guard = 0; guard < 100 && !AtEnd() && ScrollL(TCursorPosition::EFPageDown) > 0; guard++) {}
		return EKeyWasConsumed;
		}
	default:
		return EKeyWasNotConsumed;
		}
	}

// the pen on the bar: the arrows a line, the shaft a page, the thumb dragged
void CPmHelpText::HandlePointerEventL(const TPointerEvent& aEvent)
	{
	TPoint p = aEvent.iPosition;
	TInt total, shown, above;
	Model(total, shown, above);
	TRect shaft, thumb, up, down;
	PmScrollBarParts(iBar, total, shown, above, shaft, thumb, up, down);
	if (aEvent.iType == TPointerEvent::EButton1Down)
		{
		iPress = 0;
		if (!iBar.Contains(p))
			return;                          // (the text: nothing to select)
		if (up.Contains(p)) { iPress = 1; ScrollL(TCursorPosition::EFLineUp); }
		else if (down.Contains(p)) { iPress = 2; if (!AtEnd()) ScrollL(TCursorPosition::EFLineDown); }
		else if (thumb.Contains(p)) { iPress = 3; iGrab = p.iY - thumb.iTl.iY; }
		else if (p.iY < thumb.iTl.iY) ScrollL(TCursorPosition::EFPageUp);
		else if (!AtEnd()) ScrollL(TCursorPosition::EFPageDown);
		UpdateBar();
		return;
		}
	if (aEvent.iType == TPointerEvent::EDrag && iPress == 3)
		{
		TInt room = shaft.Height() - thumb.Height();
		if (room > 0 && total > shown)
			{
			TInt want = (p.iY - iGrab - shaft.iTl.iY) * (total - shown) / room;
			for (TInt guard = 0; guard < 200; guard++)
				{
				TInt at = iAbove;
				if (at + 12 < want && !AtEnd()) ScrollL(TCursorPosition::EFLineDown);
				else if (at - 12 > want && at > 0) ScrollL(TCursorPosition::EFLineUp);
				else break;
				if (iAbove == at) break;
				}
			}
		return;
		}
	if (aEvent.iType == TPointerEvent::EButton1Up && iPress)
		{
		iPress = 0;
		UpdateBar();
		}
	}

SEikControlInfo CPmHelpDialog::CreateCustomControlL(TInt aControlType)
	{
	SEikControlInfo info;
	info.iControl = NULL;
	info.iTrailerTextId = 0;
	info.iFlags = 0;
	if (aControlType == EPmCtHelpText)
		info.iControl = new(ELeave) CPmHelpText;
	return info;
	}

void CPmAppUi::HelpL(TInt aTopic)
	{
	CPmHelpDialog* dlg = new(ELeave) CPmHelpDialog(aTopic);
	dlg->ExecuteLD(R_PM_HELP_DIALOG);
	}

void CPmHelpDialog::PreLayoutDynInitL()
	{
	CDesCArrayFlat* titles = new(ELeave) CDesCArrayFlat(KHelpCount);
	CleanupStack::PushL(titles);
	TBuf<40> t;
	for (TInt i = 0; i < KHelpCount; i++)
		{
		t.Copy(TPtrC8((const TUint8*)KHelpTopics[i].iTitle));
		titles->AppendL(t);
		}
	CleanupStack::Pop();
	CEikChoiceList* cl = (CEikChoiceList*)Control(EPmDlgHelpTopic);
	cl->SetArrayL(titles);                  // it owns the array now
	if (iTopic < 0 || iTopic >= KHelpCount)
		iTopic = 0;
	cl->SetCurrentItem(iTopic);
	((CPmHelpText*)Control(EPmDlgHelpText))->SetDialog(this);
	ShowTopicL(iTopic);
	}

// the topic's text: its title in bold, then the paragraphs
void CPmHelpDialog::ShowTopicL(TInt aTopic)
	{
	const TPmHelpTopic& topic = KHelpTopics[aTopic];
	TPtrC8 title((const TUint8*)topic.iTitle);
	TPtrC8 text((const TUint8*)topic.iText);
	HBufC* b = HBufC::NewLC(title.Length() + text.Length() + 4);
	TPtr p = b->Des();
	p.Copy(title);
	p.Append(CEditableText::EParagraphDelimiter);
	for (TInt i = 0; i < text.Length(); i++)
		{
		TUint8 c = text[i];
		if (c == '\n')
			p.Append(CEditableText::EParagraphDelimiter);
		else
			p.Append((TText)c);
		}
	CEikRichTextEditor* ed = ((CPmHelpText*)Control(EPmDlgHelpText))->Editor();
	ed->SetTextL(b);
	TCharFormat cf;
	TCharFormatMask cm;
	cf.iFontSpec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	cm.SetAttrib(EAttFontStrokeWeight);
	ed->RichText()->ApplyCharFormatL(cf, cm, 0, title.Length());
	ed->HandleTextChangedL();
	ed->SetCursorPosL(0, EFalse);
	((CPmHelpText*)Control(EPmDlgHelpText))->TextChangedL();
	CleanupStack::PopAndDestroy();          // b
	}

// the keys start on the topic list (Left and Right change the topic; Down
// moves into the text, where Up and Down scroll it)
void CPmHelpDialog::PostLayoutDynInitL()
	{
	((CPmHelpText*)Control(EPmDlgHelpText))->TextChangedL();
	TryChangeFocusToL(EPmDlgHelpTopic);
	}

void CPmHelpDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId != EPmDlgHelpTopic)
		return;
	TInt t = ((CEikChoiceList*)Control(EPmDlgHelpTopic))->CurrentItem();
	if (t != iTopic && t >= 0 && t < KHelpCount)
		{
		iTopic = t;
		ShowTopicL(t);
		}
	}

TBool CPmHelpDialog::OkToExitL(TInt /*aButtonId*/)
	{
	return ETrue;
	}
