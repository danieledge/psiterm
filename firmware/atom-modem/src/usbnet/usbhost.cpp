// usbhost.cpp - see usbhost.h. MIT licence (see LICENSE at the top of the repository).
//
// How it works on the ESP32-S3 (ESP-IDF 5.3's USB Host Library):
//   - a "daemon" task runs usb_host_lib_handle_events(); a "client" task
//     runs usb_host_client_handle_events() and, when a device appears,
//     reads its descriptors (every configuration), looks for a network
//     interface we know, selects that configuration, claims the
//     interface(s), sets the alternate setting with the bulk endpoints,
//     does the class set-up, and starts the IN transfers;
//   - frames from the device go to lwIP (esp_netif_receive) in a copy, so
//     the transfer buffer goes straight back to the device;
//   - frames from lwIP (the transmit callback, on lwIP's task) are framed
//     and submitted on one of two OUT transfers; none free: dropped (TCP
//     resends);
//   - the carrier: Apple's is polled every second; NCM/ECM's is taken as up
//     (the DHCP client is the judge);
//   - the state (TUsbState) and a line of detail are kept for the modem.
// The control transfers are done with a small synchronous helper that
// pumps the client's events while it waits, which is why the set-up runs
// on the client task outside the event callback.
// UNTESTED ON HARDWARE as of this version: see docs/UPGRADE.md.
#include "usbhost.h"
#include <string.h>
#include <stdio.h>

#if AM_USB_HOST

#include <Arduino.h>
#include "usb/usb_host.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "ncm.h"
#include "ipheth.h"

namespace am {

namespace {

enum TKind { KNone, KNcm, KEcm, KApple };

struct TDevice
	{
	usb_device_handle_t dev;
	uint8_t addr;
	uint16_t vid, pid;
	TKind kind;
	uint8_t config;                          // bConfigurationValue to use
	uint8_t commIntf, dataIntf, dataAlt;     // (Apple: one interface, alt 1)
	uint8_t epIn, epOut;
	uint16_t epInMax, epOutMax;
	uint8_t macStringIndex;
	uint8_t mac[6];
	bool claimedComm, claimedData;
	NcmParams ncm;
	uint16_t ncmSeq;
	size_t inSize;
	};

struct TState
	{
	volatile int state;
	char detail[64];
	usb_host_client_handle_t client;
	TDevice d;
	bool haveDevice;
	bool deviceGone;
	usb_transfer_t* in[2];
	usb_transfer_t* out[2];
	volatile bool outBusy[2];
	esp_netif_t* netif;
	bool netifUp;
	bool carrier;
	uint32_t lastCarrierMs;
	esp_ip4_addr_t ip;
	// the synchronous control helper
	volatile bool ctrlDone;
	usb_transfer_t* ctrl;
	};

TState g;

void SetDetail(const char* aText)
	{
	strncpy(g.detail, aText, sizeof(g.detail) - 1);
	g.detail[sizeof(g.detail) - 1] = 0;
	}

void Log(const char* aFmt, ...)
	{
	char m[160];
	va_list ap;
	va_start(ap, aFmt);
	vsnprintf(m, sizeof(m), aFmt, ap);
	va_end(ap);
	Serial.println(m);
	// (the modem's log ring is on the main task; the web page /log shows
	// Serial too when the console is a device. Kept simple: the state and
	// the detail line are what AT$UP? and ATI show)
	}

// ----- control transfers, synchronous ----------------------------------------------------

void CtrlDone(usb_transfer_t* aXfer)
	{
	(void)aXfer;
	g.ctrlDone = true;
	}

// a control transfer with the setup packet already in g.ctrl->data_buffer;
// aLen: the data stage. The result, or -1
int Control(size_t aLen, uint32_t aTimeoutMs)
	{
	g.ctrl->device_handle = g.d.dev;
	g.ctrl->bEndpointAddress = 0;
	g.ctrl->callback = CtrlDone;
	g.ctrl->context = 0;
	g.ctrl->num_bytes = (int)(sizeof(usb_setup_packet_t) + aLen);
	g.ctrlDone = false;
	if (usb_host_transfer_submit_control(g.client, g.ctrl) != ESP_OK)
		return -1;
	uint32_t start = millis();
	while (!g.ctrlDone)
		{
		usb_host_client_handle_events(g.client, pdMS_TO_TICKS(10));
		if (millis() - start > aTimeoutMs)
			return -1;
		}
	if (g.ctrl->status != USB_TRANSFER_STATUS_COMPLETED)
		return -1;
	return g.ctrl->actual_num_bytes - (int)sizeof(usb_setup_packet_t);
	}

int ControlIn(uint8_t aType, uint8_t aRequest, uint16_t aValue, uint16_t aIndex, uint8_t* aOut, uint16_t aLen)
	{
	usb_setup_packet_t* s = (usb_setup_packet_t*)g.ctrl->data_buffer;
	s->bmRequestType = aType;
	s->bRequest = aRequest;
	s->wValue = aValue;
	s->wIndex = aIndex;
	s->wLength = aLen;
	int n = Control(aLen, 2000);
	if (n > 0 && aOut)
		memcpy(aOut, g.ctrl->data_buffer + sizeof(usb_setup_packet_t), n > aLen ? aLen : n);
	return n;
	}

bool ControlOut(uint8_t aType, uint8_t aRequest, uint16_t aValue, uint16_t aIndex, const uint8_t* aData, uint16_t aLen)
	{
	usb_setup_packet_t* s = (usb_setup_packet_t*)g.ctrl->data_buffer;
	s->bmRequestType = aType;
	s->bRequest = aRequest;
	s->wValue = aValue;
	s->wIndex = aIndex;
	s->wLength = aLen;
	if (aLen && aData)
		memcpy(g.ctrl->data_buffer + sizeof(usb_setup_packet_t), aData, aLen);
	return Control(aLen, 2000) >= 0;
	}

// ----- the network interface (lwIP) -------------------------------------------------------------

void FreeRx(void* aHandle, void* aBuf)
	{
	(void)aHandle;
	free(aBuf);
	}

void OutDone(usb_transfer_t* aXfer)
	{
	for (int i = 0; i < 2; i++)
		if (g.out[i] == aXfer)
			g.outBusy[i] = false;
	}

// from lwIP: one Ethernet frame for the device
esp_err_t Transmit(void* aHandle, void* aBuf, size_t aLen)
	{
	(void)aHandle;
	if (!g.haveDevice || g.d.kind == KNone)
		return ESP_FAIL;
	int slot = -1;
	for (int i = 0; i < 2; i++)
		if (!g.outBusy[i]) { slot = i; break; }
	if (slot < 0)
		return ESP_ERR_NO_MEM;                // (dropped: TCP resends)
	usb_transfer_t* x = g.out[slot];
	size_t n = 0;
	switch (g.d.kind)
		{
	case KNcm:
		n = NcmBuild(x->data_buffer, x->data_buffer_size, (const uint8_t*)aBuf, aLen, g.d.ncmSeq++, g.d.ncm);
		break;
	case KEcm:
		if (aLen <= x->data_buffer_size) { memcpy(x->data_buffer, aBuf, aLen); n = aLen; }
		break;
	case KApple:
		n = IphethPack(x->data_buffer, x->data_buffer_size, (const uint8_t*)aBuf, aLen);
		break;
	default:
		break;
		}
	if (!n)
		return ESP_FAIL;
	x->num_bytes = (int)n;
	x->device_handle = g.d.dev;
	x->bEndpointAddress = g.d.epOut;
	x->callback = OutDone;
	x->flags = (n % g.d.epOutMax == 0) ? USB_TRANSFER_FLAG_ZERO_PACK : 0;
	g.outBusy[slot] = true;
	if (usb_host_transfer_submit(x) != ESP_OK)
		{
		g.outBusy[slot] = false;
		return ESP_FAIL;
		}
	return ESP_OK;
	}

void Frame(const uint8_t* aData, size_t aLen)
	{
	if (!g.netif || aLen < 14)
		return;
	uint8_t* copy = (uint8_t*)malloc(aLen);
	if (!copy)
		return;
	memcpy(copy, aData, aLen);
	if (esp_netif_receive(g.netif, copy, aLen, 0) != ESP_OK)
		free(copy);
	}

void InDone(usb_transfer_t* aXfer)
	{
	if (aXfer->status == USB_TRANSFER_STATUS_COMPLETED && aXfer->actual_num_bytes > 0)
		{
		const uint8_t* p = aXfer->data_buffer;
		size_t n = (size_t)aXfer->actual_num_bytes;
		switch (g.d.kind)
			{
		case KNcm:
			{
			NcmDatagram d[16];
			int c = NcmParse(p, n, d, 16);
			for (int i = 0; i < c; i++)
				Frame(d[i].data, d[i].len);
			break;
			}
		case KEcm:
			Frame(p, n);
			break;
		case KApple:
			{
			const uint8_t* f;
			size_t fl;
			if (IphethUnpack(p, n, f, fl))
				Frame(f, fl);
			break;
			}
		default:
			break;
			}
		}
	if (g.haveDevice && !g.deviceGone && aXfer->status != USB_TRANSFER_STATUS_NO_DEVICE
		&& aXfer->status != USB_TRANSFER_STATUS_CANCELED)
		{
		aXfer->num_bytes = (int)g.d.inSize;
		usb_host_transfer_submit(aXfer);     // again
		}
	}

void OnIp(void* aArg, esp_event_base_t aBase, int32_t aId, void* aData)
	{
	(void)aArg;
	if (aBase != IP_EVENT)
		return;
	if (aId == IP_EVENT_ETH_GOT_IP)
		{
		ip_event_got_ip_t* e = (ip_event_got_ip_t*)aData;
		if (e->esp_netif != g.netif)
			return;
		g.ip = e->ip_info.ip;
		char d[64];
		snprintf(d, sizeof(d), ", " IPSTR, IP2STR(&e->ip_info.ip));
		SetDetail(d);
		g.state = EUsbUp;
		Log("USB: up, address " IPSTR, IP2STR(&e->ip_info.ip));
		}
	else if (aId == IP_EVENT_ETH_LOST_IP)
		{
		if (g.state == EUsbUp)
			g.state = EUsbNoCarrier;
		}
	}

bool NetifStart()
	{
	if (!g.netif)
		{
		esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
		base.if_key = "USBNET";
		base.if_desc = "usb";
		base.route_prio = 120;                // above the WiFi station (100): USB first when both are up
		esp_netif_config_t cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.base = &base;
		cfg.driver = 0;
		cfg.stack = ESP_NETIF_NETSTACK_DEFAULT_ETH;
		g.netif = esp_netif_new(&cfg);
		if (!g.netif)
			return false;
		esp_netif_driver_ifconfig_t drv;
		memset(&drv, 0, sizeof(drv));
		drv.handle = &g;
		drv.transmit = Transmit;
		drv.driver_free_rx_buffer = FreeRx;
		esp_netif_set_driver_config(g.netif, &drv);
		esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, OnIp, 0);
		}
	esp_netif_set_mac(g.netif, g.d.mac);
	esp_netif_action_start(g.netif, 0, 0, 0);
	g.netifUp = true;
	return true;
	}

void NetifCarrier(bool aUp)
	{
	if (!g.netif || !g.netifUp)
		return;
	if (aUp && !g.carrier)
		{
		g.carrier = true;
		esp_netif_action_connected(g.netif, 0, 0, 0);   // (the DHCP client starts)
		Log("USB: carrier up");
		}
	else if (!aUp && g.carrier)
		{
		g.carrier = false;
		esp_netif_action_disconnected(g.netif, 0, 0, 0);
		g.state = EUsbNoCarrier;
		Log("USB: carrier down");
		}
	}

void NetifStop()
	{
	if (g.netif && g.netifUp)
		{
		if (g.carrier)
			esp_netif_action_disconnected(g.netif, 0, 0, 0);
		esp_netif_action_stop(g.netif, 0, 0, 0);
		}
	g.carrier = false;
	g.netifUp = false;
	}

// ----- finding the device's network interface --------------------------------------------------------

// the endpoints of an interface descriptor's alternate setting
void Endpoints(const usb_config_desc_t* aCfg, const usb_intf_desc_t* aIntf, int aOffset, TDevice& aD)
	{
	for (int e = 0; e < aIntf->bNumEndpoints; e++)
		{
		int off = aOffset;
		const usb_ep_desc_t* ep = usb_parse_endpoint_descriptor_by_index(aIntf, e, aCfg->wTotalLength, &off);
		if (!ep || (ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) != USB_BM_ATTRIBUTES_XFER_BULK)
			continue;
		if (ep->bEndpointAddress & 0x80)
			{
			aD.epIn = ep->bEndpointAddress;
			aD.epInMax = ep->wMaxPacketSize;
			}
		else
			{
			aD.epOut = ep->bEndpointAddress;
			aD.epOutMax = ep->wMaxPacketSize;
			}
		}
	}

// looks through one configuration; true if a network interface was found
bool Inspect(const usb_config_desc_t* aCfg, TDevice& aD)
	{
	aD.kind = KNone;
	aD.macStringIndex = 0;
	aD.epIn = aD.epOut = 0;
	int off = 0;
	const uint8_t* p = (const uint8_t*)aCfg;
	const uint8_t* end = p + aCfg->wTotalLength;
	uint8_t curIntf = 0xff;
	bool wantData = false;
	uint8_t dataIntf = 0xff;
	// walk every descriptor in the configuration
	for (const uint8_t* q = p + aCfg->bLength; q + 2 <= end && q[0] >= 2; q += q[0])
		{
		uint8_t type = q[1];
		if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && q[0] >= 9)
			{
			const usb_intf_desc_t* i = (const usb_intf_desc_t*)q;
			curIntf = i->bInterfaceNumber;
			if (aD.kind == KNone && i->bInterfaceClass == 2 && (i->bInterfaceSubClass == 0x0d || i->bInterfaceSubClass == 0x06))
				{
				aD.kind = i->bInterfaceSubClass == 0x0d ? KNcm : KEcm;
				aD.commIntf = curIntf;
				wantData = true;
				}
			else if (aD.kind == KNone && IphethMatch(aD.vid, i->bInterfaceClass, i->bInterfaceSubClass, i->bInterfaceProtocol))
				{
				aD.kind = KApple;
				aD.commIntf = aD.dataIntf = curIntf;
				aD.dataAlt = kIphethAltSetting;
				}
			// the data interface's alternate setting with the bulk endpoints
			if (aD.kind == KApple && curIntf == aD.dataIntf && i->bAlternateSetting == kIphethAltSetting && i->bNumEndpoints >= 2)
				{
				off = (int)(q - p);
				Endpoints(aCfg, i, off, aD);
				}
			if ((aD.kind == KNcm || aD.kind == KEcm) && wantData && i->bInterfaceClass == 0x0a
				&& (dataIntf == 0xff || dataIntf == curIntf))
				{
				dataIntf = curIntf;
				if (i->bNumEndpoints >= 2)
					{
					aD.dataIntf = curIntf;
					aD.dataAlt = i->bAlternateSetting;
					off = (int)(q - p);
					Endpoints(aCfg, i, off, aD);
					}
				}
			}
		else if (type == 0x24 && q[0] >= 13 && q[2] == 0x0f && (aD.kind == KNcm || aD.kind == KEcm) && curIntf == aD.commIntf)
			aD.macStringIndex = q[3];        // the Ethernet networking functional descriptor: iMACAddress
		else if (type == 0x24 && q[0] >= 5 && q[2] == 0x06 && (aD.kind == KNcm || aD.kind == KEcm) && curIntf == aD.commIntf)
			dataIntf = q[4];                 // the union functional descriptor: the data interface's number
		}
	if (aD.kind == KNone || !aD.epIn || !aD.epOut)
		{
		aD.kind = KNone;
		return false;
		}
	aD.config = aCfg->bConfigurationValue;
	return true;
	}

// a string descriptor as ASCII (the MAC as 12 hex digits)
bool StringDescriptor(uint8_t aIndex, char* aOut, size_t aMax)
	{
	uint8_t buf[64];
	int n = ControlIn(0x80, USB_B_REQUEST_GET_DESCRIPTOR, (uint16_t)((USB_B_DESCRIPTOR_TYPE_STRING << 8) | aIndex), 0x0409, buf, sizeof(buf));
	if (n < 4)
		return false;
	size_t o = 0;
	for (int i = 2; i + 1 < n && i < buf[0] && o + 1 < aMax; i += 2)
		aOut[o++] = (char)buf[i];
	aOut[o] = 0;
	return o > 0;
	}

int Hex(char aC)
	{
	return aC >= '0' && aC <= '9' ? aC - '0' : aC >= 'a' && aC <= 'f' ? aC - 'a' + 10 : aC >= 'A' && aC <= 'F' ? aC - 'A' + 10 : -1;
	}

bool DeviceSetup()
	{
	TDevice& d = g.d;
	// select the configuration with the interface (the library opened the
	// first one; the enumeration filter that could have chosen another is
	// not compiled into the Arduino core)
	usb_device_info_t info;
	if (usb_host_device_info(d.dev, &info) == ESP_OK && info.bConfigurationValue != d.config)
		{
		if (!ControlOut(0x00, USB_B_REQUEST_SET_CONFIGURATION, d.config, 0, 0, 0))
			{
			Log("USB: SET_CONFIGURATION %u failed", d.config);
			return false;
			}
		delay(50);
		}
	// claim, and set the alternate setting with the bulk endpoints
	if (d.kind != KApple)
		{
		if (usb_host_interface_claim(g.client, d.dev, d.commIntf, 0) != ESP_OK)
			{
			Log("USB: could not claim interface %u", d.commIntf);
			return false;
			}
		d.claimedComm = true;
		}
	if (usb_host_interface_claim(g.client, d.dev, d.dataIntf, d.dataAlt) != ESP_OK)
		{
		Log("USB: could not claim interface %u alt %u", d.dataIntf, d.dataAlt);
		return false;
		}
	d.claimedData = true;
	if (d.dataAlt)
		ControlOut(0x01, USB_B_REQUEST_SET_INTERFACE, d.dataAlt, d.dataIntf, 0, 0);
	// the MAC address
	memset(d.mac, 0, 6);
	bool haveMac = false;
	if (d.kind == KApple)
		{
		uint8_t mac[6];
		if (ControlIn(0xc0, kIphethCmdGetMac, 0, d.commIntf, mac, 6) == 6)
			{
			memcpy(d.mac, mac, 6);
			haveMac = true;
			}
		}
	else if (d.macStringIndex)
		{
		char s[20];
		if (StringDescriptor(d.macStringIndex, s, sizeof(s)) && strlen(s) >= 12)
			{
			haveMac = true;
			for (int i = 0; i < 6; i++)
				{
				int h = Hex(s[i * 2]), l = Hex(s[i * 2 + 1]);
				if (h < 0 || l < 0) { haveMac = false; break; }
				d.mac[i] = (uint8_t)((h << 4) | l);
				}
			}
		}
	if (!haveMac)
		{
		// a locally administered one of our own
		d.mac[0] = 0x02; d.mac[1] = 0x50; d.mac[2] = 0x51;
		d.mac[3] = (uint8_t)(d.vid >> 8); d.mac[4] = (uint8_t)d.vid; d.mac[5] = (uint8_t)d.pid;
		}
	// the class set-up
	d.inSize = kNcmMaxFrame + 16;
	if (d.kind == KNcm)
		{
		uint8_t params[32];
		int n = ControlIn(0xa1, kNcmGetNtbParameters, 0, d.commIntf, params, sizeof(params));
		if (n < 28 || !NcmParseParams(params, (size_t)n, d.ncm))
			{
			Log("USB: NCM: no NTB parameters");
			return false;
			}
		// the blocks it sends us: 4 KB at most (our buffers), 16-bit, one
		// datagram each is fine
		uint8_t size[4] = { 0x00, 0x10, 0, 0 };    // 4096
		ControlOut(0x21, kNcmSetNtbInputSize, 0, d.commIntf, size, 4);
		ControlOut(0x21, kNcmSetNtbFormat, 0, d.commIntf, 0, 0);
		ControlOut(0x21, kNcmSetEthernetPacketFilter, kNcmFilterAll, d.commIntf, 0, 0);
		d.inSize = 4096;
		d.ncmSeq = 0;
		}
	else if (d.kind == KEcm)
		{
		ControlOut(0x21, kNcmSetEthernetPacketFilter, kNcmFilterAll, d.commIntf, 0, 0);
		d.inSize = kNcmMaxFrame + 2;
		}
	else
		d.inSize = kIphethBufSize;
	// the IN transfers
	for (int i = 0; i < 2; i++)
		{
		if (!g.in[i] || g.in[i]->data_buffer_size < d.inSize)
			{
			if (g.in[i]) usb_host_transfer_free(g.in[i]);
			g.in[i] = 0;
			if (usb_host_transfer_alloc(d.inSize, 0, &g.in[i]) != ESP_OK)
				{
				Log("USB: no memory for the transfers");
				return false;
				}
			}
		g.in[i]->device_handle = d.dev;
		g.in[i]->bEndpointAddress = d.epIn;
		g.in[i]->num_bytes = (int)d.inSize;
		g.in[i]->callback = InDone;
		g.in[i]->context = 0;
		}
	for (int i = 0; i < 2; i++)
		{
		if (!g.out[i] && usb_host_transfer_alloc(2048, 0, &g.out[i]) != ESP_OK)
			{
			Log("USB: no memory for the transfers");
			return false;
			}
		g.outBusy[i] = false;
		}
	if (!NetifStart())
		{
		Log("USB: no network interface");
		return false;
		}
	for (int i = 0; i < 2; i++)
		usb_host_transfer_submit(g.in[i]);
	Log("USB: %s %04x:%04x on interface %u (%s), MAC %02x:%02x:%02x:%02x:%02x:%02x",
		d.kind == KNcm ? "NCM" : d.kind == KEcm ? "ECM" : "Apple USB Ethernet", d.vid, d.pid, d.dataIntf,
		d.kind == KApple ? "carrier needs pairing" : "carrier assumed", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
	char det[64];
	snprintf(det, sizeof(det), " (%s %04x:%04x)", d.kind == KNcm ? "NCM" : d.kind == KEcm ? "ECM" : "iPhone", d.vid, d.pid);
	SetDetail(det);
	g.state = EUsbNoCarrier;
	if (d.kind != KApple)
		NetifCarrier(true);
	g.lastCarrierMs = 0;
	return true;
	}

void DeviceClose()
	{
	NetifStop();
	if (g.d.dev)
		{
		for (int i = 0; i < 2; i++)
			if (g.in[i]) usb_host_endpoint_halt(g.d.dev, g.d.epIn), usb_host_endpoint_flush(g.d.dev, g.d.epIn);
		if (g.d.claimedData)
			usb_host_interface_release(g.client, g.d.dev, g.d.dataIntf);
		if (g.d.claimedComm)
			usb_host_interface_release(g.client, g.d.dev, g.d.commIntf);
		usb_host_device_close(g.client, g.d.dev);
		}
	memset(&g.d, 0, sizeof(g.d));
	g.haveDevice = false;
	g.deviceGone = false;
	g.state = EUsbNone;
	SetDetail("");
	}

void DeviceOpen(uint8_t aAddr)
	{
	memset(&g.d, 0, sizeof(g.d));
	g.d.addr = aAddr;
	g.state = EUsbEnumerating;
	SetDetail("");
	if (usb_host_device_open(g.client, aAddr, &g.d.dev) != ESP_OK)
		{
		Log("USB: could not open device %u", aAddr);
		g.state = EUsbNone;
		return;
		}
	g.haveDevice = true;
	const usb_device_desc_t* dd = 0;
	if (usb_host_get_device_descriptor(g.d.dev, &dd) != ESP_OK || !dd)
		{
		DeviceClose();
		return;
		}
	g.d.vid = dd->idVendor;
	g.d.pid = dd->idProduct;
	Log("USB: device %04x:%04x, %u configuration(s)", dd->idVendor, dd->idProduct, dd->bNumConfigurations);
	bool found = false;
	for (uint8_t c = 1; c <= dd->bNumConfigurations && !found; c++)
		{
		const usb_config_desc_t* cfg = 0;
		if (usb_host_get_config_desc(g.client, g.d.dev, c, &cfg) != ESP_OK || !cfg)
			continue;
		found = Inspect(cfg, g.d);
		usb_host_free_config_desc(cfg);
		}
	if (!found)
		{
		char det[64];
		snprintf(det, sizeof(det), " (%04x:%04x)", dd->idVendor, dd->idProduct);
		SetDetail(det);
		g.state = EUsbNoDriver;
		Log("USB: no network interface we know on %04x:%04x (NCM, ECM or Apple)", dd->idVendor, dd->idProduct);
		return;                               // (left open and idle until it goes)
		}
	if (!DeviceSetup())
		{
		g.state = EUsbNoDriver;
		SetDetail(" (set-up failed)");
		}
	}

void ClientEvent(const usb_host_client_event_msg_t* aMsg, void* aArg)
	{
	(void)aArg;
	if (aMsg->event == USB_HOST_CLIENT_EVENT_NEW_DEV)
		{
		if (!g.haveDevice)
			DeviceOpen(aMsg->new_dev.address);  // (safe here: the set-up pumps events itself)
		}
	else if (aMsg->event == USB_HOST_CLIENT_EVENT_DEV_GONE)
		{
		Log("USB: device gone");
		g.deviceGone = true;
		}
	}

void ClientTask(void*)
	{
	for (;;)
		{
		usb_host_client_handle_events(g.client, pdMS_TO_TICKS(100));
		if (g.deviceGone && g.haveDevice)
			DeviceClose();
		// Apple: the carrier, once a second (it comes up only for a paired host)
		if (g.haveDevice && g.d.kind == KApple && g.netifUp && millis() - g.lastCarrierMs > kIphethCarrierPollMs)
			{
			g.lastCarrierMs = millis();
			uint8_t c = 0;
			int n = ControlIn(0xc0, kIphethCmdCarrierCheck, 0, g.d.commIntf, &c, 1);
			NetifCarrier(n >= 1 && IphethCarrier(&c, 1));
			}
		}
	}

void DaemonTask(void*)
	{
	for (;;)
		{
		uint32_t flags = 0;
		usb_host_lib_handle_events(portMAX_DELAY, &flags);
		if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
			usb_host_device_free_all();
		}
	}

} // namespace

UsbNet::UsbNet() : iStarted(false) {}

bool UsbNet::Begin()
	{
	if (iStarted)
		return true;
	memset(&g, 0, sizeof(g));
	g.state = EUsbNone;
	usb_host_config_t hc;
	memset(&hc, 0, sizeof(hc));
	hc.skip_phy_setup = false;
	hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
	if (usb_host_install(&hc) != ESP_OK)
		{
		Log("USB: usb_host_install failed");
		return false;
		}
	if (usb_host_transfer_alloc(256, 0, &g.ctrl) != ESP_OK)
		return false;
	usb_host_client_config_t cc;
	memset(&cc, 0, sizeof(cc));
	cc.is_synchronous = false;
	cc.max_num_event_msg = 5;
	cc.async.client_event_callback = ClientEvent;
	cc.async.callback_arg = 0;
	if (usb_host_client_register(&cc, &g.client) != ESP_OK)
		{
		Log("USB: client registration failed");
		return false;
		}
	xTaskCreatePinnedToCore(DaemonTask, "usbd", 4096, 0, 2, 0, 0);
	xTaskCreatePinnedToCore(ClientTask, "usbc", 6144, 0, 3, 0, 0);
	iStarted = true;
	return true;
	}

int UsbNet::State(char* aDetail, size_t aMax)
	{
	if (aMax)
		{
		strncpy(aDetail, g.detail, aMax - 1);
		aDetail[aMax - 1] = 0;
		}
	return g.state;
	}

} // namespace am

#else // not AM_USB_HOST: the stub

namespace am {

UsbNet::UsbNet() : iStarted(false) {}
bool UsbNet::Begin() { return false; }
int UsbNet::State(char* aDetail, size_t aMax) { if (aMax) aDetail[0] = 0; return EUsbNone; }

} // namespace am

#endif
