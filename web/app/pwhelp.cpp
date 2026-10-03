// pwhelp.cpp - Tools > Help on PsiWeb (Shift+Ctrl+H): the help topics, in
// an EIKON dialog with a topic list and a read-only text viewer - the same
// dialog as PsiMail's (mail/app/pmhelp.cpp), which says why the help lives
// in the program rather than in a .hlp file.
//
// The wording follows the EIKON style guide: the user's words, key names as
// the keyboard prints them, menu commands named as they appear (without
// their "..."), "pictures" not "images", "program" not "application".
#include "pwapp.h"
#include <eikchlst.h>
#include <eikrted.h>
#include <txtrich.h>

struct TPwHelpTopic { const char* iTitle; const char* iText; };

static const TPwHelpTopic KHelpTopics[] =
	{
	{ "Getting started",
	  "PsiWeb is the Links web browser on the Psion: pages are shown as plain documents, with JPEG, "
	  "PNG and GIF pictures when you ask for them; no JavaScript and no style sheets. It "
	  "reaches the Internet through a WiFi modem on the serial port or the Psion's own Internet "
	  "connection, as PsiTerm and PsiMail do.\n"
	  "\n"
	  "File > Open address (Ctrl+O, or the Open button) asks for an address; words instead of an "
	  "address are searched for. File > Home page (Ctrl+H) opens your home page, which is set in "
	  "Tools > Preferences. PsiWeb starts on its own welcome page, which needs no connection.\n"
	  "\n"
	  "PsiWeb starts its browser engine when it opens; the engine then dials or connects when a page "
	  "needs it, and says what it is doing at the bottom left." },

	{ "Reading pages",
	  "Up and Down scroll a line; Fn+Up and Fn+Down (Pg Up and Pg Dn) a screen; Go > Top of page and "
	  "End of page go to the ends. Tap a link to follow it, a field to type in it, a button to press "
	  "it; once a field is chosen, type into it. Left and Right scroll a wide page sideways.\n"
	  "\n"
	  "The scroll bar beside the page works with the pen: tap its arrows to move a few lines, tap "
	  "above or below its thumb to move a screen, or drag the thumb.\n"
	  "\n"
	  "Go > Back (Ctrl+B, or the Back button) and Forward (Ctrl+F) move through the pages you have "
	  "seen. Esc stops a page that is loading; File > Reload (Ctrl+R) fetches it again.\n"
	  "\n"
	  "View > Zoom in and Zoom out (Ctrl+M and Shift+Ctrl+M, or the Zoom in and Zoom out icons "
	  "beside the screen) go round the sizes from 50% to 200%; Normal size is 100%.\n"
	  "\n"
	  "Pictures are left out, which is much faster over a modem: View > Show pictures (Ctrl+I, or "
	  "the Pictures button) fetches the pictures of the page showing, and takes them away again. "
	  "The button stays pressed in while they show. To have them on every page, set Pictures in "
	  "Tools > Preferences. View > Show toolbar (Ctrl+T) gives the page the toolbar's "
	  "room; the engine starts again to draw at the new width.\n"
	  "\n"
	  "View > Page information (Shift+Ctrl+Q) shows the page's title and address, what the engine "
	  "is doing, the free memory and the connection in use.\n"
	  "\n"
	  "A page that asks for a user name and password brings up a dialog for them; PsiWeb remembers "
	  "them until it closes. A file PsiWeb cannot show, such as a PDF, can be saved to a disk (up "
	  "to 4 MB): PsiWeb says what it is and offers the Save as dialog; the bottom left shows how much "
	  "has come, and Esc stops it.\n"
	  "\n"
	  "Light sites suit the Psion's memory best - 68k.news, FrogFind, DuckDuckGo Lite, text.npr.org, "
	  "lite.cnn.com. On a long page with pictures, those far from the screen are let go, and made "
	  "again from memory when you come back to them; a page that still does not fit says \"Page too "
	  "big\" and shows what fits." },

	{ "Screen & greys",
	  "View > Reading mode (Shift+Ctrl+R) raises the screen's contrast and keeps the backlight on "
	  "while PsiWeb is in front; your own settings come back when it goes to the background or "
	  "closes, or the Psion is switched off (the next key turns it on again).\n"
	  "\n"
	  "Pictures are dithered once, as they are set out, by error diffusion; text and the page's "
	  "own colours are drawn in plain greys, with no pattern. The greys follow the display "
	  "calibration set in PsiTerm (Tools > Debug > Display calibration), from the next page; "
	  "it can also choose the ordered pattern of earlier versions.\n"
	  "\n"
	  "Tools > Preferences > Text: Sharp (the standard) draws text with fonts made ahead for the "
	  "Psion's greys at 100%; other zoom sizes use the scaled fonts. Scaled (as before) uses Links' "
	  "own fonts at every size. The engine starts again when it changes." },

	{ "Connections & proxy",
	  "Tools > Connection settings chooses how PsiWeb reaches the Internet, the same way as PsiTerm "
	  "and PsiMail.\n"
	  "\n"
	  "Test (Ctrl+T) tries the settings shown, before OK: whether the modem answers and at what "
	  "speed, and whether CTS allows RTS/CTS flow control; for Psion Internet, whether the "
	  "connection is up and names can be looked up (it asks before dialling).\n"
	  "\n"
	  "Modem: a serial WiFi modem (such as a WiRSa or a WiFi232) on the Psion's serial port. PsiWeb "
	  "dials the server with ATDT host:port. Set the baud rate to the modem's; 115200 with RTS/CTS "
	  "flow control is fastest if the cable carries those lines.\n"
	  "\n"
	  "Psion TCP/IP: the Psion's own dial-up (PPP) connection, set up in the Control panel's Internet "
	  "and Modems settings. PsiWeb starts it when it needs to and the Psion's connection dialogs "
	  "appear.\n"
	  "\n"
	  "A proxy (Tools > Preferences > Use a proxy), such as WebOne on a computer on your network, is "
	  "strongly recommended: it fetches https pages for the Psion, which has no time for a TLS "
	  "handshake on every page, and keeps one connection open so the modem dials once. Without one, "
	  "http pages go direct and https pages use PsiWeb's own TLS 1.3.\n"
	  "\n"
	  "File > Disconnect (Ctrl+U) hangs up and frees the serial port for PsiTerm or PsiMail. The "
	  "port can be used by one program at a time; the Remote link must be off too." },

	{ "If the engine stops",
	  "The browser engine runs as its own program, so if it stops, the Psion carries on. PsiWeb "
	  "then says so on the page and Tools > Restart browser engine starts it again. Preferences and connection settings that the engine reads when it "
	  "starts also restart it, on the same page." },

	{ "Updating PsiWeb",
	  "Tools > Update PsiWeb looks for a newer PsiWeb on GitHub (the published releases, or the test "
	  "builds) or on a local server on your own network, downloads it in pieces to the memory disk, "
	  "checks its signature and offers to install it. Esc stops the download. PsiWeb closes while "
	  "the installer runs; start it again from the Extras bar afterwards.\n"
	  "\n"
	  "Tools > About PsiWeb (Shift+Ctrl+A) shows the version you have." },

	{ "Keyboard shortcuts",
	  "Up/Down, Fn+Up/Fn+Down (Pg Up/Pg Dn): scroll\n"
	  "Esc: stop loading\n"
	  "\n"
	  "Ctrl+O or Ctrl+L: open an address, or search for words\n"
	  "Ctrl+H / Ctrl+R: home page / reload\n"
	  "Ctrl+B / Ctrl+F: back / forward\n"
	  "Ctrl+M / Shift+Ctrl+M, or the Zoom icons: zoom in / out\n"
	  "Ctrl+I / Ctrl+T: show pictures (this page, on or off) / show toolbar\n"
	  "Shift+Ctrl+Q: page information\n"
	  "Shift+Ctrl+R: Reading mode\n"
	  "Ctrl+U: disconnect\n"
	  "Ctrl+K / Shift+Ctrl+H / Shift+Ctrl+A / Ctrl+E: preferences / help / about / close" }
	};

static const TInt KHelpCount = sizeof(KHelpTopics) / sizeof(KHelpTopics[0]);

void CPwAppUi::HelpL()
	{
	CPwHelpDialog* dlg = new(ELeave) CPwHelpDialog(0);
	dlg->ExecuteLD(R_PW_HELP_DIALOG);
	}

void CPwHelpDialog::PreLayoutDynInitL()
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
	CEikChoiceList* cl = (CEikChoiceList*)Control(EPwDlgHelpTopic);
	cl->SetArrayL(titles);                  // it owns the array now
	if (iTopic < 0 || iTopic >= KHelpCount)
		iTopic = 0;
	cl->SetCurrentItem(iTopic);
	ShowTopicL(iTopic);
	}

// the topic's text: its title in bold, then the paragraphs
void CPwHelpDialog::ShowTopicL(TInt aTopic)
	{
	const TPwHelpTopic& topic = KHelpTopics[aTopic];
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
	CEikRichTextEditor* ed = (CEikRichTextEditor*)Control(EPwDlgHelpText);
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
void CPwHelpDialog::PostLayoutDynInitL()
	{
	TryChangeFocusToL(EPwDlgHelpTopic);
	}

void CPwHelpDialog::HandleControlStateChangeL(TInt aControlId)
	{
	if (aControlId != EPwDlgHelpTopic)
		return;
	TInt t = ((CEikChoiceList*)Control(EPwDlgHelpTopic))->CurrentItem();
	if (t != iTopic && t >= 0 && t < KHelpCount)
		{
		iTopic = t;
		ShowTopicL(t);
		}
	}

TBool CPwHelpDialog::OkToExitL(TInt /*aButtonId*/)
	{
	return ETrue;
	}
