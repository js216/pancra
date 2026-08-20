# health.sh --- IS THE SERVICE ACTUALLY SERVING? One answer, for every path.
#
# Sourced (not executed) by deploy.sh, rollback.sh, restore.sh and rotate.sh,
# after pancra.conf and with $SSH set.
#
# It exists because the three paths had three different answers, and two of
# them were weaker than the one that mattered:
#
#   - deploy.sh checked the pid, the log, AND the public URL;
#   - rollback.sh and restore.sh checked the pid and the log, inline, and
#     called that "running again" / "restored".
#
# So the two paths taken when something has ALREADY gone wrong -- the two
# where being sure matters most -- were the two that never asked whether the
# service could be reached. A rollback could report success with the process
# up and the port answering nothing.
#
# THE READY LINE MUST BE FROM THIS START. The log is append-only, so
# `tail -n 50 | grep listening` matches the readiness line of the PREVIOUS
# start just as happily as this one: restart a server that then dies
# immediately, and the check passes on the evidence of the run before it.
# Every wait therefore takes a MARK first -- the log's size before the restart
# -- and looks only at what was written after it.

# The log's current size, as a per-start marker. 0 if there is no log yet.
log_mark() {
   $SSH "$PANCRA_HOST" "wc -c < '$PANCRA_LOG' 2>/dev/null || echo 0" |
      tr -dc '0-9'
}

# 1 when the process is alive AND said it was listening AFTER $1 bytes of log.
# With no argument it accepts any readiness line, which is only right for a
# service that was already running before this script started.
#
# $2, WHEN GIVEN, IS THIS START'S TAG (see start.sh): the readiness line must
# come after the banner naming it, not merely after a byte offset. The offset
# alone is a claim about the log's length at a moment in the past, and a log
# that is truncated or rotated in between makes it point into the middle of an
# older start -- so the check reads a previous run's readiness line and calls
# a server that never came up healthy.
#
# THE TAG REPLACES THE OFFSET; IT IS NOT ADDED TO IT. Applying both looks
# stricter and is strictly worse, because the offset is the half that a
# rotation invalidates: truncate the log between the mark and the check and
# `tail -c +$((mark+1))` skips PAST this start's own banner, the search finds
# nothing, and a server that came up perfectly is reported unhealthy -- which
# in deploy.sh means a good build is automatically rolled back. The banner is
# unique to one start, so once it is found the offset has nothing left to add.
#
# AND THE RANGE ENDS AT THE NEXT BANNER, not at end of file. `,$p` prints
# everything after our banner, including the output of any LATER start: two
# deploys racing, ours dead and theirs up, and ours reads THEIR readiness line
# and calls itself healthy. That is the same wrong-start bug the tag exists to
# end, reached from the other direction.
health_since() {
   mark=${1:-0}
   tag=${2:-}
   if [ -n "$tag" ]; then
      $SSH "$PANCRA_HOST" "sh -s" <<EOF >/dev/null 2>&1
set -eu
pid=\$(cat '$PANCRA_PID')
kill -0 "\$pid"
sed -n '/=== pancra $tag ===/,/=== pancra /p' '$PANCRA_LOG' |
   grep -q '$PANCRA_READY_LINE'
EOF
      return
   fi
   $SSH "$PANCRA_HOST" "sh -s" <<EOF >/dev/null 2>&1
set -eu
pid=\$(cat '$PANCRA_PID')
kill -0 "\$pid"
tail -c +$((mark + 1)) '$PANCRA_LOG' | grep -q '$PANCRA_READY_LINE'
EOF
}

# ---- IS THE LIVE PROCESS RUNNING THE ARTIFACT WE INSTALLED? ---------------
#
# THE HASH ON DISK AND THE PID IN THE FILE WERE NEVER CONNECTED. A deploy
# hashed the file it copied, then separately observed that some process was
# alive and that some log line had appeared -- three facts about a board, none
# of which had to be about the same program. The case is not exotic: replacing
# a running executable by rename leaves the OLD inode open, so a start that
# fails while the previous process survives gives a live pid, a readiness line
# from before, and a new binary on disk that nothing is running. Every check
# passes and the deploy reports the new build live.
#
# /proc/<pid>/exe follows the inode, not the path, which is exactly the
# distinction that matters here.
exe_is() {
   # ${1:-}, not $1: every caller here is inside a script that runs under
   # `set -eu`, where reading an unset $1 does not return an error -- it kills
   # the shell. A health check that aborts the deploy it was asked about is
   # the one failure mode this file must not have.
   want=${1:-}
   [ -n "$want" ] || return 1
   got=$($SSH "$PANCRA_HOST" "sh -s" <<EOF 2>/dev/null | tr -dc '0-9a-f'
pid=\$(cat '$PANCRA_PID' 2>/dev/null) || exit 1
sha256sum "/proc/\$pid/exe" 2>/dev/null | awk '{print \$1}'
EOF
)
   [ -n "$got" ] && [ "$got" = "$want" ]
}

# ---- DOES THE PUBLIC ROUTE ARRIVE HERE? -----------------------------------
#
# The listener, asked directly, without going through the front door. On its
# own it says little; paired with the public probe it is the whole point --
# the public name serving a page with the marker proves SOME backend is up,
# and a stale NAT rule or a proxy pointing at a retired instance satisfies
# that just as happily as this one does. Both answering is what ties the
# public route to the process this deploy started.
# ASKED FROM THE BOARD, over the ssh channel that is already open, against
# 127.0.0.1. Two reasons, and the second is the stronger one:
#
#   - PANCRA_HOST IS AN SSH DESTINATION, not necessarily a hostname. Building
#     a URL out of it assumed curl and ssh resolve names the same way, and
#     they do not: ssh reads ~/.ssh/config, so an alias with its own HostName
#     or User gives ssh the board and curl something that does not resolve --
#     or, worse, resolves somewhere else. It also required the deploy machine
#     to reach the board's PANCRA_PORT directly, which is exactly what a NAT
#     or proxy front door exists to make unnecessary.
#   - 127.0.0.1 CANNOT BE ANYTHING ELSE. A probe from outside can be satisfied
#     by whatever the address currently reaches; a probe on the loopback of
#     the machine holding the pid file can only be answered by a process on
#     that machine. That is a stronger claim about identity, which is the
#     whole point of this check.
#
# --insecure because the certificate is issued for the PUBLIC name and this
# connection is to 127.0.0.1: verifying the hostname here would fail on a
# perfectly correct deployment. TLS as a stranger sees it is url_ok's job.
backend_ok() {
   $SSH "$PANCRA_HOST" "sh -s" <<EOF >/dev/null 2>&1
set -eu
$PANCRA_BACKEND_PROBE | grep -qi '$PANCRA_URL_MARKER'
EOF
}

# ---- THE FRONT DOOR ---------------------------------------------------
#
# WHAT SITS BETWEEN THE PUBLIC PORT AND THIS LISTENER, in one sentence, for a
# message an operator can act on. PANCRA_FRONT/PANCRA_FRONT_OWNER are declared
# in pancra.conf; see the long note there for why they exist.
#
# THE PATTERNS HERE MUST MATCH front_declared's BELOW -- `proxy:?*`, not
# `proxy:*`. A bare `proxy:` is not a named component, and the two functions
# disagreeing about that is how a value one of them rejects gets described by
# the other as ": terminates TLS on 443", with an empty name.
front_door() {
   case "$PANCRA_FRONT" in
      direct) printf 'direct: the board answers on %s itself' \
                 "$PANCRA_PUBLIC_PORT" ;;
      nat) printf 'NAT: something forwards %s -> %s, owned by %s' \
              "$PANCRA_PUBLIC_PORT" "$PANCRA_PORT" "$PANCRA_FRONT_OWNER" ;;
      proxy:?*) printf '%s: terminates TLS on %s and forwards to %s, owned by %s' \
                   "${PANCRA_FRONT#proxy:}" "$PANCRA_PUBLIC_PORT" \
                   "$PANCRA_PORT" "$PANCRA_FRONT_OWNER" ;;
      *) printf 'UNDECLARED' ;;
   esac
}

# 1 when the topology has been declared. A deploy is not verified until this
# holds: every check below tests the public URL, so an unexplained pass is
# resting on a component nobody has written down and nobody is maintaining --
# and the first time it breaks, the procedure has no step for it.
front_declared() {
   case "$PANCRA_FRONT" in
      # DIRECT MEANS THE SAME PORT, and saying otherwise is not a topology,
      # it is two contradictory statements. Declared direct while the public
      # port and the listener disagree, front_door() would report "the board
      # answers on 443 itself" about a server listening on 8443 -- the exact
      # confident-and-wrong message this whole mechanism exists to end. It is
      # also what a hurried operator produces: set PANCRA_FRONT=direct, leave
      # PANCRA_PUBLIC_PORT at its 443 default, change nothing else.
      direct)
         [ "$PANCRA_PUBLIC_PORT" = "$PANCRA_PORT" ] || return 1
         ;;
      nat | proxy:?*) ;;
      *) return 1 ;;
   esac
   [ -n "$PANCRA_FRONT_OWNER" ] && [ "$PANCRA_FRONT_OWNER" != unset ]
}

# The refusal, written once, because it is needed in two places.
front_refuse() {
   echo "the deployment's front door is UNDECLARED." >&2
   echo "  The server listens on $PANCRA_PORT and $PANCRA_URL is served on" >&2
   echo "  $PANCRA_PUBLIC_PORT. Something maps one to the other, and until" >&2
   echo "  pancra.conf says what, no procedure here can restore it, back it" >&2
   echo "  up, or roll it back -- so nothing here is reported verified." >&2
   echo "  Set PANCRA_FRONT to direct, nat, or proxy:<name>, and" >&2
   echo "  PANCRA_FRONT_OWNER to who maintains it." >&2
}

# AND IT IS A PRECONDITION, checked before anything is installed. That is not
# tidiness: when the only check lived in wait_healthy_since, deploy.sh reached
# it AFTER the swap and the restart, so an undeclared front door arrived as
# "the new build is NOT healthy". A perfectly good deploy was therefore
# installed, refused, and AUTOMATICALLY ROLLED BACK; the rollback ran the same
# check and failed it for the same reason; and the operator was told "the board
# is DOWN" twice about a board that was up and serving the whole time.
#
# A topology nobody has written down is a reason not to START. It is not
# evidence about the build, and it must never be reported as an outage.
require_front() {
   front_declared || { front_refuse; return 1; }
}

# ...and the public URL answers with a page we recognise. A DIFFERENT
# question from health_since: the process being up is not the service being
# reachable, and the gap between them is a certificate that will not load, a
# port nothing forwards, or a proxy pointing at the wrong backend.
#
# WHICH OF THOSE IT IS depends on the front door, so the failure names it:
# "the public URL did not answer" sends an operator to the server they can see
# is running, and the component that actually broke is the one they were never
# told about.
url_ok() {
   body=$(curl --fail --silent --show-error --max-time 20 "$PANCRA_URL" || true)
   printf '%s' "$body" | grep -qi "$PANCRA_URL_MARKER"
}

# Up to 30 seconds for this start to report itself listening, then the public
# URL. 1 only when BOTH hold. $1 is the mark taken before the restart.
# $2 this start's tag (see start.sh), $3 the sha256 of the artifact that was
# installed. Both optional, and both are how the evidence stops being three
# unrelated observations about a board.
wait_healthy_since() {
   mark=${1:-0}
   wtag=${2:-}
   wexe=${3:-}
   i=0
   while [ $i -lt 30 ]; do
      if health_since "$mark" "$wtag"; then break; fi
      sleep 1
      i=$((i + 1))
   done
   health_since "$mark" "$wtag" || return 1
   if [ -n "$wexe" ] && ! exe_is "$wexe"; then
      echo "the process that is running is NOT the binary that was installed." >&2
      echo "  A live pid and a new file on disk are two facts, not one:" >&2
      echo "  replacing a running executable by rename leaves the old inode" >&2
      echo "  open, so a failed start with the previous process still up looks" >&2
      echo "  exactly like a successful one." >&2
      return 1
   fi
   if ! backend_ok; then
      echo "the process is listening but 127.0.0.1:$PANCRA_PORT did not answer." >&2
      echo "  That is this server, addressed directly -- not the front door." >&2
      return 1
   fi
   # Kept here as well as in the precondition, so a caller that never ran
   # require_front still cannot report a deploy verified over a front door
   # nobody has described.
   if ! front_declared; then
      front_refuse
      return 1
   fi
   if ! url_ok; then
      echo "this backend answers but $PANCRA_URL does not." >&2
      echo "  Front door: $(front_door)" >&2
      echo "  The server itself is serving, so the break is between the" >&2
      echo "  public name and this board -- not the server." >&2
      return 1
   fi
   return 0
}
