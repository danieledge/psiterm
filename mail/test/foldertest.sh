#!/bin/bash
# foldertest.sh [psimail-host] - folder commands against fakeimap.py:
# create (top level and under Work, a non-ASCII name sent as modified
# UTF-7), rename (the local files follow), delete (refused for the Inbox, a
# standard folder and a folder with children; offline).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
H=${1:-$HERE/../../build/mail-host/psimail-host}
PORT=1179
STORE=$(mktemp -d /tmp/pmft.XXXXXX)
python3 "$HERE/fakeimap.py" $PORT > "$STORE/server.log" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null; rm -rf "$STORE"' EXIT
sleep 0.5
export PM_HOST=127.0.0.1 PM_PORT=$PORT PM_TLS=0 PM_USER=x PM_PASS=x
fail=0
run() { "$H" -s "$STORE" "$@" 2>/dev/null | grep -v '^\['; }
check() { # check "expected" "got"
	if [ "$2" != "$1" ]; then echo "FAIL: wanted '$1', got '$2'"; fail=1; else echo "ok: $2"; fi
}
check "folders: OK " "$(run folders)"
check "mkfolder: OK Folder \"Caf$(printf '\xe9') 2026\" created" "$(run mkfolder - "Caf$(printf '\xe9') 2026")"
grep -q 'CREATE "Caf&AOk- 2026"' "$STORE/server.log" && echo "ok: sent as modified UTF-7" || { echo "FAIL: not modified UTF-7"; fail=1; }
grep -q '^-.*Caf&AOk- 2026	Caf. 2026' "$STORE/A0/folders.txt" && echo "ok: in folders.txt" || { echo "FAIL: folders.txt"; fail=1; }
check "mkfolder: OK Folder \"Notes\" created" "$(run mkfolder Work Notes)"
grep -q 'CREATE "Work/Notes"' "$STORE/server.log" && echo "ok: under Work with the delimiter" || { echo "FAIL: child name"; fail=1; }
check "mkfolder: FAILED Mailbox already exists" "$(run mkfolder Work Notes)"
check "mkfolder: FAILED No folder name entered" "$(run mkfolder - "")"
check "sync: OK No new messages" "$(run sync "Work/Notes")"
[ -d "$STORE"/A0/F* ] && echo "ok: the folder has files" || { echo "FAIL: no store dir"; fail=1; }
before=$(ls -d "$STORE"/A0/F*)
check "renfolder: OK Folder renamed \"Old notes\"" "$(run renfolder "Work/Notes" "Old notes")"
after=$(ls -d "$STORE"/A0/F*)
[ "$before" != "$after" ] && [ -f "$after/index.txt" ] && echo "ok: the files followed the rename" || { echo "FAIL: store not moved ($before -> $after)"; fail=1; }
grep -q '^-.*Work/Old notes' "$STORE/A0/folders.txt" && echo "ok: renamed in folders.txt" || { echo "FAIL: folders.txt after rename"; fail=1; }
check "renfolder: FAILED The Inbox can't be renamed" "$(run renfolder INBOX Mail)"
check "renfolder: FAILED A standard folder can't be renamed" "$(run renfolder Trash Bin)"
check "delfolder: FAILED The Inbox can't be deleted" "$(run delfolder INBOX)"
check "delfolder: FAILED A standard folder can't be deleted" "$(run delfolder Trash)"
check "delfolder: FAILED Mailbox has children" "$(run delfolder Work)"
check "delfolder: OFFLINE Working offline" "$(PM_OFFLINE=1 run delfolder "Work/Old notes")"
check "delfolder: OK Folder deleted" "$(run delfolder "Work/Old notes")"
[ -z "$(ls -d "$STORE"/A0/F* 2>/dev/null)" ] && echo "ok: its files went too" || { echo "FAIL: store dir left"; fail=1; }
grep -q 'Old notes' "$STORE/A0/folders.txt" && { echo "FAIL: still in folders.txt"; fail=1; } || echo "ok: gone from folders.txt"
check "delfolder: FAILED No such folder" "$(run delfolder Nowhere)"
[ $fail = 0 ] && echo "ALL OK" || echo "FAILED"
exit $fail
