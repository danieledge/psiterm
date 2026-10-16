# iPhone (and other) USB tethering on the AtomS3 Lite: status

**Status for this release: experimental and unsupported.** The supported,
primary way for the Psion to use an iPhone's internet is the iPhone's
**Personal Hotspot over WiFi**, with the Atom as a WiFi station
(`AT$SSID`/`AT$PASS`, or the web page). Nothing in the firmware selects the
USB path by itself: the USB host is only started with `AT$USB=1` saved, and
AUTO only routes through USB once a device has a DHCP address.

USB tethering is ten separate problems. Each has its own status.

| # | Concern | What exists in `src/usbnet/` | Status | What is missing |
|---|---|---|---|---|
| 1 | **Enumeration**: the S3's USB-OTG as a host; reading the device and configuration descriptors | ESP-IDF 5.3 `usb_host` library; `UsbNet::Begin` installs it with a daemon task and a client task; `DeviceOpen` reads the device descriptor and every configuration (`usb_host_get_config_desc`) | Implemented, **Compiles**, Not tested on hardware | A session with a device and the log |
| 2 | **Identification**: which device and which interface | `Inspect()` walks the descriptors: CDC-NCM (class 2/0x0d), CDC-ECM (2/0x06) with the union and Ethernet functional descriptors, Apple's vendor interface (0xFF/0xFD/0x01) by VID 0x05ac | Implemented, Compiles; `IphethMatch` Host-unit-tested | Hardware confirmation of real descriptors |
| 3 | **Apple interface handling**: selecting the configuration that holds the interface, the alternate setting, MAC, carrier | `SET_CONFIGURATION` by a control transfer (the host library's enumeration filter is not compiled into the Arduino core), claim, `SET_INTERFACE` alt 1, `GET_MAC` (0x00), carrier polled with 0x45 every second | Implemented, Compiles, Not tested; the library is not told about the configuration change (grey area) | Verification that the library tolerates the configuration switch; else a rebuild of the core with `CONFIG_USB_HOST_ENABLE_ENUM_FILTER_CALLBACK` |
| 4 | **Pairing / trust**: the phone only reports the carrier up to a *paired* host ("Trust This Computer") | Nothing | **Not implemented** | See below |
| 5 | **Framing**: Ethernet frames in and out | NCM NTB16 parse/build, ECM raw frames, Apple's 2-byte-header frames | `ncm.cpp` and `ipheth.cpp` **Host-unit-tested** (37 checks: round trips, bad blocks); transfer submission Compiles only | Hardware |
| 6 | **Interface integration**: frames into the IP stack | A custom `esp_netif` driver (`NetifStart`, `Transmit`, `esp_netif_receive` with a copy per frame), route priority above WiFi | Implemented, Compiles, Not tested | Hardware; memory check with two IN and two OUT transfers in flight |
| 7 | **DHCP**: an address from the phone | The ETH-type netif's DHCP client, started by `esp_netif_action_connected` once the carrier is up; `IP_EVENT_ETH_GOT_IP` sets `EUsbUp` | Implemented, Compiles, Not tested | Hardware |
| 8 | **DNS and routing**: the modem's sockets using the USB route | lwIP's default netif by `route_prio` (USB 120 > WiFi 100); DNS servers from the phone's DHCP | Implemented by configuration, Not tested | Confirmation that a `WiFiClient` connect goes out through the USB netif when both are up |
| 9 | **Reconnection**: the phone re-enumerating, tethering toggled | `DEV_GONE` → close, netif stop; a new device → set-up again; the uplink manager's 5 s hold-off | Implemented, Compiles; the hold-off Host-unit-tested | Hardware |
| 10 | **Power and USB role**: the Atom as host must supply VBUS; the S3's port is wired as a device (Rd on CC) | Nothing in software can change this | **Known limitation** | A 5 V supply to the Atom's rail that reaches VBUS (to be measured), and for a USB-C iPhone an adapter that presents Rp on CC; see HARDWARE.md section 5 |

### Why it is unsupported

Concern 4 decides everything for an iPhone. Without pairing, concerns 1–3
can succeed (the phone enumerates, the interface can be claimed) and the
firmware will report *no carrier (tethering off, or an unpaired iPhone)*,
AUTO will stay on WiFi, and that is the designed outcome. Concern 10 means
even the experiment needs a power arrangement that has not been checked.

An Android phone with USB tethering on (NCM, no pairing) or a CDC
Ethernet adapter is the realistic first hardware test of concerns 1, 2 and
5–9; it has not been done.

### What completing Apple pairing would require (not impossible; not small)

The pairing is the `usbmuxd` + `lockdownd` protocol that a PC performs when
an iPhone is plugged in:

1. Claim the "Apple Mobile Device" interface (class 0xFF/0xFE/0x02) and
   speak **usbmux**: a multiplexer that carries TCP-like connections over
   the bulk endpoints (version negotiation, connect to port 62078).
2. On that connection, **lockdownd**: binary or XML **plist** messages
   (`QueryType`, `GetValue`, `Pair`, `ValidatePair`, `StartSession`).
3. For `Pair`: generate a **host RSA-2048 key and certificate**, read the
   device's public key, sign a device certificate with the host's CA, and
   send the pair record; the phone shows "Trust This Computer" and answers.
4. `StartSession` then runs **TLS 1.2 over the usbmux connection with the
   host certificate as client certificate** (mbedTLS on the S3 can do this).
5. Keep the **pair record** (host ID, certificates, the escrow bag) in NVS
   so the next plug-in needs no prompt; `ValidatePair` on reconnect.
6. Only then does the USB Ethernet interface's carrier check return "up".

Everything needed exists on the S3 (mbedTLS, NVS, the USB host); the work
is a careful port of the relevant part of `libimobiledevice` (plist, usbmux
framing, lockdown messages) without its dependencies, and testing against
real phones and iOS versions. Estimate: a project of its own, weeks rather
than days, to be done after the USB host basics have been seen working with
a non-Apple device.

### Not implemented at all

- RNDIS (older Android tethering, Windows): would be a small class driver.
- USB hubs (the host library has them off in this core).
- Anything in the Arduino core's `sdkconfig` that cannot be changed from
  `platformio.ini` (the enumeration filter, hub support).
