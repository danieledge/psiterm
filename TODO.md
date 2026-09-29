# TODO

## Bugs

- **Screen shifts off the display, then the menu no longer opens.** Seen on a
  5mx (photo, 29 Sep 2026): the terminal text is drawn offset up and to the left
  (first lines cut off at the top, first characters cut off at the left); after
  that the Menu key does nothing. Screen showed Serial port info output plus two
  lines ending "(-5)" (KErrNotSupported). Happens "sometimes"; trigger unknown.
  Suspects: view origin/extent after a zoom or font change, a mis-sized redraw
  after a dialog, or an invisible modal dialog (which would also block the menu).
  Next: a screenshot (Debug > Screenshot) when it happens, and note what was done
  just before.

## Ideas

- Download URL...: fetch any https:// file to the Psion (TLS now works).
- mosh (parked).
