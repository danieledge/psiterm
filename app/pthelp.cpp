// pthelp.cpp - Tools > Help on PsiTerm (Shift+Ctrl+H): the help topics, in
// an EIKON dialog with a topic list and a read-only text viewer - the same
// dialog as PsiMail's (mail/app/pmhelp.cpp), which says why the help lives
// in the program rather than in a .hlp file.
//
// The wording follows the EIKON style guide: the user's words, key names as
// the keyboard prints them (Ctrl, Shift, Fn, Tab, Enter, Esc), menu commands
// named as they appear (without their "..."), "server" for the machine at
// the other end, "program" not "application".
#include "psiterm.h"
#include <eikchlst.h>
#include <eikrted.h>
#include <txtrich.h>

struct TPtHelpTopic { const char* iTitle; const char* iText; };

static const TPtHelpTopic KHelpTopics[] =
	{
	{ "Getting started",
	  "PsiTerm logs in to a server over SSH, through a WiFi modem on the Psion's serial port or the "
	  "Psion's own Internet connection, and shows a terminal: a shell, tmux, vim, htop, Claude Code.\n"
	  "\n"
	  "File > SSH to (Shift+Ctrl+S) lists your saved servers. The first time it asks for a new one: a "
	  "name, the address (host, or host:port for a port other than 22), the user name, and how to log "
	  "in - with an SSH key, with a password typed each time, or with a saved password.\n"
	  "\n"
	  "Connect, and the status line at the bottom shows the dialling, the handshake and the login. "
	  "The first time PsiTerm meets a server it shows the server's key and asks whether to trust it.\n"
	  "\n"
	  "File > End SSH (Shift+Ctrl+D), or the toolbar's End SSH button, ends the session. If the SSH "
	  "program has not stopped after 10 seconds PsiTerm ends it; choosing End SSH again ends it at once.\n"
	  "\n"
	  "The start screen shows the PsiTerm logo; File > SSH to lists the saved servers. Keys typed there "
	  "go to the modem, so AT commands work as in a plain terminal." },

	{ "The terminal",
	  "Everything you type goes to the server, including every Ctrl+letter (Ctrl+C, Ctrl+D, Ctrl+Z): "
	  "PsiTerm's own shortcuts are all Shift+Ctrl+letter. Fn+Up/Down are Pg Up/Pg Dn and Fn+Left/Right "
	  "are Home/End, as the server sees them.\n"
	  "\n"
	  "Shift+Fn+Up/Down (Shift+Pg Up/Pg Dn) look back through earlier text; Shift+Fn+Right (Shift+End) or "
	  "typing returns to the live screen. Drag the pen on the right-hand edge to scroll.\n"
	  "\n"
	  "Drag the pen over text to select it; Edit > Copy (Shift+Ctrl+C) copies it to the clipboard and "
	  "Edit > Paste (Shift+Ctrl+V) types the clipboard to the server. Edit > Select screen selects "
	  "everything shown.\n"
	  "\n"
	  "When a program asks for the mouse (tmux with the mouse on, vim, htop), a tap is a click and a "
	  "drag up or down scrolls; Shift with the pen still selects text.\n"
	  "\n"
	  "The zoom icons on the sidebar, or View > Zoom in and Zoom out (Shift+Ctrl+M), go round the "
	  "five sizes - Terminus 12, 14, 16 and 18 and a small Courier - and View > Font picks one. View > Bold text draws bold where the server asks; "
	  "View > Show status line keeps the connection state and the clock at the bottom. Tools > "
	  "Preferences (Shift+Ctrl+K) sets the theme, the cursor, the bell and the start screen.\n"
	  "\n"
	  "The toolbar on the right has SSH to (End SSH while connected), then Send snippet, Keys and "
	  "Files, which each pop up a list. Files has Send file, Get file and Log to file, as on the "
	  "File menu. View > Show toolbar (Shift+Ctrl+B) hides it: the terminal takes "
	  "its room - 106 columns instead of 95 in Terminus 12, 80 instead of 71 in Terminus 14 and 16, "
	  "64 instead of 57 in Terminus 18 - and programs on the server reflow, even mid-session. "
	  "PsiTerm remembers the choice." },

	{ "Keys & snippets",
	  "The Keys menu types what the Psion's keyboard lacks: Shift+Tab (Shift+Ctrl+T), Insert, Ctrl+\\ "
	  "and F1 to F12.\n"
	  "\n"
	  "A snippet is text you send often - a command, a prompt. Snippets > Manage snippets makes, "
	  "changes and deletes them and sends one; your top-level snippets are also on the Snippets menu and "
	  "the toolbar's Send snippet list, and any snippet can have a Shift+Ctrl+letter shortcut of "
	  "its own. A snippet can sit in a folder (type the folder name when you make or edit it); each "
	  "folder is a cascade on the menu. The Claude Code folder comes ready-made with that program's "
	  "keys - Interrupt (\\e), Rewind (\\e\\e), Switch mode (\\e[Z) - and its /clear, /compact, /resume "
	  "and /help commands, all of which you can change or delete.\n"
	  "\n"
	  "In a snippet's text, \\n presses Enter, \\t presses Tab, \\e presses Esc and ^C sends Ctrl+C (any "
	  "letter after ^). Then press Enter adds a final Enter." },

	{ "tmux",
	  "The tmux menu sends tmux's commands for you: the prefix (Ctrl+B, or Ctrl+A if your tmux is set "
	  "that way - tmux > Prefix key) and then the key. Windows: new, next, previous, choose, rename. "
	  "Panes: split, next pane, zoom. Also scroll / copy mode, the mouse on or off, and detach.\n"
	  "\n"
	  "With tmux > Windows as tabs on, PsiTerm asks tmux for its windows (a command run on the "
	  "server, on a channel of its own) and shows them as tabs at the top of the screen, like a "
	  "dialog's pages, in place of tmux's own line: tap one to go to it, or press Ctrl+Tab and "
	  "Shift+Ctrl+Tab for the next and previous. PsiTerm tells tmux to switch; it does not type "
	  "the prefix into whatever is running. With many windows, the arrows at the ends show the "
	  "rest. tmux's prompts and messages show in the tabs' place.\n"
	  "\n"
	  "If the server will not run commands, or its tmux is on another socket, PsiTerm falls back to "
	  "reading tmux's status line. Tools > Debug > Check tmux tabs shows which way is in use.\n"
	  "\n"
	  "When a session drops (the Psion switched off, the WiFi gone), PsiTerm reconnects if "
	  "Connection settings > Reconnect if dropped is Yes. A Command on login such as tmux new -A -s "
	  "psion, set for the server in File > SSH to > Edit, lands you back where you were." },

	{ "SSH keys",
	  "Tools > SSH keys lists your keys. New makes an Ed25519 key on the Psion (PsiTerm asks you to "
	  "type some keys first, for randomness); Import reads an OpenSSH or Dropbear key file (Ed25519 "
	  "or RSA, without a passphrase) from a disk; Show prints the public key and its fingerprint; Edit "
	  "renames a key or makes a new one in its place; Delete removes it.\n"
	  "\n"
	  "Once logged in to a server with a password, File > Install login key on server adds a key to "
	  "the server's ~/.ssh/authorized_keys. Then choose that key under Log in with in File > SSH to > "
	  "Edit, and the server stops asking for the password." },

	{ "Files & logs",
	  "While logged in, PsiTerm can copy files to and from the server over the same connection - no "
	  "second login. It uses SFTP, which almost every SSH server offers.\n"
	  "\n"
	  "File > Files > Send file: choose the file on the Psion, then the server folder it goes to (Enter sends "
	  "it to the folder shown; Open, or a second tap, goes into a folder; Up or Backspace goes back). "
	  "If the server already has a file of that name, PsiTerm asks before replacing it.\n"
	  "\n"
	  "File > Files > Get file: choose the file in the server's folders (Enter opens a folder or fetches a "
	  "file), then where it goes on the Psion. A file that arrives only in part - Stop, a full disk, "
	  "a lost connection - is not kept, and an older Psion file of the same name is left as it was.\n"
	  "\n"
	  "A window shows how much has moved; Stop (Esc) ends the transfer. Over a 57600 baud modem a "
	  "100 KB file takes about 20 seconds.\n"
	  "\n"
	  "File > Files > Log to file writes everything the server sends to a file, as plain text (colours and "
	  "cursor movements taken out) or exactly as received. Log shows on the status line while it "
	  "runs; choose the command again to stop. If the disk fills up, the log stops and says so." },

	{ "Connections",
	  "Tools > Connection settings chooses how PsiTerm reaches the server, the same way as PsiMail "
	  "and PsiWeb.\n"
	  "\n"
	  "Test (Ctrl+T) tries the settings shown, before OK: whether the modem answers and at what "
	  "speed, and whether CTS allows RTS/CTS flow control; for Psion Internet, whether the "
	  "connection is up and names can be looked up (it asks before dialling).\n"
	  "\n"
	  "Modem: a serial WiFi modem (such as a WiRSa or a WiFi232) on the Psion's serial port. PsiTerm "
	  "dials the server with ATDT host:port. Set the baud rate to the modem's; 115200 with RTS/CTS "
	  "flow control is fastest if the cable carries those lines. File > Connect brings the link up "
	  "now, with no SSH or update: the Psion's Internet is dialled, or the modem is checked, and the "
	  "result says what it found; PsiMail and PsiWeb then find it ready. File > Hang up modem (Shift+Ctrl+U) "
	  "ends the call.\n"
	  "\n"
	  "Psion Internet: the Psion's own dial-up (PPP) connection, set up in the Control panel's "
	  "Internet and Modems settings. PsiTerm starts it when it connects and the Psion's connection "
	  "dialogs appear; set the Internet service to Direct. Psion Internet: first send is a modem "
	  "command sent before dialling (ATDT777 starts a WiRSa's PPP).\n"
	  "\n"
	  "The serial port can be used by one program at a time: switch the Remote link off (System "
	  "screen, Ctrl+L) and close PsiMail or PsiWeb's connection first." },

	{ "Screen & greys",
	  "View > Reading mode raises the screen's contrast and keeps the backlight on while PsiTerm is in "
	  "front. Your own contrast and backlight come back when PsiTerm goes to the background or closes, "
	  "or the Psion is switched off; the next key turns it on again. If you change the contrast "
	  "meanwhile, yours is kept. How much, and whether the light stays on, is set in Screen greys "
	  "below, for all three programs.\n"
	  "\n"
	  "Tools > Preferences > Screen greys (the same button is in PsiMail's and PsiWeb's Preferences) "
	  "opens on \"Which looks best?\": the same picture and the same coloured text drawn four ways - "
	  "Standard, Lighter shadows, Darker shadows, and As before (off). Look at the doorway and the tree "
	  "(they should show detail, not be black), the clouds (they should not vanish into the white) and "
	  "the ball (it should look smoothly round). Press 1 to 4, or Left and Right, to choose; then Up and "
	  "Down fine-tune the one chosen, a small step lighter or darker each time, and its name says by how "
	  "much. Enter uses it in PsiTerm, PsiMail and PsiWeb; Esc closes without saving. Either way you are "
	  "back in Preferences. The greys change pictures and coloured text only: plain black text is not "
	  "changed.\n"
	  "\n"
	  "A on that screen opens the detailed settings: the Psion's 16 greys as bars, with ramps drawn as "
	  "PsiMail and PsiWeb draw pictures and plain greys. Tab picks a setting; Up and Down change it "
	  "(on Levels, Left and Right pick a bar and Up makes the greys near it lighter); Del goes back to "
	  "the standard; Enter saves for PsiTerm, PsiMail and PsiWeb; Esc closes without saving. It is right "
	  "when the ramps look smooth and evenly stepped. It can also turn the calibration off (plain "
	  "greys, as before) and choose how pictures are dithered: error diffusion (smoother) or the "
	  "ordered pattern of earlier versions. The chart docs/display-chart.png shows what to aim for.\n"
	  "\n"
	  "Tools > Preferences > Coloured text: dark greys (the standard) keep coloured text dark enough "
	  "to read on the white screen; lighter greys are as earlier versions drew it." },

	{ "Updating PsiTerm",
	  "Tools > Update PsiTerm looks for a newer PsiTerm on GitHub (the published releases, or the test "
	  "builds) or on a local server on your own network, downloads it to the memory disk, checks its "
	  "signature and offers to install it. PsiTerm closes while the installer runs; start it again "
	  "from the Extras bar afterwards.\n"
	  "\n"
	  "Tools > About PsiTerm (Shift+Ctrl+A) shows the version you have. Tools > Debug holds the "
	  "developer's tools: a speed test, the serial port's details, screenshots." },

	{ "Keyboard shortcuts",
	  "Every plain Ctrl+letter goes to the server. PsiTerm's own keys:\n"
	  "\n"
	  "Shift+Ctrl+S / Shift+Ctrl+D / Shift+Ctrl+U: SSH to / end SSH / hang up the modem\n"
	  "Shift+Ctrl+C / Shift+Ctrl+V: copy / paste\n"
	  "Shift+Ctrl+M: zoom (round the five sizes)\n"
	  "Shift+Ctrl+B: show or hide the toolbar\n"
	  "Shift+Pg Up / Shift+Pg Dn (Shift+Fn+Up/Down): scroll back / forward\n"
	  "Shift+Home / Shift+End (Shift+Fn+Left/Right): oldest text / live screen\n"
	  "Shift+Ctrl+T: Shift+Tab\n"
	  "Ctrl+Tab / Shift+Ctrl+Tab: next / previous tmux window (as tabs)\n"
	  "Esc while connecting: stop; Enter while waiting to reconnect: now\n"
	  "Shift+Ctrl+K / Shift+Ctrl+H / Shift+Ctrl+A / Shift+Ctrl+E: preferences / help / about / close\n"
	  "Shift+Ctrl+ a letter of your own: a snippet" }
	};

static const TInt KHelpCount = sizeof(KHelpTopics) / sizeof(KHelpTopics[0]);

void CPsiTermAppUi::HelpL()
	{
	CPtHelpDialog* dlg = new(ELeave) CPtHelpDialog(0);
	dlg->ExecuteLD(R_PT_HELP_DIALOG);
	}

void CPtHelpDialog::SetSizeAndPositionL(const TSize& aSize)
	{
	TSize screen = iEikonEnv->ScreenDevice()->SizeInPixels();
	TSize size(aSize.iWidth < screen.iWidth - 8 ? aSize.iWidth : screen.iWidth - 8,
		aSize.iHeight < screen.iHeight - 8 ? aSize.iHeight : screen.iHeight - 8);
	SetCornerAndSizeL(EHCenterVCenter, size);
	}

void CPtHelpDialog::PreLayoutDynInitL()
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
	CEikChoiceList* cl = (CEikChoiceList*)Control(EPtDlgHelpTopic);
	cl->SetArrayL(titles);                  // it owns the array now
	if (iTopic < 0 || iTopic >= KHelpCount)
		iTopic = 0;
	cl->SetCurrentItem(iTopic);
	ShowTopicL(iTopic);
	}

// the topic's text: its title in bold, then the paragraphs
void CPtHelpDialog::ShowTopicL(TInt aTopic)
	{
	const TPtHelpTopic& topic = KHelpTopics[aTopic];
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
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(EPtDlgHelpText);
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
void CPtHelpDialog::PostLayoutDynInitL()
	{
	TryChangeFocusToL(EPtDlgHelpTopic);
	}

void CPtHelpDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId != EPtDlgHelpTopic)
		return;
	TInt t = ((CEikChoiceList*)Control(EPtDlgHelpTopic))->CurrentItem();
	if (t != iTopic && t >= 0 && t < KHelpCount)
		{
		iTopic = t;
		ShowTopicL(t);
		}
	}

TBool CPtHelpDialog::OkToExitL(TInt /*aButtonId*/)
	{
	return ETrue;
	}
