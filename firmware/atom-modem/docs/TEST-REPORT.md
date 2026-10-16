# Test report: Atom modem 2.0, 4 October 2026

Environment: Ubuntu 24.04 on `open-codeserver`, g++ 13.3, zlib, python3
3.12 with Pillow 11.3, PlatformIO 6.2.0 (in a scratch venv); platforms
pioarduino `platform-espressif32` 53.03.13 (Arduino-ESP32 3.1.3, ESP-IDF
5.3.0) and `espressif32@6.9.0` (Arduino-ESP32 2.0.17). No AtomS3 Lite, no
Psion on this bench: everything below is host-level.

## Before the review's changes (the audit)

Worktree `worktree-agent-af77482e307b6834a` at `4c91a99` (dev) with the 2.0
work uncommitted (34 changed/new paths).

```
make -C hosttest clean; make -C hosttest test
  test_modem        371 checks, 0 failed
  test_usbnet        37 checks, 0 failed
  test_proxy        180 checks, 0 failed
  test_proxy_tinfl  180 checks, 0 failed
pio run -t clean; pio run -e atoms3-lite -e m5stack-atom
  atoms3-lite   SUCCESS  Flash 1,334,216 B (39.9 % of 3,342,336)  RAM 54,956 B static
  m5stack-atom  SUCCESS  Flash 1,105,329 B (35.1 % of 3,145,728)  RAM 56,052 B static
python3 mail/test/imgtest.py --rounds 30   (PsiMail's decoders, ASan, fuzz)
  ok: no crashes, no sanitizer reports, no leaks
```

## Changes made in the review

- WiFi reachability: `TWifiState` (down / joined-unconfirmed /
  joined-Internet / recovering) in `uplink.*`; the Internet check
  (`AT$CHK`, `Hal::ProbeInternet`/`Internet`, a connect in its own task on
  the board); rejoining after 20 s down then every 30 s; transitions
  logged; `ATI`/`AT$UP?` show the state. `Settings2.chkHost` from the spare
  bytes (record still 256 bytes).
- Tests: the `reachability` section (35 new checks) and the WiFi-state
  checks in `uplink`; the baud-fallback tests count fall-back lines rather
  than assume an empty log.
- Documents: `HARDWARE.md` (new), `USB-TETHERING.md` (new),
  `USB-KEYBOARD.md` (new), `VALIDATION.md` (new), this report; `README.md`
  and `UPGRADE.md` corrected (RS232 Base primary, hotspot primary, USB
  experimental, `AT$CHK`).

## After

```
make -C hosttest test
  test_modem        406 checks, 0 failed
  test_usbnet        37 checks, 0 failed
  test_proxy        180 checks, 0 failed
  test_proxy_tinfl  180 checks, 0 failed
pio run -e atoms3-lite -e m5stack-atom
  atoms3-lite   SUCCESS  Flash 1,336,540 B (40.0 %)  RAM 55,004 B static
  m5stack-atom  SUCCESS  Flash 1,106,913 B (35.2 %)  RAM 56,100 B static
psiexecd.py end to end through hostmodem (scratch script): OK
```

Warnings in the firmware's own sources: none (the vendored picojpeg and
pmjprog have four pre-existing `-Wsequence-point`/unused warnings).

## Not run here (needs the bench)

Everything in VALIDATION.md section 2: 115200 baseline with the Psion,
the hotspot, 230400, RTS/CTS, the hand-off features on the device, the
web pages in a browser, free-heap figures, the USB host.
