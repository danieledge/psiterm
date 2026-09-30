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

struct TPmHelpTopic { const char* iTitle; const char* iText; };

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
	  "Then press Shift+Ctrl+C (Check mail) to fetch your folders and the Inbox. The first time PsiMail "
	  "talks to a server it shows the server's key and asks whether to trust it." },

	{ "Reading messages",
	  "The folders are on the left and the messages of the open folder on the right; unread messages "
	  "are in bold. Move with the arrow keys, open with Enter, and go back with Esc. Tab moves between "
	  "the folder list and the messages.\n"
	  "\n"
	  "A message's text is downloaded when you open it (up to 64 KB - Message > Get whole message "
	  "fetches the rest). Left and Right step to the previous and next message. Tab moves between the "
	  "links and attachments in a message and Enter opens one; Ctrl+P shows the message as a web page "
	  "in PsiWeb.\n"
	  "\n"
	  "Message > Save attachment (Ctrl+S) saves a file to D:\\PsiMail\\Attachments. Delete moves a "
	  "message to the Trash; Edit > Archive puts it in the Archive folder; Message > Unread and Flagged "
	  "mark it.\n"
	  "\n"
	  "Edit > Find (Ctrl+F) searches the open folder on the server. Tap a column heading, or use View > "
	  "Sort, to change the order." },

	{ "Writing messages",
	  "Message > Create new email (Ctrl+N) opens a new message; Ctrl+R replies, Shift+Ctrl+R replies to "
	  "everyone and Ctrl+W forwards. Type addresses separated by commas.\n"
	  "\n"
	  "In the text, Ctrl+B, Ctrl+I and Ctrl+U make bold, italic and underlined text, which is sent as "
	  "HTML with a plain copy for older programs. The Attachments button adds files from the Psion (up "
	  "to 8).\n"
	  "\n"
	  "Send puts the message in the Outbox and sends it straight away when you are online. Save as "
	  "draft keeps it in the Outbox to finish later: open the Outbox (Ctrl+B) and press Enter on it. A "
	  "copy of each message you send is saved in the Sent folder unless the account settings say "
	  "otherwise.\n"
	  "\n"
	  "A signature, if you set one in the account settings, goes at the end of every new message." },

	{ "Folders",
	  "Every folder on the server is in the list on the left, with the number of unread messages after "
	  "its name. Standard folders - Inbox, Sent, Drafts, Trash, Junk and Archive - have their own "
	  "pictures.\n"
	  "\n"
	  "File > Folder > New folder makes a folder, at the top level or inside another one. Rename folder "
	  "and Delete folder act on the highlighted folder (or the open one). The Inbox and the standard "
	  "folders can't be renamed or deleted, and a folder that holds other folders must be emptied of "
	  "them first. Deleting a folder deletes the messages in it too.\n"
	  "\n"
	  "Edit > Move to folder (Ctrl+X) moves the selected message. View > Go to (Ctrl+G) opens any "
	  "folder, and Ctrl+I goes straight to the Inbox.\n"
	  "\n"
	  "File > Folder > Check this folder (Ctrl+Y) fetches new messages for the open folder alone; Get "
	  "older messages (Shift+Ctrl+G) brings in the next 50 older ones. Check mail fetches every folder "
	  "and sends what is waiting in the Outbox." },

	{ "Download ahead and offline",
	  "PsiMail keeps the newest 50 messages of each folder on the CF card (or the internal disk), so "
	  "they can be read without a connection. After Check mail it also downloads the text of the newest "
	  "messages ahead, so they open at once; the number is set in the account settings (Download "
	  "ahead), and Off turns it off.\n"
	  "\n"
	  "File > Work offline (Shift+Ctrl+W) stops PsiMail connecting. Anything you do offline - deleting, "
	  "moving, flagging, sending - is kept and done the next time you check mail. Making, renaming and "
	  "deleting folders needs the server, so PsiMail asks to go online for those.\n"
	  "\n"
	  "Esc stops a download that is running; File > Disconnect (Ctrl+U) hangs up." },

	{ "The calendar",
	  "PsiMail can keep the Psion's Agenda in step with a CalDAV calendar such as Fastmail's. Turn it on "
	  "in Tools > Calendar settings: the server (caldav.fastmail.com, or another server's address), the "
	  "password if it differs from the mail one, your time zone and the Agenda file.\n"
	  "\n"
	  "File > Sync calendar (Shift+Ctrl+Y) fetches changes and sends new Agenda entries; with Sync with "
	  "the Agenda on, Check mail does it too. View > Go to > Calendar (Shift+Ctrl+N) shows the week, or "
	  "the month with Ctrl+Q; Event > Create new event (Ctrl+N) adds an entry to the Agenda, which is "
	  "then sent to the server.\n"
	  "\n"
	  "Repeating entries made on the Psion stay on the Psion; to-do lists and anniversaries are not "
	  "synced." },

	{ "Connections",
	  "Tools > Connection settings chooses how PsiMail reaches the Internet, the same way as PsiTerm.\n"
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
	  "checked against the key you trusted the first time." },

	{ "Updating PsiMail",
	  "Tools > Update PsiMail looks for a newer PsiMail on GitHub (the published releases, or the test "
	  "builds) or on a local server on your own network, downloads it in pieces to the CF card, checks "
	  "its signature and offers to install it. PsiMail closes while the installer runs; start it again "
	  "from the Extras bar afterwards.\n"
	  "\n"
	  "Tools > About PsiMail shows the version you have." },

	{ "Keyboard shortcuts",
	  "Up/Down, PgUp/PgDn, Home/End: move and scroll\n"
	  "Enter or Right: open; Esc or Left: back\n"
	  "Left/Right in a message: previous / next message\n"
	  "Tab: between the folders and the messages; in a message, the next link\n"
	  "Del: delete (to the Trash)\n"
	  "\n"
	  "Shift+Ctrl+C: check mail (send & receive)\n"
	  "Ctrl+Y / Shift+Ctrl+G: check this folder / get older messages\n"
	  "Ctrl+U / Ctrl+Z / Shift+Ctrl+W: disconnect / stop / work offline\n"
	  "Ctrl+N / Ctrl+R / Shift+Ctrl+R / Ctrl+W: new / reply / reply to all / forward\n"
	  "Ctrl+D / Ctrl+X / Shift+Ctrl+E: delete / move to folder / archive\n"
	  "Shift+Ctrl+U / Shift+Ctrl+F: unread / flagged\n"
	  "Ctrl+S: save an attachment; Ctrl+P: view as a web page\n"
	  "Ctrl+I / Ctrl+G / Ctrl+B / Ctrl+F: Inbox / go to folder / Outbox / find\n"
	  "Ctrl+M / Shift+Ctrl+M: zoom in / out\n"
	  "Ctrl+T / Shift+Ctrl+T / Shift+Ctrl+L: toolbar / title bar / folder list\n"
	  "Shift+Ctrl+Q / Shift+Ctrl+B: status information / sort\n"
	  "Shift+Ctrl+N / Ctrl+Q: the calendar / week or month\n"
	  "Shift+Ctrl+Y / Shift+Ctrl+D: sync the calendar / go to today\n"
	  "Ctrl+K / Shift+Ctrl+H / Shift+Ctrl+A / Ctrl+E: preferences / help / about / close" }
	};

static const TInt KHelpCount = sizeof(KHelpTopics) / sizeof(KHelpTopics[0]);

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
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(EPmDlgHelpText);
	ed->SetTextL(b);
	TCharFormat cf;
	TCharFormatMask cm;
	cf.iFontSpec.iFontStyle.SetStrokeWeight(EStrokeWeightBold);
	cm.SetAttrib(EAttFontStrokeWeight);
	ed->RichText()->ApplyCharFormatL(cf, cm, 0, title.Length());
	ed->HandleTextChangedL();
	ed->SetCursorPosL(0, EFalse);
	CleanupStack::PopAndDestroy();          // b
	}

// the keys start on the topic list (Left and Right change the topic; Down
// moves into the text, where Up and Down scroll it)
void CPmHelpDialog::PostLayoutDynInitL()
	{
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
