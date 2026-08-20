#!/bin/bash
# harnesstest.sh --- the test OF srv/test/testlib.sh.
#
# WHY A TEST NEEDS A TEST.
#
# Everything the server promises is asserted through testlib.sh: ~750
# assertions in synctest.sh, 57 in faulttest.sh, 19 in tlstest.sh, and among
# them the ones that say a copied session cookie stops working, that a refused
# request wrote nothing to the database, and that a hostile round-one cannot
# burn somebody's pairing code. Every one of those is worth exactly as much as
# the helper underneath it. Two defects in that helper were found by reading:
#
#   * `req` did not remove the files it was about to write. curl leaves the
#     BODY file untouched when it cannot connect (measured, curl 8: .code
#     becomes "000", .hdr is truncated to nothing, .body is NOT WRITTEN), so a
#     request that never happened returned the previous response. An assertion
#     looking for "Sign in" in the answer to a hostile request passed against
#     the login page from two requests earlier.
#
#   * `pick_port` asked the kernel for a free port and closed the socket before
#     the server was started, so between the two another suite could take it.
#     The bad case is not the one that fails: it is our server dying on
#     EADDRINUSE while ANOTHER suite's server answers on the same number, which
#     the old readiness poll accepted as "ready".
#
# Neither of those can be caught by any suite that USES the helper, because a
# helper that reports success is indistinguishable from a helper that worked.
# So this file breaks each rule on purpose and requires the failure. Every case
# is negative: it is not "the harness works", it is "the harness cannot be made
# to pass over a request that did not happen".
#
# It runs the suites it tests as CHILD PROCESSES, in their own temporary
# directories, and inspects their output and exit status. Nothing here asserts
# anything about the server.
set -u

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/test/testlib.sh"

need python3 "the harness's own test" || exit 1
need curl "the harness's own test" || exit 1

DIR=$(mktemp -d)
T_TMP=$DIR
KIDS=
cleanup() {
   for k in $KIDS; do
      kill "$k" 2>/dev/null || true
      wait "$k" 2>/dev/null || true
   done
   rm -rf "$DIR"
}
trap cleanup EXIT INT TERM

# A suite under test, run as its own script so that its `fail` and ours cannot
# be confused. It sources testlib.sh exactly as the real suites do.
#
# Stdout AND stderr are captured together, because the diagnostics this file is
# checking for deliberately go to stderr: a `req` inside `$(...)` cannot print
# to stdout without splicing its complaint into the response body.
mini() { # mini <name> <script-body> -- runs it, leaves output in $OUT and status in $RC
   mkdir -p "$DIR/$1"
   {
      printf 'set -u\n'
      printf '. "%s/srv/test/testlib.sh"\n' "$HERE"
      printf 'T_TMP=%s/%s\n' "$DIR" "$1"
      printf '%s\n' "$2"
      printf 't_end\n'
      printf 'exit $fail\n'
   } > "$DIR/$1.sh"
   OUT=$(bash "$DIR/$1.sh" 2>&1)
   RC=$?
}

# A server that answers, so a SUCCESSFUL request has something to succeed
# against. Deliberately not srv/sync: this file tests the harness, and pulling
# in the binary under test would make a broken server look like a broken
# harness.
STUBPORT=$(pick_port)
python3 - "$STUBPORT" <<'PY' > "$DIR/stub.log" 2>&1 &
import http.server, socketserver, sys


class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"THE FIRST RESPONSE: Sign in"
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("X-Marker", "first")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("127.0.0.1", int(sys.argv[1])), H) as s:
    s.serve_forever()
PY
STUB=$!
KIDS="$KIDS $STUB"
U="http://127.0.0.1:$STUBPORT"
if wait_ready "$STUB" "$U/"; then
   t_ok "the stub server is up (and this file's own fixture is honest)"
else
   t_bad "the stub server did not start -- nothing below proves anything"
   t_end
   exit 1
fi
# A SERVER THAT ANSWERS 200 AND THEN HANGS UP MID-BODY. It promises 100 bytes
# and sends ten. curl exits 18 (partial file) with %{http_code} STILL 200, so
# this is the one failure the status check cannot see: the status is right, the
# body is a fragment, and an assertion looking for text near the top of a page
# would find it. Only curl's exit status catches it.
TRUNCPORT=$(pick_port)
python3 - "$TRUNCPORT" <<'PY2' > "$DIR/trunc.log" 2>&1 &
import socket, sys

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", int(sys.argv[1])))
srv.listen(8)
while True:
    c, _ = srv.accept()
    try:
        c.recv(4096)
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nTHE FIRST ")
    except OSError:
        pass
    c.close()
PY2
TRUNC=$!
KIDS="$KIDS $TRUNC"

# A port with NOTHING on it, for the requests that must fail. Taken from
# pick_port so it is a port no other suite in this run will be given.
DEADPORT=$(pick_port)

echo "== item 66: a request that did not happen is not an answer =="

# THE ISOLATING CASE. Two requests: the first succeeds, the second is made to a
# closed port. The assertion on the second looks for text that only the FIRST
# response contains. Before the fix curl left .body alone on a connection
# failure, so the second assertion read the first response and passed.
#
# This is the exact shape of synctest.sh's "a session cookie smuggled in the
# request body does not sign you in": it looks for "Sign in" in the answer to a
# hostile request, and the request before it is a failed login whose page also
# says "Sign in".
mini stale "
U=$U
ck \"the first request really was answered\" \"THE FIRST\" \"\$(req 200 GET /)\"
U=http://127.0.0.1:$DEADPORT
ck \"the second request must not inherit the first one's body\" \
   \"THE FIRST\" \"\$(req 200 GET /)\"
"
# "FAIL ", not just the name: an assertion NAME appears in the transcript
# whether it passed or failed, so matching the name alone is an assertion that
# cannot fail. (Found by mutation: with the artifact-clearing removed, the
# stale body came back, the child's assertion PASSED on it, and this check
# still matched the ok line. Every verdict this file reads about is spelled
# with its prefix now.)
ck "a failed request no longer returns the previous response" \
   "FAIL the second request must not inherit" "$OUT"
ck "...and the suite FAILS rather than passing on it" "^1$" "$RC"
# ...and it says WHY, in the words of the thing that went wrong. A run that
# fails for an unexplained reason sends the reader to the wrong file.
ck "...naming curl's failure, not just the missing text" "curl FAILED" "$OUT"
# The first request must still have passed: a helper that fails everything
# proves nothing about the one that should fail.
ck "...while the request that DID happen still passes" \
   "ok   the first request really was answered" "$OUT"

# A RESPONSE THAT WAS CUT OFF is not a response, even though its status is
# perfect. This is the case the required status CANNOT catch -- curl reports
# 200 because the status line arrived -- and it is the reason the exit status is
# checked separately rather than trusted to disagree with the code.
mini truncated "
U=http://127.0.0.1:$TRUNCPORT
ck \"the fragment that did arrive looks like the page\" \"THE FIRST\" \"\$(req 200 GET /)\"
"
ck "a body cut off mid-transfer fails despite its 200" \
   "FAIL GET /: curl FAILED" "$OUT"
ck "...and the run is red" "^1$" "$RC"
ck "...even though the fragment satisfied the body assertion" \
   "ok   the fragment that did arrive looks like the page" "$OUT"

# THE FORBIDDEN-STRING TWIN. `nk` passes on an empty string, so clearing the
# artifacts does not save an assertion of the form "the answer does not contain
# X" -- it makes it pass MORE reliably. Only the required status catches it.
# synctest.sh has exactly this: `nk "a bare round number is still routed" "no
# such pairing round" "$(req ...)"`.
mini nkempty "
U=http://127.0.0.1:$DEADPORT
nk \"a forbidden string is absent from a response that never arrived\" \
   \"anything at all\" \"\$(req 200 GET /)\"
"
# AND THE HONEST ACCOUNT OF WHAT SAVES IT. The `nk` itself STILL PASSES here:
# the body is empty, the forbidden string is duly absent, and no amount of
# clearing artifacts can make a "does not contain" assertion notice that there
# was nothing to look at. What makes the run red is the request having been
# required to produce a status it did not produce. That is the whole argument
# for making the status a mandatory argument rather than a habit.
ck "the vacuous 'does not contain' still passes -- it cannot know" \
   "ok   a forbidden string is absent" "$OUT"
ck "...but the run is red anyway, because the request had to answer 200" \
   "FAIL GET /: curl FAILED" "$OUT"
ck "...and that failure is the suite's verdict" "^1$" "$RC"

# THE HEADER TWIN. curl truncates the header dump to nothing when it fails, and
# `nk_hdr` on no headers used to pass: "...and it sets no session cookie" was
# true of every request the server never saw.
mini nkhdr "
U=http://127.0.0.1:$DEADPORT
req 200 GET / >/dev/null
nk_hdr \"...and it sets no session cookie\" \"Set-Cookie\"
"
ck "'no such header' over NO headers is a failure, not a pass" \
   "FAIL .*there are NO headers" "$OUT"
ck "...and it fails the suite" "^1$" "$RC"

# THE REQUIRED-HEADER TWIN, which fails either way and must say WHY.
# `ck_hdr` over an empty header block fails on its own -- there is nothing for
# the regex to match -- so the verdict is not what this case buys. The
# diagnostic is: "wanted Content-Type, got: " sends the reader to the routing
# code, and "there are NO headers to look at" sends them to the request.
mini ckhdr "
U=http://127.0.0.1:$DEADPORT
req 200 GET / >/dev/null
ck_hdr \"...as HTML, with the encoding named\" \"Content-Type: text/html\"
"
ck "a required header over NO headers says there were none" \
   "FAIL .*there are NO headers to look at" "$OUT"

# THE STATUS IS NOT OPTIONAL. The old two-argument form must not be usable:
# `req GET /` would otherwise mean "expect a response whose status is GET",
# which is a request nobody has stated an expectation for.
mini nostatus "
U=$U
ck \"the old form still works\" \"THE FIRST\" \"\$(req GET /)\"
"
ck "a request made without an expected status is refused" \
   "FAIL req 'GET' '/' '': the first argument must be" "$OUT"
ck "...and fails the suite even though the body assertion passed" "^1$" "$RC"

# ...AND A REFUSED CALL STILL DESTROYS THE LAST RESPONSE. This is the same
# defect reached through the door that was built to close it: if `req` returned
# early on a malformed call while the previous response was still on disk, the
# ck_code after it would read that one and pass. The status file must be gone.
mini refused_clears "
U=$U
req 200 GET / >/dev/null
req GET /
ck_code \"the status of a request that was REFUSED before it was made\" 200
"
ck "a call req refuses leaves no previous response behind" \
   "FAIL the status of a request that was REFUSED" "$OUT"
ck "...and says there is no status at all, not the old one" \
   "got 'nothing at all'" "$OUT"

# A WRONG STATUS, with a body that satisfies the assertion. This is the case
# the required argument buys that clearing the files does not: the server
# answered, the text is right, and the status says the answer means something
# else. (The stub answers 200, so 404 is a status it will never send.)
mini wrongcode "
U=$U
ck \"the body says what it should\" \"THE FIRST\" \"\$(req 404 GET /)\"
"
ck "a response with the wrong status fails even when the body matches" \
   "FAIL GET /: wanted HTTP 404" "$OUT"
ck "...and the suite goes red" "^1$" "$RC"

# NOWHERE TO PUT THE RESPONSE. A suite that forgets T_TMP had every request
# written to /.body -- unwritable, so every request silently had no artifacts
# at all and every ck_code read nothing.
mini notmp "
U=$U
T_TMP=
ck \"a request with nowhere to store its response\" \"THE FIRST\" \"\$(req 200 GET /)\"
"
ck "a request with no T_TMP is a failure, not a silent one" "^1$" "$RC"
# AND IT SAYS SO IN THOSE WORDS. Without the check the run still goes red --
# curl fails to write to /.body and the curl-exit rule catches it -- but the
# diagnostic then blames the network for a suite that simply forgot a variable.
ck "...naming the missing directory, not curl" \
   "T_TMP is not a directory" "$OUT"

# AND THE VERDICT SURVIVES THE SUBSHELL. This is the mechanism the four cases
# above depend on: `fail=1` inside `$(...)` is discarded, so a suite that
# reaches its exit line without t_end would print "all tests passed" over a
# request that never happened. The proof is that the SAME script without t_end
# exits 0 -- which is what the old code did every time.
mini drain "
U=http://127.0.0.1:$DEADPORT
X=\$(req 200 GET /)
"
ck "a failed request nobody asserted on still fails the run" "^1$" "$RC"
ck "...and says which request it was" "FAIL GET /: curl FAILED" "$OUT"

# A REFUSAL PROVES A REFUSAL ONLY IF THERE WAS AN ANSWER. `curl -w
# '%{http_code}'` prints 000 when it never got a status line, and 000 is not
# 200 -- so every fault case shaped "if the code is not 200, the server refused
# as it should" was satisfied by a server that was never reached. faulttest.sh
# has five of those and they are the whole of what that suite asserts.
for AC in 000 "" nonsense 0 20 2000; do
   if answered "$AC"; then
      t_bad "answered '$AC' says the server replied when it did not"
      break
   fi
done
case $AC in
2000) t_ok "a code that is not an HTTP status is not an answer (000, empty, 0, 20, 2000)" ;;
esac
ACBAD=0
for AC in 200 201 303 400 404 405 429 500 503; do
   answered "$AC" || ACBAD=$((ACBAD + 1))
done
ck "...and every real status still is one" "^0$" "$ACBAD"

echo "== item 67: a port is not ours until the kernel says so =="

# WHO OWNS THE PORT. The stub is listening on STUBPORT; this shell is not.
ck_owns "the stub server owns the port it is serving" "$STUB" "$STUBPORT"
if own_port "$$" "$STUBPORT"; then
   t_bad "own_port credits THIS shell with a port the stub server holds"
else
   t_ok "...and this shell, which merely talks to it, does not"
fi
# A CHECK THAT CANNOT FAIL IS NOT A CHECK. If own_port answered yes to
# everything the ownership assertion in every suite would be decoration.
if own_port "$STUB" "$DEADPORT"; then
   t_bad "own_port says a process owns a port NOBODY is listening on"
else
   t_ok "own_port says no when nothing is listening at all"
fi

# NO PORT IS HANDED OUT TWICE IN A RUN. pick_port used to close its probe
# socket before returning, so two calls in a row could be answered with the
# same number -- restoredrill.sh makes exactly two, back to back, for two
# servers that must run at the same time.
PP=$(for i in $(seq 1 40); do pick_port; done)
ck "forty ports were obtained" "^40$" "$(printf '%s\n' "$PP" | grep -c .)"
ck "...and no two of them are the same" "^0$" \
   "$(printf '%s\n' "$PP" | sort | uniq -d | grep -c .)"
# ...and the memo is how that is enforced across subshells, so every number
# handed out must be IN it. NOT COVERED BY EXECUTION: that the skip branch
# fires. Making the kernel offer the same ephemeral port twice on demand is not
# something a test can arrange from userspace, so what is pinned here is that
# the record exists to be checked against, not that it has ever been hit.
PPMISS=0
for pp in $PP; do
   grep -qx "$pp" "$T_PORTMEMO" || PPMISS=$((PPMISS + 1))
done
ck "...and every one of them was recorded in the memo" "^0$" "$PPMISS"

# A CONNECTION TO A PORT IS NOT OWNERSHIP OF IT. This holds a client socket
# open to the stub while the question is asked: a check that matched any socket
# rather than a LISTENING one would credit the client -- and in a suite that
# means the very curl the poll just made could vouch for the wrong process.
python3 - "$STUBPORT" > "$DIR/client.pid" 2>/dev/null <<'PY3' &
import os, socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
print(os.getpid(), flush=True)
time.sleep(60)
PY3
CLIENT=$!
cl_i=0
while [ "$cl_i" -lt 100 ] && [ ! -s "$DIR/client.pid" ]; do
   sleep 0.1
   cl_i=$((cl_i + 1))
done
KIDS="$KIDS $CLIENT"
ck "a client is connected to the stub" "^[0-9][0-9]*$" \
   "$(cat "$DIR/client.pid" 2>/dev/null || echo none)"
if own_port "$(cat "$DIR/client.pid" 2>/dev/null)" "$STUBPORT"; then
   t_bad "own_port credits a CONNECTION to the port as ownership of it"
else
   t_ok "a process merely connected to the port does not own it"
fi

# THE CASE THAT IS NOT CONTRIVED AT ALL: a process using the port as the SOURCE
# of an outgoing connection. Ports come out of the ephemeral range, which is
# also where the kernel takes source ports from, so a curl this very suite runs
# can be assigned a source port equal to the number another suite is about to
# start a server on. That process holds a socket whose LOCAL port is the number
# in question and it is serving nothing; crediting it would mean a readiness
# poll vouching for a client.
OUTPORT=$(pick_port)
python3 - "$OUTPORT" "$STUBPORT" > "$DIR/out.pid" 2>/dev/null <<'PY4' &
import os, socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.connect(("127.0.0.1", int(sys.argv[2])))
print(os.getpid(), flush=True)
time.sleep(60)
PY4
OUTGOING=$!
og_i=0
while [ "$og_i" -lt 100 ] && [ ! -s "$DIR/out.pid" ]; do
   sleep 0.1
   og_i=$((og_i + 1))
done
KIDS="$KIDS $OUTGOING"
ck "a process is using the port as an outgoing source port" "^[0-9][0-9]*$" \
   "$(cat "$DIR/out.pid" 2>/dev/null || echo none)"
if own_port "$(cat "$DIR/out.pid" 2>/dev/null)" "$OUTPORT"; then
   t_bad "own_port credits a SOURCE port as a listener: a client would vouch\
 for a server that is not there"
else
   t_ok "...and that is not owning the port either -- only LISTEN counts"
fi

# THE COLLISION, ARRANGED. A squatter takes the port that pick_port is about to
# hand out, exactly as a parallel suite would in the window between the probe
# socket closing and the server binding. `serve` must notice that the port is
# not ours, try another, and come back with one that is.
#
# pick_port is redefined for this case rather than given a test hook: the
# hazard is "the number you were given is already taken", and handing out a
# taken number is the shortest way to say that.
python3 - > "$DIR/squat.port" 2>/dev/null <<'PY' &
import socket, time
s = socket.socket()
s.bind(("0.0.0.0", 0))
s.listen(8)
print(s.getsockname()[1], flush=True)
time.sleep(120)
PY
SQUAT=$!
KIDS="$KIDS $SQUAT"
sq_i=0
while [ "$sq_i" -lt 100 ] && [ ! -s "$DIR/squat.port" ]; do
   sleep 0.1
   sq_i=$((sq_i + 1))
done
SQPORT=$(cat "$DIR/squat.port" 2>/dev/null)
ck "the squatter took a port and said which" "^[0-9][0-9]*$" "${SQPORT:-none}"
ck_owns "...and really holds it" "$SQUAT" "${SQPORT:-0}"

# The suite under test is a child again, because it has to redefine pick_port
# and start a server, and doing that in this shell would leave the redefinition
# in place for everything after it.
cat > "$DIR/collide.sh" <<COLLIDE
set -u
. "$HERE/srv/test/testlib.sh"
T_TMP=$DIR
# The first port offered is the squatter's. After that, the real one.
#
# THE FLAG IS A FILE, not a variable: serve calls this as \`T_PORT=\$(pick_port)\`
# and a command substitution is a subshell, so \`FIRST=0\` would be discarded and
# every attempt would be handed the squatter's port again. That is the same
# subshell rule that makes t_defer necessary, met here while writing the test
# for it.
rm -f "$DIR/offered"
pick_port() {
   if [ ! -f "$DIR/offered" ]; then
      : > "$DIR/offered"
      printf '%s\n' "$SQPORT"
      return 0
   fi
   python3 -c 'import socket
s = socket.socket()
s.bind(("0.0.0.0", 0))
print(s.getsockname()[1])
s.close()'
}
# A server of our own: a listener that answers one line of HTTP, so that
# "something answered" is true only when OUR process is the one listening.
up() {
   python3 -c 'import socket, sys
srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    srv.bind(("0.0.0.0", int(sys.argv[1])))
except OSError:
    sys.exit(3)
srv.listen(8)
while True:
    c, _ = srv.accept()
    c.recv(4096)
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nhi")
    c.close()' "\$1" &
   T_PID=\$!
}
if serve "the colliding server" http / up; then
   ck_owns "the server ended up on a port of its own" "\$T_PID" "\$T_PORT"
   if [ "\$T_PORT" = "$SQPORT" ]; then
      t_bad "it settled on the squatter's port"
   else
      t_ok "...which is not the port the squatter holds"
   fi
   kill "\$T_PID" 2>/dev/null
   wait "\$T_PID" 2>/dev/null
else
   t_bad "serve gave up instead of moving to a free port"
fi
t_end
exit \$fail
COLLIDE
COUT=$(bash "$DIR/collide.sh" 2>&1)
CRC=$?
ck "a stolen port is detected and another one is used" \
   "ok   ...which is not the port the squatter holds" "$COUT"
ck "...and the ownership check on the new port passes" \
   "ok   the server ended up on a port of its own" "$COUT"
ck "...and that run is green" "^0$" "$CRC"

# THE VERIFICATION IS WHAT CLOSES THE RACE, NOT THE RETRY. With the squatter
# ANSWERING HTTP on the port -- which is what another suite's server is -- a
# readiness poll that stops at "something answered" reports success while our
# own process is dead. This case starts a server that cannot bind (the port is
# taken and it does not retry) and requires wait_ready to say no even though
# the port answers perfectly.
python3 - "$DIR/http.port" > "$DIR/httpsquat.log" 2>&1 <<'PY' &
import http.server, socketserver, sys


class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"hi")

    def log_message(self, *a):
        pass


socketserver.TCPServer.allow_reuse_address = True
with socketserver.TCPServer(("0.0.0.0", 0), H) as s:
    open(sys.argv[1], "w").write("%d\n" % s.server_address[1])
    s.serve_forever()
PY
HSQUAT=$!
KIDS="$KIDS $HSQUAT"
hs_i=0
while [ "$hs_i" -lt 100 ] && [ ! -s "$DIR/http.port" ]; do
   sleep 0.1
   hs_i=$((hs_i + 1))
done
HPORT=$(cat "$DIR/http.port" 2>/dev/null)
ck "an HTTP squatter is answering on a port of its own" "^[0-9][0-9]*$" \
   "${HPORT:-none}"
# A process that lives, and does NOT hold that port: the pid is alive and the
# port answers, which is precisely the pair of facts the old wait_ready
# accepted as "the server is ready".
sleep 300 &
IMPOSTOR=$!
KIDS="$KIDS $IMPOSTOR"
if wait_ready "$IMPOSTOR" "http://127.0.0.1:${HPORT:-0}/" 2>/dev/null; then
   t_bad "wait_ready accepted a live pid and a port SOMEBODY ELSE answers on"
else
   t_ok "wait_ready refuses a port its own process does not own"
fi

echo "== item 68: a batch is not timed until every request in it succeeded =="

# TEN REQUESTS THAT ALL FAIL ARE FASTER THAN TEN THAT SUCCEED. The case in
# synctest.sh launched ten curls, ignored every exit status and asserted only
# elapsed time, so a server that was not there passed it instantly. req_bg
# gives each request its own artifacts and exits nonzero unless it got the
# status it was told to expect, which makes `wait` an assertion.
mini batchdead "
U=http://127.0.0.1:$DEADPORT
P=
for i in 1 2 3 4 5 6 7 8 9 10; do req_bg \"\$i\" 200 GET / & P=\"\$P \$!\"; done
N=0
for p in \$P; do if wait \"\$p\"; then N=\$((N + 1)); fi; done
ck \"all ten concurrent requests were answered 200\" \"^10\$\" \"\$N\"
"
ck "a batch of ten failed requests is not a batch of ten answers" \
   "FAIL all ten concurrent requests were answered" "$OUT"
ck "...and the run fails" "^1$" "$RC"

# ...and one whose answers are TRUNCATED: ten 200s, ten cut-off bodies. The
# status check passes on every one of them, so this is what the per-request
# exit status is for.
mini batchtrunc "
U=http://127.0.0.1:$TRUNCPORT
P=
for i in 1 2 3; do req_bg \"\$i\" 200 GET / & P=\"\$P \$!\"; done
N=0
for p in \$P; do if wait \"\$p\"; then N=\$((N + 1)); fi; done
ck \"all three concurrent requests completed\" \"^3\$\" \"\$N\"
"
ck "a batch whose answers were cut off is not a batch of answers" \
   "FAIL all three concurrent requests completed" "$OUT"
ck "...and that run fails too" "^1$" "$RC"

# ...and the same batch against a server that IS there passes, so the case
# above is not simply a helper that always says no.
mini batchlive "
U=$U
P=
for i in 1 2 3 4 5 6 7 8 9 10; do req_bg \"\$i\" 200 GET / & P=\"\$P \$!\"; done
N=0
for p in \$P; do if wait \"\$p\"; then N=\$((N + 1)); fi; done
ck \"all ten concurrent requests were answered 200\" \"^10\$\" \"\$N\"
F=0
for i in 1 2 3 4 5 6 7 8 9 10; do
  case \$(bg_body \"\$i\") in *'THE FIRST'*) F=\$((F + 1)) ;; esac
done
ck \"...each with its own body, not one shared file\" \"^10\$\" \"\$F\"
"
ck "ten concurrent requests to a live server all pass" "^0$" "$RC"
ck "...each having kept its OWN response" \
   "ok   ...each with its own body" "$OUT"

t_end
if [ "$fail" = 0 ]; then
   printf '\033[1;32mharnesstest\033[0m: the harness cannot pass over a request that did not happen\n'
else
   printf 'harnesstest: FAILED\n'
fi
exit $fail
