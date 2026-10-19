# TODO

## Bugs

- PsiTerm: the Get file dialog looks unfinished (Dan's photo, 12 Oct 2026). The column headings (Location, Name,
  Size, Modified) are cut off at the top and bunched up; Size is empty for
  folders; the folder/file counts float loose under the tree; and there is
  empty space between the list and the Up / Cancel / Get buttons. Bring it
  up to the built-in programs' file dialogs.
  Redesigned in 0.90 (checked in the emulator, not yet on the device): laid
  out as EIKON's own Browse dialog (Word > Open file > Browse). It has the
  standard title bar, a framed Location pane and a framed list that run
  down to the bottom in whole rows, and the buttons down the right with
  Cancel and Get at the bottom. Headings are bold and aligned with their
  columns (Size and Modified right-aligned). Folders have no size, and
  dates are in figures in the Psion's date format. The pane's rows line up
  with the list's, and its foot sits under a rule.
  Favourites (also Dan, 12 Oct): one key or tap now adds the folder shown
  (Favourite, Ctrl+F, which takes it out again), the favourites are listed
  in the Location pane (one tap goes there), and Go to (Ctrl+G) is a
  smaller dialog that fits the screen. The Up button has gone: Backspace,
  Ctrl+U, the ".." row and the pane all go up.
- PsiTerm: over Psion Internet (PPP) with the link already up, connecting
  hangs up and dials again. That is not needed over PPP: use the open link.
- PsiMail: over PPP with the link already up, it can stick on "TCP
  connection open" - the message stays and nothing happens. (Likely
  related to the PsiTerm one above, in psiglue's PPP route.)

- ~~Screen shifts off the display, then the menu no longer opens.~~ Fixed in
  0.25: the Debug/update window was bigger than the 640x240 screen, so EIKON
  centred it partly off-screen (title and buttons hidden, modal so the menu
  was blocked). The window is now smaller and clamped to the screen.

## Ideas

- Download URL...: fetch any https:// file to the Psion (TLS now works).
- mosh (parked).
- 16-grey icons for our apps (Dan, 17 Oct 2026): the 5mx shows 16 greys
  but every app's icons use 4. Hand-draw the app icons (48, 32, 24) and
  toolbar pictures for 16 greys - shading, soft edges on curves, highlights -
  and have the icon tools write them (bmconv /4). An automatic conversion of
  the flat colour artwork was tried and is no better than today's 4 greys.
  Start with the 48 px app icons as samples.
  Built-in apps too? Their .aif and .mbm files are in ROM (Z:\System\Apps\…).
  Find out in the emulator whether EPOC takes an app's icon from C: for a
  ROM app (apparc looks for the .aif beside the .app, so probably not
  without a stand-in app); toolbar pictures are loaded by full ROM path, so
  they likely can't be replaced.
