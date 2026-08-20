#!/bin/sh
# restore.sh --- put a backup back on the board, deliberately.
#
# SEPARATE FROM ROLLBACK, and separate from deploy, because it is the only
# operation here that DESTROYS data: everything synced since the backup was
# taken is gone when this finishes. Nothing calls it automatically.
#
# What it does, in order: verify the backup locally, stop the server, move the
# live database aside (never delete it -- the "restore" that turns out to have
# been the wrong file is a real Tuesday), install the backup, start, and check
# it comes up. The displaced database stays on the board under a timestamp.
#
# AND IT PUTS THE DISPLACED DATABASE BACK WHEN THE RESTORE DOES NOT SERVE.
# That was the hole this script had longest, and it is worth stating as the
# operator saw it: the working set had been moved aside to
# backups/displaced-<stamp>.db*, the backup was installed, the start or the
# health wait failed -- and the script stopped there. So the board was left
# with the service DOWN, the restored file (the one that had just proved it
# would not serve) installed as the live database, and the complete working set
# it had displaced sitting one directory away, needing three renames nobody had
# written down at an hour when nobody wants to be inventing them. Every
# ingredient of the recovery was present and the recovery was manual.
#
# THREE DIRECTIONS THAT WENT WRONG WHEN IT WAS DONE BY HAND, all of which this
# now does in one remote shell:
#
#   1. THE PROCESS WAS STILL RUNNING. "The service did not come up healthy"
#      does not mean "nothing is running": a database that loads and then
#      answers no request at all gives a live pid and a readiness line. Moving
#      files under a live server and starting a second one leaves two processes
#      on one data directory and a pid file naming one of them.
#   2. THE RESTORED FILES WERE DELETED to make room. They are the evidence --
#      the operator restored that backup for a reason and now needs to know why
#      it would not serve. They are quarantined under backups/refused-<stamp>,
#      not removed.
#   3. THE -wal AND -shm WERE FORGOTTEN. Putting back sync.db alone, while the
#      refused start's own sync.db-wal is still lying in the data directory, is
#      a database plus a DIFFERENT database's write-ahead log: sqlite replays
#      it into the file at the next open. That is not a failed restore any
#      more, it is a corrupted one. The set moves as a set, both ways.
#
# ...AND WHETHER THE RECOVERY ITSELF WORKED IS ITS OWN ANSWER. Three outcomes,
# never blurred (rotate.sh reports the same three about a certificate):
#
#   * the restore worked;
#   * the restore failed AND THE PREVIOUS DATABASE IS BACK and serving -- an
#     incident that is over, and the operator has a backup to look at;
#   * the restore failed AND THE ROLLBACK FAILED, so THE BOARD IS DOWN. That
#     one has to be unmistakable: it is the only one that needs a human now.
#
# Usage:  ./srv/deploy/restore.sh <backup.db>
set -eu

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/deploy/pancra.conf"

SSH="$PANCRA_SSH"
. "$HERE/srv/deploy/health.sh"
. "$HERE/srv/deploy/start.sh"
. "$HERE/srv/deploy/lock.sh"
say() { printf 'restore: %s\n' "$*"; }
fail() { printf 'restore: FAILED: %s\n' "$*" >&2; exit 1; }
OP=$(op_id)

SRC=${1:-}
[ -n "$SRC" ] || fail "usage: restore.sh <backup.db>"
[ -f "$SRC" ] || fail "no such backup: $SRC"

# THE VERIFIER RUNS HERE, so it is the NATIVE build and not PANCRA_LOCAL_BIN:
# that one is the artifact for the board (riscv64), which this machine cannot
# execute. The path comes from the contract like every other one.
VERIFY=$PANCRA_VERIFY_BIN
case "$VERIFY" in
   /*) ;;
   *) VERIFY=$HERE/$VERIFY ;;
esac
[ -x "$VERIFY" ] ||
   fail "$VERIFY is needed to verify the backup first (make srv)"
"$VERIFY" verify "$SRC" || fail "$SRC does not verify; NOT restoring it"

printf 'restore: this REPLACES the live database on %s. Everything synced\n' "$PANCRA_HOST"
printf '         since %s was taken will be lost. Type YES to continue: ' "$SRC"
read -r answer
[ "$answer" = "YES" ] || fail "cancelled"

# ---- ONE OPERATION AT A TIME, AND NOT BEFORE THE HUMAN HAS ANSWERED -------
#
# TAKEN AFTER THE PROMPT, deliberately. Everything above it is local -- the
# backup is verified here, and the question is asked here -- and a lock held
# across a `read` is a lock held for as long as somebody takes to find their
# terminal. That is not a theoretical wait: this prompt exists precisely
# because a restore is the one operation that destroys data, so people go and
# check something before typing YES. A board that refused every deploy for the
# duration of somebody's second thoughts would be a lock making an incident
# worse, which is the failure mode a lock is supposed to prevent.
#
# From here down it is the board's turn, and every step of it is shared: the
# staged copy, the pid file, the process, the database and both its side files.
# NO require_front PRECONDITION STILL (see the top of this file): the lock is
# about not colliding, not about being sure -- a recovery must still run.
lock_take restore "$OP" || exit 1

stamp=$($SSH "$PANCRA_HOST" "date -u +%Y%m%dT%H%M%SZ")
# THE TWO HALVES OF ONE ATTEMPT, named with the same stamp so they can be read
# as a pair months later: what was taken out of the way, and -- if it comes to
# that -- what was put back out of the way in its place.
#
# AND WITH THIS OPERATION'S ID, because the stamp is only good to the second.
# Two restores that started within the same second -- one operator retrying
# while the first is still running, which is exactly what a stuck restore
# invites -- wrote the same displaced-<stamp>.db, so the second overwrote the
# working set the first had put there and the first's rollback then reinstalled
# the second's files, or a mixture of the two. `displaced-<stamp>-` still sorts
# by date and still pairs with its refused- twin.
DISPLACED=$PANCRA_BACKUPS/displaced-$stamp-$OP.db
REFUSED=$PANCRA_BACKUPS/refused-$stamp-$OP.db
# THE STAGED COPY, likewise. Two restores staged at one `sync.db.restoring`,
# and whichever renamed second installed the OTHER operator's backup while
# telling its own operator that the file they named was now live. Under the
# lock they cannot overlap at all; this is what keeps the damage impossible on
# the day somebody breaks the lock (PANCRA_LOCK_BREAK=1) about a holder that
# was not in fact gone.
STAGED=$PANCRA_DB.restoring-$OP

$PANCRA_SCP "$SRC" "$PANCRA_HOST:$STAGED" || fail "copy failed"

# BEFORE the restart, so the readiness line waited for is this start's.
tag=$(start_tag)
mark=$(log_mark)

$SSH "$PANCRA_HOST" "sh -s" <<EOF || fail "the board refused the restore"
set -eu
'$PANCRA_BIN' verify '$STAGED'
$(stop_block)
# ASIDE, NOT DELETED -- with its log and index, which belong to it and would
# otherwise be replayed into the restored file.
mkdir -p '$PANCRA_BACKUPS'
for f in '' '-wal' '-shm'; do
   [ -f "$PANCRA_DB\$f" ] && mv "$PANCRA_DB\$f" '$DISPLACED'"\$f" || true
done
mv '$STAGED' '$PANCRA_DB'
$(start_block "$tag")
EOF

# RESTORED MEANS SERVING THE RESTORED DATA, not merely a process that started.
#
# This was its own inline loop asking for a live pid and a readiness line
# anywhere in the last 50 lines of an append-only log -- which the PREVIOUS
# start's line satisfies, so a server that came up on the restored file and
# died immediately still reported "restored". And nothing checked the public
# URL, so a database that loads but a service nobody can reach reported the
# same. Both questions are health.sh's now, the same ones deploy asks.
if wait_healthy_since "$mark" "$tag"; then
   say "restored and answering $PANCRA_URL; the displaced database is"
   say "  $DISPLACED"
   exit 0
fi
# WHY IT FAILED DECIDES WHAT TO SAY -- see the same note in rollback.sh. An
# undeclared front door is a missing line in pancra.conf, and reporting it as
# an unreachable service after a restore is how a good restore gets undone.
#
# CHECKED BEFORE THE ROLLBACK BELOW, deliberately and in this order: the
# restored database is installed, the server is listening on it and the only
# thing missing is a line in a configuration file. Rolling that back would
# throw away a restore that WORKED, on the strength of a fact about this
# repository rather than about the board -- and the operator would be told the
# backup could not be restored when it had been.
if ! front_declared && health_since "$mark"; then
   echo "restore: the restored database is in place and the server is" >&2
   echo "  listening on $PANCRA_PORT, but it was NOT verified from" >&2
   echo "  outside: the front door is undeclared (above), so nothing here" >&2
   echo "  could check $PANCRA_URL. The displaced database is" >&2
   echo "  $DISPLACED. NOTHING WAS ROLLED BACK: the restore is in place" >&2
   echo "  and this is a missing line in pancra.conf, not an outage." >&2
   exit 1
fi

# ---- THE RESTORE DID NOT SERVE: PUT THE PREVIOUS DATABASE BACK ------------
say "the restored database is NOT serving; putting the previous one back"

# IS THERE ANYTHING TO GO BACK TO? Restoring onto a board that had no database
# -- a rebuilt board, new hardware, the first thing a restore is ever used for
# -- displaces nothing, so there is no working set to reinstall and the three
# renames below would quarantine the only copy the board has and leave the data
# directory EMPTY. Say that instead of doing it.
#
# ASKED FOR A WORD RATHER THAN AN EXIT STATUS, and the message covers both ways
# of not getting it. `! $SSH ... "[ -f x ]"` cannot tell "the board says no"
# from "the board did not answer", and every ssh failure would then have been
# reported as the confident and wrong "this board had no database before the
# restore" -- about a board holding a complete working set.
# ssh's own stderr is NOT swallowed here: if the reason there is no answer is
# "Connection refused", that sentence is the most useful line in the report.
if [ "$($SSH "$PANCRA_HOST" "[ -f '$DISPLACED' ] && echo yes" || true)" \
     != yes ]; then
   echo "restore: FAILED: the restored database does not serve, and there is" >&2
   echo "  NO previous database to put back: $DISPLACED is not there --" >&2
   echo "  either this board had no database before the restore (a rebuilt" >&2
   echo "  board, new hardware), or it can no longer be asked. THE BOARD IS" >&2
   echo "  DOWN and nothing was moved: the restored file is still installed" >&2
   echo "  as $PANCRA_DB. See $PANCRA_LOG for why it would not serve." >&2
   exit 1
fi

# THE MARK COMES FIRST, before the restart writes the line it is meant to
# witness. Taken afterwards it sits PAST this start's own banner and readiness
# line, and then the undeclared-front-door branch below -- which asks
# health_since about that offset -- reads a board that had just come back up
# perfectly as one that never started, and reports it DOWN. That exact bug was
# found and fixed once in rotate.sh; it is the same two lines here.
backtag=$(start_tag)
back=$(log_mark)

# ONE REMOTE SHELL, with the service down for the whole swap: stop, quarantine
# the complete restored set, reinstall the complete displaced set, start.
#
# `set -eu` is what makes the reinstall all-or-nothing, and the file that makes
# that non-negotiable is the -wal. A `mv` that fails -- a full filesystem, a
# permission change, a file somebody is holding -- aborts the block, no start
# happens, and the caller reports the rollback as failed with the pieces named.
# A loop that carried on would start the service on a database whose
# write-ahead log was left behind in backups/: a perfectly valid database
# missing exactly the transactions that arrived most recently, which opens,
# queries, and is discovered months later as "some of my history is gone".
# That is the same loss `cp sync.db` produces and restoredrill.sh measures.
# The other order is worse still: sync.db from one database beside
# sync.db-wal from another, which sqlite does not reject, it MERGES.
if ! $SSH "$PANCRA_HOST" "sh -s" <<EOF
set -eu
$(stop_block)
mkdir -p '$PANCRA_BACKUPS'
# QUARANTINED, NOT DELETED, and for two independent reasons. It is the evidence
# -- the operator chose that backup and now has to find out why the server
# would not serve it, which cannot be done from a file that was overwritten.
# And its -wal/-shm MUST leave the data directory: the refused start created
# them, they describe the refused file, and leaving one beside the database
# reinstalled below is the corruption case above, not a leftover.
#
# WHICH IS WHY THE TWO ARE NOT TREATED THE SAME WHEN THE MOVE FAILS. The .db is
# best-effort: it is a copy of a backup the operator still has in their hand
# (they named its path to run this), so a full or unwritable backups directory
# must not stop a recovery for the sake of a second copy -- the reinstall below
# will overwrite it. The -wal and -shm are NOT best-effort: they belong to a
# database that is about to be moved away, and a quarantine that quietly failed
# would leave one beside the database put back in its place, which sqlite
# merges. If they cannot be kept they are removed. Nothing of value is in them
# -- that start never answered a request -- and merging them is unrecoverable.
[ -f "$PANCRA_DB" ] && mv "$PANCRA_DB" '$REFUSED' || true
for f in '-wal' '-shm'; do
   [ -f "$PANCRA_DB\$f" ] || continue
   mv "$PANCRA_DB\$f" '$REFUSED'"\$f" || rm -f "$PANCRA_DB\$f"
done
# THE COMPLETE SET, in one direction only. Each file that was displaced comes
# back; anything that was not displaced is not invented.
for f in '' '-wal' '-shm'; do
   [ -f '$DISPLACED'"\$f" ] || continue
   mv '$DISPLACED'"\$f" "$PANCRA_DB\$f"
done
[ -f '$PANCRA_DB' ] || {
   echo "the displaced database did not come back to $PANCRA_DB" >&2
   exit 1
}
$(start_block "$backtag")
EOF
then
   echo "restore: FAILED: the restored database does not serve AND THE" >&2
   echo "  ROLLBACK ITSELF FAILED on the board: THE BOARD IS DOWN." >&2
   echo "  Every step in that block that can fail comes BEFORE the start, so" >&2
   echo "  the service is stopped and one of the renames did not happen." >&2
   echo "  Both sets are present and named, and PART of the working set may" >&2
   echo "  already have moved back:" >&2
   echo "    from before the restore: $DISPLACED* and/or $PANCRA_DB*" >&2
   echo "    the backup that would not serve: $REFUSED*" >&2
   echo "  Put ONE complete set at $PANCRA_DB -- a .db with ITS OWN -wal and" >&2
   echo "  -shm, never a mixture of two -- and start the service." >&2
   exit 1
fi

# THE SAME HEALTH QUESTION, not a weaker one. The whole reason this script no
# longer trusts a live pid is that the two paths taken when something has
# already gone wrong were the two that never asked whether the service could be
# reached -- and this is now the most "already gone wrong" path of them all.
if wait_healthy_since "$back" "$backtag"; then
   echo "restore: FAILED: $SRC is a database this server will not serve." >&2
   echo "  THE ROLLBACK SUCCEEDED: the database from before the restore is" >&2
   echo "  back at $PANCRA_DB with its -wal and -shm, the service is running" >&2
   echo "  on it and $PANCRA_URL answers. Nothing was lost and the board is" >&2
   echo "  NOT down." >&2
   echo "  The backup that would not serve is kept for you at $REFUSED*" >&2
   echo "  -- see $PANCRA_LOG for what the server said about it." >&2
   exit 1
fi
# ...and the same asymmetry as above: an undeclared front door is not an
# outage, and saying "the board is DOWN" about a board that is up and listening
# sends an incident operator to fix the wrong thing. This is the branch the
# mark above has to be taken before the restart for.
if ! front_declared && health_since "$back"; then
   echo "restore: FAILED: $SRC is a database this server will not serve." >&2
   echo "  THE ROLLBACK SUCCEEDED as far as anything here can tell: the" >&2
   echo "  database from before the restore is back at $PANCRA_DB with its" >&2
   echo "  -wal and -shm and the server is listening on $PANCRA_PORT again." >&2
   echo "  It was NOT verified from outside: the front door is undeclared" >&2
   echo "  (above), so nothing here could check $PANCRA_URL. That is a" >&2
   echo "  missing line in pancra.conf -- the board is NOT known to be down." >&2
   echo "  The backup that would not serve is kept at $REFUSED*" >&2
   exit 1
fi
echo "restore: FAILED: $SRC does not serve AND THE PREVIOUS DATABASE DID NOT" >&2
echo "  COME BACK HEALTHY EITHER: THE BOARD IS DOWN." >&2
echo "  The files were moved: $PANCRA_DB is the database from before the" >&2
echo "  restore (with its -wal and -shm) and the backup that would not serve" >&2
echo "  is at $REFUSED*. It is the SERVICE that is not answering on it, so" >&2
echo "  $PANCRA_LOG is where the reason is -- this is no longer about which" >&2
echo "  database is installed." >&2
exit 1
