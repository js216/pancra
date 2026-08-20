#!/bin/sh
# rollback.sh --- put a previously deployed executable back, and start it.
#
# The releases directory is filled by deploy.sh, which copies the running
# binary aside under its own content hash BEFORE it replaces it. So a rollback
# needs no build, no cross-compiler and no network: everything it installs is
# already on the board.
#
# THE DATABASE IS NOT TOUCHED. A rollback is "run the previous code", not
# "return to the previous state" -- the data written since is real data, and
# restoring a database over it would lose exactly the syncs that happened
# while the bad build was up. Restoring is a separate, deliberate act:
# ./srv/deploy/restore.sh.
#
# Usage:  ./srv/deploy/rollback.sh [<hash>]      (default: the newest release)
set -eu

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/deploy/pancra.conf"

SSH="$PANCRA_SSH"
. "$HERE/srv/deploy/health.sh"
. "$HERE/srv/deploy/start.sh"
. "$HERE/srv/deploy/lock.sh"
say() { printf 'rollback: %s\n' "$*"; }
fail() { printf 'rollback: FAILED: %s\n' "$*" >&2; exit 1; }

OP=$(op_id)
# BEFORE THE RELEASE IS EVEN CHOSEN, because with no argument the choice is a
# READ of the board (`ls -t releases | head -1`) and a deploy running beside
# this one is writing that directory. Rolling back to "the newest release"
# while another operation adds one is how a rollback installs the build it was
# started to escape.
#
# RE-ENTRANT WHEN DEPLOY CALLS IT: deploy.sh runs this script itself when a new
# build does not come up healthy, holding the lock the whole time. lock.sh
# recognises its own token and lets the child through -- a recovery that
# deadlocked on its parent would be the worst possible way to learn about
# locking. See the note there.
lock_take rollback "$OP" || exit 1

want=${1:-}
if [ -z "$want" ]; then
   want=$($SSH "$PANCRA_HOST" "ls -t '$PANCRA_RELEASES' 2>/dev/null | head -1" || true)
   want=${want#sync-}
fi
[ -n "$want" ] || fail "no release to roll back to in $PANCRA_RELEASES"
say "restoring $want"

# BEFORE the restart: the readiness line waited for below has to be one this
# start writes, not one already in the log. See health.sh.
tag=$(start_tag)
mark=$(log_mark)

$SSH "$PANCRA_HOST" "sh -s" <<EOF || fail "the board refused the rollback"
set -eu
src='$PANCRA_RELEASES/sync-$want'
[ -f "\$src" ] || { echo "no such release: \$src" >&2; exit 1; }
got=\$(sha256sum "\$src" | awk '{print \$1}')
[ "\$got" = '$want' ] || { echo "\$src hashes \$got, not $want" >&2; exit 1; }
cp -p "\$src" '$PANCRA_BIN.new-$OP'
chmod +x '$PANCRA_BIN.new-$OP'
$(stop_block)
mv '$PANCRA_BIN.new-$OP' '$PANCRA_BIN'
$(start_block "$tag")
EOF

# RUNNING IS NOT SERVING, and a rollback is exactly when that distinction
# matters: this path is taken because something is already wrong.
#
# This used to be its own inline loop asking two weaker questions than
# deploy.sh asked -- a live pid, and a readiness line anywhere in the last 50
# lines of an append-only log, which the PREVIOUS start's line satisfies. So a
# rollback could report "running again" with the process up, the port
# answering nothing, and the evidence coming from the run before it.
#
# Same operation as deploy now, and the same public probe (health.sh).
if wait_healthy_since "$mark" "$tag" "$want"; then
   say "$want is running again and answering $PANCRA_URL"
   exit 0
fi
# WHY IT FAILED DECIDES WHAT TO SAY. wait_healthy_since refuses an undeclared
# front door as well as an unreachable service, and this script does NOT check
# that up front the way deploy.sh does -- a recovery that will not recover
# because a variable was never filled in is worse than the missing variable.
# So it runs, and then reports accurately: a missing line in pancra.conf is not
# an outage, and telling an incident operator "the board is DOWN" about a board
# that is up and listening sends them to fix the wrong thing.
if ! front_declared && health_since "$mark"; then
   echo "rollback: $want is installed and listening on $PANCRA_PORT," >&2
   echo "  but it was NOT verified from outside: the front door is" >&2
   echo "  undeclared (above), so nothing here could check $PANCRA_URL." >&2
   echo "  That is a missing line in pancra.conf, not an outage -- the" >&2
   echo "  board is NOT known to be down. Declare it and run duosmoke." >&2
   exit 1
fi
fail "$want was installed but the service is not reachable; the board is DOWN"
