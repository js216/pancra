#!/bin/bash
# restoredrill.sh --- prove the backup procedure by USING it.
#
# A backup nobody has ever restored is a hope. This is the drill that turns it
# into a fact, and it runs in `make check` on the host: a throwaway server, a
# few synced rows, a backup taken WHILE it is running, then a restore into a
# fresh directory and a second server that has to serve the same data.
#
# THE FAILURE IT EXISTS TO CATCH is not that the copy is missing -- it is that
# the copy is PRESENT AND SHORT. The database runs in WAL mode, so the recent
# commits sit in sync.db-wal until a checkpoint folds them in; `cp sync.db` is
# therefore a valid database missing exactly the rows that arrived most
# recently, and every symptom of that appears at restore time, months later,
# as "some of my history is gone". The drill takes both copies and compares
# them, so the difference is measured rather than assumed.
#
# Run by `make restoredrill`; it needs nothing but the native build.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/test/testlib.sh"

DIR=$(mktemp -d)
T_TMP=$DIR
SRVPID=
SRV2PID=
cleanup() {
  for p in $SRVPID $SRV2PID; do
    kill "$p" 2>/dev/null || true
    wait "$p" 2>/dev/null || true
  done
  rm -rf "$DIR"
}
trap cleanup EXIT INT TERM

need python3 "the restore drill" || exit 1
need curl "the restore drill" || exit 1

SYNCBIN=${1:-$HERE/build/srv/sync}
CLIBIN=$(dirname "$SYNCBIN")/synccli
[ -x "$SYNCBIN" ] || { echo "restoredrill: $SYNCBIN is not built"; exit 1; }
cp "$SYNCBIN" "$DIR/sync"
cp "$CLIBIN" "$DIR/synccli"
mkdir -p "$DIR/live" "$DIR/restored"
cd "$DIR" || exit 1

# TWO PORTS, TWO SERVERS, AND THEY MUST NOT BE THE SAME PORT. These were two
# pick_port calls in a row, and pick_port used to close its probe socket before
# returning -- so the kernel was free to answer the second call with the number
# it had just given the first, and the restored server would have died on the
# port the live one was about to take. They are chosen by `serve` now, one at a
# time, each verified to belong to the process that got it.
DB=$DIR/live/sync.db
export SYNCCLI_CODE_FILE=$T_TMP/.clicode

echo "== a running server with data in it =="
printf 'correcthorse\n' | ./sync adduser jk@example.com stdin live >/dev/null 2>&1
live_up() { # live_up <port>
  ./sync "$1" live >/dev/null 2>&1 &
  T_PID=$!
}
if ! serve "the live server" http / live_up; then
  echo "restoredrill: the server did not start"; exit 1
fi
PORT=$T_PORT
SRVPID=$T_PID
U="http://127.0.0.1:$PORT"
ck_owns "the live server owns its own port" "$SRVPID" "$PORT"

# Pair a phone and push buckets through the real protocol, so what is backed
# up is what a real install holds -- rows the SERVER wrote, not rows a test
# inserted behind its back.
curl -s -c jar.txt -o /dev/null -X POST \
     -d 'email=jk@example.com&password=correcthorse' $U/login
SET=$(curl -s -b jar.txt $U/settings)
CSRF=$(printf '%s' "$SET" | grep -o 'name=csrf value="[0-9a-f]*"' | head -1 |
       sed 's/.*value="//;s/"//')
SET=$(curl -s -b jar.txt -X POST -d "csrf=$CSRF" $U/settings/pair)
CODE=$(printf '%s' "$SET" | grep -o 'class=code>[0-9]\{6\}' | sed 's/.*>//')
PAIR=$(./synccli 127.0.0.1 $PORT pair jk@example.com "$CODE" 2>&1)
APPUID=$(printf '%s' "$PAIR" | awk '{print $1}')
KEY=$(printf '%s' "$PAIR" | awk '{print $2}')
ck "the drill's phone is paired" "^[0-9a-f]\{32\}$" "$KEY"

# Enough buckets that the write-ahead log certainly holds the tail of them.
DAYS="20000 20001 20002 20003 20004 20005 20006 20007"
for d in $DAYS; do
  : > b.txt
  i=0
  while [ $i -lt 40 ]; do
    printf '17280%05d,1%02d,0,-70,3,7,17280%05d,-420,0\n' \
      "$((d % 1000 * 40 + i))" "$((i % 99))" "$((d % 1000 * 40 + i))" >> b.txt
    i=$((i + 1))
  done
  ./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" PUT /v1/bucket/readings/$d b.txt >/dev/null 2>&1
done
LIVE_ROWS=$(dbq "$DB" "SELECT count(*) FROM logrow" | head -1)
ck "the live database holds every pushed row" "^320$" "$LIVE_ROWS"

echo "== the two ways to copy a live database =="
# THE WRONG ONE, kept as a measurement rather than a warning in a comment.
cp "$DB" naive.db
# THE RIGHT ONE, through the server's own backup command, with the server
# still running and still being written to.
BK=$(./sync backup "$DIR/backup.db" live 2>&1)
ck "the backup command reports what it copied" "backed up" "$BK"
ck "...and the backup verifies before anyone relies on it" "verifies" \
   "$(./sync verify "$DIR/backup.db" 2>&1)"

BK_ROWS=$(dbq "$DIR/backup.db" "SELECT count(*) FROM logrow" | head -1)
NAIVE_ROWS=$(dbq naive.db "SELECT count(*) FROM logrow" 2>/dev/null | head -1)
ck "the backup holds EVERY row the live database holds" "^$LIVE_ROWS$" "$BK_ROWS"
# Not asserted as a failure -- a checkpoint may have landed just before the
# copy, and then `cp` is accidentally complete. Printed, because the number is
# the argument: this is how much a plain copy loses when it loses anything.
printf '  note plain cp captured %s of %s rows\n' "${NAIVE_ROWS:-0}" "$LIVE_ROWS"

echo "== a backup nobody has restored is a hope =="
# The live server keeps running throughout, exactly as it would while somebody
# checks yesterday's backup on their laptop.
cp "$DIR/backup.db" restored/sync.db
ck "the restored file verifies as this server's database" "verifies" \
   "$(./sync verify restored/sync.db 2>&1)"
restored_up() { # restored_up <port>
  ./sync "$1" restored >/dev/null 2>&1 &
  T_PID=$!
}
if ! serve "the restored server" http / restored_up; then
  t_bad "the restored database did not start a server"
else
  PORT2=$T_PORT
  SRV2PID=$T_PID
  U2="http://127.0.0.1:$PORT2"
  t_ok "a server starts on the restored database"
  # ...and it is OUR restored server answering, not the live one still running
  # a few lines up. Two servers in one script is exactly where a repeated port
  # would have gone unnoticed: both answer, both hold this phone's key, and the
  # digest below would have been the LIVE database's.
  ck_owns "...on a port of its own, not the live server's" "$SRV2PID" "$PORT2"
  # THE POINT OF THE WHOLE PROCEDURE: the phone that was paired against the
  # original can still sync against the restore, and its history is there.
  DIG=$(./synccli 127.0.0.1 $PORT2 req "$APPUID" "$KEY" GET /v1/digest 2>&1)
  ck "the restored server answers the SAME phone's signed request" \
     "^readings $LIVE_ROWS " "$DIG"
  BODY=$(./synccli 127.0.0.1 $PORT2 req "$APPUID" "$KEY" GET /v1/bucket/readings/20000 2>&1)
  ck "...and hands back a bucket of readings" "^17280[0-9]*,1[0-9]*," "$BODY"
  curl -s --max-time 10 -o /dev/null -c jar2.txt -X POST \
       -d 'email=jk@example.com&password=correcthorse' $U2/login
  ck "the account and its password came back too" "sid" "$(cat jar2.txt)"
fi

echo "== what a corrupt or foreign file does =="
# The drill is only worth anything if a BAD backup fails it. Each of these is
# a file somebody could hand the restore procedure by mistake.
head -c 4096 /dev/urandom > junk.db
./sync verify junk.db >/dev/null 2>&1
ck "random bytes are not accepted as a backup" "^1$" "$?"
python3 - <<'PY'
import sqlite3
c = sqlite3.connect("empty.db")
c.execute("CREATE TABLE something(x)")
c.commit()
c.close()
PY
./sync verify empty.db >/dev/null 2>&1
ck "a valid database that is not this server's is refused" "^1$" "$?"
# A backup TRUNCATED in transit: the shape sqlite calls corrupt.
cp "$DIR/backup.db" cut.db
python3 - <<'PY'
import os
n = os.path.getsize("cut.db")
os.truncate("cut.db", n // 2)
PY
./sync verify cut.db >/dev/null 2>&1
ck "a truncated backup is refused" "^1$" "$?"

# ...and a failed backup must never replace a good one. The destination here
# is a directory that does not exist, which is the ordinary way a scheduled
# backup fails: someone moved the disk.
cp "$DIR/backup.db" keep.db
./sync backup "$DIR/nosuchdir/x.db" live >/dev/null 2>&1
ck "a backup that cannot be written fails loudly" "^1$" "$?"
ck "...and the previous backup is untouched" "verifies" \
   "$(./sync verify keep.db 2>&1)"

echo "== a backup whose directory entry was never synced is its OWN answer =="
# WHAT THIS IS ABOUT, in the shape it takes at 3am. The backup is taken through
# sqlite so it sees the write-ahead log, it is integrity- and schema-checked
# while it is still called .part, and it is renamed into place. Then the command
# printed "backed up ... -> ..." and exited 0. The file's CONTENTS were on the
# disk; the directory entry naming them was not. Power goes at 02:00:05, and the
# backup the operator was told they had at 02:00:04 is not in the directory on
# the next boot. Nothing warned anybody, because nothing knew.
#
# ARRANGED WITHOUT ANY FAULT INJECTION, which matters -- a lever the production
# build carries only for a test is a lever, and this failure has a real shape.
# A directory that is write+execute and not readable (0300) accepts the rename
# perfectly (creating an entry needs write and search, not read) and refuses
# open(O_RDONLY), which is what the fsync needs. That is a backups directory
# whose mode somebody tightened, and it is measured here rather than described.
mkdir -p tight
./sync backup "$DIR/tight/ok.db" live >/dev/null 2>&1
ck "a backup into an ordinary directory reports plain success" "^0$" "$?"
chmod 0300 tight
UNS=$(./sync backup "$DIR/tight/u.db" live 2>&1)
UNSRC=$?
chmod 0700 tight
# THE THREE-WAY DISTINCTION IS THE WHOLE POINT, so it is asserted as three
# separate facts and not as "non-zero". 0 would be the old lie ("it worked"),
# and 1 would be the opposite lie -- deploy.sh stops on 1 with "refusing to
# deploy over unsaved data", and backup.sh moves the arrived copy aside as
# unverified, both about a backup that is present, complete and verified.
ck "...and one whose directory could not be synced does NOT report success" \
   "^[^0]" "$UNSRC"
ck "...nor reports the plain failure that means nothing was published" \
   "^2$" "$UNSRC"
ck "...it says DURABILITY UNCERTAIN, in words an operator can act on" \
   "DURABILITY UNCERTAIN" "$UNS"
# ...AND IT REALLY WAS PUBLISHED. This is the half that makes it a third
# outcome rather than a failure: the rename happened, so the destination names
# the new backup and the previous one at that name is already gone. A caller
# told "failed" here would go looking for a backup that is sitting in front of
# it, and one told "worked" would trust a name that may not survive a reboot.
if [ -f "$DIR/tight/u.db" ]; then
  t_ok "...and the backup IS there: the rename is past the point of no return"
else
  t_bad "the durability-uncertain report was made about a file that is missing"
fi
ck "...and it is a complete, verifiable database, not a torn one" "verifies" \
   "$(./sync verify "$DIR/tight/u.db" 2>&1)"
# The stray .part must still be gone: a half-made backup left beside a good one
# is another database's file under a name that sorts next to it.
if [ -e "$DIR/tight/u.db.part" ]; then
  t_bad "the staging file was left behind beside the published backup"
else
  t_ok "...and nothing was left staged beside it"
fi

echo "== a backup destination that IS the live database is refused =="
# WHAT THIS PREVENTS, in the order it happens. `sync backup` stages at
# <dest>.part and RENAMES it onto <dest>. Aim <dest> at the running server's
# own database and the rename unlinks the inode the server holds open: every
# later request still succeeds, writing to a file with no name and a -wal
# belonging to nothing, and the next restart opens the backup instead. Every
# sync taken between the backup and the restart is gone, and nothing anywhere
# reported a failure. That is why each of these is a REFUSAL and not a warning.
#
# The spellings matter as much as the case. A check written against the
# configured path string refuses `/data/sync.db` and accepts the symlink, the
# hard link and the `..` -- which are the three an operator actually types,
# because they are what a backups directory and a cron line look like.
LIVE_INO=$(python3 -c 'import os,sys; print(os.stat(sys.argv[1]).st_ino)' "$DB")
mkdir -p aliasdir
ln -s "$DB" aliasdir/symlinked.db
ln "$DB" aliasdir/hardlinked.db 2>/dev/null && HAVE_LINK=1 || HAVE_LINK=0
ln -s "$DIR/live" aliasdir/livedir

no_backup() { # no_backup <name> <destination>
  out=$(./sync backup "$2" live 2>&1); rc=$?
  if [ "$rc" = 0 ]; then
    t_bad "$1: the backup was ALLOWED (exit 0) onto $2"
  else
    ck "$1" "REFUSING to back up onto" "$out"
  fi
}

no_backup "the live database's own path is refused" "$DB"
no_backup "...and so is a SYMLINK to it: identity, not spelling" \
          "$DIR/aliasdir/symlinked.db"
no_backup "...and a path that reaches it through .." \
          "$DIR/live/../live/sync.db"
no_backup "...and one that reaches it through a symlinked DIRECTORY" \
          "$DIR/aliasdir/livedir/sync.db"
if [ "$HAVE_LINK" = 1 ]; then
  no_backup "...and a HARD LINK, which no path comparison can see" \
            "$DIR/aliasdir/hardlinked.db"
else
  t_bad "the hard-link case could not be set up, so it was not tested"
fi
# THE SIDECARS ARE THE DATABASE TOO. The -wal holds every commit since the
# last checkpoint; renaming a backup over it destroys exactly the rows the
# person taking a backup was trying to protect.
no_backup "...and the write-ahead log beside it" "$DB-wal"
no_backup "...and the shared-memory index" "$DB-shm"
# STAGING NAMES ANOTHER OPERATION OWNS -- see srv/deploy/lock.sh. A file at
# `sync.db.restoring-<op>` is what restore.sh installs AS the live database,
# and `sync.db.part` is deleted by the next backup before it writes.
no_backup "...and the name a restore stages into and then installs" \
          "$DB.restoring-4242-deadbeef"
no_backup "...and the name a backup of the live database stages into" \
          "$DB.part"

# ...AND THE LIVE DATABASE IS STILL THE ONE THE SERVER HAS OPEN. This is the
# half that says the refusals happened BEFORE anything was written: same
# inode, same rows, same server.
NOW_INO=$(python3 -c 'import os,sys; print(os.stat(sys.argv[1]).st_ino)' "$DB")
ck "the live database is still the same file it was" "^$LIVE_INO$" "$NOW_INO"
ck "...with every row still in it" "^$LIVE_ROWS$" \
   "$(dbq "$DB" "SELECT count(*) FROM logrow" | head -1)"
DIG3=$(./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" GET /v1/digest 2>&1)
ck "...and the running server still serves it" "^readings $LIVE_ROWS " "$DIG3"

# THE CONTROL, and it is not optional: a rule that refuses everything passes
# every case above. An ordinary destination must still work, and what it
# produces must still be a restorable backup.
mkdir -p okdir
OK=$(./sync backup "$DIR/okdir/good.db" live 2>&1)
ck "a destination of its own is still accepted" "backed up" "$OK"
ck "...and what it wrote verifies" "verifies" \
   "$(./sync verify "$DIR/okdir/good.db" 2>&1)"
ck "...and holds every row, so the check did not cost the backup anything" \
   "^$LIVE_ROWS$" "$(dbq "$DIR/okdir/good.db" "SELECT count(*) FROM logrow" | head -1)"

echo "== verification never touches the file it is verifying =="
# The old shape noted which sidecars were absent, opened the file, and then
# unlinked `<path>-wal` and `<path>-shm` BY NAME. srv/deploy/restore.sh runs
# `verify` on a file staged in the LIVE data directory while the board is
# running, so "another process created a real -wal in that window" is the
# ordinary case rather than the exotic one -- and the -wal it would have
# deleted is every commit since the last checkpoint.
#
# Here the live server is running and its -wal exists throughout.
[ -f "$DB-wal" ] && t_ok "the live server has a write-ahead log to lose" ||
  t_bad "the live -wal is missing, so this case would prove nothing"
WAL_INO=$(python3 -c 'import os,sys; print(os.stat(sys.argv[1]).st_ino)' "$DB-wal")
./sync verify "$DB" >/dev/null 2>&1
if [ -f "$DB-wal" ]; then
  ck "verifying the LIVE database leaves its -wal exactly where it was" \
     "^$WAL_INO$" \
     "$(python3 -c 'import os,sys; print(os.stat(sys.argv[1]).st_ino)' "$DB-wal")"
else
  t_bad "verifying the live database DELETED its write-ahead log"
fi
ck "...and the server it belongs to still answers with every row" \
   "^readings $LIVE_ROWS " \
   "$(./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" GET /v1/digest 2>&1)"

# ...and the other half of the same rule: verifying a file that has NO
# sidecars must not leave any behind, because that is what put a -wal and a
# -shm in the backups directory in the first place.
./sync verify "$DIR/okdir/good.db" >/dev/null 2>&1
LEFT=$(ls -A okdir | grep -v '^good\.db$' | tr '\n' ' ')
empty "verifying a backup leaves nothing at all beside it" "$LEFT"

t_end
if [ "$fail" = 0 ]; then
  printf '\033[1;32mrestoredrill\033[0m: backup, verify and restore all work\n'
else
  printf 'restoredrill: FAILED\n'
fi
exit $fail
