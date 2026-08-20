# testlib.sh --- the assertion vocabulary the shell suites share.
#
# Sourced, never executed. Every suite that talks to a running server used to
# invent its own helpers, and each one invented a different way to be WRONG:
#
#   * a check whose tool was absent printed PASS (interop.sh asked sqlite3 for
#     a row count, got nothing because sqlite3 is not installed, and reported
#     "the server's own database holds exactly the surviving row");
#   * a check that matched a substring of the BODY passed whatever status the
#     server sent, so a 404 or a 500 carrying the right word was a pass;
#   * a measurement read out of /proc fell back to 0 when the read failed, and
#     0 was inside the threshold, so an unreadable /proc was a green test;
#   * a fixed port made two concurrent suites fight for one listener, and a
#     `pkill -f` cleanup killed the other suite's server -- or its own script;
#   * a request that FAILED left the previous response's body in place, so the
#     next assertion read the last page that worked and passed on it;
#   * a port was asked of the kernel, released, and only then handed to a
#     server, so "something answered there" was never the same claim as "our
#     server is serving it".
#
# So the rules here are: a helper that cannot RUN fails, a request is asserted
# on status AND headers AND body AND the database it changed, every response
# artifact is destroyed before the request that replaces it, no request is made
# without stating the status it expects, and every server is started on a port
# the KERNEL confirms that server owns and killed by the pid we captured.
#
# srv/test/harnesstest.sh is the test OF this file: it breaks each rule on
# purpose and requires the failure. A rule added here without a case there is a
# rule nobody has watched fail.
#
# POSIX sh: srv/test/tlstest.sh is /bin/sh. No `local`, no arrays, no bashisms.

# The suite-wide verdict. Every helper below sets it; every caller exits on it.
fail=0

# ---------------------------------------------------------------- diagnostics

t_ok()   { printf '  ok   %s\n' "$1"; }
t_bad()  { printf '  FAIL %s\n' "$1"; fail=1; }
# One line, bounded: a failure that prints a whole HTML page buries the others.
t_show() { printf '%s' "$1" | tr '\n' '|' | cut -c1-300; }

# ------------------------------------------------- a verdict from a SUBSHELL
#
# `fail=1` DOES NOT ESCAPE A COMMAND SUBSTITUTION, and the busiest helper in
# this file is used inside one:
#
#     ck "the page says so" "Sign in" "$(req 200 GET /)"
#
# `$(...)` is a subshell, so anything req assigns -- including the verdict --
# is discarded when it exits. That is not a style problem, it is the reason a
# broken request could go unreported: the one helper that KNOWS the request
# failed is the one helper that cannot say so.
#
# So a failure raised inside a subshell is written to a FILE, and t_end (which
# every suite calls in its parent shell, just before it prints its verdict)
# folds those records into `fail`. The diagnostic also goes to stderr straight
# away, so a human reading the run sees it next to the assertions it explains
# rather than only in the summary. Stderr, not stdout: stdout inside `$(...)`
# is the RESPONSE BODY, and a diagnostic printed there would be spliced into
# the very text the caller is about to assert on.
t_defer() { # t_defer <message>
   printf '  FAIL %s\n' "$1" >&2
   if [ -n "${T_TMP:-}" ] && [ -d "$T_TMP" ]; then
      printf '%s\n' "$1" >> "$T_TMP/.deferred"
   else
      # Nowhere to leave the record, so the only safe thing is to be loud in
      # the parent's own variable. This path is reached only when a suite has
      # not set T_TMP, which every helper below also refuses to work without.
      fail=1
   fi
}

# THE LAST WORD. Called by every suite immediately before it prints its
# verdict and exits, in the parent shell, so that:
#
#   * a request whose curl FAILED becomes a failure even when the suite never
#     asserted anything about it (the case item 66 was filed for: a request
#     that quietly did not happen used to leave the previous response in place
#     and no evidence anywhere);
#   * the port memo pick_port keeps is removed.
#
# It is safe to call in a suite that made no requests at all.
t_end() {
   if [ -n "${T_TMP:-}" ] && [ -s "$T_TMP/.deferred" ]; then
      while IFS= read -r te_line; do
         [ -n "$te_line" ] || continue
         printf '  FAIL %s\n' "$te_line"
         fail=1
      done < "$T_TMP/.deferred"
      rm -f "$T_TMP/.deferred"
   fi
   rm -f "$T_PORTMEMO"
}

# ------------------------------------------------------- required tools
#
# A SKIP IS A FAILURE UNLESS SOMEBODY ASKED FOR ONE. These suites are the only
# thing exercising the wire protocol, the TLS layer and the storage; a missing
# tool that quietly removes assertions leaves the gate green over untested
# code, which is worse than a red one.
need() { # need <tool> <what it is for>
   if command -v "$1" >/dev/null 2>&1; then
      return 0
   fi
   if [ "${ALLOW_SKIP:-0}" = "1" ]; then
      printf '  skip %s absent: %s NOT TESTED (ALLOW_SKIP=1)\n' "$1" "$2"
      return 1
   fi
   printf '  FAIL %s is required: %s could not be tested.\n' "$1" "$2"
   printf '       install it, or re-run with ALLOW_SKIP=1 to accept that.\n'
   fail=1
   return 1
}

# ------------------------------------------------------------------ the port
#
# ASK THE KERNEL, do not pick a number. `make check -j4` runs srvcheck,
# srvasan, tlstest, tlsasan and interoptest at once, and hardcoded ports meant
# they raced for a listener: the loser's assertions all got an empty response,
# which reads exactly like a protocol break.
#
# ASKING IS NOT THE SAME AS HOLDING, which is the whole of item 67. bind(0)
# tells the kernel to pick a free port and then the socket is CLOSED, because
# there is no way to hand an open listener to `./sync <port>` -- the server
# calls socket()/bind()/listen() itself (srv/http.c, http_listen) and takes a
# port number on the command line, not a file descriptor. Between the close
# and the server's own bind there is a window, and with `make -j8` passing its
# jobserver into the sanitizer submakes there are a dozen suites in it at once.
# Three things can happen in that window, and only the first one is honest:
#
#   1. our server binds the port. Fine.
#   2. somebody else binds it first, our server's bind fails with EADDRINUSE
#      and it exits. wait_ready notices the pid is gone and the suite says
#      "the server did not start" -- a red run naming the wrong cause, which
#      is the flake three separate agents have chased in this tree.
#   3. somebody else's SERVER binds it first and ours dies. Our pid may not
#      have exited yet when wait_ready looks, so `kill -0` succeeds; the poll
#      then curls the port and the OTHER suite's server answers 200. wait_ready
#      returns success and the whole suite runs against a stranger's database.
#      In faulttest that means every fault-injection assertion runs against a
#      server with no faults injected -- a green suite over untested code.
#
# Case 3 is why the fix is not "retry until the bind works": a retry loop
# cannot tell case 3 from success. What closes it is asking the kernel WHICH
# PROCESS holds the listening socket, which is what own_port does below, and
# what wait_ready now requires before it reports a server ready.
#
# The memo is a FILE because pick_port is called as `PORT=$(pick_port)` -- a
# subshell, where a shell variable would not survive. Two calls in a row
# (restoredrill.sh asks for two ports, back to back) could otherwise hand back
# the SAME number: the first socket is closed before the second bind, so the
# kernel is free to offer it again, and the second server then dies on a port
# the first one is about to take.
T_PORTMEMO=${TMPDIR:-/tmp}/pancra-testports-$$
rm -f "$T_PORTMEMO"

pick_port() {
   python3 - "$T_PORTMEMO" <<'PY'
import socket, sys

memo = sys.argv[1]
try:
    with open(memo) as f:
        taken = {int(line) for line in f if line.strip()}
except OSError:
    taken = set()

for _ in range(64):
    s = socket.socket()
    # NO SO_REUSEADDR: the point of this bind is to find out whether the port
    # is free, and SO_REUSEADDR would let it succeed over a socket in
    # TIME_WAIT -- a port a server can indeed bind, but one an in-flight
    # connection may still be using.
    #
    # 0.0.0.0, not 127.0.0.1, because that is what srv/http.c binds
    # (INADDR_ANY). A port held by another process on some other interface is
    # free on the loopback address and NOT free for our server, so probing the
    # loopback answers a different question from the one being asked.
    try:
        s.bind(("0.0.0.0", 0))
        port = s.getsockname()[1]
    finally:
        s.close()
    if port in taken:
        continue
    # Recorded BEFORE it is printed, and with O_APPEND, so that a second
    # pick_port in this run -- even from a parallel subshell of it -- cannot
    # be handed the same number.
    with open(memo, "a") as f:
        f.write("%d\n" % port)
    print(port)
    sys.exit(0)
sys.exit(1)
PY
}

# WHICH PROCESS IS LISTENING ON A PORT, asked of the kernel rather than
# inferred from the fact that something answered.
#
# /proc/net/tcp lists every socket with its state and its inode;
# /proc/<pid>/fd/* is a symlink to `socket:[<inode>]` for each socket a process
# holds. Matching the two gives the owner. `ss -ltnp` would too, but it is not
# installed everywhere and needs privilege to name pids that are not ours --
# and ours is the only pid this has to name.
#
# Prints the owning pids, one per line. Exit status:
#   0  a listener was found and at least one pid was attributed to it
#   1  nothing is listening on that port
#   2  the question could not be asked (no /proc/net/tcp)
#   3  something is listening and no pid could be attributed (another user's)
port_owner() { # port_owner <port>
   python3 - "$1" <<'PY'
import os, sys

port = int(sys.argv[1])
inodes = set()
found = False
seen_a_table = False
for path in ("/proc/net/tcp", "/proc/net/tcp6"):
    try:
        with open(path) as f:
            next(f, None)            # the header row
            seen_a_table = True
            for line in f:
                col = line.split()
                if len(col) < 10:
                    continue
                # 0A is TCP_LISTEN. Anything else is a connection, and a
                # connection to the port is not ownership of it.
                if col[3] != "0A":
                    continue
                if int(col[1].rsplit(":", 1)[1], 16) != port:
                    continue
                found = True
                inodes.add(col[9])
    except OSError:
        continue
if not seen_a_table:
    sys.stderr.write("port_owner: /proc/net/tcp cannot be read\n")
    sys.exit(2)
if not found:
    sys.exit(1)

pids = []
for name in os.listdir("/proc"):
    if not name.isdigit():
        continue
    d = "/proc/%s/fd" % name
    try:
        entries = os.listdir(d)
    except OSError:
        continue                     # somebody else's process: not ours to see
    for fd in entries:
        try:
            target = os.readlink(os.path.join(d, fd))
        except OSError:
            continue
        if target.startswith("socket:[") and target[8:-1] in inodes:
            pids.append(name)
            break
if not pids:
    sys.exit(3)
for p in pids:
    print(p)
PY
}

# Does <pid> hold the listening socket on <port>? This is the assertion that
# closes case 3 above: "something answered on that port" and "the process we
# started is serving that port" are different claims, and only the second one
# means the suite is testing its own server.
#
# A question that cannot be ASKED is a failure, not a pass -- the same rule as
# `need` above, and for the same reason: on a machine with no procfs this
# check would otherwise wave every collision through. ALLOW_SKIP=1 is the
# deliberate opt-out.
own_port() { # own_port <pid> <port>
   op_out=$(port_owner "$2" 2>/dev/null)
   op_rc=$?
   case $op_rc in
   0) for op_p in $op_out; do
         [ "$op_p" = "$1" ] && return 0
      done
      T_PORT_OWNER=$op_out
      return 1 ;;
   1) T_PORT_OWNER="nothing at all"
      return 1 ;;
   2) if [ "${ALLOW_SKIP:-0}" = "1" ]; then
         return 0
      fi
      T_PORT_OWNER="UNKNOWN: /proc/net/tcp could not be read, so which process\
 owns the port could not be established (re-run with ALLOW_SKIP=1 to accept\
 that)"
      return 1 ;;
   *) T_PORT_OWNER="a process this user cannot see"
      return 1 ;;
   esac
}

# A NAMED assertion, for the suites that want the ownership check in the
# transcript rather than only inside wait_ready's verdict.
ck_owns() { # ck_owns <name> <pid> <port>
   if own_port "$2" "$3"; then
      t_ok "$1 (pid $2 owns port $3)"
   else
      t_bad "$1: pid $2 does NOT own port $3 -- that belongs to ${T_PORT_OWNER}"
   fi
}

# Poll until it answers, rather than sleeping a guess. A sleep that is too
# short fails a correct server on a loaded machine; one that is too long is
# paid by every run forever.
#
# The pid is watched as well as the port: a server that DIED is not a server
# that is still starting, and waiting the full budget for it hides the reason.
#
# AND THE PID MUST OWN THE PORT. A poll that stops at "somebody answered" is
# satisfied by another suite's server on the same number (case 3 above), and
# every assertion afterwards is then made against a stranger. The port is read
# out of the url, so all fourteen launch sites in this tree gained the check
# without changing a line: they all already pass the pid and a url with an
# explicit port.
wait_ready() { # wait_ready <pid> <url> [extra curl args...]
   wr_pid=$1
   wr_url=$2
   shift 2
   wr_port=$(printf '%s' "$wr_url" |
             sed -n 's|^[a-zA-Z][a-zA-Z0-9+.-]*://[^/]*:\([0-9][0-9]*\).*|\1|p')
   if [ -z "$wr_port" ]; then
      printf 'wait_ready: %s names no port, so ownership cannot be checked\n' \
         "$wr_url" >&2
      return 1
   fi
   wr_i=0
   while [ "$wr_i" -lt 100 ]; do
      if ! kill -0 "$wr_pid" 2>/dev/null; then
         return 1
      fi
      if curl --fail --silent --max-time 2 "$@" "$wr_url" >/dev/null 2>&1 &&
         own_port "$wr_pid" "$wr_port"; then
         return 0
      fi
      sleep 0.1
      wr_i=$((wr_i + 1))
   done
   # The timeout has a REASON, and "it never answered" and "it answered and the
   # port is somebody else's" call for different next moves.
   printf 'wait_ready: gave up on %s (port %s is held by %s)\n' \
      "$wr_url" "$wr_port" "${T_PORT_OWNER:-nothing this check could name}" >&2
   return 1
}

# PICK, LAUNCH, VERIFY -- and on a collision, do all three again.
#
# The launch function is called with the chosen port and must background
# exactly one process, leaving its pid in T_PID. On success T_PORT holds the
# port, T_PID the pid, and the kernel has confirmed that that pid owns that
# port; the caller can build its urls from T_PORT and trust them.
#
# The retry is what turns case 2 above from a red run naming the wrong cause
# into a run that simply proceeds. It is NOT what makes the result
# trustworthy: wait_ready's ownership check is. A retry without it would
# happily "recover" onto another suite's server.
serve() { # serve <label> <scheme> <ready-path> <launch-fn> [curl args...]
   sv_label=$1
   sv_scheme=$2
   sv_path=$3
   sv_fn=$4
   shift 4
   sv_try=1
   while [ "$sv_try" -le 5 ]; do
      T_PORT=$(pick_port)
      if [ -z "${T_PORT:-}" ]; then
         printf '%s: the kernel would not name a free port\n' "$sv_label" >&2
         return 1
      fi
      T_PID=
      "$sv_fn" "$T_PORT"
      if [ -z "${T_PID:-}" ]; then
         printf '%s: the launch function left no pid in T_PID\n' "$sv_label" >&2
         return 1
      fi
      if wait_ready "$T_PID" "$sv_scheme://127.0.0.1:$T_PORT$sv_path" "$@"; then
         return 0
      fi
      # WHY IT DID NOT COME UP DECIDES WHETHER TO TRY AGAIN.
      #
      # Retrying a server that died of its own accord would turn one honest
      # failure into five, and faulttest.sh has cases where refusing to start
      # is the CORRECT behaviour under test -- a retry loop there would be
      # five launches to reach an answer already known.
      #
      # So the kernel is asked who holds the port. Somebody else holding it is
      # the collision this loop exists for; nobody holding it means our own
      # process failed for its own reasons and the caller should hear about
      # that. (If the intruder let go between our bind failing and this look,
      # the caller hears the old "it did not start" -- one lost retry, not a
      # wrong answer.)
      sv_own=$(port_owner "$T_PORT" 2>/dev/null)
      sv_orc=$?
      kill "$T_PID" 2>/dev/null
      wait "$T_PID" 2>/dev/null
      sv_retry=0
      case $sv_orc in
      0) case " $sv_own " in
         *" $T_PID "*) ;;             # ours, and it still would not answer
         *) sv_retry=1 ;;             # somebody else's: the collision
         esac ;;
      3) sv_retry=1                   # held by a process this user cannot see
         sv_own="a process this user cannot see" ;;
      esac
      if [ "$sv_retry" = 0 ]; then
         T_PID=
         return 1
      fi
      printf '%s: port %s is held by %s, not by us (attempt %d of 5); trying another port\n' \
         "$sv_label" "$T_PORT" "$sv_own" "$sv_try" >&2
      sv_try=$((sv_try + 1))
   done
   T_PID=
   return 1
}

# THE MIRROR IMAGE: a server that is SUPPOSED to refuse to start.
#
# tlstest starts servers with a deliberately broken certificate or key and
# asserts that they exit, and that the log says WHY. A stolen port breaks that
# in the most confusing way available: the process exits, which satisfies "it
# did not start", and the log says "bind: Address already in use" instead of
# the diagnostic under test -- so the case fails, naming the credential rule,
# for a reason that has nothing to do with credentials. That is a red run that
# sends the reader to the wrong file.
#
# So a run lost to EADDRINUSE is retried on another port rather than reported.
# Exit status:
#   0  it exited on its own account; the log is the caller's to assert on
#   1  it was still running when the budget ran out (the caller's failure)
#   2  five ports in a row were taken; nothing was tested
serve_refuses() { # serve_refuses <label> <logfile> <launch-fn>
   sr_try=1
   while [ "$sr_try" -le 5 ]; do
      T_PORT=$(pick_port)
      if [ -z "${T_PORT:-}" ]; then
         return 2
      fi
      T_PID=
      "$3" "$T_PORT"
      sr_i=0
      while [ "$sr_i" -lt 60 ] && kill -0 "$T_PID" 2>/dev/null; do
         sleep 0.1
         sr_i=$((sr_i + 1))
      done
      if kill -0 "$T_PID" 2>/dev/null; then
         return 1
      fi
      wait "$T_PID" 2>/dev/null
      if grep -q "Address already in use" "$2" 2>/dev/null; then
         printf '%s: port %s belonged to somebody else; trying another\n' \
            "$1" "$T_PORT" >&2
         sr_try=$((sr_try + 1))
         continue
      fi
      return 0
   done
   return 2
}

# ------------------------------------------------------------- text assertions

ck() { # ck <name> <required-regex> <actual>
   if printf '%s' "$3" | grep -q -- "$2"; then
      t_ok "$1"
   else
      t_bad "$1: wanted '$2', got: $(t_show "$3")"
   fi
}

nk() { # nk <name> <forbidden-regex> <actual>
   if printf '%s' "$3" | grep -q -- "$2"; then
      t_bad "$1: did NOT want '$2', got: $(t_show "$3")"
   else
      t_ok "$1"
   fi
}

empty() { # empty <name> <actual> -- grep cannot match "nothing"
   if [ -z "$2" ]; then
      t_ok "$1"
   else
      t_bad "$1: wanted nothing, got: $(t_show "$2")"
   fi
}

# ------------------------------------------------------------ HTTP assertions
#
# The whole point of this block: a response is a STATUS, a set of HEADERS and a
# BODY, and an assertion that reads only the body is three quarters blind.
#
# `req` echoes the body so it composes with `ck`/`nk` in a command substitution
# -- and because a substitution runs in a subshell, where a variable assignment
# would be lost, the status and headers go to FILES that the assertions below
# read back. $T_TMP must be set by the caller (a directory only this run uses).
#
# WHY THE STATUS IS THE FIRST ARGUMENT AND NOT OPTIONAL (item 66).
#
# This helper used to be `req <METHOD> <path>` and it did not remove the files
# it was about to write. Measured, on this machine, with curl 8:
#
#     curl -s -D .hdr -o .body -w '%{http_code}' <closed port> > .code
#       .code  becomes "000"      (the shell truncates it, curl writes 000)
#       .hdr   becomes EMPTY      (curl opens the dump file, then fails)
#       .body  IS NOT TOUCHED AT ALL -- it still holds the PREVIOUS response
#
# So a request that never happened returned the last response that did. What
# that looked like to somebody reading a green suite:
#
#   * synctest.sh asserted that a session id smuggled in the request BODY does
#     not sign you in, by checking the answer says "Sign in". The request
#     before it was a failed login, whose page also says "Sign in". Had that
#     curl failed, the smuggling assertion would have passed against the
#     previous page -- a security control reported as working by a request the
#     server never saw.
#   * the same file asserted that a bare pairing round number IS still routed,
#     with `nk ... "no such pairing round"`. A FORBIDDEN-string assertion
#     passes on an empty body, so clearing the artifacts is not enough on its
#     own: a request that failed proves nothing either way, and the only thing
#     that catches it is knowing what the status should have been.
#
# Hence: clear everything first, and take the expected status as the FIRST
# argument, before the method. A caller physically cannot ask for a body
# without stating the status it expects, which is a convention nobody can
# forget rather than a rule everybody must remember. The 41 `ck_code` lines
# that follow requests in synctest.sh are kept as well -- they name the rule in
# the transcript, which this check cannot do from inside a subshell.
req() { # req <expected-status> <METHOD> <path> [curl args...] -- echoes body
   # THE ARGUMENTS ARE CHECKED BEFORE THEY ARE READ. The two-argument form is
   # what this helper is being converted AWAY from, so `req GET /` must produce
   # a named failure -- not `$3: unbound variable` from `set -u`, which reads
   # as a bug in the harness rather than as the rule it is.
   #
   # "GET" is not a status: that is what stops the old form from silently
   # meaning "expect a response whose status is the word GET".
   if [ -z "${T_TMP:-}" ] || [ ! -d "${T_TMP:-}" ]; then
      t_defer "req ${2:-} ${3:-}: \$T_TMP is not a directory, so this request\
 had nowhere to put its response and NOTHING below it was tested"
      return 1
   fi
   # EVERY ARTIFACT GONE FIRST, AND BEFORE THE ARGUMENTS ARE EVEN CHECKED. Not
   # "overwritten by the next response" -- removed, so that a curl which never
   # writes one leaves nothing behind to be read as this request's answer.
   #
   # Before the checks, because a call this helper REFUSES is still a request
   # that did not happen: returning early with the previous response still on
   # disk would leave the next ck_code/ck_hdr reading it, which is the exact
   # defect, reached by the exact door that was meant to close it.
   rm -f "$T_TMP/.body" "$T_TMP/.hdr" "$T_TMP/.code" "$T_TMP/.what"
   case ${1:-} in
   [1-5][0-9][0-9]) ;;
   *) t_defer "req '${1:-}' '${2:-}' '${3:-}': the first argument must be the\
 expected HTTP status; a request whose status nobody states is a request that\
 can fail unnoticed"
      return 1 ;;
   esac
   if [ "$#" -lt 3 ]; then
      t_defer "req $1 ${2:-}: a method and a path are both required"
      return 1
   fi
   rq_want=$1
   rq_m=$2
   rq_p=$3
   shift 3
   printf '%s %s\n' "$rq_m" "$rq_p" > "$T_TMP/.what"
   curl -s -X "$rq_m" -D "$T_TMP/.hdr" -o "$T_TMP/.body" \
        -w '%{http_code}' "$@" "$U$rq_p" > "$T_TMP/.code" 2>/dev/null
   rq_rc=$?
   rq_got=$(cat "$T_TMP/.code" 2>/dev/null)
   if [ "$rq_rc" -ne 0 ]; then
      t_defer "$rq_m $rq_p: curl FAILED (exit $rq_rc), so there is no response\
 here and every assertion that reads one is meaningless"
   elif [ "$rq_got" != "$rq_want" ]; then
      t_defer "$rq_m $rq_p: wanted HTTP $rq_want, got '${rq_got:-nothing at all}'"
   fi
   # Missing rather than stale when the request failed: `cat` of a file curl
   # never created prints nothing, which is the honest answer.
   cat "$T_TMP/.body" 2>/dev/null
}

# ONE REQUEST OUT OF A CONCURRENT BATCH (item 68).
#
# Ten requests at once cannot share one set of response files: whichever curl
# finishes last decides what every assertion reads, which is item 66's
# staleness with a race on top. So each slot gets its own artifacts.
#
# It also EXITS nonzero on any failure, so that `wait` on the background job is
# itself a check -- the case this was written for launched ten curls and
# ignored every exit status, and ten instant connection failures are FASTER
# than ten answered requests, so total failure was the easiest way to pass the
# timing assertion it guarded.
req_bg() { # req_bg <slot> <expected-status> <METHOD> <path> [curl args...]
   rb_slot=$1
   rb_want=$2
   rb_m=$3
   rb_p=$4
   shift 4
   rm -f "$T_TMP/.body$rb_slot" "$T_TMP/.hdr$rb_slot" "$T_TMP/.code$rb_slot"
   curl -s -X "$rb_m" -D "$T_TMP/.hdr$rb_slot" -o "$T_TMP/.body$rb_slot" \
        -w '%{http_code}' "$@" "$U$rb_p" > "$T_TMP/.code$rb_slot" 2>/dev/null
   rb_rc=$?
   rb_got=$(cat "$T_TMP/.code$rb_slot" 2>/dev/null)
   if [ "$rb_rc" -ne 0 ]; then
      t_defer "$rb_m $rb_p (slot $rb_slot): curl FAILED (exit $rb_rc)"
      return 1
   fi
   if [ "$rb_got" != "$rb_want" ]; then
      t_defer "$rb_m $rb_p (slot $rb_slot): wanted HTTP $rb_want, got '$rb_got'"
      return 1
   fi
   return 0
}

bg_body() { # bg_body <slot> -- the body one slot received
   cat "$T_TMP/.body$1" 2>/dev/null
}

# DID THE SERVER ANSWER AT ALL? `curl -w '%{http_code}'` prints 000 when it
# never got a status line, and 000 is not 200 -- which quietly satisfies every
# assertion of the shape
#
#     if [ "$CODE" != "200" ]; then t_ok "the page failed rather than render"
#
# and there are several of those, because "the storage broke, so the page must
# refuse" is the whole point of faulttest.sh. A server that was never reached
# refuses everything, perfectly, for free. So the fault cases below ask this
# first: an answer that is not 200 proves a refusal only if there WAS an
# answer.
answered() { # answered <code>
   case ${1:-} in
   [1-5][0-9][0-9]) return 0 ;;
   *) return 1 ;;
   esac
}

# The status of the LAST req. Named rather than compared inline so a failure
# says which number it wanted.
ck_code() { # ck_code <name> <expected-status>
   cc_got=$(cat "$T_TMP/.code" 2>/dev/null)
   if [ "$cc_got" = "$2" ]; then
      t_ok "$1 ($2)"
   else
      t_bad "$1: wanted HTTP $2, got '${cc_got:-nothing at all}'\
 (last request: $(cat "$T_TMP/.what" 2>/dev/null))"
   fi
}

ck_hdr() { # ck_hdr <name> <required-regex>
   ch_h=$(cat "$T_TMP/.hdr" 2>/dev/null)
   if [ -z "$ch_h" ]; then
      t_bad "$1: there are NO headers to look at\
 (last request: $(cat "$T_TMP/.what" 2>/dev/null))"
      return
   fi
   ck "$1" "$2" "$ch_h"
}

# NO HEADERS IS NOT "the forbidden header is absent".
#
# This is the fail-open twin of the stale body above, and it is worse, because
# clearing the artifacts does not fix it: a curl that fails leaves the header
# dump EMPTY, and `nk` on an empty string passes every time. Every assertion of
# the form "...and it sets no session cookie" or "...and is not even typed as a
# GIF" would have passed on a request the server never answered.
nk_hdr() { # nk_hdr <name> <forbidden-regex>
   nh_h=$(cat "$T_TMP/.hdr" 2>/dev/null)
   if [ -z "$nh_h" ]; then
      t_bad "$1: there are NO headers, so '$2' being absent from them proves\
 nothing (last request: $(cat "$T_TMP/.what" 2>/dev/null))"
      return
   fi
   nk "$1" "$2" "$nh_h"
}

# ------------------------------------------------------------- the database
#
# The state a request LEFT BEHIND, read from outside both implementations.
# "the reply said stored 2 rows" and "there are two rows" are different
# claims, and only the second one is the one that matters after a crash.
#
# Fails when the query fails. The check this replaces piped sqlite3's stderr to
# /dev/null and treated an empty answer as "nothing to complain about", so on a
# machine with no sqlite3 -- this one -- it printed PASS forever.
dbq() { # dbq <db-file> <sql>  -- rows on stdout, one per line, fields by '|'
   python3 - "$1" "$2" <<'PY'
import sqlite3, sys
try:
    c = sqlite3.connect(sys.argv[1])
    for row in c.execute(sys.argv[2]):
        print("|".join("" if v is None else str(v) for v in row))
    c.close()
except Exception as e:
    sys.stderr.write("query failed: %s\n" % e)
    sys.exit(1)
PY
}

dbx() { # dbx <db-file> <sql>  -- run one statement; no rows expected
   python3 - "$1" "$2" <<'PY'
import sqlite3, sys
try:
    c = sqlite3.connect(sys.argv[1], isolation_level=None)
    c.execute(sys.argv[2])
    c.close()
except Exception as e:
    sys.stderr.write("statement failed: %s\n" % e)
    sys.exit(1)
PY
}

ck_db() { # ck_db <name> <sql> <required-regex>   (against $DB)
   if ! cd_out=$(dbq "$DB" "$2" 2>&1); then
      t_bad "$1: the database query did not RUN: $(t_show "$cd_out")"
      return
   fi
   ck "$1" "$3" "$cd_out"
}

nk_db() { # nk_db <name> <sql> <forbidden-regex>  (against $DB)
   if ! nd_out=$(dbq "$DB" "$2" 2>&1); then
      t_bad "$1: the database query did not RUN: $(t_show "$nd_out")"
      return
   fi
   nk "$1" "$3" "$nd_out"
}
