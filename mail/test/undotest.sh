#!/bin/bash
# undotest.sh [psimail-host] - Edit > Undo (engine/undo.c) against fakeimap.py:
# a delete (to the Trash) and a move undone on a server with MOVE and
# UIDPLUS (COPYUID gives the uids) and on one without (COPY + EXPUNGE, found
# again by Message-ID); a move made offline and undone before it reached the
# server (its pending.txt line goes); a move the server made, undone offline
# (an UNMOVE line, sent at the next check); the downloaded text coming back;
# the message not taken for new mail afterwards; and the refusals.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
H=${1:-$HERE/../../build/mail-host/psimail-host}
PORT=1181
fail=0
check() { # check "expected" "got"
	if [ "$2" != "$1" ]; then echo "FAIL: wanted '$1', got '$2'"; fail=1; else echo "ok: $2"; fi
}
yes() { # yes "what" command...
	if "${@:2}"; then echo "ok: $1"; else echo "FAIL: $1"; fail=1; fi
}
export PM_HOST=127.0.0.1 PM_PORT=$PORT PM_TLS=0 PM_USER=x PM_PASS=x
run() { "$H" -s "$STORE" "$@" 2>/dev/null | grep -v '^\['; }
row() { awk -F'\t' -v u="$1" '$1==u{print $6}' "$IDX"; }

for mode in uidplus plain; do
	echo "== server: $mode"
	STORE=$(mktemp -d /tmp/pmundo.XXXXXX)
	python3 "$HERE/fakeimap.py" $PORT $([ $mode = plain ] && echo --no-uidplus) > "$STORE/server.log" 2>&1 &
	SRV=$!
	sleep 0.5
	check "folders: OK " "$(run folders)"
	check "sync: OK 5 new" "$(run sync INBOX)"
	IDX=$(grep -l "modem log" "$STORE"/A0/F*/index.txt)
	D=$(dirname "$IDX")
	run body INBOX 102 > /dev/null
	yes "102 downloaded" test -f "$D/102.txt"

	# a delete (to the Trash), undone
	check "move: OK " "$(run move INBOX 102 '')"
	yes "102 gone from the Inbox's files" test -z "$(row 102)"
	yes "its text kept for undo" test -f "$STORE/A0/undo/1.txt"
	got=$(run undo INBOX 102)
	check "undo: OK Undone" "$(echo "$got" | head -1)"
	check "  back as uid 106" "$(echo "$got" | sed -n 2p)"
	check "modem log" "$(row 106)"
	yes "its text came back with it" test -f "$D/106.txt"
	yes "the undo list is empty" test ! -s "$STORE/A0/undo.txt"
	if [ $mode = uidplus ]; then
		grep -q "UID MOVE 1 \"INBOX\"" "$STORE/server.log" && echo "ok: moved back with UID MOVE" || { echo "FAIL: no UID MOVE back"; fail=1; }
	else
		grep -q "SEARCH HEADER Message-ID \"<102@example.com>\"" "$STORE/server.log" && echo "ok: found by Message-ID" || { echo "FAIL: no Message-ID search"; fail=1; }
		grep -q "UID COPY 1 \"INBOX\"" "$STORE/server.log" && echo "ok: copied back (no MOVE)" || { echo "FAIL: no UID COPY back"; fail=1; }
	fi
	check "sync: OK No new messages" "$(run sync INBOX)"
	check "modem log" "$(row 106)"
	check "sync: OK No new messages" "$(run sync Trash)"

	# a move to a folder (as Archive does), undone
	check "move: OK " "$(run move INBOX 101 Work)"
	got=$(run undo INBOX 101)
	check "undo: OK Undone" "$(echo "$got" | head -1)"
	check "hello" "$(row "$(echo "$got" | sed -n 2p | awk '{print $4}')")"

	# offline: the move waits in pending.txt; undone before it went
	check "move: OFFLINE Moved here; the server will be told next time" "$(PM_OFFLINE=1 run move INBOX 104 Work)"
	yes "queued" grep -q "^MOVE	INBOX	104	Work" "$STORE/A0/pending.txt"
	got=$(PM_OFFLINE=1 run undo INBOX 104)
	check "undo: OK Undone" "$(echo "$got" | head -1)"
	check "  back as uid 104" "$(echo "$got" | sed -n 2p)"
	yes "the pending move went" test -z "$(grep MOVE "$STORE/A0/pending.txt" 2>/dev/null)"
	check "another" "$(row 104)"
	check "sync: OK No new messages" "$(run sync INBOX)"
	check "another" "$(row 104)"

	# moved on the server, undone offline: back here at once, the server next time
	check "move: OK " "$(run move INBOX 105 '')"
	got=$(PM_OFFLINE=1 run undo INBOX 105)
	check "undo: OFFLINE Put back here; the server will be told next time" "$(echo "$got" | head -1)"
	check "last" "$(row 105)"
	yes "an UNMOVE waits" grep -q "^UNMOVE	Trash	" "$STORE/A0/pending.txt"
	check "sync: OK No new messages" "$(run sync INBOX)"
	yes "sent: pending.txt is empty" test ! -s "$STORE/A0/pending.txt"
	yes "the row has the server's new uid" test -z "$(row 105)"
	check "1" "$(grep -c "	last	" "$IDX")"

	# refusals
	check "undo: FAILED Nothing to undo" "$(run undo INBOX 999)"
	check "move: OK " "$(run move INBOX 103 '')"
	run sync Trash > /dev/null
	tuid=$(awk -F'\t' '$6=="big one"{print $1}' "$(grep -l "big one" "$STORE"/A0/F*/index.txt | grep -v "$IDX")")
	check "move: OK " "$(run move Trash "$tuid" '')"
	check "undo: FAILED Not undone - the message is no longer in Trash" "$(run undo INBOX 103)"

	kill $SRV 2>/dev/null
	wait $SRV 2>/dev/null
	rm -rf "$STORE"
done
[ $fail = 0 ] && echo "ALL OK" || echo "FAILED"
exit $fail
