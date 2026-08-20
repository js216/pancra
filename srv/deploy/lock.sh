# lock.sh --- ONE OPERATION AT A TIME ON THE BOARD. One lock, every verb.
#
# Sourced (not executed) by deploy.sh, rollback.sh, restore.sh, rotate.sh and
# backup.sh, after pancra.conf and with $SSH set. The third of the three shared
# pieces: health.sh owns "is it serving?", start.sh owns "make it run", and this
# one owns "is anybody else already doing this?".
#
# ---- WHY, IN THE WORDS OF WHAT WENT WRONG WITHOUT IT ---------------------
#
# Every procedure here was individually careful and none of them had heard of
# each other. They all write the same handful of names on the board, and two
# running at once is not an exotic scenario -- it is what an incident looks
# like. Somebody starts a restore because the data is wrong; somebody else, on
# another laptop or in another terminal, re-deploys the last good build because
# the service is down. Both are reasonable. Together they were:
#
#   THE STAGED ARTIFACT. deploy.sh scp'd the new binary to `sync.new` and
#   rollback.sh cp'd the old one to the SAME `sync.new`, and each then renamed
#   that name over the live executable. Interleaved, the deploy publishes the
#   rollback's binary or the other way about -- and the deploy's own hash check
#   passed, because it hashed the copy BEFORE the other script overwrote it. The
#   board then runs a build nobody asked for and both scripts report success.
#
#   THE PID FILE AND THE PROCESS. Every stop_block reads $PANCRA_PID and kills
#   what it names. So the second operation kills the first operation's brand
#   new server, the first operation's health wait times out on a process the
#   second one replaced, and deploy.sh AUTOMATICALLY ROLLS BACK a perfectly good
#   build because of it. Worse if it is slower still: the kill lands on a pid the
#   kernel has already handed to something else.
#
#   THE LIVE DATABASE. restore.sh moves sync.db, sync.db-wal and sync.db-shm out
#   of the way and a backup moves through the same directory reading them. A
#   backup taken across that move is a database plus half of another one's log.
#
#   THE HEALTH EVIDENCE. health_since reads the pid file and greps the log. Both
#   are shared, so with two starts in flight the evidence a script reads can be
#   about the other script's process -- which is the exact class of bug the start
#   tag was introduced to end, reached from a direction the tag cannot see.
#
# So: ONE lock, held by ONE operation, from the first thing it changes on the
# board until its final health verdict or its rollback -- and refused with a
# sentence that says WHO holds it and SINCE WHEN, because "busy" sends an
# operator to guess, and at 3am they guess "it must be stuck" and force it.
#
# ---- WHY mkdir ----------------------------------------------------------
#
# `mkdir` is the atomic primitive that is atomic EVERYWHERE: one syscall, it
# either creates the directory or fails with EEXIST, and it does so on every
# filesystem this could ever run on. `set -C` over a lockfile looks equivalent
# and is not -- noclobber's create-exclusive is not atomic over NFS, and the
# board's root has been on stranger things than NFS. `flock` is not on a
# busybox board at all. The comparison matters because a lock that is USUALLY
# exclusive is worse than no lock: it removes the operator's own caution
# ("better not run two of these") and replaces it with a guarantee that fails
# under exactly the load that makes people run two of these.
#
# ---- ...AND HOW IT IS BROKEN, BECAUSE A STUCK LOCK IS ITS OWN OUTAGE -----
#
# The lock is held by a script running on the OPERATOR'S machine, not on the
# board, so the board cannot ask whether the holder is still alive: the pid in
# the lock is a pid on another computer. An ssh session that dies mid-deploy
# therefore leaves a directory on the board that no living thing will ever
# remove, and from then on every deploy, rollback, backup and restore refuses.
# That is a self-inflicted outage discovered at the worst possible moment.
#
# Two ways out, both loud, neither silent:
#
#   * AGE. A lock older than PANCRA_LOCK_STALE (an hour by default) is broken
#     automatically, and the breaking is printed. No operation here runs for an
#     hour -- the longest is a binary copy plus a thirty-second health wait --
#     so an hour-old lock is a crash artifact by an order of magnitude, and
#     preferring a wrong refusal for ever over a right break after an hour is
#     not caution, it is just a different failure.
#   * PANCRA_LOCK_BREAK=1. For the operator who KNOWS the holder is gone and is
#     not going to wait an hour to say so. Named in the refusal itself, because
#     an escape hatch nobody is told about is one that gets invented badly.
#
# A lock whose `since` file is missing (a crash in the microseconds between the
# mkdir and the write) has no age, so it is never broken by time -- only by
# PANCRA_LOCK_BREAK. The refusal says exactly that rather than reporting an age
# it does not have.

# THE OPERATION'S OWN NAME, for the staging paths that used to be shared.
#
# Deploy and rollback both staged at `sync.new`, restore at `sync.db.restoring`,
# and a backup's destination is stamped only to the second -- so two backups in
# one second write the same file, and sqlite writes the same `<dest>.part`
# underneath both. The lock makes the overlap impossible; these make the DAMAGE
# impossible even when the lock has been broken by an operator who was wrong
# about the holder being gone. Both, deliberately: the lock is the rule and this
# is what happens when the rule is overridden.
#
# $$ is this script's pid, which is unique among the operations running on this
# machine; the random half covers two machines deploying to one board.
op_id() {
   printf '%s-%s' "$$" \
      "$(od -An -N4 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')"
}

# Whether THIS shell took the lock (and must therefore drop it). Not exported:
# a child process re-enters the lock, it does not own it.
PANCRA_LOCK_MINE=0

# ---- TAKE IT ------------------------------------------------------------
#
# lock_take <operation-name> <op-id>
#
# 0 when this shell may proceed, 1 when it must not (and the refusal has been
# printed, naming the holder). Installs the traps that drop it again.
#
# RE-ENTRANT BY TOKEN, not by a flag. deploy.sh runs rollback.sh as a child when
# the new build does not come up healthy -- the single most important path in
# this whole directory -- and a child that blocked on its parent's lock would
# deadlock the recovery instead of performing it. So the parent exports the
# lock's token and the child, finding the SAME token on the board, proceeds
# without taking or dropping anything. Compared against the board rather than
# trusted as a flag: a PANCRA_LOCK_HELD left in an operator's environment from
# an earlier session must not silently disable locking for everything they run.
lock_take() {
   lt_op=${1:-operation}
   lt_id=${2:-}
   if [ -n "${PANCRA_LOCK_HELD:-}" ]; then
      lt_have=$($SSH "$PANCRA_HOST" \
         "cat '$PANCRA_LOCK/token' 2>/dev/null" 2>/dev/null | tr -dc 'a-zA-Z0-9-')
      if [ -n "$lt_have" ] && [ "$lt_have" = "$PANCRA_LOCK_HELD" ]; then
         return 0
      fi
   fi
   lt_who="$lt_op ($(id -un 2>/dev/null || echo someone)@$(hostname 2>/dev/null || echo somewhere), pid $$)"
   lt_break=${PANCRA_LOCK_BREAK:-0}
   lt_stale=${PANCRA_LOCK_STALE:-3600}
   # ONE REMOTE SHELL for the whole decision, because the decision is the
   # atomicity. Split into "ask whether it is locked" and then "lock it", the
   # gap between the two round trips is precisely the window this exists to
   # close -- two scripts would both read "free" and both mkdir, and one of the
   # mkdirs would fail with EEXIST and be reported as an error rather than as
   # the answer. Here the mkdir IS the question.
   lt_out=$($SSH "$PANCRA_HOST" "sh -s" <<EOF 2>/dev/null
lock='$PANCRA_LOCK'
now=\$(date -u +%s)
if ! mkdir "\$lock" 2>/dev/null; then
   who=; [ -f "\$lock/owner" ] && who=\$(cat "\$lock/owner" 2>/dev/null)
   since=; [ -f "\$lock/since" ] && since=\$(cat "\$lock/since" 2>/dev/null)
   age=-1
   case "\$since" in
      '' | *[!0-9]*) ;;
      *) age=\$((now - since)) ;;
   esac
   # Breakable by an operator who says so, or by age -- never by an age this
   # cannot compute. A lock with no 'since' is a crash between the mkdir and
   # the write, and guessing an age for it would make the automatic break fire
   # on a lock that might be one millisecond old.
   if [ '$lt_break' = 1 ] || { [ \$age -ge 0 ] && [ \$age -gt $lt_stale ]; }; then
      printf 'BROKE\t%s\t%s\n' "\$age" "\$who"
      rm -rf "\$lock"
      # AND IF SOMEBODY ELSE WON THE RETRY, THEY WON. Two operators breaking
      # the same stale lock in the same instant is a race with a correct
      # outcome: one mkdir succeeds, the other is refused and says so.
      if ! mkdir "\$lock" 2>/dev/null; then
         printf 'BUSY\t%s\t%s\n' "-1" "the operation that just took it"
         exit 0
      fi
   else
      printf 'BUSY\t%s\t%s\n' "\$age" "\$who"
      exit 0
   fi
fi
printf '%s\n' '$lt_id' > "\$lock/token"
printf '%s\n' '$lt_who' > "\$lock/owner"
printf '%s\n' "\$now" > "\$lock/since"
printf 'TAKEN\t%s\t%s\n' "\$now" "\$(date -u +%Y-%m-%dT%H:%M:%SZ)"
EOF
)
   case "$lt_out" in
      *TAKEN*) ;;
      *)
         lock_refuse "$lt_op" "$lt_out"
         return 1
         ;;
   esac
   case "$lt_out" in
      BROKE*)
         lt_bage=$(printf '%s' "$lt_out" | sed -n '1s/^BROKE\t\([^\t]*\)\t.*$/\1/p')
         lt_bwho=$(printf '%s' "$lt_out" | sed -n '1s/^BROKE\t[^\t]*\t//p')
         echo "$lt_op: BROKE A STALE LOCK on $PANCRA_HOST and went ahead." >&2
         echo "  It was held by: ${lt_bwho:-nobody recorded}" >&2
         echo "  Age: ${lt_bage}s (limit PANCRA_LOCK_STALE=${lt_stale}s${lt_break:+, PANCRA_LOCK_BREAK=$lt_break})." >&2
         echo "  If that operation is in fact STILL RUNNING, stop reading and" >&2
         echo "  stop it: two of these at once can publish each other's binary" >&2
         echo "  or interleave a database move." >&2
         ;;
   esac
   PANCRA_LOCK_MINE=1
   PANCRA_LOCK_HELD=$lt_id
   export PANCRA_LOCK_HELD
   # DROPPED ON EVERY EXIT, including the ones that are not `exit` at all.
   # restore.sh alone has eight terminating paths and three of them are
   # incidents; a lock released on seven of eight is a lock that jams the board
   # on the day of the eighth. INT and TERM get their own handlers because a
   # trap on those does not terminate the shell by itself -- an operator's ^C
   # would otherwise run the handler and carry straight on with the deploy.
   trap 'lock_drop' EXIT
   trap 'lock_drop; exit 130' INT
   trap 'lock_drop; exit 143' TERM
   return 0
}

# The refusal, spelled out, because "busy" is what makes people force things.
lock_refuse() {
   lr_op=$1
   lr_age=$(printf '%s' "$2" | sed -n '1s/^BUSY\t\([^\t]*\)\t.*$/\1/p')
   lr_who=$(printf '%s' "$2" | sed -n '1s/^BUSY\t[^\t]*\t//p')
   echo "$lr_op: FAILED: another operation is running on this board." >&2
   echo "  holder: ${lr_who:-unknown (the lock was taken this instant, or the" \
        "board could not be asked)}" >&2
   if [ -n "$lr_age" ] && [ "$lr_age" -ge 0 ] 2>/dev/null; then
      echo "  since:  ${lr_age}s ago (board clock)" >&2
   else
      echo "  since:  unknown -- the lock has no timestamp, which means it was" >&2
      echo "          interrupted between being created and being described." >&2
   fi
   echo "  lock:   $PANCRA_LOCK on $PANCRA_HOST" >&2
   echo "  Deployments, rollbacks, backups, restores and certificate rotations" >&2
   echo "  share the staged artifact names, the pid file, the running process" >&2
   echo "  and the live database. Two at once can publish each other's binary," >&2
   echo "  kill each other's server, or interleave a database move -- so this" >&2
   echo "  one did NOTHING and changed NOTHING." >&2
   echo "  Wait for it. If you know that operation is gone (the terminal was" >&2
   echo "  closed, the ssh died), re-run with PANCRA_LOCK_BREAK=1; a lock older" >&2
   echo "  than PANCRA_LOCK_STALE=${PANCRA_LOCK_STALE:-3600}s is broken" >&2
   echo "  automatically." >&2
}

# ---- DROP IT ------------------------------------------------------------
#
# Idempotent, because the INT and TERM handlers exit and the EXIT trap then
# runs a second time.
#
# AND IT CHECKS THE TOKEN FIRST. If this operation's lock was broken as stale
# while it was still running -- the whole point of the break being that it can
# be wrong -- the lock on the board now belongs to SOMEBODY ELSE, and removing
# it on the way out would hand a third operation into the middle of theirs. The
# one thing worse than a stuck lock is a lock that silently stops being one.
lock_drop() {
   [ "${PANCRA_LOCK_MINE:-0}" = 1 ] || return 0
   PANCRA_LOCK_MINE=0
   $SSH "$PANCRA_HOST" "sh -s" >/dev/null 2>&1 <<EOF || true
lock='$PANCRA_LOCK'
[ -d "\$lock" ] || exit 0
have=\$(cat "\$lock/token" 2>/dev/null)
[ "\$have" = '$PANCRA_LOCK_HELD' ] || exit 0
rm -rf "\$lock"
EOF
}
