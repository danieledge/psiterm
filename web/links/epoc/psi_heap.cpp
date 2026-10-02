// psi_heap.cpp - memory figures for Links on the Psion (psi_drv.c calls
// these when a page starts and when it has finished loading). The emulator
// harness and the PC build have their own (emu/links_rt.c, psi_mem.c).
//
// Each page's figures go to PsiWeb.log as
//   mem <address>: heap 1234 KB in 567 cells, heap size 2345 KB, free RAM 6789 KB
// (the heap seldom gives memory back, so its size is close to the peak)
// and heap_used is set for the app.

#include <e32std.h>
#include <e32hal.h>

extern "C" {
#include "psiweb.h"
extern void* pwb_shared();
extern void pw_log(const char*);
}

extern "C" void psi_mem_page_start()
	{
	}

extern "C" void psi_mem_report(const char* aWhat)
	{
	TInt cells = 0, used = 0;
	cells = User::AllocSize(used);
	TInt size = User::Heap().Size();       // what the heap has taken from the system
	TMemoryInfoV1Buf mem;
	UserHal::MemoryInfo(mem);
	PwShared* s = (PwShared*)pwb_shared();
	if (s)
		{
		s->heap_used = used;
		s->free_ram = mem().iFreeRamInBytes;
		}
	TBuf8<200> line;
	line.Append(_L8("mem "));
	TInt n = 0;
	while (aWhat && aWhat[n] && n < 80) n++;
	line.Append((const TUint8*)(aWhat ? aWhat : ""), n);
	line.AppendFormat(_L8(": heap %d KB in %d cells, heap size %d KB, free RAM %d KB"),
		used / 1024, cells, size / 1024, mem().iFreeRamInBytes / 1024);
	char b[201];
	Mem::Copy(b, line.Ptr(), line.Length());
	b[line.Length()] = 0;
	pw_log(b);
	}
