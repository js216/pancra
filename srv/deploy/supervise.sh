#!/bin/sh
# supervise.sh --- the thing that starts the server again when nothing else
# will. THIS FILE RUNS ON THE BOARD.
#
# Everything else in srv/deploy runs on the operator's machine and reaches the
# board over ssh, which is exactly why none of it could answer the two
# questions this file exists for:
#
#   THE SERVER EXITED AT 03:41 and nobody was awake. The deployment left one
#   detached process and a pid file; when that process died the pid file went
#   on naming it, the front door went on forwarding to a port with nothing
#   behind it, and every phone syncing to this board got connection refused
#   until somebody noticed and ran `make duodeploy` by hand. The recovery was
#   already written -- deploy.sh restarts a dead service -- it just needed a
#   person, at a keyboard, who had noticed.
#
#   THE BOARD REBOOTED and the service simply was not there afterwards. There
#   was no line in any startup script, because a deploy over ssh starts a
#   process, and a process is not a service.
#
# ---- WHAT IT KNOWS, AND WHERE FROM --------------------------------------
#
# Nothing by itself. Every path, every timeout and the start command itself
# come from the environment file the deploy generates -- the transcription of
# srv/deploy/pancra.conf that the deploy leaves at PANCRA_SUPERVISE_ENV. There
# is no second contract on the board: this script has no defaults to disagree
# with the contract about, and it refuses to run rather than guess.
#
# THE START COMMAND IS NOT IN THIS FILE. It is generated into that env file by
# start_template() in srv/deploy/start.sh, with the per-start tag left as a
# placeholder, and substituted here. srv/deploy/start.sh is the ONE place the
# stop/start sequence exists -- `make deploycheck` fails the build if it
# appears anywhere else -- and it stayed that way when this was added: what the
# board holds is a copy this deployment GENERATED, refreshed by every deploy,
# not a second one somebody has to remember to keep in step. The last time
# there were four hand-written copies, one of them hard-coded the port and the
# pem paths and never recorded the new pid.
#
# ---- WHAT IT WILL NOT DO ------------------------------------------------
#
# IT NEVER SIGNALS ANYTHING. It starts a service that is gone; it does not stop
# one that is there. That asymmetry is deliberate: the only handle it has on a
# process is a pid file, a pid file survives a reboot, and after a reboot the
# number in it belongs to whatever the kernel handed it to next -- an ntpd, a
# dropbear, the board's own sensor logger. A watchdog that killed what a stale
# pid file named would be a machine for shooting bystanders. Stopping is the
# operator's verb and it is done through the deployment scripts.
#
# IT DOES NOT RESTART A SERVER THAT IS RUNNING BADLY. A process that is up and
# not answering is a different fault with a different remedy: telling the two
# apart means probing the service, and acting on it means killing a process --
# see above. What this watches for is ABSENCE, which is the failure the board
# has actually had.
#
# Usage (on the board -- the deploy and the boot hook both do this for you):
#
#   supervise.sh run    <env>   the loop. One instance per board.
#   supervise.sh once   <env>   a single pass, for a board whose own timer
#                               (cron, a supervising init) would rather call
#                               something periodically than keep a process.
#   supervise.sh stop   <env>   stop the WATCHDOG. Not the server: see above.
#   supervise.sh status <env>   what it thinks, on one line each, exit 0 when
#                               the service is up and is the installed binary.
set -u

SUP_VERB=${1:-run}
SUP_ENV=${2:-}
[ -n "$SUP_ENV" ] || {
   echo "supervise.sh: no environment file given." >&2
   echo "  usage: supervise.sh run|once|stop|status <env>" >&2
   echo "  The deploy generates it from srv/deploy/pancra.conf; without it" >&2
   echo "  this script knows no paths at all, and inventing some is how a" >&2
   echo "  second contract gets born." >&2
   exit 2
}
[ -f "$SUP_ENV" ] || {
   echo "supervise.sh: $SUP_ENV is not there, so there is nothing to" >&2
   echo "  supervise. Re-run the deploy: it writes this file." >&2
   exit 2
}
# shellcheck disable=SC1090
. "$SUP_ENV"

# EVERY ONE OF THEM, CHECKED. A missing value under `set -u` would kill the
# loop at the first pass -- at three in the morning, in the background, with
# the reason in a log nobody is reading. An env file written by an older deploy
# is exactly how that happens.
for v in PANCRA_BIN PANCRA_PID PANCRA_LOG PANCRA_READY_LINE PANCRA_DOWN \
   PANCRA_LOCK PANCRA_LOCK_STALE PANCRA_SUPERVISE_PID PANCRA_SUPERVISE_LOG \
   PANCRA_SUPERVISE_INTERVAL PANCRA_SUPERVISE_READY PANCRA_SUPERVISE_BACKOFF \
   PANCRA_SUPERVISE_GRACE; do
   eval "val=\${$v:-}"
   [ -n "$val" ] || {
      echo "supervise.sh: $SUP_ENV does not set $v." >&2
      echo "  It was written by a deploy that did not know about it; run a" >&2
      echo "  deploy from a checkout that does." >&2
      exit 2
   }
done
command -v pancra_start_template >/dev/null 2>&1 || {
   echo "supervise.sh: $SUP_ENV carries no start command." >&2
   echo "  It is generated by start_template() in srv/deploy/start.sh and is" >&2
   echo "  the only copy of the start this board has; without it a restart" >&2
   echo "  would have to be invented here, which is the one thing this" >&2
   echo "  deployment does not allow." >&2
   exit 2
}

# ---- SAYING SO ----------------------------------------------------------
#
# Into the watchdog's OWN log, never the server's. sync.log is read by
# health.sh as a sequence of starts delimited by `=== pancra <tag> ===`
# banners, and a line of commentary between a banner and its readiness line is
# a watchdog editing the evidence somebody else is reading.
#
# Every line says WHEN, because the only question ever asked of this log is
# "what happened while I was asleep", and a line without a time cannot answer
# it. The board has no RTC and its clock is wrong until ntpd catches up, which
# is worth knowing about the first lines after a boot.
note() {
   printf '%s supervisor[%s]: %s\n' \
      "$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo '????')" "$$" "$*" \
      >> "$PANCRA_SUPERVISE_LOG" 2>/dev/null || true
}

now() { date -u +%s 2>/dev/null || echo 0; }

# ---- ONE WATCHDOG PER BOARD ---------------------------------------------
#
# TWO OF THEM IS WORSE THAN NONE. Both would see the service gone at the same
# instant, both would run the start command, and the two servers would share
# one data directory and one pid file -- with the file naming whichever wrote
# it last, so every later stop kills one of the two and leaves the other. And
# two is not exotic: the deploy launches one at the end of every deploy, and
# the boot hook launches one at every boot, and those meet the first time
# anybody deploys to a board that has just been rebooted.
#
# `mkdir` is the claim, for the reason lock.sh gives at length: it is the one
# create-or-fail primitive that is atomic on every filesystem, and `set -C`
# over a lockfile is not.
#
# AND THE CLAIM IS BROKEN WHEN ITS HOLDER IS NOT THERE -- with the pid checked
# for IDENTITY, not merely for life. This directory survives a reboot; the pid
# in it does not, and after a reboot that number belongs to something else
# entirely. A claim tested with `kill -0` alone would therefore be honoured
# for ever after the first reboot, and the watchdog would refuse to start on
# every boot from then on -- silently, having been installed precisely to
# survive reboots. /proc/<pid>/cmdline naming this script is what tells the
# holder apart from its heir.
SUP_CLAIM=$PANCRA_SUPERVISE_PID.claim
sup_claim() {
   if mkdir "$SUP_CLAIM" 2>/dev/null; then
      sup_claim_write
      return 0
   fi
   other=$(cat "$SUP_CLAIM/pid" 2>/dev/null | tr -dc '0-9')
   if [ -n "$other" ] && kill -0 "$other" 2>/dev/null &&
      tr '\0' ' ' < "/proc/$other/cmdline" 2>/dev/null | grep -q 'supervise'; then
      return 1
   fi
   note "the previous watchdog (pid ${other:-none}) is gone; taking over"
   rm -rf "$SUP_CLAIM"
   mkdir "$SUP_CLAIM" 2>/dev/null || return 1
   sup_claim_write
   return 0
}
sup_claim_write() {
   printf '%s\n' "$$" > "$SUP_CLAIM/pid"
   printf '%s\n' "$$" > "$PANCRA_SUPERVISE_PID"
   # DROPPED ON EVERY EXIT, including the ones that are not `exit`: a claim
   # left behind by a watchdog that was killed is a board with no supervision
   # that refuses to have any -- and the reboot that would clear it is the
   # event this exists to survive.
   trap 'sup_release' EXIT
   trap 'sup_release; exit 130' INT
   trap 'sup_release; exit 143' TERM
}
sup_release() {
   [ "$(cat "$SUP_CLAIM/pid" 2>/dev/null | tr -dc '0-9')" = "$$" ] || return 0
   rm -rf "$SUP_CLAIM"
   rm -f "$PANCRA_SUPERVISE_PID"
}

# ---- IS THE SERVICE THERE, AND IS IT OURS? ------------------------------
#
# IDENTITY-BOUND, in the sense health.sh's exe_is established: a live pid and
# an installed binary are two facts about a board, not one. The two ways they
# come apart are both ordinary here:
#
#   * AFTER A REBOOT the pid file still holds a number from before it, and on a
#     freshly booted board the low numbers are handed straight back out. `kill
#     -0` then succeeds against ntpd, and a watchdog that asked only that would
#     conclude the server was running and never start it -- failing at exactly
#     the job it was installed for, in the case it was installed for.
#   * A REPLACED EXECUTABLE keeps its old inode open for the process still
#     running it, so a start that failed while the previous process survived
#     leaves a live pid, a readiness line, and a new file on disk that nothing
#     is running.
#
# /proc/<pid>/exe follows the inode rather than the path, which is precisely
# the distinction both cases turn on.
#
# CACHED ON THE PAIR (pid, what the binary looks like on disk), because this
# runs every few seconds for months on a board with a 1 GHz core: in the steady
# state -- same pid, same binary -- it is a `kill -0` and two string compares,
# and the two sha256 sums are computed only when one of them has changed.
# Anything that could make the answer different changes one of the two: a
# restart changes the pid, an install changes the binary's size or its mtime.
SUP_OK_PID=
SUP_OK_TOKEN=
srv_pid() { cat "$PANCRA_PID" 2>/dev/null | tr -dc '0-9'; }
srv_ok() {
   pid=$(srv_pid)
   [ -n "$pid" ] || return 1
   kill -0 "$pid" 2>/dev/null || return 1
   token=$(ls -ln "$PANCRA_BIN" 2>/dev/null)
   if [ "$pid" = "$SUP_OK_PID" ] && [ "$token" = "$SUP_OK_TOKEN" ]; then
      return 0
   fi
   want=$(sha256sum "$PANCRA_BIN" 2>/dev/null | awk '{print $1}')
   got=$(sha256sum "/proc/$pid/exe" 2>/dev/null | awk '{print $1}')
   [ -n "$want" ] && [ -n "$got" ] && [ "$want" = "$got" ] || return 1
   SUP_OK_PID=$pid
   SUP_OK_TOKEN=$token
   return 0
}

# 0 when this start's own readiness line is in the log after this start's own
# banner. The same claim health.sh makes over ssh, made here: the log is
# append-only, so "is `listening on port ...` in it" is satisfied by the start
# BEFORE this one -- which is how a server that starts and dies immediately
# gets called healthy, and a watchdog that believed that would count a failing
# start as a success and stop backing off.
#
# AND THE RANGE ENDS AT THE NEXT BANNER, so a later start's line cannot be read
# as this one's.
started_ok() {
   srv_ok || return 1
   sed -n "/=== pancra $1 ===/,/=== pancra /p" "$PANCRA_LOG" 2>/dev/null |
      grep -q "$PANCRA_READY_LINE"
}

# ---- IS SOMEBODY DELIBERATELY DOING THIS? -------------------------------
#
# The one rule that makes a watchdog safe to have: a service that is down
# BECAUSE AN OPERATOR IS WORKING ON IT must be left alone. deploy, rollback,
# rotate and restore all stop the server on purpose and start it again a moment
# later, and a watchdog that started the old binary in that window would give
# the board two servers on one data directory and a pid file naming whichever
# start wrote it last.
#
# TWO GATES, because they cover different lengths of time:
#
#   THE LOCK is held by the operation from before its first change until its
#   final verdict (lock.sh), so it covers the whole of a deploy including the
#   copy, the swap and the thirty-second health wait. It is the gate that
#   matters.
#   THE MARKER (PANCRA_DOWN) is raised by the stop itself and cleared by the
#   start (start.sh), so it also covers the case the lock does not: an
#   operation that has already dropped its lock and left the service down --
#   a rotation whose swap failed halfway, which stops exactly there and says
#   so.
#
# AND THIS WATCHDOG NEVER TAKES THE LOCK. It is not an operation: it installs
# nothing, moves nothing and races nobody, and a lock it held would be a lock
# the next deploy is refused by -- taken by a background process on the board
# that the refusal message could not usefully name. Worse, it would have to be
# taken at the moment the service is down, which is the moment an operator is
# most likely to be deploying. It reads the lock and stands aside; it does not
# join the queue.
#
# BOTH GATES EXPIRE, because both are files, and a file outlives the thing it
# was about. An ssh that dies between a stop and a start leaves a marker; a
# laptop closed mid-deploy leaves a lock. Obeying either for ever turns the
# watchdog into the reason the board stayed down -- the exact outage it was
# installed to end.
hands_off() {
   if [ -f "$PANCRA_DOWN" ]; then
      since=$(cat "$PANCRA_DOWN" 2>/dev/null | tr -dc '0-9')
      if [ -z "$since" ]; then
         # NO TIMESTAMP MEANS IT CANNOT BE AGED, and the choice then is between
         # obeying it for ever and ignoring it now. lock.sh makes the opposite
         # choice about a lock with no `since`, deliberately: a lock's holder
         # may still be working, and breaking it can corrupt an install. This
         # marker is only ever raised for the seconds between one stop and one
         # start, so an unreadable one is far likelier to be the remains of
         # something that died than a live operation -- and the lock below is
         # still there to protect a real one.
         note "the stop marker $PANCRA_DOWN has no timestamp, so it cannot" \
            "be aged; treating it as abandoned"
      else
         age=$(( $(now) - since ))
         if [ "$age" -ge 0 ] && [ "$age" -le "$PANCRA_SUPERVISE_GRACE" ]; then
            SUP_WHY="an operation stopped the service ${age}s ago"
            return 0
         fi
         note "the stop marker is ${age}s old (grace" \
            "${PANCRA_SUPERVISE_GRACE}s): whatever stopped the service never" \
            "started it again, so this is no longer a deliberate stop"
      fi
   fi
   if [ -d "$PANCRA_LOCK" ]; then
      lsince=$(cat "$PANCRA_LOCK/since" 2>/dev/null | tr -dc '0-9')
      lwho=$(cat "$PANCRA_LOCK/owner" 2>/dev/null)
      if [ -z "$lsince" ]; then
         SUP_WHY="an operation holds the board lock (${lwho:-nobody recorded})"
         return 0
      fi
      lage=$(( $(now) - lsince ))
      if [ "$lage" -ge 0 ] && [ "$lage" -le "$PANCRA_LOCK_STALE" ]; then
         SUP_WHY="${lwho:-an operation} has held the board lock for ${lage}s"
         return 0
      fi
      note "the board lock is ${lage}s old (PANCRA_LOCK_STALE" \
         "${PANCRA_LOCK_STALE}s) and held by ${lwho:-nobody recorded};" \
         "it is not evidence that anybody is still working"
   fi
   return 1
}

# ---- START IT -----------------------------------------------------------
#
# A TAG PER ATTEMPT, minted here, on the board -- the deploy cannot mint them
# for a watchdog that will still be restarting this service in a month. It
# names the start in the log, and the readiness line is then attributable to
# THIS attempt rather than to whichever start left the last one.
sup_tag() {
   printf 'watchdog-%s-%s' "$$" "$(now)"
}
sup_start() {
   tag=$(sup_tag)
   note "the service is not running ($1); starting it [$tag]"
   # THE ONE COPY OF THE START, generated by srv/deploy/start.sh into the env
   # file and handed a tag here. Piped into `sh` rather than eval'd so that a
   # template with a syntax error kills one attempt and is reported, instead of
   # killing the watchdog.
   pancra_start_template | sed "s|@PANCRA_TAG@|$tag|g" | sh
   i=0
   while [ "$i" -lt "$PANCRA_SUPERVISE_READY" ]; do
      if started_ok "$tag"; then
         note "the service is back: pid $(srv_pid), running the installed" \
            "binary, and it says it is listening [$tag]"
         return 0
      fi
      sleep 1
      i=$((i + 1))
   done
   return 1
}

# ---- ONE PASS -----------------------------------------------------------
#
# The whole policy, in the order the questions have to be asked.
SUP_FAILS=0
SUP_DELAY=$PANCRA_SUPERVISE_INTERVAL
SUP_WHY=
# Whether the last pass found somebody deliberately holding the service down,
# so the log says it once per operation rather than once per poll.
SUP_STANDING=0
sup_pass() {
   if srv_ok; then
      if [ "$SUP_FAILS" -gt 0 ]; then
         note "the service is up again after $SUP_FAILS failed start(s)"
      fi
      SUP_FAILS=0
      SUP_DELAY=$PANCRA_SUPERVISE_INTERVAL
      return 0
   fi
   SUP_WHY=
   if hands_off; then
      # SAID ONCE PER OPERATION, not once per pass: a deploy takes half a
      # minute and a five-second poll would write six identical lines into the
      # log for every deploy anybody ever does, which is how a log stops being
      # read at all.
      if [ "$SUP_STANDING" = 0 ]; then
         note "standing by: $SUP_WHY. This is a deliberate stop, not a crash."
         SUP_STANDING=1
      fi
      return 0
   fi
   if [ "$SUP_STANDING" = 1 ]; then
      note "the operation that was holding the service down is finished"
      SUP_STANDING=0
   fi
   why="the pid file names nothing alive"
   pid=$(srv_pid)
   if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
      # AND IT IS NOT KILLED. See the top of this file: after a reboot that pid
      # belongs to somebody else, and this watchdog does not shoot bystanders.
      why="pid $pid is alive but is NOT running $PANCRA_BIN"
      why="$why -- a stale pid file, or a start that failed while the"
      why="$why previous process survived"
   fi
   if sup_start "$why"; then
      SUP_FAILS=0
      SUP_DELAY=$PANCRA_SUPERVISE_INTERVAL
      return 0
   fi
   # ---- IT DID NOT COME UP -------------------------------------------
   #
   # DOUBLING, AND NEVER GIVING UP. The two wrong answers here are a tight
   # restart loop and a watchdog that stops trying, and the second is the
   # tempting one -- "it has failed five times, something is really wrong".
   # But what is usually wrong is fixed from outside and without a redeploy:
   # ntpd sets a clock that made the certificate look not-yet-valid, a full
   # filesystem is emptied, an operator drops a working pem in place. A
   # watchdog that gave up would leave the board down after the cause was
   # gone. So it keeps trying, at a rate that costs nothing: the delay doubles
   # to PANCRA_SUPERVISE_BACKOFF and stays there.
   #
   # The tight loop is the other half. A server that exits immediately -- a
   # certificate it refuses, a database from a newer build -- restarted every
   # five seconds is a second fault on top of the first: it fills sync.log with
   # the same paragraph until the filesystem the database lives on is full.
   SUP_FAILS=$((SUP_FAILS + 1))
   SUP_DELAY=$((SUP_DELAY * 2))
   [ "$SUP_DELAY" -le "$PANCRA_SUPERVISE_BACKOFF" ] ||
      SUP_DELAY=$PANCRA_SUPERVISE_BACKOFF
   note "start attempt $SUP_FAILS did not come up healthy within" \
      "${PANCRA_SUPERVISE_READY}s; backing off ${SUP_DELAY}s before the next." \
      "See $PANCRA_LOG for what the server said."
   return 1
}

case "$SUP_VERB" in
   run)
      sup_claim || {
         # NOT AN ERROR. The deploy launches one at the end of every deploy and
         # the boot hook launches one at every boot; the second of those to run
         # says so and exits 0, because a non-zero exit here would make a
         # perfectly correct deploy look like a failed one.
         note "another watchdog is already running; leaving it to it"
         exit 0
      }
      note "watching $PANCRA_BIN every ${PANCRA_SUPERVISE_INTERVAL}s" \
         "(pid file $PANCRA_PID, log $PANCRA_LOG)"
      while :; do
         sup_pass || true
         sleep "$SUP_DELAY"
      done
      ;;
   once)
      sup_claim || exit 0
      sup_pass || exit 1
      ;;
   stop)
      # THE WATCHDOG, NOT THE SERVICE. Stopping the server is the deployment's
      # job and is done with the deployment's own verbs; this exists so that
      # turning supervision OFF is a thing an operator can do on purpose,
      # rather than by hunting for a pid with `ps` at the point where they have
      # already decided the watchdog is in their way.
      sup_pid=$(cat "$PANCRA_SUPERVISE_PID" 2>/dev/null | tr -dc '0-9')
      [ -n "$sup_pid" ] || { echo "no watchdog is recorded as running"; exit 0; }
      # ---- AND THE NUMBER IS CHECKED BEFORE IT IS SIGNALLED -------------
      #
      # THE ONE PLACE THIS FILE SENDS A SIGNAL, so it is the one place it can
      # shoot a bystander -- and it reads a pid out of a file that survives a
      # reboot while the process in it does not. On a freshly booted board the
      # low numbers go straight back out, so this file names ntpd or dropbear
      # and `supervise.sh stop` would end it, successfully and silently. That
      # is the same defect stop_block in srv/deploy/start.sh was carrying, and
      # a watchdog is a worse place for it: nobody is watching the watchdog.
      #
      # THE CLAIM DIRECTORY ALREADY KNOWS HOW TO ASK THIS -- see sup_claim,
      # which will not take over from a pid whose /proc/<pid>/cmdline still
      # names this script. Asked here for the same reason and in the same way.
      # A pid file that names a stranger is quarantined rather than obeyed:
      # left in place, the next `stop` would try again, and the one after that.
      if ! tr '\0' ' ' < "/proc/$sup_pid/cmdline" 2>/dev/null |
           grep -q 'supervise'; then
         stale=$PANCRA_SUPERVISE_PID.stale-$(now)-$$
         mv "$PANCRA_SUPERVISE_PID" "$stale" 2>/dev/null ||
            rm -f "$PANCRA_SUPERVISE_PID"
         note "asked to stop pid $sup_pid, which is not a watchdog; NOT" \
            "signalling it. The pid file has been quarantined at $stale."
         echo "supervise.sh: pid $sup_pid is not a watchdog -- NOT signalling it." >&2
         echo "  $PANCRA_SUPERVISE_PID survived a reboot the process in it did" >&2
         echo "  not, and that number now belongs to something else. It has" >&2
         echo "  been moved to $stale; nothing is supervising this service." >&2
         exit 1
      fi
      kill "$sup_pid" 2>/dev/null || true
      i=0
      while kill -0 "$sup_pid" 2>/dev/null && [ $i -lt 25 ]; do
         sleep 0.2
         i=$((i + 1))
      done
      kill -0 "$sup_pid" 2>/dev/null && kill -9 "$sup_pid" 2>/dev/null
      rm -rf "$SUP_CLAIM"
      rm -f "$PANCRA_SUPERVISE_PID"
      note "stopped on request; NOTHING is supervising this service now"
      echo "watchdog $sup_pid stopped; the server itself was left running"
      ;;
   status)
      sup_pid=$(cat "$PANCRA_SUPERVISE_PID" 2>/dev/null | tr -dc '0-9')
      if [ -n "$sup_pid" ] && kill -0 "$sup_pid" 2>/dev/null; then
         echo "watchdog: running (pid $sup_pid)"
      else
         echo "watchdog: NOT running"
      fi
      if srv_ok; then
         echo "service:  running (pid $(srv_pid)), and it is $PANCRA_BIN"
      else
         echo "service:  NOT running the installed binary"
      fi
      SUP_WHY=
      if hands_off; then
         echo "stopped:  on purpose -- $SUP_WHY"
      fi
      srv_ok
      ;;
   *)
      echo "supervise.sh: unknown verb '$SUP_VERB'" >&2
      echo "  usage: supervise.sh run|once|stop|status <env>" >&2
      exit 2
      ;;
esac
