// psiholdtest.cpp - host test for PsiHoldFind (ssh/psilink.h): psihold.exe
// is found by reading \System\Apps\ on each disk, not from a list of
// program names. A card laid out as the Psion's (several program folders,
// 8.3 capitals as tools/emu/mkcard.ts writes them, some without psihold.exe,
// a file among the folders, disks missing or not ready) is held in a map;
// the stand-ins answer DriveList, GetDir and Entry from it, case-blind as
// FAT is.
//   in ssh/: make -f test/Makefile.host psiholdtest && ./psiholdtest
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <map>
#include <set>
#include <string>
#include <vector>

// ---- EPOC stand-ins: only what TPsiLink uses ----------------------------------
typedef int TInt;
typedef int TBool;
typedef unsigned char TUint8;
typedef unsigned TUint;
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
	void Append(unsigned c) { Append((TUint8)c); }
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

struct CDir;
struct RFs
	{
	TInt DriveList(TBuf<26>& aList);
	TInt GetDir(const TDesC& aWild, unsigned aAtt, int aSort, CDir*& aDir);
	TInt Entry(const TDesC& aName, struct TEntry& aEntry);
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


// ---- the directory side: a disk is the set of its files' full names ---------
typedef TBuf<26> TDriveList;
const TInt KMaxDrives = 26, KErrNotReady = -18, KErrPathNotFound = -12;
const unsigned KEntryAttDir = 0x10, KEntryAttMatchExclusive = 0x40000000;
enum { ESortNone = 0 };
static std::set<std::string> gFiles;          // "C:\SYSTEM\APPS\PSITERM\PSIHOLD.EXE"...
static std::string gDrives = "CDZ";           // present disks
static std::string gNotReady;                 // present but no medium (D: with no card)
static int gDirReads = 0;                     // GetDir calls, to show it stays cheap
static std::string Up(std::string s) { for (size_t i = 0; i < s.size(); i++) s[i] = toupper((unsigned char)s[i]); return s; }

struct TEntry
	{
	TFileName iName;
	unsigned iAtt;
	TBool IsDir() const { return (iAtt & KEntryAttDir) != 0; }
	};
struct CDir
	{
	std::vector<TEntry> iE;
	TInt Count() const { return (TInt)iE.size(); }
	const TEntry& operator[](TInt i) const { return iE[i]; }
	};
TInt RFs::DriveList(TDriveList& aList)
	{
	aList.iN = 26;
	for (int i = 0; i < 26; i++)
		aList.iB[i] = gDrives.find((char)('A' + i)) != std::string::npos;
	return KErrNone;
	}
TInt RFs::GetDir(const TDesC& aWild, unsigned aAtt, int, CDir*& aDir)
	{
	gDirReads++;
	std::string w = Up(Key(aWild));
	if (w.size() < 2 || w[w.size() - 1] != '*' || aAtt != (KEntryAttDir | KEntryAttMatchExclusive))
		return -6;                               // only what PsiHoldFind asks
	if (gNotReady.find(w[0]) != std::string::npos)
		return KErrNotReady;
	std::string dir = w.substr(0, w.size() - 1);  // "C:\SYSTEM\APPS\"
	std::set<std::string> subs, files;
	bool any = false;
	for (std::set<std::string>::iterator it = gFiles.begin(); it != gFiles.end(); ++it)
		if (it->compare(0, dir.size(), dir) == 0)
			{
			any = true;
			std::string rest = it->substr(dir.size());
			size_t s = rest.find('\\');
			if (s == std::string::npos) files.insert(rest); else subs.insert(rest.substr(0, s));
			}
	if (!any)
		return KErrPathNotFound;
	CDir* d = new CDir;
	for (std::set<std::string>::iterator it = subs.begin(); it != subs.end(); ++it)
		{
		TEntry e; e.iName.Copy(Lit(it->c_str())); e.iAtt = KEntryAttDir; d->iE.push_back(e);
		}
	(void)files;                                 // (KEntryAttMatchExclusive: folders only)
	aDir = d;
	return KErrNone;
	}
TInt RFs::Entry(const TDesC& aName, TEntry& aEntry)
	{
	std::string n = Up(Key(aName));
	if (gNotReady.find(n[0]) != std::string::npos)
		return KErrNotReady;
	if (!gFiles.count(n))
		return KErrNotFound;
	aEntry.iAtt = 0;
	return KErrNone;
	}

#define PSILINK_STRUCT_ONLY
#define PSILINK_HOLDFIND
#include "../psilink.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string Found(RFs& fs, bool& aOk)
	{
	TFileName exe;
	aOk = PsiHoldFind(fs, exe) != 0;
	return aOk ? Key(exe) : std::string();
	}

int main()
	{
	RFs fs;
	bool ok;
	std::string f;

	// no program installed: not found
	gFiles.clear();
	f = Found(fs, ok);
	CHECK(!ok);

	// several program folders on C:, only one with psihold.exe (not one the
	// code could know by name), a loose file in \System\Apps\ and a folder
	// whose name with psihold.exe would not fit a TFileName
	gFiles.clear();
	gFiles.insert("C:\\SYSTEM\\APPS\\CALC\\CALC.APP");
	gFiles.insert("C:\\SYSTEM\\APPS\\JOTTER\\JOTTER.APP");
	gFiles.insert("C:\\SYSTEM\\APPS\\README.TXT");
	gFiles.insert("C:\\SYSTEM\\APPS\\" + std::string(245, 'L') + "\\PSIHOLD.EXE");
	gFiles.insert("C:\\SYSTEM\\APPS\\ZZPROG\\ZZPROG.APP");
	gFiles.insert("C:\\SYSTEM\\APPS\\ZZPROG\\PSIHOLD.EXE");
	gDirReads = 0;
	f = Found(fs, ok);
	CHECK(ok && Up(f) == "C:\\SYSTEM\\APPS\\ZZPROG\\PSIHOLD.EXE");
	CHECK(gDirReads == 1);                       // one read of C:'s System\Apps folder

	// none on C:, one on D: (the card): found there; Z: is never read
	gFiles.clear();
	gFiles.insert("C:\\SYSTEM\\APPS\\CALC\\CALC.APP");
	gFiles.insert("D:\\SYSTEM\\APPS\\PSIMAIL\\PSIMAIL.APP");
	gFiles.insert("D:\\SYSTEM\\APPS\\PSIMAIL\\PSIHOLD.EXE");
	gFiles.insert("Z:\\SYSTEM\\APPS\\ROMPROG\\PSIHOLD.EXE");
	gDirReads = 0;
	f = Found(fs, ok);
	CHECK(ok && Up(f) == "D:\\SYSTEM\\APPS\\PSIMAIL\\PSIHOLD.EXE");
	CHECK(gDirReads == 2);

	// C: first when both have one
	gFiles.insert("C:\\SYSTEM\\APPS\\PSITERM\\PSIHOLD.EXE");
	f = Found(fs, ok);
	CHECK(ok && Up(f) == "C:\\SYSTEM\\APPS\\PSITERM\\PSIHOLD.EXE");

	// only in the ROM: not used
	gFiles.clear();
	gFiles.insert("Z:\\SYSTEM\\APPS\\ROMPROG\\PSIHOLD.EXE");
	f = Found(fs, ok);
	CHECK(!ok);

	// D: present but no card in it, and a disk that is not there at all
	gFiles.clear();
	gFiles.insert("D:\\SYSTEM\\APPS\\PSIWEB\\PSIHOLD.EXE");
	gNotReady = "D";
	f = Found(fs, ok);
	CHECK(!ok);
	gNotReady.clear();
	gDrives = "CZ";
	f = Found(fs, ok);
	CHECK(!ok);
	gDrives = "CDZ";
	f = Found(fs, ok);
	CHECK(ok && Up(f) == "D:\\SYSTEM\\APPS\\PSIWEB\\PSIHOLD.EXE");

	// many program folders, psihold.exe in the last one: still one read
	gFiles.clear();
	for (int i = 0; i < 60; i++)
		{
		char b[64];
		snprintf(b, sizeof b, "C:\\SYSTEM\\APPS\\PROG%02d\\PROG%02d.APP", i, i);
		gFiles.insert(b);
		}
	gFiles.insert("C:\\SYSTEM\\APPS\\PROG59\\PSIHOLD.EXE");
	gDirReads = 0;
	f = Found(fs, ok);
	CHECK(ok && Up(f) == "C:\\SYSTEM\\APPS\\PROG59\\PSIHOLD.EXE" && gDirReads == 1);

	printf("psiholdtest: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
	}
