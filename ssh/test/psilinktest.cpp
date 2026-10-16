// psilinktest.cpp - host test for the shared PsiLink.ini (ssh/psilink.h):
// every file version loads, "Stay connected" (v4) is standard for a new
// Psion but off for an older file (an upgrade keeps the idle hang-up), it
// survives a save, a v4 file still reads right in the parts an older
// program (v3) knows, no proxy address is built in, and DevBox finds
// PsiProxy's services (the proxy, or PsiDevBox.ini behind the Atom's). A few lines of EPOC stand-ins
// (descriptors, RFs, RFile over a map) let TPsiLink build on a PC.
//   in ssh/: make -f test/Makefile.host psilinktest && ./psilinktest
#include <stdio.h>
#include <string.h>
#include <map>
#include <string>

// ---- EPOC stand-ins: only what TPsiLink uses ----------------------------------
typedef int TInt;
typedef int TBool;
typedef unsigned char TUint8;
enum { EFalse = 0, ETrue = 1 };
const TInt KErrNone = 0, KErrNotFound = -1;
enum { EFileRead = 0, EFileShareReadersOnly = 0, EFileWrite = 1 };

struct TDesC
	{
	const TUint8* iP;
	TInt iN;
	TInt Length() const { return iN; }
	TUint8 operator[](TInt i) const { return iP[i]; }
	TDesC Mid(TInt a, TInt l) const { TDesC r = { iP + a, l }; return r; }
	};
inline TDesC Lit(const char* s) { TDesC r = { (const TUint8*)s, (TInt)strlen(s) }; return r; }
#define _L(s) Lit(s)
#define _LIT(name, s) static const TDesC name = { (const TUint8*)s, sizeof(s) - 1 }

template <int N> struct TBuf
	{
	TUint8 iB[N];
	TInt iN;
	TBuf() : iN(0) {}
	TBuf(const TDesC& d) : iN(0) { Copy(d); }
	operator TDesC() const { TDesC r = { iB, iN }; return r; }
	TInt Length() const { return iN; }
	TInt MaxLength() const { return N; }
	void Zero() { iN = 0; }
	void Copy(const TDesC& d) { iN = d.iN < N ? d.iN : N; memcpy(iB, d.iP, iN); }
	template <int M> void Copy(const TBuf<M>& b) { Copy((TDesC)b); }
	void Append(TUint8 c) { if (iN < N) iB[iN++] = c; }
	void Append(char c) { Append((TUint8)c); }
	void Append(int c) { Append((TUint8)c); }
	void Append(const TDesC& d) { for (TInt i = 0; i < d.iN; i++) Append(d.iP[i]); }
	template <int M> void Append(const TBuf<M>& b) { Append((TDesC)b); }
	TUint8 operator[](TInt i) const { return iB[i]; }
	TDesC Mid(TInt a, TInt l) const { TDesC r = { iB + a, l }; return r; }
	bool operator==(const TBuf& o) const { return iN == o.iN && !memcmp(iB, o.iB, iN); }
	};
#define TBuf8 TBuf
typedef TBuf<256> TFileName;

static std::map<std::string, std::string> gDisk;     // the "C:" of the test
static std::string Key(const TDesC& d) { return std::string((const char*)d.iP, d.iN); }

struct RFs
	{
	TInt MkDirAll(const TDesC&) { return KErrNone; }
	TInt Replace(const TDesC& a, const TDesC& b) { gDisk[Key(b)] = gDisk[Key(a)]; gDisk.erase(Key(a)); return KErrNone; }
	TInt Delete(const TDesC& a) { gDisk.erase(Key(a)); return KErrNone; }
	};
struct RFile
	{
	std::string iName;
	std::string iData;
	TInt Open(RFs&, const TDesC& n, TInt) { iName = Key(n); if (!gDisk.count(iName)) return KErrNotFound; iData = gDisk[iName]; return KErrNone; }
	TInt Replace(RFs&, const TDesC& n, TInt) { iName = Key(n); iData.clear(); gDisk[iName] = ""; return KErrNone; }
	template <int N> TInt Read(TBuf<N>& d) { d.iN = (TInt)iData.size() < N ? (TInt)iData.size() : N; memcpy(d.iB, iData.data(), d.iN); return KErrNone; }
	template <int N> TInt Write(const TBuf<N>& d) { iData.append((const char*)d.iB, d.iN); return KErrNone; }
	TInt Flush() { gDisk[iName] = iData; return KErrNone; }
	void Close() {}
	};

#define PSILINK_STRUCT_ONLY
#include "../psilink.h"

// ---- the test --------------------------------------------------------------
static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char* KFile = "C:\\System\\Data\\PsiLink.ini";

static void Put(const std::string& aBytes) { gDisk.clear(); gDisk[KFile] = aBytes; }

static std::string V1() { return std::string("PL\x01\x04\x01\x01\x07" "ATDT777", 14); }
static std::string V2() { return V1().replace(2, 1, "\x02") + std::string("\x01\x04host\x50\x1f", 8); }
static std::string V3(int aTls) { std::string s = V2().replace(2, 1, "\x03"); s += (char)aTls; return s; }
static std::string V4(int aTls, int aKeep) { std::string s = V3(aTls).replace(2, 1, "\x04"); s += (char)aKeep; return s; }

// what an older program (v3: no iKeepUp) reads: the parts it knows
static void OldReader(const std::string& f, int& aNet, int& aProxyPort, int& aTls)
	{
	const unsigned char* d = (const unsigned char*)f.data();
	aNet = d[5];
	int pos = 7 + d[6];
	pos++;                                   // useproxy
	int len = d[pos++];
	pos += len;
	aProxyPort = d[pos] | (d[pos + 1] << 8);
	pos += 2;
	aTls = (int)f.size() > pos ? d[pos] : 0;
	}

int main()
	{
	RFs fs;
	TPsiLink l;

	// no file: Load fails; the defaults stay connected
	gDisk.clear();
	l.SetDefaults();
	CHECK(!l.Load(fs));
	CHECK(l.iKeepUp == 1);
	CHECK(TPsiLink().iKeepUp == 1);

	// no proxy address built in; PsiProxy's standard port
	CHECK(l.iProxyHost.Length() == 0 && l.iUseProxy == 0 && l.iProxyPort == 8081 && !l.ProxyUsable());

	// v1, v2, v3: everything they hold; an upgrade keeps the idle hang-up
	Put(V1());
	l.SetDefaults(); l.iKeepUp = 1;
	CHECK(l.Load(fs) && l.iLoadedVer == 1 && l.iNetMode == 1 && l.iBaudIndex == 4 && l.iKeepUp == 0);
	CHECK(l.iPppStart.Length() == 7 && l.iUseProxy == 0 && l.iProxyHost.Length() == 0 && !l.ProxyUsable());
	Put(V2());
	l.SetDefaults(); l.iKeepUp = 1;
	CHECK(l.Load(fs) && l.iLoadedVer == 2 && l.iUseProxy == 1 && l.iProxyPort == 8016 && l.iKeepUp == 0);
	Put(V3(1));
	l.SetDefaults(); l.iKeepUp = 1;
	CHECK(l.Load(fs) && l.iLoadedVer == 3 && l.iModemTls == 1 && l.iProxyPort == 8016 && l.iKeepUp == 0);

	// v4 both ways
	Put(V4(1, 0));
	l.SetDefaults();
	CHECK(l.Load(fs) && l.iLoadedVer == 4 && l.iModemTls == 1 && l.iKeepUp == 0);
	Put(V4(0, 1));
	l.SetDefaults();
	CHECK(l.Load(fs) && l.iModemTls == 0 && l.iKeepUp == 1);
	// a truncated v4 (the keep byte lost): as an older file
	Put(V3(0).replace(2, 1, "\x04"));
	l.SetDefaults(); l.iKeepUp = 1;
	CHECK(l.Load(fs) && l.iKeepUp == 0);
	// a later version with more after it: still read
	Put(V4(1, 0).replace(2, 1, "\x05") + "\x07\x07");
	l.SetDefaults();
	CHECK(l.Load(fs) && l.iKeepUp == 0 && l.iModemTls == 1 && l.iLoadedVer == 4);

	// save and load again: "Disconnect when idle" kept, the rest as it was
	Put(V3(1));
	l.SetDefaults();
	l.Load(fs);
	l.iKeepUp = 0;
	l.Save(fs);
	std::string saved = gDisk[KFile];
	CHECK(saved.size() == V4(1, 0).size() && saved[2] == 4 && saved[saved.size() - 1] == 0);
	CHECK(!gDisk.count(std::string(KFile) + "~"));    // the temporary file renamed over it
	TPsiLink m;
	m.SetDefaults();
	CHECK(m.Load(fs) && m.iKeepUp == 0 && m.iModemTls == 1 && m.iNetMode == 1 && m.iProxyPort == 8016 && m.iPppStart == l.iPppStart);
	m.iKeepUp = 1;
	m.Save(fs);
	m.SetDefaults(); m.iKeepUp = 0;
	CHECK(m.Load(fs) && m.iKeepUp == 1 && m.iLoadedVer == 4);

	// an older program reading a v4 file sees what it knows unchanged
	int net, port, tls;
	OldReader(saved, net, port, tls);
	CHECK(net == 1 && port == 8016 && tls == 1);

	// garbage is refused or read safely
	Put(std::string("PL\x04\x09\x01\x01\xff", 7));
	l.SetDefaults();
	CHECK(l.Load(fs) && l.iBaudIndex == 4 && l.iPppStart.Length() == 0 && l.iKeepUp == 0);
	Put("XX");
	CHECK(!l.Load(fs));

	// DevBox: where Send log goes. A real proxy: itself.
	TBuf<60> h;
	int dport = 0;
	gDisk.clear();
	l.SetDefaults();
	CHECK(!l.DevBox(fs, h, dport));                   // no proxy, no PsiDevBox.ini: nowhere
	l.iUseProxy = 1; l.iProxyHost.Copy(_L("proxy.example")); l.iProxyPort = 8016;
	CHECK(l.DevBox(fs, h, dport) && h == TBuf<60>(_L("proxy.example")) && dport == 8016);
	// the Atom's own proxy (either name, any case), no PsiDevBox.ini: nowhere
	l.iProxyHost.Copy(_L("192.168.7.1"));
	CHECK(!l.DevBox(fs, h, dport));
	l.iProxyHost.Copy(_L("PsiProxy"));
	CHECK(!l.DevBox(fs, h, dport));
	CHECK(!TPsiLink::IsAtomProxy(TBuf<60>(_L("192.168.7.10"))) && !TPsiLink::IsAtomProxy(TBuf<60>(_L("psiproxy2"))));
	// ...with PsiDevBox.ini: that
	gDisk["C:\\System\\Data\\PsiDevBox.ini"] = "devbox.example\r\n8090\r\n";
	CHECK(l.DevBox(fs, h, dport) && h == TBuf<60>(_L("devbox.example")) && dport == 8090);
	// a real proxy still wins over the file
	l.iProxyHost.Copy(_L("10.0.0.5"));
	CHECK(l.DevBox(fs, h, dport) && h == TBuf<60>(_L("10.0.0.5")) && dport == 8016);
	// a damaged PsiDevBox.ini is ignored
	l.iProxyHost.Copy(_L("192.168.7.1"));
	const char* bad[] = { "", "\r\n8081\r\n", "host\r\n", "host\r\n0\r\n", "host\r\n70000\r\n",
		"ho st\r\n8081\r\n", "host\r\nabc\r\n" };
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		{
		gDisk["C:\\System\\Data\\PsiDevBox.ini"] = bad[i];
		CHECK(!l.DevBox(fs, h, dport));
		}
	gDisk["C:\\System\\Data\\PsiDevBox.ini"] = std::string(80, 'a') + "\r\n8081\r\n";   // too long
	CHECK(!l.DevBox(fs, h, dport));
	gDisk["C:\\System\\Data\\PsiDevBox.ini"] = "devbox\n8081";                       // bare \n, no last line end
	CHECK(l.DevBox(fs, h, dport) && h == TBuf<60>(_L("devbox")) && dport == 8081);

	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
	}
