#!/bin/sh
# deploy.sh --- install a locally built server on the board, atomically, and
# put the previous one back if the new one does not come up healthy.
#
# NOTHING ELSE DEPLOYS. `make duosmoke` verifies what is already running and
# never installs anything: a smoke test that deploys cannot tell you whether
# the thing you deployed last week is still alive, which is the only question
# it exists to answer.
#
# The sequence, and why each step is there:
#
#   1. BUILD LOCALLY and check the machine type. A native binary copied to a
#      riscv64 board is an "Exec format error" at startup -- the service is
#      then simply gone, and the failure looks like a crash rather than a bad
#      copy.
#   2. COPY BESIDE, NEVER OVER. The running executable is replaced by rename,
#      so a connection that dies mid-copy leaves the old one in place.
#   3. KEEP THE PREVIOUS ONE, by content hash, under releases/. A rollback
#      then needs no build, no toolchain and no network.
#   4. STOP, START, and CHECK: the pid is alive, the log says it is listening,
#      and the public URL answers with a page we recognise.
#   5. ROLL BACK AUTOMATICALLY if any of that fails. A deploy that leaves a
#      dead service behind and prints an error is a deploy that happened at
#      three in the afternoon and was noticed at eight in the evening.
#
# Usage:  ./srv/deploy/deploy.sh [--restart] [path/to/sync]
#
#   --restart   restart the service even when the board already runs this
#               exact binary. What applies a new certificate or a changed
#               configuration: neither is in the executable, so nothing about
#               the binary changes and the deploy would otherwise do nothing.
set -eu

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/deploy/pancra.conf"

FORCE=0
if [ "${1:-}" = "--restart" ] || [ "${1:-}" = "--force" ]; then
   FORCE=1
   shift
fi
LOCAL=${1:-$HERE/$PANCRA_LOCAL_BIN}
SSH="$PANCRA_SSH"
. "$HERE/srv/deploy/health.sh"
. "$HERE/srv/deploy/start.sh"
. "$HERE/srv/deploy/lock.sh"
. "$HERE/srv/deploy/supervisor.sh"
say() { printf 'deploy: %s\n' "$*"; }
fail() { printf 'deploy: FAILED: %s\n' "$*" >&2; exit 1; }

# ---- EVERY WAY THIS SCRIPT SUCCEEDS ENDS HERE ----------------------------
#
# SUPERVISION IS PART OF A DEPLOYMENT, not a separate errand somebody performs
# once. There are three ways for a deploy to succeed -- the board already runs
# this binary and is healthy, it runs this binary and had to be restarted, and
# a new binary was installed -- and each of them used to be its own `exit 0`.
# Installing the watchdog on one of the three would have been the deployment
# equivalent of the bug this whole directory keeps finding: a procedure that is
# correct on the path somebody remembered.
#
# THE MOST-TAKEN PATH IS THE FIRST ONE. A board that is up and healthy is
# exactly a board nobody restarts, so if supervision were installed only where
# something is started, the board that had been up for eight months -- the one
# with the most to lose from a crash -- would be the one that never got it.
#
# AND IT RUNS AFTER THE HEALTH VERDICT, never before: the watchdog starts a
# server that is not running, and starting one in the middle of a deploy that
# is about to roll back is the fight this deployment is careful not to have.
# By this point the service is up, checked, and the lock is still held.
#
# A FAILURE HERE IS NOT A FAILED DEPLOY. The service is live and was verified;
# what is missing is its safety net, and exiting non-zero would make `make
# duodeploy` report a red deployment of a board that is serving. So it is said
# in full, at the end, where it is the last thing on the screen.
deploy_done() {
   dd_rc=0
   supervisor_install "$OP" || dd_rc=$?
   if [ "$dd_rc" != 0 ]; then
      echo "deploy: WARNING: THE SERVICE IS LIVE AND UNSUPERVISED." >&2
      echo "  The deployment itself worked -- the binary above is installed," >&2
      echo "  running and answering -- but the reason is printed above, and" >&2
      echo "  until it is fixed this board recovers from nothing by itself." >&2
   fi
   exit 0
}

# THIS OPERATION'S OWN NAME, for every path it stages under. `sync.new` was
# shared with rollback.sh, which stages the release it is putting back at the
# same name and then renames it over the live executable -- so the two, run
# together, published each other's binary while both hash checks passed. See
# lock.sh.
OP=$(op_id)

# ------------------------------------------------------------ the front door
#
# BEFORE ANYTHING IS COPIED, because every health check below goes through it.
# Checked here rather than only in wait_healthy_since: that one runs after the
# swap and the restart, so an undeclared front door read as "the new build is
# NOT healthy" and this script rolled a perfectly good deploy back, then
# reported the board DOWN while it was up. See health.sh.
#
# NOT in rollback.sh or restore.sh: those run when something is already wrong,
# and a recovery that refuses to recover because a variable was never filled in
# is worse than the missing variable. They install, start, and then say they
# could not verify it from outside.
require_front || fail "nothing was installed; declare the front door and re-run"

# ...AND SO IS WHO RESTARTS IT, for the same reason and at the same moment. A
# value this repository cannot install is a deployment that would come up
# unsupervised at the END of a successful deploy, which is a strange time to
# learn that a variable was misspelt -- and the operator has by then been told
# the board is live, which it is, so the sentence they remember is the good
# one. Refuse it here, where nothing has been touched.
supervisor_declared ||
   fail "PANCRA_SUPERVISOR='${PANCRA_SUPERVISOR:-}' is not watchdog or none"

# ---------------------------------------------------------------- the artifact

[ -f "$LOCAL" ] || fail "no such build: $LOCAL (run 'make duo' first)"
file "$LOCAL" | grep -q "$PANCRA_ARCH" ||
   fail "$LOCAL is not a $PANCRA_ARCH binary -- $(file -b "$LOCAL")"
# STATIC, because the board's libc is not the one this was linked against.
file "$LOCAL" | grep -q 'statically linked' ||
   fail "$LOCAL is dynamically linked; the board has no matching libc"
hash_local=$(sha256sum "$LOCAL" | awk '{print $1}')
say "installing $hash_local"

# ---------------------------------------------------------- health, and start
#
# Defined HERE, above every path that needs them, because BOTH the install and
# the same-binary case use them. They used to be written out inline after the
# install, which is how the same-binary case came to have neither: it compared
# two hashes and exited 0 without ever asking whether anything was running.

# health_since / url_ok / wait_healthy_since come from health.sh, shared
# with rollback.sh and restore.sh -- see the note there on why the three
# paths must ask the SAME question.

# Stop whatever is running and start what is installed. setsid + </dev/null,
# and NOT a bare `&`: a backgrounded child of the ssh session is killed when
# that session closes, so the deploy "succeeds" and the service is gone the
# moment the terminal is.
start_service() {
   $SSH "$PANCRA_HOST" "sh -s" <<EOF || fail "the service did not start"
set -eu
$(stop_block)
$(start_block "$1")
EOF
}


# --------------------------------------------------------- one at a time
#
# TAKEN HERE: after every check that can be answered without the board, and
# before the first fact this script READS from it. Not merely before the first
# write -- the same-hash decision below is made out of `hash_live`, and a
# concurrent operation that swaps the executable between that read and the
# restart turns "the board already runs this exact binary, and it is healthy"
# into a sentence about a binary that is no longer installed.
#
# Held until this script exits, however it exits: lock.sh's traps cover the
# rollback path at the bottom too, which is where an abandoned lock would hurt
# most (the board is already unhealthy, and now nothing can be deployed to it).
lock_take deploy "$OP" || exit 1

# ------------------------------------------------------------- what is running

# The hash of what is there now, so the rollback has something to name and the
# release directory is not filled with copies of the same binary.
hash_live=$($SSH "$PANCRA_HOST" "sha256sum '$PANCRA_BIN' 2>/dev/null | awk '{print \$1}'" || true)
# ------------------------------------------- the board already has this build
#
# "SAME HASH" IS NOT "NOTHING TO DO". This exited 0 the moment the two hashes
# matched -- before looking at the pid, the log or the URL -- so a deploy of
# an unchanged build could not restart a service that had DIED, and a re-run
# after a crash reported success while the board answered nothing. Deploying
# the same build is the most natural thing to do when something is wrong,
# which is exactly when this path was useless.
#
# It also could not apply a new certificate or a changed configuration:
# neither is in the executable, so nothing about the binary changes. That is
# what --restart is for.
#
# AND "SAME HASH" IS A FACT ABOUT A FILE, NOT ABOUT A PROCESS. hash_live is
# the sha256 of $PANCRA_BIN on disk; it says nothing about what the live pid is
# executing. This path used to accept a live pid, a readiness line anywhere in
# the log and any public page carrying the marker -- four unrelated
# observations -- and report "already running this exact binary, and it is
# healthy" about a process running something else entirely. It is the MOST
# taken path (a re-deploy of an unchanged build), so it must ask exactly what
# wait_healthy_since asks: this pid's /proc/<pid>/exe, and this backend
# answering directly, not only the public name.
if [ "$hash_live" = "$hash_local" ]; then
   if [ "$FORCE" = 1 ]; then
      say "the board already runs this binary; restarting as asked"
   elif health_since && exe_is "$hash_local" && backend_ok && url_ok; then
      say "the board is already running this exact binary, and it is healthy"
      deploy_done
   else
      say "the board runs this binary but is NOT healthy; restarting"
   fi
   tag=$(start_tag)
   mark=$(log_mark)
   start_service "$tag"
   if wait_healthy_since "$mark" "$tag" "$hash_local"; then
      say "$hash_local is live and answering $PANCRA_URL"
      deploy_done
   fi
   # NOTHING TO ROLL BACK TO: the binary on the board is the one we were
   # asked to install, so there is no previous release this could put back.
   fail "restarted, but the service did not come up healthy -- see $PANCRA_LOG"
fi

$SSH "$PANCRA_HOST" "mkdir -p '$PANCRA_RELEASES' '$PANCRA_BACKUPS'" ||
   fail "cannot prepare $PANCRA_RELEASES on $PANCRA_HOST"

# ------------------------------------------------- a backup before any restart
#
# Taken with the server still running, through sqlite's own backup API: the
# database is in WAL mode, so `cp sync.db` is a copy missing every transaction
# since the last checkpoint -- which is exactly the data a restore is for.
if [ -n "$hash_live" ]; then
   stamp=$($SSH "$PANCRA_HOST" "date -u +%Y%m%dT%H%M%SZ")
   # STAMPED TO THE SECOND IS NOT UNIQUE. This name is shared with backup.sh,
   # which builds it the same way from the same command -- so a scheduled
   # backup and a deploy landing in one second wrote the same destination, and
   # underneath it the same `<dest>.part` that sqlite stages into. Two sqlite
   # backups writing one .part is a torn file that one of them then verifies
   # and publishes. The stamp still leads, so `ls` still sorts by date.
   bak=$PANCRA_BACKUPS/sync-$stamp-$OP.db
   # `|| rc=$?`, not a bare call followed by `$?`: this script runs under
   # `set -e`, where a command that exits 2 outside a condition context kills
   # the shell before anything can look at the status -- and the whole point of
   # status 2 is that it must be looked at.
   rc=0
   $SSH "$PANCRA_HOST" "'$PANCRA_BIN' backup '$bak' '$PANCRA_DATA'" || rc=$?
   case $rc in
      0) say "backed up to $bak" ;;
      # 2 IS "PUBLISHED, DURABILITY UNKNOWN" (see srv/sync.c). The backup is
      # written, verified and readable right now; only its directory entry's
      # survival of a power cut is in doubt. Treating that as a failed backup
      # would stop a deploy over data that HAS been saved, and treating it as a
      # clean one would hide the one fact worth hearing. So: say it, loudly,
      # and carry on -- the deploy is not what is uncertain.
      2)
         say "backed up to $bak"
         echo "deploy: WARNING: DURABILITY UNCERTAIN for that backup -- it is" >&2
         echo "  on the board now and verified, but the board could not sync" >&2
         echo "  $PANCRA_BACKUPS, so a power loss in the next moments can erase" >&2
         echo "  the directory entry. The deploy continues (the data IS saved" >&2
         echo "  as of this instant); copy it off the board if it matters." >&2
         ;;
      *) fail "could not take a backup; refusing to deploy over unsaved data" ;;
   esac
fi

# ------------------------------------------------------------------- the copy

STAGED=$PANCRA_BIN.new-$OP
$PANCRA_SCP "$LOCAL" "$PANCRA_HOST:$STAGED" || fail "copy failed"
remote_new=$($SSH "$PANCRA_HOST" "sha256sum '$STAGED' | awk '{print \$1}'")
[ "$remote_new" = "$hash_local" ] ||
   fail "the copy on the board hashes $remote_new, not $hash_local"

# ---------------------------------------------------------- stop, swap, start
#
# The old binary is kept BEFORE the swap, under its own hash, so the rollback
# target exists even if everything after this point fails.
$SSH "$PANCRA_HOST" "sh -s" <<EOF || fail "the install step failed on the board"
set -eu
if [ -f '$PANCRA_BIN' ]; then
   cp -p '$PANCRA_BIN' '$PANCRA_RELEASES/sync-$hash_live'
fi
chmod +x '$STAGED'
# Replacing a running executable by rename is safe (the kernel keeps the old
# inode open); start_service below stops the old process and runs the new one.
mv '$STAGED' '$PANCRA_BIN'
EOF
tag=$(start_tag)
mark=$(log_mark)
start_service "$tag"

# ------------------------------------------------------------- is it healthy?
#
# The mark is taken BEFORE the restart, so the readiness line this waits for
# has to be one THIS start wrote. Without it the check passes on the previous
# run's line, and a server that starts and dies immediately looks healthy.

if wait_healthy_since "$mark" "$tag" "$hash_local"; then
   say "$hash_local is live and answering $PANCRA_URL"
   deploy_done
fi

# ------------------------------------------------------------------ rollback

printf 'deploy: the new build is NOT healthy -- rolling back\n' >&2
if [ -z "$hash_live" ]; then
   fail "there is no previous release to roll back to; the board is DOWN"
fi
"$HERE/srv/deploy/rollback.sh" "$hash_live" ||
   fail "the rollback ALSO failed; the board is DOWN"
fail "rolled back to $hash_live; the new build did not come up"
