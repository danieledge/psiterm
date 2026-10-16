// wgconf.h - the WireGuard client's configuration: a wg-quick style text
// (what the UniFi gateway's "WireGuard VPN Server" hands out for a client)
// parsed into a fixed record kept in NVS, plus the pure decisions the tunnel
// needs: whether it should be up (AT$WG Off/On/Auto), and whether a
// destination goes through it (split tunnel by AllowedIPs). Plain C++ with
// no Arduino or lwIP calls, so the host tests exercise all of it; the lwIP
// side is board/wgtunnel.cpp.
//
// Secrets: the private key and the preshared key live in this record, in
// NVS only. Nothing prints them: WgSummary says "set", error messages name
// the field and never quote its value, and the modem's log hides any
// AT$WGL= line. The repository holds no real or real-looking key.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_WGCONF_H
#define ATOM_WGCONF_H

#include <stdint.h>
#include <stddef.h>

namespace am {

static const int kWgAllowedMax = 8;           // AllowedIPs (IPv4) kept; more is an error
static const int kWgDnsMax = 2;
static const size_t kWgKeyText = 45;          // base64 of 32 bytes (44) and its NUL
static const size_t kWgTextMax = 2048;        // the longest config text taken

// an IPv4 prefix; the address as lwIP's ip4_addr holds it (bytes a,b,c,d in
// memory order: see Ip4() in ppp.h)
struct WgPrefix
	{
	uint32_t ip;
	uint8_t len;               // 0..32
	uint8_t pad[3];
	};

struct WgConfig
	{
	uint32_t magic;            // kWgMagic: a valid record
	uint16_t version;
	uint16_t length;           // sizeof(WgConfig) when saved
	char privKey[kWgKeyText];  // [Interface] PrivateKey (base64); SECRET
	char pubKey[kWgKeyText];   // [Peer] PublicKey (base64)
	char psk[kWgKeyText];      // [Peer] PresharedKey (base64), "" = none; SECRET
	uint8_t pad0;
	uint32_t addr;             // [Interface] Address (the tunnel's own IPv4)
	uint8_t addrLen;           // ... and its prefix length
	uint8_t nDns;
	uint8_t nAllowed;
	uint8_t pad1;
	uint32_t dns[kWgDnsMax];   // [Interface] DNS (IPv4 only; names and IPv6 are skipped)
	WgPrefix allowed[kWgAllowedMax];   // [Peer] AllowedIPs (IPv4 only)
	char endpoint[64];         // [Peer] Endpoint's host (a name or a dotted quad)
	uint16_t port;             // ... and port
	uint16_t keepalive;        // [Peer] PersistentKeepalive, s (0 = off)
	uint16_t listenPort;       // [Interface] ListenPort (0 = any)
	uint16_t mtu;              // [Interface] MTU (0 = the default, 1420)
	uint8_t spare[32];
	};

static const uint32_t kWgMagic = 0x41545747;  // 'ATWG'
static const uint16_t kWgVersion = 1;

void WgClear(WgConfig& aC);
// a record read from NVS: true if it is a complete, sane config
bool WgAccept(WgConfig& aC, size_t aLength);
// is there a usable config (keys, address, peer, AllowedIPs)?
bool WgValid(const WgConfig& aC);

// Parse a wg-quick config ([Interface] PrivateKey, Address, DNS, ListenPort,
// MTU; [Peer] PublicKey, PresharedKey, AllowedIPs, Endpoint,
// PersistentKeepalive). IPv6 entries are skipped; one peer only. On success
// aOut is the new config; on failure aOut is untouched and aWhy says which
// field is wrong (never its value).
bool WgParse(const char* aText, WgConfig& aOut, char* aWhy, size_t aWhyMax);

// a key's base64 text decodes to exactly 32 bytes (into aOut if not 0)
bool WgKeyOk(const char* aB64, uint8_t* aOut = 0);
// base64 (standard alphabet, padded) into aOut; -1 if malformed or too long
int Base64Decode(const char* aIn, uint8_t* aOut, size_t aMax);

// "a.b.c.d" (the ip4_addr byte order) and back
bool ParseIp4(const char* aText, uint32_t& aOut);
void Ip4Text(uint32_t aIp, char* aOut, size_t aMax);
// a prefix length's mask, in the ip4_addr byte order
uint32_t PrefixMask(int aLen);
// a mask's prefix length (the count of leading ones), as ip4_addr
int MaskLen(uint32_t aMask);
bool InPrefix(uint32_t aIp, uint32_t aNet, int aLen);

// What a reader may see: the address, DNS, AllowedIPs, endpoint, keepalive,
// and "set"/"none" for the keys. One "Name: value" per line ('\n').
void WgSummary(const WgConfig& aC, char* aOut, size_t aMax);
// "192.168.10.0/24,10.0.0.0/8"
void WgAllowedText(const WgConfig& aC, char* aOut, size_t aMax);

// ----- the decisions (host-tested) ----------------------------------------------

// Should the tunnel be up? aUplinkIp/aUplinkMask: the address the Atom has
// on its uplink (WiFi or USB), 0 if none.
enum TWgWant
	{
	EWgWantOff = 0,        // AT$WG=OFF
	EWgWantNoConfig,       // no usable config pasted
	EWgWantNoUplink,       // no network to reach the endpoint over
	EWgWantHome,           // Auto, and already on the home network
	EWgWantUp              // the tunnel should be up
	};
TWgWant WgDecide(int aMode, const WgConfig& aC, bool aUplinkUp, uint32_t aUplinkIp, uint32_t aUplinkMask);
const char* WgWantName(TWgWant aW);

// Auto's "already at home" test: the uplink address lies inside one of the
// AllowedIPs (other than a default route), or the tunnel's DNS server is on
// the uplink's own subnet. Either means the home network is reachable
// without the tunnel (and a tunnel to our own gateway would hairpin).
bool WgAtHome(const WgConfig& aC, uint32_t aUplinkIp, uint32_t aUplinkMask);

// The longest AllowedIPs prefix that holds aDest, or -1 if none does.
int WgBestPrefix(const WgPrefix* aAllowed, int aCount, uint32_t aDest);
// The split-tunnel routing rule, as the lwIP route hook applies it: aDest
// goes into the tunnel when an AllowedIPs prefix holds it and that prefix is
// LONGER than the most specific directly connected route to it
// (aConnectedLen: the prefix length of the netif subnet holding aDest, 32
// for a point-to-point peer such as the Psion over PPP, -1 if none). The
// endpoint itself never goes into the tunnel (else 0.0.0.0/0 would loop),
// and neither do broadcast, multicast or 0.0.0.0. Longest prefix wins, as in
// any router, so "AllowedIPs = 0.0.0.0/0" still leaves the WiFi's own
// subnet and the Psion's PPP address on their interfaces.
bool WgUseTunnel(const WgPrefix* aAllowed, int aCount, uint32_t aDest, int aConnectedLen, uint32_t aEndpoint);

} // namespace am

#endif
