#!/bin/sh
# backup.sh --- take a WAL-safe copy of the live database and bring it home.
#
# `cp sync.db` IS NOT A BACKUP. The database runs in WAL mode, so the most
# recent commits live in sync.db-wal until a checkpoint folds them in; a plain
# copy is therefore a database missing exactly the syncs that happened most
# recently, and one taken while a checkpoint is running can be torn outright.
# Both failures are silent: the copy opens, queries answer, and the data that
# is gone is the data you were trying to keep.
#
# So the copy is taken by the server binary itself, through sqlite's online
# backup API (`sync backup`), which reads through a real connection -- WAL
# included -- and restarts itself if a writer moves a page underneath it. The
# copy is verified before it is published, on the board and again here.
#
# Usage:  ./srv/deploy/backup.sh [local-directory]
set -eu

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/deploy/pancra.conf"

OUT=${1:-$HERE/build/backups}
SSH="$PANCRA_SSH"
. "$HERE/srv/deploy/lock.sh"
say() { printf 'backup: %s\n' "$*"; }
fail() { printf 'backup: FAILED: %s\n' "$*" >&2; exit 1; }
OP=$(op_id)

# THE VERIFIER IS REQUIRED, AND REQUIRED FIRST.
#
# This used to check for build/srv/sync only AFTER the transfer, and when it
# was missing it printed a note and exited ZERO -- announcing the destination
# path exactly as a verified backup does. Every caller then treats the file as
# a backup, and the day it is needed is the day anybody finds out it was never
# checked. That is worse than having no backup: an unverified copy occupies
# the place a good one would have.
#
# Checked BEFORE the board is asked to do any work, so a machine that cannot
# verify never produces an artifact to be confused about. restore.sh has
# required the same binary, in the same way, all along.
# FROM THE CONTRACT, not spelled out here. This was the last path in these
# scripts naming a local build directory itself: restore.sh's copy became
# PANCRA_VERIFY_BIN when item 50 landed, and a contract that covers one of two
# users is half a contract -- the half that drifts is the one nobody reads.
[ -x "$PANCRA_VERIFY_BIN" ] ||
   fail "$PANCRA_VERIFY_BIN is needed to verify the backup (run 'make srv')"

mkdir -p "$OUT"

# ONE OPERATION AT A TIME. A backup is not a read-only observation of the
# board: it runs a process against the live data directory, writes into
# backups/, and prunes that directory afterwards. Every one of those meets a
# restore moving sync.db and its -wal out from underneath it, or a deploy
# writing its own pre-deploy backup into the same directory in the same second.
# Taken here rather than at the top so a checkout with no verifier is refused
# without ever locking the board -- a refusal that costs somebody else an
# operation is a refusal that made things worse.
lock_take backup "$OP" || exit 1

stamp=$($SSH "$PANCRA_HOST" "date -u +%Y%m%dT%H%M%SZ") || fail "no ssh"
# THE STAMP IS ONLY GOOD TO THE SECOND, and deploy.sh builds its pre-deploy
# backup's name exactly the same way. Two of them in one second wrote one file
# -- and, underneath it, one `<dest>.part` for two sqlite backups to stage
# into. The date still leads the name, so the directory still reads and sorts
# by when the backup was taken.
remote="$PANCRA_BACKUPS/sync-$stamp-$OP.db"
local_name="sync-$stamp-$OP.db"

# THREE OUTCOMES FROM THE BOARD, not two (srv/sync.c): 0 published and its
# directory entry synced, 2 published and readable but the directory could not
# be synced, anything else nothing was published. `|| rc=$?` because this runs
# under `set -e`, which would otherwise kill the script on the status that most
# needs reading.
rc=0
$SSH "$PANCRA_HOST" "mkdir -p '$PANCRA_BACKUPS' && '$PANCRA_BIN' backup '$remote' '$PANCRA_DATA'" ||
   rc=$?
UNSYNCED=0
case $rc in
   0) ;;
   2) UNSYNCED=1 ;;
   *) fail "the board could not take a backup" ;;
esac

$PANCRA_SCP "$PANCRA_HOST:$remote" "$OUT/$local_name" || fail "copy home failed"

# VERIFIED HERE TOO. The board checked the copy it made; this checks the copy
# that arrived, because a truncated transfer is a file that exists and is
# wrong -- and a backup is only ever read on the day it has to work.
#
# A COPY THAT DOES NOT VERIFY IS MOVED ASIDE, not left where it landed. Left
# in place it sits in the backup directory under a name that sorts by date
# like every good one, so the next restore -- or the next person reading
# `ls` -- picks it as the most recent backup. The .unverified suffix keeps the
# evidence for diagnosis while making it unusable by accident.
if ! "$PANCRA_VERIFY_BIN" verify "$OUT/$local_name"; then
   mv -f "$OUT/$local_name" "$OUT/$local_name.unverified" 2>/dev/null ||
      rm -f "$OUT/$local_name"
   fail "the backup that arrived does not verify (kept as $local_name.unverified)"
fi

# Keep the last 14 on the board; the local copies are the user's to prune.
$SSH "$PANCRA_HOST" "ls -t '$PANCRA_BACKUPS'/sync-*.db 2>/dev/null | tail -n +15 | xargs -r rm -f" || true

# ---- AND WHETHER THE BOARD'S COPY WILL SURVIVE A POWER CUT ----------------
#
# A THIRD ANSWER, not a warning tacked onto success. The copy in $OUT is here,
# complete and verified, and that is the artifact this command exists to
# produce -- so this is emphatically not a failure and must not read as one to
# whatever runs it on a schedule. But the copy ON THE BOARD, which is the one
# the retention above keeps fourteen of and the one a restore reaches for when
# this machine is not the machine in the room, was renamed into a directory
# nobody could sync. Its bytes are on the disk under a name that may not be.
#
# Reported as its own exit status for the same reason srv/sync.c has one: "it
# worked" would have somebody trust fourteen copies of which the newest might
# not exist after a reboot, and "it failed" would have them re-run a backup
# that is sitting in front of them, verified.
if [ "$UNSYNCED" = 1 ]; then
   printf 'backup: %s\n' "$OUT/$local_name"
   echo "backup: DURABILITY UNCERTAIN ON THE BOARD. The copy above is here," >&2
   echo "  complete and verified -- that part is done. But $PANCRA_HOST could" >&2
   echo "  not sync $PANCRA_BACKUPS after publishing $remote, so a power loss" >&2
   echo "  there can erase that directory entry and the board's newest backup" >&2
   echo "  with it. The local copy is unaffected. Check the directory's" >&2
   echo "  permissions and the board's filesystem before relying on the" >&2
   echo "  fourteen kept there." >&2
   exit 2
fi

say "$OUT/$local_name"
