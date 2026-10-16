# Feasibility: the Psion's keyboard as a USB keyboard for the iPhone

**Status: a future investigation. Nothing of it is in the firmware, and the
primary WiFi modem mode is not changed for it.** This note records what is
known so the question can be answered properly later.

## The idea

The Psion types into PsiTerm; PsiTerm sends characters down the serial line
to the Atom; the Atom presents itself to an iPhone over USB-C as a USB HID
keyboard and sends the keystrokes. The Psion's excellent keyboard then
types into the phone.

## What the hardware allows

- **USB role.** The ESP32-S3's native USB can be a **device**, and the
  Arduino core includes TinyUSB with a HID keyboard class (`USBHIDKeyboard`).
  A USB keyboard is a device, which is the role the AtomS3 Lite's USB-C is
  already wired for (pull-downs on CC, powered from VBUS). This part is the
  easy direction: no host stack, no VBUS to supply, no pairing.
- **The phone as host.** An iPhone with Lightning needs a camera-adapter
  (OTG) cable to be a USB host; a USB-C iPhone (15 and later) is a host on
  a plain USB-C to USB-C cable and supplies VBUS. iOS supports USB keyboards
  natively (text input, shortcuts), without pairing.
- **Power.** As a device the Atom is powered by the phone's VBUS: a Lightning
  phone through a camera adapter supplies about 100 mA, which is marginal
  for an ESP32-S3 with WiFi on; a USB-C phone supplies more. The RS232 Base
  adds little. Whether WiFi can stay on while on phone power is a bench
  question; the keyboard mode could run with WiFi off.
- **D+/D− exposure.** The S3's USB pins (GPIO19/20) go to the USB-C only, so
  the modem's UART (G5/G6) is untouched by the USB role; serial and USB can
  coexist electrically.

## Coexistence with the modem modes

- **Programming and the console** use the same USB-C in the same device
  role. The firmware would have to switch between "CDC console" and "HID
  keyboard" (TinyUSB can present both as a composite device, which iOS
  tolerates), and the download mode (hold the button through a reset)
  always wins for flashing.
- **The modem keeps working** while the keyboard mode is on: the serial
  line, WiFi and the sockets are separate from USB. A PsiTerm session could
  be "a terminal to the Atom" in which keystrokes are forwarded, or a
  normal SSH session; the firmware would need a way to tell them apart
  (below).
- **The USB host mode** (`AT$USB=1`, for tethering) and the keyboard mode
  are mutually exclusive: one port, one role at a time, with a restart
  between them. This is fine, because the hotspot path needs neither.

## The hard part: what the Psion sends

PsiTerm sends **terminal characters**, not key events: a letter, a control
code (Ctrl+C is byte 3), an escape sequence for arrows and function keys
(`ESC [ A`), nothing at all for a modifier pressed on its own, and no
key-up. A USB keyboard reports **scan codes with modifiers and key-up**.
The mapping is therefore:

- Printable characters → the scan code and shift state for a US (or the
  phone's) layout; the Psion's layout and the phone's must agree, or
  symbols come out wrong.
- Control bytes → Ctrl + letter; `ESC [ ...` → arrows, Home/End, Page
  Up/Down, function keys; `\r` → Enter; backspace (8 or 127) → Backspace.
- No way to send a bare modifier, Cmd shortcuts, or key repeat timing: iOS
  shortcuts that need Cmd (⌘) cannot be typed from terminal characters
  unless the firmware invents an escape ("the Psion's Menu key as ⌘"),
  which would need a PsiTerm-side change to emit something for it.
- Latency: a keystroke crosses the serial line (a byte at 115200 is 87 µs)
  and then USB (1 ms polling); fine.

A better design for a keyboard mode would have PsiTerm send **key events**
(code, modifiers, down/up) in a small escape protocol when the mode is on,
which is a PsiTerm change (the app sees the raw EPOC key events). That is
also what would make the Psion's Fn/Menu/diamond keys usable.

## iOS limits worth knowing

- iOS accepts standard boot-protocol and report-protocol keyboards; no
  driver, no "Trust" prompt for HID.
- Text fields only: a keyboard cannot "open an app" except through the
  shortcuts iOS already gives external keyboards (Cmd+Space, Cmd+H…), which
  need ⌘.
- A Lightning phone may show "accessory not supported" if the device draws
  too much; the camera adapter's power budget is the constraint.
- No USB networking is happening at the same time in this mode; the phone
  is a host, the Atom a keyboard. (The reverse, the Atom as host for
  tethering, is `USB-TETHERING.md`.)

## Verdict

Feasible as a device-mode feature on the existing AtomS3 Lite (no new
hardware except the phone's own adapter cable), independent of the serial
and WiFi paths, with two design decisions to take first: the composite USB
device (console + keyboard) and the key-event protocol from PsiTerm. Not
started; not to be started before the modem path has been validated on the
Psion (VALIDATION.md).
