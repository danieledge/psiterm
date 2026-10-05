// ipheth.h - Apple's USB Ethernet interface, as an iPhone offers it for
// USB tethering: not CDC-ECM/NCM but a vendor interface (class 0xFF,
// subclass 0xFD, protocol 1), what Linux's ipheth driver talks to. Frames
// go on the bulk endpoints with two bytes in front of each. The carrier
// is read with a vendor control request, and comes up only once the host
// has paired with the phone (usbmuxd/lockdownd: "Trust This Computer"),
// which this firmware does not do: see docs/UPGRADE.md, section 12.
// Pure functions, no USB calls: the ESP32-S3's USB host task uses them
// and the host tests run them.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_IPHETH_H
#define ATOM_IPHETH_H

#include <stdint.h>
#include <stddef.h>

namespace am {

static const uint16_t kAppleVid = 0x05ac;
static const uint8_t kIphethClass = 0xff;
static const uint8_t kIphethSubclass = 0xfd;
static const uint8_t kIphethProtocol = 1;
static const uint8_t kIphethAltSetting = 1;      // the alternate setting with the bulk endpoints

// vendor requests (bRequestType 0xc0: vendor, device to host), wIndex = the interface
static const uint8_t kIphethCmdGetMac = 0x00;    // 6 bytes
static const uint8_t kIphethCmdEnableNcm = 0x04; // (newer phones: frames as NCM blocks; not used)
static const uint8_t kIphethCmdCarrierCheck = 0x45;   // 1 byte
static const uint8_t kIphethCarrierOn = 0x04;
static const uint32_t kIphethCarrierPollMs = 1000;

static const size_t kIphethAlign = 2;            // the bytes in front of each frame
static const size_t kIphethMaxFrame = 1514;
static const size_t kIphethBufSize = kIphethMaxFrame + kIphethAlign;

bool IphethMatch(uint16_t aVid, uint8_t aClass, uint8_t aSubclass, uint8_t aProtocol);
bool IphethCarrier(const uint8_t* aAnswer, size_t aLen);
// a bulk transfer from the phone: the frame in it (false: too short)
bool IphethUnpack(const uint8_t* aIn, size_t aLen, const uint8_t*& aFrame, size_t& aFrameLen);
// a frame for the phone: the bytes to send (0: too big)
size_t IphethPack(uint8_t* aOut, size_t aOutMax, const uint8_t* aFrame, size_t aFrameLen);

} // namespace am

#endif
