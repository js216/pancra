# start.sh --- HOW THE SERVICE STOPS AND STARTS. One definition, every path.
#
# Sourced (not executed) by deploy.sh, rollback.sh, restore.sh and rotate.sh,
# after pancra.conf and with $SSH set. The companion to health.sh: that file
# owns "is it serving?", this one owns "make it run".
#
# It exists for the same reason health.sh does -- the sequence was written out
# inline in every script that needed it, and the copies drifted:
#
#   - deploy.sh, rollback.sh and restore.sh each had their own stop-and-start,
#     identical by luck rather than by construction;
#   - README.md's certificate rotation had a FOURTH copy, typed out for an
#     operator to paste, with the port, the data directory and both pem paths
#     spelled as literals instead of read from pancra.conf -- so a board on a
#     different port silently got a server on 8443;
#   - and that fourth copy ended at `... &` with no `echo $! > sync.pid`. The
#     pid file kept naming the process that had just been killed. Every later
#     health check then read a pid that was dead, or worse, one the kernel had
#     since handed to something else; and the next deploy's stop step killed
#     whatever that was instead of the server.
#
# THESE EMIT SHELL TEXT rather than running it, because the callers need to do
# their own work BETWEEN the stop and the start -- rollback renames the
# executable, restore replaces the database, rotate swaps the pem files -- and
# all of it has to happen in the one remote shell, while the service is down.

# Stop whatever the pid file names: politely, then not. Silent if nothing is
# running, because "already stopped" is a fine state to start from.
stop_block() {
   cat <<EOF
# THIS STOP WAS ASKED FOR -- and something else is now watching this service.
#
# The watchdog (srv/deploy/supervise.sh) starts the server again when it finds
# it gone, which is the whole point of it, and "gone" is exactly what a deploy,
# a rotation and a restore each arrange on purpose for a second or two. Without
# a marker the two are indistinguishable: the watchdog would start the OLD
# binary in the middle of the swap, the procedure would then start the new one
# over the top of it, two servers would be sharing one data directory, and the
# pid file would name whichever wrote it last. A supervisor that races the
# deploy is worse than no supervisor.
#
# So the stop announces itself, in a file, with the board's clock in it -- and
# start_block below removes it. It is raised HERE, in the one stop every verb
# uses, rather than in each of them: a procedure that forgot would not fail,
# it would just occasionally fight the watchdog, at the least reproducible
# moment there is.
#
# THE CLOCK IS IN IT so the marker can expire. An operation whose ssh dies
# between this line and its start leaves the marker behind, and a watchdog that
# obeyed it for ever would be the reason the board stayed down. See
# PANCRA_SUPERVISE_GRACE.
mkdir -p '$(dirname "$PANCRA_DOWN")' 2>/dev/null || true
date -u +%s > '$PANCRA_DOWN' 2>/dev/null || true
# ---- AND A PID IS NOT AN IDENTITY -------------------------------------
#
# THIS SENT TERM AND THEN KILL TO WHATEVER NUMBER WAS IN THE FILE. A pid is an
# index into a table the kernel reuses, and it is reused soonest in exactly the
# situations this directory exists for:
#
#   * THE SERVER CRASHED. The pid file goes on naming it, the number is handed
#     out again within minutes on a busy board, and the next deploy's stop step
#     kills whatever got it.
#   * THE BOARD REBOOTED. The pid file survives the reboot; the process does
#     not. On a freshly booted board the low numbers go straight back out, so
#     the file now names ntpd, dropbear, or the board's own sensor logger --
#     and the procedure that recovers the service kills one of them instead.
#
# Neither is exotic and neither leaves a trace: the signal succeeds, this stop
# reports nothing, and the damage is to a program nobody was thinking about.
# lock.sh has said for a while that this is what an unlocked second operation
# does; the lock does not help here, because there is no second operation --
# only a stale file.
#
# SO IDENTITY IS ESTABLISHED BEFORE ANY SIGNAL, and it is two facts, not one:
#
#   THE EXECUTABLE. sha256 of /proc/<pid>/exe against the sha256 the start
#   recorded of the file it launched. It follows the INODE rather than the
#   path, which is what makes it survive a deploy: the swap renames a new
#   binary over the old name, and the process still running the old inode is
#   the one this stop has to kill.
#   THE START TIME. Field 22 of /proc/<pid>/stat, fixed when the process was
#   created. The hash alone would accept a SECOND copy of the same executable
#   that happens to hold the recycled number -- which is not a hypothesis on a
#   board where the thing most likely to be started again is this server.
#
# ...AND WHEN THERE IS NO RECORD AT ALL -- a pid file from a deployment older
# than this paragraph, or one an operator wrote by hand -- the fallback is the
# weaker claim that can still be made from the board alone: /proc/<pid>/exe
# must resolve to \$PANCRA_BIN (or to it with a " (deleted)" suffix, which is
# what the kernel shows once a deploy has renamed over that name). That is
# deliberately not a refusal: refusing would leave a RUNNING server unstopped
# and then start a second one over the top of it, two servers on one data
# directory, which is a worse failure than the one being fixed. A record that
# is present and DISAGREES is different -- that is evidence, not absence of it,
# and it is refused.
#
# A LIVE PROCESS THAT IS NOT OURS IS NOT SIGNALLED AND THE PID FILE IS
# QUARANTINED, loudly. Left in place it would be re-read by the next operation,
# by health.sh, and by the watchdog, each of them reasoning about a stranger.
if [ -f '$PANCRA_PID' ]; then
   pid=\$(tr -dc '0-9' < '$PANCRA_PID' 2>/dev/null) || pid=
   ours=no
   why=
   if [ -z "\$pid" ]; then
      why="the pid file holds no number"
   elif [ ! -d "/proc/\$pid" ]; then
      # Nothing is alive under that number, which is a perfectly good state to
      # start from -- and the file is removed rather than quarantined, because
      # a dead pid is what every ordinary stop leaves behind and keeping one
      # copy of it per stop would be a directory of litter, not evidence.
      why=dead
   else
      # THE COMM FIELD CAN CONTAIN SPACES AND BRACKETS, so nothing may count
      # fields from the left of /proc/<pid>/stat. Everything after the ") "
      # that closes it is numeric, so the last one is the right one to cut at:
      # state becomes field 1 and start time, field 22, becomes field 20.
      live_start=\$(sed 's/.*[)] //' "/proc/\$pid/stat" 2>/dev/null |
         awk '{print \$20}') || live_start=
      live_exe=\$(sha256sum "/proc/\$pid/exe" 2>/dev/null |
         awk '{print \$1}') || live_exe=
      live_path=\$(readlink "/proc/\$pid/exe" 2>/dev/null) || live_path=
      want_start=
      want_exe=
      if [ -r '$PANCRA_PID_ID' ]; then
         want_start=\$(awk '\$1 == "start" { print \$2 }' \
            '$PANCRA_PID_ID' 2>/dev/null) || want_start=
         want_exe=\$(awk '\$1 == "exe" { print \$2 }' \
            '$PANCRA_PID_ID' 2>/dev/null) || want_exe=
      fi
      if [ -n "\$want_start" ] && [ -n "\$want_exe" ]; then
         if [ "\$live_start" = "\$want_start" ] &&
            [ "\$live_exe" = "\$want_exe" ]; then
            ours=yes
         else
            why="pid \$pid is NOT the process this deployment started"
            why="\$why (it began at \${live_start:-an unreadable time} running"
            why="\$why \${live_exe:-an unreadable executable}; the start"
            why="\$why recorded \$want_start and \$want_exe)"
         fi
      else
         case "\$live_path" in
            '$PANCRA_BIN' | '$PANCRA_BIN '*) ours=yes ;;
            *)
               why="pid \$pid is running \${live_path:-something this shell"
               why="\$why cannot read}, which is not $PANCRA_BIN, and there is"
               why="\$why no identity record to check it against"
               ;;
         esac
      fi
   fi
   if [ "\$ours" = yes ]; then
      kill "\$pid" 2>/dev/null || true
      i=0
      while kill -0 "\$pid" 2>/dev/null && [ \$i -lt 50 ]; do sleep 0.2; i=\$((i+1)); done
      # ASKED AGAIN BEFORE THE SIGNAL THAT CANNOT BE CAUGHT. The loop above
      # ends either because the process is gone or because ten seconds passed,
      # and in the first case the number is free for the kernel to hand to
      # something else before this line runs. The start time is the cheap half
      # of the identity and the half a new process cannot share.
      still=\$(sed 's/.*[)] //' "/proc/\$pid/stat" 2>/dev/null |
         awk '{print \$20}') || still=
      if [ -n "\$still" ] && [ "\$still" = "\$live_start" ]; then
         kill -9 "\$pid" 2>/dev/null || true
      fi
      # THE NUMBER OF A PROCESS THIS STOP JUST ENDED IS THE MOST DANGEROUS
      # THING IN THIS DIRECTORY, so it does not outlive it. start_block writes
      # a fresh pair a moment later; a stop that is never followed by a start
      # leaves no number for anybody to signal.
      rm -f '$PANCRA_PID' '$PANCRA_PID_ID' 2>/dev/null || true
   elif [ "\$why" = dead ]; then
      rm -f '$PANCRA_PID' '$PANCRA_PID_ID' 2>/dev/null || true
   else
      stale='$PANCRA_PID'.stale-\$(date -u +%s 2>/dev/null || echo 0)-\$\$
      mv '$PANCRA_PID' "\$stale" 2>/dev/null || rm -f '$PANCRA_PID'
      mv '$PANCRA_PID_ID' "\$stale.id" 2>/dev/null ||
         rm -f '$PANCRA_PID_ID' 2>/dev/null || true
      echo "NOT SIGNALLING pid \$pid: \$why." >&2
      echo "  A pid file is a number, and the kernel reuses numbers -- soonest" >&2
      echo "  after the crash or the reboot that makes this file stale. Sending" >&2
      echo "  TERM and KILL to it would have stopped a program that has nothing" >&2
      echo "  to do with this deployment, silently and successfully." >&2
      echo "  The file has been quarantined at \$stale; the service is treated" >&2
      echo "  as already stopped, which is a fine state to start from." >&2
   fi
fi
EOF
}

# A TOKEN FOR ONE START, so the log can be read as a sequence of starts rather
# than a pile of lines. Generated here, on the machine running the deploy --
# the board is not asked for anything, and two deploys racing cannot mint the
# same one.
start_tag() {
   printf 'start-%s-%s' "$$" \
      "$(od -An -N6 -tx1 /dev/urandom 2>/dev/null | tr -d ' \n')"
}

# Start it from the configured paths, detached from this ssh session, and
# RECORD THE PID -- which is not decoration: it is the only handle every other
# procedure here has on the process.
#
# $1 IS THIS START'S TAG, written into the log before the process is launched.
# The byte offset taken by log_mark answers "what was written after this
# point", which is the right question only as long as nothing truncates or
# rotates the log between the mark and the check -- and when something does,
# the offset silently points into the middle of an older start and the check
# reads the WRONG start's readiness line. The banner is the same claim made in
# the log's own words, so it survives that.
start_block() {
   cat <<EOF
printf '=== pancra $1 ===\n' >> '$PANCRA_LOG'
cd '$PANCRA_DATA'
# PANCRA_ORIGIN is read from the ENVIRONMENT, not argv: the CLI's argument list
# is a fixed contract (srv/sync.c refuses an unexpected count), and this is
# configuration rather than an operand. Exported here so the server the deploy
# starts is the one that knows its own public name -- without it the compiled
# default applies, which is right for this deployment and wrong for any other.
PANCRA_ORIGIN='$PANCRA_ORIGIN'
export PANCRA_ORIGIN
setsid '$PANCRA_BIN' '$PANCRA_PORT' '$PANCRA_DATA' '$PANCRA_CERT' '$PANCRA_KEY' \
   </dev/null >>'$PANCRA_LOG' 2>&1 &
srv=\$!
echo \$srv > '$PANCRA_PID'
# ---- WHAT WAS STARTED, NOT MERELY ITS NUMBER --------------------------
#
# The number above is what every stop in this deployment signals, and a number
# is not an identity: the kernel hands it out again, soonest after the crash or
# the reboot that makes this file stale. So the start -- which is the only
# moment anything KNOWS what it launched -- writes that down beside it, and
# stop_block above refuses to signal a process that does not match.
#
# THE HASH IS OF THE FILE, NOT OF /proc/\$srv/exe, and that is not a shortcut.
# This runs microseconds after the fork, possibly before setsid has exec'd, so
# /proc/<pid>/exe may still name setsid itself. The file is the inode that is
# about to be executed, which is exactly what the stop reads back through
# /proc/<pid>/exe -- and it stays right when a later deploy renames another
# binary over this name, because that link follows the inode.
#
# THE START TIME IS THE PROCESS'S OWN and is fixed at fork, so reading it now
# is safe from the same race. It is the half of the identity that a second copy
# of the same executable cannot forge.
{ printf 'pid %s\n' "\$srv"
  printf 'start %s\n' "\$(sed 's/.*[)] //' "/proc/\$srv/stat" 2>/dev/null |
     awk '{print \$20}')"
  printf 'exe %s\n' "\$(sha256sum '$PANCRA_BIN' 2>/dev/null | awk '{print \$1}')"
} > '$PANCRA_PID_ID' 2>/dev/null || true
# THE SERVICE IS NO LONGER STOPPED ON PURPOSE. Cleared AFTER the pid file is
# written, never before: between the two the watchdog would find no marker and
# a pid file still naming the process this start replaced, decide the service
# was gone, and start a second one.
rm -f '$PANCRA_DOWN' 2>/dev/null || true
EOF
}

# ---- THE SAME START, WITH THE TAG LEFT OPEN -------------------------------
#
# The watchdog on the board has to start the server too, and it cannot be
# handed a tag at generation time: it starts the service at three in the
# morning, weeks after the deploy that installed it, and each of its attempts
# needs its OWN tag or the log stops being readable as a sequence of starts and
# health_since goes back to matching some previous start's readiness line.
#
# So the deploy emits this ONCE into the board's supervise.env, with the tag
# left as the placeholder @PANCRA_TAG@, and the watchdog substitutes a tag it
# mints itself before running it. What is on the board is therefore GENERATED
# from the block above rather than being a second copy of it -- which is the
# whole rule this file exists to enforce, and the rule that four hand-written
# copies of the start command broke last time.
start_template() {
   start_block '@PANCRA_TAG@'
}

# ---- ...AND HOW THE WATCHDOG ITSELF IS LAUNCHED ---------------------------
#
# Detached, exactly as the server is, and for the same reason twice over: a
# process backgrounded from an ssh session dies with the session, so a watchdog
# started that way would be gone the moment the deploy's terminal closed --
# leaving a deployment that reports supervision installed and has none. At boot
# there is no session to die with, but the block runs inside the board's own
# startup script, and one that blocks there holds up everything after it.
#
# ONE COPY, HERE, for the same reason the start command has one copy: this is
# the second thing in the deployment that puts a process into the background,
# and `make deploycheck` refuses a `setsid` anywhere outside this file.
#
# GUARDED, because it runs at boot from a script that is not ours. If the
# watchdog is missing -- a half-finished install, a wiped root -- the boot must
# carry on and the rest of that script must still run. supervise.sh itself
# refuses to start a second instance, so running this when one is already up is
# a no-op rather than a race.
supervisor_block() {
   cat <<EOF
if [ -x '$PANCRA_SUPERVISE' ]; then
   setsid '$PANCRA_SUPERVISE' run '$PANCRA_SUPERVISE_ENV' \
      </dev/null >>'$PANCRA_SUPERVISE_LOG' 2>&1 &
fi
EOF
}
