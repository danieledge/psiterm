// shots.h - more pictures for render_screens: an extension module's own
// test build (src/ext.h) adds a file with a static ShotRegister, whose
// function lays out the module's pages (StatusModel::LayExt) and saves each
// with aSave; they are drawn after the modem's own, the same way.
// MIT licence (see LICENSE at the top of the repository).
#ifndef ATOM_SHOTS_H
#define ATOM_SHOTS_H

#include "../src/status.h"

typedef void (*ShotSave)(const char* aName, const am::Layout& aLayout);
struct ShotRegister
	{
	explicit ShotRegister(void (*aRun)(ShotSave aSave));
	};

#endif
