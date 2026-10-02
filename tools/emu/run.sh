#!/bin/bash
# tools/emu/run.sh NAME [EVENT...]  - runs the emulator on build/emu/card.img
#
# Boots a Series 5mx, opens Extras and the first program on the card, plays
# the events, and saves a screenshot every second to build/emu/NAME/ plus
# build/emu/NAME/end.png (the last frame that isn't blank).
#   EVENT  "T KEY [HOLD]"  press an EStdKey code at T seconds (simulated)
#          "T tap X Y"     tap the screen (X is about the LCD x + 45)
# Keys: Enter 3, Esc 4, Menu 148, arrows 14-17 (left right up down),
# LeftCtrl 22, letters as ASCII capitals. PsiMail's window is up at ~78 s,
# PsiTerm's at ~45 s: send keys after that.
#   EMU_SECS=N   extra seconds after the last event (default 5)
#   EMU_ROM=...  ROM file (default: the 5mx 1.05(260) English ROM)
#   PSION_EMU    the psionEmulators checkout (default ../psionEmulators)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
EMU=${PSION_EMU:-$(cd "$REPO/.." && pwd)/psionEmulators}
ROM=${EMU_ROM:-"$EMU/roms/Series5mx/5mx_v1.05(260)_eng/5mx_v1.05(260)_eng.bin"}
CARD=${EMU_CARD:-$REPO/build/emu/card.img}
N=${1:?usage: run.sh NAME [EVENT...]}; shift
OUT=$REPO/build/emu/$N
rm -rf "$OUT"; mkdir -p "$OUT"
A=(--tap-seq 37 670 262 --tap-seq 39 335 215)    # Extras icon, then the first program
last=41
for e in "$@"; do
	set -- $e; t=${1%.*}; [ "$t" -gt "$last" ] && last=$t
	if [ "$2" = tap ]; then A+=(--tap-seq "$1" "$3" "$4"); else A+=(--press-key "$1" "$2" "${3:-8}"); fi
done
POST=$((last - 35 + ${EMU_SECS:-5}))
timeout 900 "$EMU/harness/run" "$ROM" --device 5mx --boot-seconds 35 --card-path "$CARD" \
	--post-attach-seconds "$POST" --quiet-logs --screenshot "$OUT/end.pgm" \
	"${A[@]}" --screenshot-every 1 "$OUT/s" > "$OUT/log" 2>&1 || true
grep -iE "panic|KERN-|WSERV|USER [0-9]" "$OUT/log" | head -3 || true
python3 - "$OUT" <<'PY'
import sys, glob
from PIL import Image
out = sys.argv[1]
for f in sorted(glob.glob(out + '/*.pgm')):
    Image.open(f).convert('L').save(f[:-4] + '.png')
frames = sorted(glob.glob(out + '/s-*.png'))
for f in reversed(frames):        # the last frame that isn't blank grey
    im = Image.open(f).convert('L')
    if len(set(im.crop((0, 0, 560, 240)).getdata())) > 2:
        im.save(out + '/end.png'); break
PY
echo "$OUT/end.png"
