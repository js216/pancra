#!/bin/bash
# faulttest.sh --- what the server does when a statement does not finish.
#
# THE RULE THIS PROVES, at the two boundaries the ordinary tests cannot reach:
#
#   * a PREPARE that fails. Every read path prepares before it steps, and a
#     prepare that returns NULL used to be indistinguishable from a query that
#     found nothing -- an empty digest, an empty day, a page with no rows.
#     Empty is an answer a client acts on: the phone compares digests to decide
#     what to send, so "the server has nothing" makes it re-upload, and a
#     restore hands back a history that reads as complete.
#
#   * a COMMIT that fails. By then every row of the bucket is in the
#     transaction and the reply is one line from saying "stored 40 rows". The
#     answer has to be that nothing was stored -- and the database has to still
#     hold what it held before, because a bucket PUT REPLACES a day.
#
# Neither can be arranged with a real database on demand, so the server is
# built once with -DDB_FAULTS (a build nothing ships) and told which call to
# fail. Each case below runs a real request against a real database and then
# asks the STORAGE what happened, not the reply.
#
# Run by `make faulttest`.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/test/testlib.sh"

DIR=$(mktemp -d)
T_TMP=$DIR
SRVPID=
cleanup() {
  [ -n "$SRVPID" ] && { kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; }
  rm -rf "$DIR"
}
trap cleanup EXIT INT TERM

need python3 "the fault-injection suite" || exit 1
need curl "the fault-injection suite" || exit 1

SYNCBIN=${1:-$HERE/build/srv-fault/sync}
CLIBIN=$(dirname "$SYNCBIN")/synccli
[ -x "$SYNCBIN" ] || { echo "faulttest: $SYNCBIN is not built"; exit 1; }
cp "$SYNCBIN" "$DIR/sync"
cp "$CLIBIN" "$DIR/synccli"
cd "$DIR" || exit 1
DB=$DIR/sync.db

# The fault build must BE a fault build: without the compile-time switch every
# case below would pass by doing nothing at all. (Through `adduser`, not a
# bare `sync` -- a bare one is a SERVER and would sit there for ever.)
if ! printf 'pw\n' | DB_FAIL_PREPARE=1 ./sync adduser probe@example.com \
     stdin . 2>&1 |
     grep -q "INJECTED FAULT"; then
  echo "faulttest: $SYNCBIN was not built with -DDB_FAULTS"
  exit 1
fi
t_ok "the fault build injects faults (otherwise this suite proves nothing)"

# THE PORT IS NOT SETTLED UNTIL A SERVER OF OURS HOLDS IT.
#
# This suite starts and kills its server SIX times, so it has six windows in
# which the number it is holding on to can be taken by another suite -- and
# `make -j8` runs about a dozen of these at once. What that used to look like:
# the fault server dies on EADDRINUSE, the poll sees a pid not yet reaped and a
# 200 from the OTHER suite's server on the same port, and every fault-injection
# assertion below runs against a server with no faults injected. All green,
# nothing tested.
#
# So each start goes through fault_serve: a fresh port, and no assertion until
# the kernel says our pid owns it. PORT and U are re-read from it every time,
# because a retry lands on a different number.
FAULT_LOG=/dev/null
FAULT_VAR=
FAULT_VAL=
fault_up() { # fault_up <port>
  if [ -n "$FAULT_VAR" ]; then
    env "$FAULT_VAR=$FAULT_VAL" ./sync "$1" . > "$FAULT_LOG" 2>&1 &
  else
    ./sync "$1" . > "$FAULT_LOG" 2>&1 &
  fi
  T_PID=$!
}
fault_serve() { # fault_serve <label> [<env-var> <env-value>]
  FAULT_VAR=${2:-}
  FAULT_VAL=${3:-}
  if serve "$1" http / fault_up; then
    PORT=$T_PORT
    SRVPID=$T_PID
    U="http://127.0.0.1:$PORT"
    return 0
  fi
  SRVPID=
  return 1
}
# PORT and U are set by the first fault_serve below, and re-set by every one
# after it. Nothing between here and there speaks to a server.
printf 'correcthorse\n' | ./sync adduser jk@example.com stdin . >/dev/null 2>&1
export SYNCCLI_CODE_FILE=$T_TMP/.clicode
# The signed client prints the body; the status goes to a file (see testlib).
ck_clicode() { # ck_clicode <name> <expected-status>
  cl_got=$(cat "$SYNCCLI_CODE_FILE" 2>/dev/null)
  if [ "$cl_got" = "$2" ]; then t_ok "$1 ($2)"
  else t_bad "$1: wanted HTTP $2, got '${cl_got:-no status at all}'"; fi
}

# A paired phone with one day of readings already stored, so every case below
# has something to lose.
fault_serve "the fixture server" ||
  { echo "faulttest: server did not start"; exit 1; }
ck_owns "the fixture server owns its port" "$SRVPID" "$PORT"
curl -s -c jar.txt -o /dev/null -X POST \
     -d 'email=jk@example.com&password=correcthorse' $U/login
CSRF=$(curl -s -b jar.txt $U/settings |
       grep -o 'name=csrf value="[0-9a-f]*"' | head -1 | sed 's/.*value="//;s/"//')
CODE=$(curl -s -b jar.txt -X POST -d "csrf=$CSRF" $U/settings/pair |
       grep -o 'class=code>[0-9]\{6\}' | sed 's/.*>//')
PAIR=$(./synccli 127.0.0.1 $PORT pair jk@example.com "$CODE" 2>&1)
APPUID=$(echo "$PAIR" | cut -d' ' -f1)
KEY=$(echo "$PAIR" | cut -d' ' -f2)
ck "the suite's phone is paired" "^[0-9a-f]\{32\}$" "$KEY"

# TODAY's bucket, so the row is inside the window the page's recent table
# draws from -- a fixture the page cannot show proves nothing about the page.
NOW=$(date -u +%s)
DAY=$((NOW / 86400))
T1=$((DAY * 86400 + 3600))
T2=$((T1 + 300))
# 187/188: values that appear nowhere else in a rendered page, so finding
# one on the page means the row is really on it ("120" also appears in the
# refresh interval, and would have matched an empty page).
printf '%d,187,0,-70,3,7,%d,0,0\n%d,188,2,-70,3,7,%d,0,0\n' "$T1" "$T1" "$T2" "$T2" > good.txt
./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" PUT /v1/bucket/readings/$DAY good.txt >/dev/null 2>&1
ck_db "the day starts with two rows" \
   "SELECT count(*) FROM logrow WHERE bucket=$DAY" "^2$"

kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; SRVPID=

# ---------------------------------------------------------------- the COMMIT
echo "== a COMMIT that fails stores NOTHING =="
# A bucket PUT deletes the day and re-inserts it inside one transaction, so a
# failed COMMIT is the case where the client is told the truth or loses a day.
printf '%d,150,0,-70,3,7,%d,0,0\n%d,155,0,-70,3,7,%d,0,0\n' \
   "$((T1 + 600))" "$((T1 + 600))" "$((T1 + 900))" "$((T1 + 900))" > new.txt
FAULT_LOG=/dev/null
fault_serve "the failing-COMMIT server" DB_FAIL_COMMIT 1 ||
  { echo "faulttest: fault server did not start"; exit 1; }
OUT=$(./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" PUT /v1/bucket/readings/$DAY new.txt 2>&1)
nk "a failed COMMIT is never reported as stored" "stored" "$OUT"
ck_clicode "...the client is told 500" 500
# THE POINT: the day the PUT was replacing is still the day that is there.
ck_db "...and the rows that were there are STILL there" \
   "SELECT count(*) FROM logrow WHERE bucket=$DAY" "^2$"
ck_db "...unchanged, not replaced by the half-written batch" \
   "SELECT line FROM logrow WHERE bucket=$DAY ORDER BY line LIMIT 1" \
   "^$T1,187,"
# ...and the server is still usable afterwards: a failed transaction must not
# leave the connection inside one.
ck "the server still answers after a failed COMMIT" "^readings 2 " \
   "$(./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" GET /v1/digest 2>&1)"
kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; SRVPID=

# --------------------------------------------------------------- the PREPARE
echo "== a PREPARE that fails is never an empty answer =="
# BY NAME, not by ordinal. Counting prepares process-wide cannot aim: every
# request prepares several statements and so does the readiness poll this
# script makes while the server starts, so an ordinal landed on startup
# traffic and every asserted request returned the plain truth -- the
# assertions passed without the case they name ever happening.
#
# Each case below therefore names the statement, and PROVES it was hit: the
# server's own stderr must carry the injection line for that SQL, or the case
# fails as "never armed" rather than passing on an untouched request.
prep_case() { # prep_case <sql-fragment> <what it reads> <check-fn>
  : > srvfault.log
  FAULT_LOG=srvfault.log
  if ! fault_serve "$2" DB_FAIL_PREPARE_SQL "$1"; then
    t_ok "$2: the server refuses to start rather than serve blind"
    return
  fi
  "$3"
  # THE PROOF THAT THE CASE RAN. Without it a typo in the fragment is a green
  # test over a request that never failed.
  if grep -q "INJECTED FAULT DB_FAIL_PREPARE_SQL" srvfault.log; then
    t_ok "$2: the injected prepare failure really happened"
  else
    t_bad "$2: the fault never fired -- the case proved nothing"
  fi
  kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; SRVPID=
}

# THE DIGEST. An empty 200 tells the phone the server holds nothing, and the
# phone then re-uploads its whole history.
check_digest() {
  D=$(./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" GET /v1/digest 2>&1)
  DC=$(cat "$SYNCCLI_CODE_FILE" 2>/dev/null)
  if [ "$DC" = "500" ]; then
    t_ok "the digest fails outright (500)"
  else
    t_bad "the digest answered $DC with '$(t_show "$D")' instead of failing"
  fi
}
prep_case "SELECT log,bucket,line FROM logrow" "the digest" check_digest

# THE PER-LOG DIGEST, which the phone uses to pick which days to send.
check_logdigest() {
  D=$(./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" GET /v1/digest/readings 2>&1)
  DC=$(cat "$SYNCCLI_CODE_FILE" 2>/dev/null)
  if [ "$DC" = "500" ]; then
    t_ok "the per-log digest fails outright (500)"
  else
    t_bad "the per-log digest answered $DC with '$(t_show "$D")'"
  fi
}
prep_case "SELECT bucket,line FROM logrow" "the per-log digest" check_logdigest

# THE BUCKET, which a restore believes.
check_bucket() {
  B=$(./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" GET /v1/bucket/readings/$DAY 2>&1)
  BC=$(cat "$SYNCCLI_CODE_FILE" 2>/dev/null)
  if [ "$BC" = "500" ]; then
    t_ok "the bucket read fails outright (500)"
  else
    t_bad "the bucket answered $BC with '$(t_show "$B")' instead of failing"
  fi
}
prep_case "SELECT line FROM logrow" "the bucket read" check_bucket

# THE PAGE'S OWN SCAN: a browser has no status code a phone reads, so the
# temptation is to render what could be read.
check_page() {
  PC=$(curl -s -b jar.txt -o pbody.html -w '%{http_code}' $U/)
  if ! answered "$PC"; then
    t_bad "the page was never answered at all ($PC): a request that did not\
 happen refuses everything, which is not what this case is about"
  elif [ "$PC" != "200" ]; then
    t_ok "the page fails rather than renders ($PC)"
  elif grep -qi "could not be read" pbody.html; then
    t_ok "...or says in words that it could not be read whole"
  else
    t_bad "the page rendered as complete without its readings"
  fi
}
prep_case "AND bucket >= ?" "the recent-readings table" check_page

# THE SESSION LOOKUP: a lookup that did not run is not "not signed in", and
# showing the login form tells a signed-in reader their password stopped
# working.
check_session() {
  LC=$(curl -s -b jar.txt -o sbody.html -w '%{http_code}' $U/)
  if ! answered "$LC"; then
    t_bad "the session page was never answered at all ($LC)"
  elif [ "$LC" = "500" ]; then
    t_ok "a session that cannot be looked up is a 500"
  elif grep -qi "name=password" sbody.html; then
    t_bad "a failed session lookup showed the LOGIN FORM"
  else
    t_ok "the page did not claim the reader was signed out ($LC)"
  fi
}
# THE PLOT IMAGE. This is the one that answered 200 with a perfectly formed,
# correctly labelled, EMPTY graph: db_prep failing returned "no points", and
# no points is a legitimate answer for a window nobody wore a sensor in. A
# reader could not tell a database that would not answer from a week off.
check_plot() {
  GC=$(curl -s -b jar.txt -o pbody.gif -w '%{http_code}' $U/plot-24h.gif)
  if ! answered "$GC"; then
    t_bad "the plot was never answered at all ($GC)"
  elif [ "$GC" != "200" ]; then
    t_ok "the plot fails outright ($GC) rather than drawing an empty graph"
  else
    t_bad "the plot answered 200 with $(wc -c <pbody.gif) bytes of image"
  fi
}
prep_case "AND log='readings' AND bucket BETWEEN" "the plot query" check_plot

# THE PAIRING BUDGET. Three tries stand between a six-digit code and an
# exhaustive search, and the charge used to have its result dropped: a
# database that would not count the attempt let the exchange go ahead anyway.
check_charge() {
  # A LIVE CODE TO ATTEMPT. It cannot come from the settings page: that page
  # refuses to mint one while an app is already paired, and unpairing here
  # would take the key every case after this one signs with. The row is what
  # the page would have written.
  PUID=$(dbq "$DB" "SELECT id FROM user WHERE email='jk@example.com'")
  dbx "$DB" "INSERT INTO pairing(user_id,code,expires_at,tries)
             VALUES($PUID,'424242',$(date +%s) + 3600,0)
             ON CONFLICT(user_id) DO UPDATE SET code=excluded.code,
               expires_at=excluded.expires_at, tries=0"
  # `pair` runs the whole exchange; it must not get past round 1, because
  # round 1 is where the try is charged.
  RC=$(./synccli 127.0.0.1 $PORT pair jk@example.com 424242 2>&1)
  if printf '%s' "$RC" | grep -q "server error"; then
    t_ok "an attempt that cannot be COUNTED is refused, not made"
  else
    t_bad "the pairing round went ahead uncounted: $(t_show "$RC")"
  fi
  # ...and the refusal is not "no code": the row above is live, and the same
  # exchange against an unfaulted server is the one that paired this suite's
  # phone at the top of the file.
  if printf '%s' "$RC" | grep -q "no pairing code is active"; then
    t_bad "the round never reached the charge: $(t_show "$RC")"
  else
    t_ok "...and it was the CHARGE that refused it, not a missing code"
  fi
  dbx "$DB" "DELETE FROM pairing WHERE user_id=$PUID"
}
prep_case "UPDATE pairing SET tries=tries+1" "the pairing charge" check_charge

# THE THROTTLE'S OWN LOOKUP. Counting a failure is one half; READING the
# count is the other, and it used to answer "not throttled" when the query
# could not be prepared. A database that is merely busy therefore removed the
# only thing standing between this form and an offline-speed password search
# -- silently, and precisely when the server is least able to afford it.
check_thr_read() {
  # An address that IS over the limit, so "not throttled" can only come from
  # the failed read. Written directly: the point is what the reader does with
  # a count it cannot see, not how the count got there.
  dbx "$DB" "INSERT INTO login_fail(email,n,first_at)
             VALUES('jk@example.com', 99, $(date +%s))
             ON CONFLICT(email) DO UPDATE SET n=99, first_at=excluded.first_at"
  LC=$(curl -s -o thrbody.html -w '%{http_code}' -X POST \
       -d 'email=jk@example.com&password=definitelywrong' $U/login)
  if [ "$LC" = "503" ]; then
    t_ok "a throttle counter that cannot be READ refuses the attempt"
  elif [ "$LC" = "429" ]; then
    t_bad "the 429 came from somewhere other than the faulted read"
  else
    t_bad "the login answered $LC with the throttle unreadable -- free guesses"
  fi
  dbx "$DB" "DELETE FROM login_fail WHERE email='jk@example.com'"
}
prep_case "SELECT n,first_at FROM login_fail" "the throttle lookup" \
          check_thr_read

# THE PAIRING BUDGET IS ONE TRANSACTION. The increment and the burn were two
# statements: the count landed, the DELETE that spends a used-up code did not,
# and `tries` climbed past PAIR_TRIES with the code still live. Fault the
# BURN and require that the increment did not survive it either.
check_charge_atomic() {
  PUID=$(dbq "$DB" "SELECT id FROM user WHERE email='jk@example.com'")
  dbx "$DB" "INSERT INTO pairing(user_id,code,expires_at,tries)
             VALUES($PUID,'313131',$(date +%s) + 3600,0)
             ON CONFLICT(user_id) DO UPDATE SET code=excluded.code,
               expires_at=excluded.expires_at, tries=0"
  RC=$(./synccli 127.0.0.1 $PORT pair jk@example.com 313131 2>&1)
  if printf '%s' "$RC" | grep -q "server error"; then
    t_ok "a burn that cannot run refuses the round"
  else
    t_bad "the round went ahead with the budget unenforced: $(t_show "$RC")"
  fi
  TR=$(dbq "$DB" "SELECT tries FROM pairing WHERE user_id=$PUID")
  if [ "$TR" = "0" ]; then
    t_ok "...AND THE INCREMENT DID NOT SURVIVE IT: both or neither"
  else
    t_bad "tries is $TR: the charge landed while the burn did not"
  fi
  dbx "$DB" "DELETE FROM pairing WHERE user_id=$PUID"
}
prep_case "DELETE FROM pairing WHERE user_id=? AND tries>=?" \
          "the pairing burn" check_charge_atomic

# THE LOGIN THROTTLE. An uncounted failure is an unthrottled guess.
check_throttle() {
  LC=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
       -d 'email=jk@example.com&password=definitelywrong' $U/login)
  if [ "$LC" = "500" ]; then
    t_ok "a failure that cannot be RECORDED refuses rather than answers free"
  else
    t_bad "the login answered $LC without counting the attempt"
  fi
}
prep_case "INSERT INTO login_fail" "the login throttle" check_throttle

# SIGN OUT EVERYWHERE. What someone clicks when they believe another person is
# signed in as them; reported as done while the DELETE failed, the old cookies
# keep working for a year.
check_signout() {
  SC=$(curl -s -b jar.txt -o /dev/null -w '%{http_code}' -X POST \
       -d "csrf=$CSRF" $U/settings/signout-all)
  if [ "$SC" = "500" ]; then
    t_ok "a sign-out-everywhere that did not happen says so"
  else
    t_bad "sign-out-everywhere answered $SC without dropping the sessions"
  fi
}
prep_case "DELETE FROM session WHERE user_id=?" "the sign-out-everywhere" \
          check_signout

# UNPAIR. A revocation: the phone's key stops being accepted.
check_unpair() {
  UC=$(curl -s -b jar.txt -o /dev/null -w '%{http_code}' -X POST \
       -d "csrf=$CSRF" $U/settings/unpair)
  if [ "$UC" = "500" ]; then
    t_ok "an unpair that did not delete the key says so"
  else
    t_bad "unpair answered $UC with the app key still in place"
  fi
}
prep_case "DELETE FROM app WHERE user_id=?" "the unpair" check_unpair

prep_case "FROM session WHERE selector=?" "the session lookup" check_session

# LOGGING OUT, which is the one state change whose BROWSER half cannot fail.
# Clearing the cookie and redirecting to the login form always works, so a
# logout over a database that will not delete anything looked, to the person
# who clicked it, exactly like a logout that worked -- while the session row
# kept the year of validity it was issued with. Somebody finishing up on a
# shared machine is shown a completed logout and walks away from a session that
# any copy of that cookie still opens. session_drop was `void`: it discarded
# the result of its prepare and of its step, so there was nothing for the route
# to check even if it had wanted to.
#
# THREE ASSERTIONS, because each without the others is passable by the bug: the
# answer must be a failure, the row must still be there (the answer and the
# storage agreeing is the actual contract), and the browser must still be
# signed in -- a 500 that clears the cookie anyway leaves the user unable to
# retry a logout that did not happen.
check_logout() {
  # Its own session. The failure is supposed to leave one behind, and the cases
  # after this one still need jar.txt's session to work.
  curl -s -c lojar.txt -o /dev/null -X POST \
       -d 'email=jk@example.com&password=correcthorse' $U/login
  LOSEL=$(awk '$6 == "sid" {print $7}' lojar.txt | tail -1 | cut -d: -f1)
  LOCSRF=$(curl -s -b lojar.txt $U/ | grep -o 'name=csrf value="[0-9a-f]*"' |
           head -1 | sed 's/.*value="//; s/"//')
  # -c AS WELL AS -b, so the response's Set-Cookie really is applied to the
  # jar. With -b alone the jar cannot change, and "the browser is still signed
  # in" was then true no matter what the response said -- an assertion that
  # could not fail. This way the third check below is about what a browser
  # would actually do next.
  LOC=$(curl -s -b lojar.txt -c lojar.txt -o lobody.html -w '%{http_code}' \
        -X POST -d "csrf=$LOCSRF" $U/logout)
  if [ "$LOC" = "500" ]; then
    t_ok "a logout whose session delete could not run says so"
  else
    t_bad "logout answered $LOC over a session delete that never ran"
  fi
  LON=$(dbq "$DB" "SELECT count(*) FROM session WHERE selector='$LOSEL'")
  if [ "$LON" = "1" ]; then
    t_ok "...and the row is still there, exactly as that answer implies"
  else
    t_bad "the session row count is '$LON': the answer and the storage disagree"
  fi
  if curl -s -b lojar.txt $U/ | grep -q "name=password"; then
    t_bad "the failed logout signed the browser out anyway -- no way to retry"
  else
    t_ok "...and the user really is still signed in, as the page told them"
  fi
  dbx "$DB" "DELETE FROM session WHERE selector='$LOSEL'"
}
prep_case "DELETE FROM session WHERE selector=?" "the logout" check_logout

# --------------------------------------------- MAINTENANCE, WHICH FAILS OPEN
#
# THE OTHER HALF OF THE RULE. Everything above is fail-CLOSED: a read that did
# not run must never be served as an answer. These three writes are not
# answers -- the session's rolling expiry, the nonce window prune, and the
# app's last_seen stamp -- and applying the same rule to them would turn a
# full memory card into a total outage for requests whose signature verified
# perfectly well.
#
# So the request must still succeed, AND the failure must be reported. Both
# halves are asserted, because each without the other is a defect: a silent
# success is the original bug (the write's result was simply discarded), and
# a rejection is a self-inflicted outage.
maint_ok() { # maint_ok <what the log should name>
  MC=$(cat "$SYNCCLI_CODE_FILE" 2>/dev/null)
  if [ "$MC" = "200" ]; then
    t_ok "the signed request still succeeds"
  else
    t_bad "a failed maintenance write REJECTED a good request ($MC)"
  fi
  if grep -q "auth maintenance: $1 FAILED" srvfault.log; then
    t_ok "...and the failure is reported, naming the operation"
  else
    t_bad "...but nothing said $1 had failed: $(t_show "$(cat srvfault.log)")"
  fi
}

check_prune() {
  ./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" GET /v1/digest >/dev/null 2>&1
  maint_ok "nonce window prune"
}
prep_case "DELETE FROM nonce WHERE seen_at" "the nonce window prune" \
  check_prune

check_lastseen() {
  ./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" GET /v1/digest >/dev/null 2>&1
  maint_ok "app last_seen stamp"
}
prep_case "UPDATE app SET last_seen=?" "the last_seen stamp" check_lastseen

# THE SESSION'S ROLLING EXPIRY, which is the one with teeth. The other two are
# housekeeping the user never sees; this one is what keeps somebody who is
# using the site LOGGED IN, and the two ways to get it wrong are opposites:
#
#   * reject the request because the extension failed, and a browser that was
#     perfectly well signed in is thrown back to the login form by a write
#     that had nothing to do with whether it was signed in;
#   * drop the failure silently -- the original bug -- and the session ages
#     out at its ORIGINAL expiry while the person is still using it, which
#     afterwards looks like nothing at all: no error, no log line, just a
#     login form one day.
#
# AND THE RETRY. The claim in auth.c is that no retry has to be scheduled
# because the write is driven by state the write itself would have changed:
# the update fires while `now - last_seen > SESS_BUMP`, and a failed update
# leaves last_seen exactly where it was, so the NEXT request tries again
# instead of waiting another day. That is only true if `last_seen` is read
# fresh from the row each time and nothing else stamps it, which is a property
# of the code and therefore something a test can hold still. So this case
# makes two requests and requires the second one to have tried AGAIN.
check_bump() {
  # The row says the session has not been touched since the epoch, so the
  # rolling expiry is due. Nothing else about the session changes: expires_at
  # is still a year out, which is what makes this a live session whose
  # EXTENSION fails rather than an expired one.
  dbx "$DB" "UPDATE session SET last_seen=0"
  BC1=$(curl -s -b jar.txt -o bbody.html -w '%{http_code}' $U/)
  N1=$(grep -c "auth maintenance: session rolling expiry FAILED" srvfault.log)
  BC2=$(curl -s -b jar.txt -o bbody2.html -w '%{http_code}' $U/)
  N2=$(grep -c "auth maintenance: session rolling expiry FAILED" srvfault.log)

  if [ "$BC1" = "200" ] && [ "$BC2" = "200" ]; then
    t_ok "an expiry extension that failed does not log the reader out"
  else
    t_bad "a failed rolling expiry answered $BC1 then $BC2 to a valid session"
  fi
  # ...and not by rendering the login form under a 200, which is the same
  # outcome wearing the right status code.
  if grep -qi "name=password" bbody.html; then
    t_bad "a failed rolling expiry showed the LOGIN FORM"
  else
    t_ok "...and did not put the login form in front of a valid session"
  fi
  if [ "${N1:-0}" -ge 1 ]; then
    t_ok "...and the failure is reported, naming the operation"
  else
    t_bad "...but nothing said the rolling expiry had failed: $(t_show "$(cat srvfault.log)")"
  fi
  # THE RETRY, stated as the only thing that distinguishes it from a write
  # that gave up: the second request attempted it too.
  if [ "${N2:-0}" -gt "${N1:-0}" ]; then
    t_ok "...and the NEXT request retried it rather than waiting a day"
  else
    t_bad "the rolling expiry was not retried ($N1 then $N2 attempts)"
  fi
  # Leave the session looking recently used again, so the cases after this one
  # meet the same fixture every other case did.
  dbx "$DB" "UPDATE session SET last_seen=$(date +%s)"
}
prep_case "UPDATE session SET expires_at=" "the session rolling expiry" \
  check_bump

# ------------------------------------------------------------------ the STEP
echo "== a STEP that fails PARTWAY THROUGH a scan =="
# The case the whole rule is about, and the one a damaged file cannot aim at:
# every reader here has real rows to hand back before the failure, so the
# tempting answer -- "return what I read" -- is available and wrong.
# step_case with a check of its own, for a statement whose failure is not
# visible on the home page.
step_case_fn() { # step_case_fn <sql-fragment> <name> <check-fn>
  : > srvfault.log
  # FIRST-step, because this reader takes one row and finalises: a fault
  # placed after the real rows is never stepped to (see fault_step_wrap).
  FAULT_LOG=srvfault.log
  if ! fault_serve "$2" DB_FAIL_STEP_FIRST_SQL "$1"; then
    t_bad "$2: the server did not start with a step fault armed"
    return
  fi
  "$3"
  if grep -q "INJECTED FAULT DB_FAIL_STEP_SQL" srvfault.log; then
    t_ok "$2: the injected step failure really happened"
  else
    t_bad "$2: the fault never fired -- the case proved nothing"
  fi
  # ...and it must not have fired as a PREPARE error, which is what a
  # column-count mismatch in the wrapper produces.
  if grep -q "do not have the same number of result columns" srvfault.log; then
    t_bad "$2: the wrapper broke the statement instead of its STEP"
  fi
  kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; SRVPID=
}

step_case() { # step_case <sql-fragment> <what it reads>
  FAULT_LOG=/dev/null
  if ! fault_serve "$2" DB_FAIL_STEP_SQL "$1"; then
    t_bad "the server did not start with a step fault armed on: $2"
    return
  fi
  SCODE=$(curl -s -b jar.txt -o sbody.html -w '%{http_code}' $U/)
  if ! answered "$SCODE"; then
    t_bad "$2: the page was never answered at all ($SCODE)"
  elif [ "$SCODE" != "200" ]; then
    t_ok "$2: a scan that stopped is a failure, not a short answer ($SCODE)"
  elif grep -qi "could not be read" sbody.html; then
    t_ok "$2: ...or says in words that it could not be read whole"
  else
    t_bad "$2: the page rendered what it managed to read AS THE RECORD"
  fi
  kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; SRVPID=
}
# The two scans behind the home page: the recent table, and the newest value
# that becomes the big number.
# THE THROTTLE'S READ, mid-scan. The prepare case above covers a query that
# never ran; this one covers the branch that decides what a step which did not
# FINISH means -- BUSY, an I/O error, a damaged page. It answers "no row",
# which used to read as "this address is not throttled".
#
# It needs the wrapper to preserve the column count: this SELECT returns two,
# and the wrapper appended a one-column UNION ALL, so the statement failed to
# PREPARE and the case would have passed through the prepare path with the
# step branch never executed. See fault_step_wrap.
step_thr() {
  dbx "$DB" "INSERT INTO login_fail(email,n,first_at)
             VALUES('jk@example.com', 99, $(date +%s))
             ON CONFLICT(email) DO UPDATE SET n=99, first_at=excluded.first_at"
  LC=$(curl -s -o /dev/null -w '%{http_code}' -X POST \
       -d 'email=jk@example.com&password=definitelywrong' $U/login)
  if [ "$LC" = "503" ]; then
    t_ok "a throttle scan that STOPPED refuses, and says it is transient"
  elif [ "$LC" = "500" ]; then
    t_bad "the refusal came from the PREPARE path, not the step branch"
  else
    t_bad "the login answered $LC over an unreadable throttle counter"
  fi
  dbx "$DB" "DELETE FROM login_fail WHERE email='jk@example.com'"
}
step_case_fn "SELECT n,first_at FROM login_fail" "the throttle scan" step_thr

step_case "AND bucket >= ?" "the recent-readings table"
step_case "ORDER BY bucket DESC, line DESC LIMIT 400" "the newest reading"

# ...and with no fault injected, the same requests are all normal: a suite that
# fails everything proves nothing either.
echo "== the same requests, with nothing injected =="
FAULT_LOG=/dev/null
fault_serve "the fault-free server" ||
  { echo "faulttest: server did not start"; exit 1; }
ck "the digest is served" "^readings 2 " \
   "$(./synccli 127.0.0.1 $PORT req "$APPUID" "$KEY" GET /v1/digest 2>&1)"
ck_clicode "...as a 200" 200
ck "the page is served, with the reading on it" "187" \
   "$(curl -s -b jar.txt $U/)"

echo "== a password change is one transaction, on BOTH surfaces =="
#
# ITEM 149. The two halves of a password change -- the new hash, and the
# revocation of every session issued against the old one -- used to be two
# separate commits, with the CLI doing the second one itself and the browser
# path not doing it at all. Half of that applied is the worst state of the
# three: a stolen cookie goes on signing somebody in against a password its
# owner has just changed BECAUSE they thought it was stolen, and the server
# has told them they are safe.
#
# They are one transaction in user_set_password (srv/auth.c) now. Proving it
# needs the revocation to fail with the hash already written, which a real
# database will not do on request -- hence this suite. The injected fault
# names session_drop_all's statement by substring.
#
# WHAT MAKES THIS DIFFERENT FROM ASSERTING AN ERROR CODE: the old, broken code
# returned a failure here too. It had already committed the new password. So
# the assertion is which password opens the account afterwards, not what the
# command printed.
printf 'firstpassword\n' | ./sync adduser pwtx@example.com stdin . \
    >/dev/null 2>&1
PWUID=$(dbq "$DB" "SELECT id FROM user WHERE email='pwtx@example.com'")
ck "the fixture account for the password cases exists" "^[0-9][0-9]*$" "$PWUID"
rm -f pw.jar
curl -s -c pw.jar -o /dev/null -X POST \
     -d 'email=pwtx@example.com&password=firstpassword' $U/login
ck_db "...and it has one live browser session, which is what there is to lose" \
   "SELECT count(*) FROM session WHERE user_id=$PWUID" "^1$"

# THE CLI, with the revocation broken. The fault variable is set on THIS
# process only -- the server above is untouched -- so the only prepare it can
# reach is the DELETE inside user_set_password.
printf 'secondpassword\n' |
  DB_FAIL_PREPARE_SQL='DELETE FROM session WHERE user_id=?' \
  ./sync passwd pwtx@example.com stdin . >pwfail.out 2>pwfail.err
PWRC=$?
if [ "$PWRC" != 0 ]; then
  t_ok "sync passwd exits nonzero when the revocation cannot run"
else
  t_bad "sync passwd exited 0 over a revocation that did not happen"
fi
if grep -q "INJECTED FAULT" pwfail.err; then
  t_ok "...and the fault really fired (without it this case proves nothing)"
else
  t_bad "the revocation fault never fired: every case below is meaningless"
fi
ck "...and it says the password was NOT changed" "NOT changed" "$(cat pwfail.err)"

# A LOOKUP THAT DID NOT RUN IS NOT AN ACCOUNT THAT IS NOT THERE.
#
# user_by_email answers 0 for both, and the CLI callers used to pass NULL for
# its failure output -- so an unreadable database told the operator "no such
# user", which sends them to check the spelling of an address that is
# perfectly correct while the real fault goes unmentioned. The two messages
# are what this pins; the exit status is the same either way, so a status
# check alone cannot tell them apart.
DB_FAIL_PREPARE_SQL='SELECT id FROM user' \
  ./sync logout pwtx@example.com . >lk.out 2>lk.err || true
ck "a lookup that could not run says the DATABASE did not answer" \
   "could not look up" "$(cat lk.err)"
nk "...and does NOT report it as a missing account" \
   "no such user" "$(cat lk.err)"
# The control: with no fault, the same command on an address nobody has still
# says exactly what it always said.
./sync logout nobody@example.com . >lk2.out 2>lk2.err || true
ck "an address nobody has is still 'no such user'" \
   "no such user" "$(cat lk2.err)"
nk "...and is not dressed up as a database fault" \
   "did not answer" "$(cat lk2.err)"

# THE ASSERTION THAT TELLS THE FIX FROM THE BUG.
ck "THE OLD PASSWORD STILL SIGNS IN: the hash rolled back with the revocation" \
   "sid" "$(rm -f pwold.jar
            curl -s -c pwold.jar -o /dev/null -X POST \
                 -d 'email=pwtx@example.com&password=firstpassword' $U/login
            cat pwold.jar)"
ck "...and the password the failed command tried to set does NOT" \
   "Wrong email or password" \
   "$(curl -s -X POST \
       -d 'email=pwtx@example.com&password=secondpassword' $U/login)"
ck "...and the session that was live before it is live still: no mixed state" \
   "pwtx@example.com" "$(curl -s -b pw.jar $U/)"

# THE CONTROL: the same command with nothing injected.
dbx "$DB" "DELETE FROM session WHERE user_id=$PWUID"
rm -f pw.jar
curl -s -c pw.jar -o /dev/null -X POST \
     -d 'email=pwtx@example.com&password=firstpassword' $U/login
printf 'secondpassword\n' | ./sync passwd pwtx@example.com stdin . \
    >pwok.out 2>pwok.err
ck "sync passwd succeeds with nothing injected" "all sessions signed out" \
   "$(cat pwok.out)"
ck_db "...and EVERY session that account had is gone" \
   "SELECT count(*) FROM session WHERE user_id=$PWUID" "^0$"
ck "...so the cookie that was live before the change has stopped working" \
   "Sign in" "$(curl -s -b pw.jar $U/)"
ck "...and the NEW password signs in" "sid" \
   "$(rm -f pwnew.jar
      curl -s -c pwnew.jar -o /dev/null -X POST \
           -d 'email=pwtx@example.com&password=secondpassword' $U/login
      cat pwnew.jar)"
ck "...and the old one no longer does" "Wrong email or password" \
   "$(curl -s -X POST \
       -d 'email=pwtx@example.com&password=firstpassword' $U/login)"

# ---- THE SAME FAILURE, THROUGH THE BROWSER FORM ------------------------
#
# The other surface, and the one that carried the defect: /settings/password
# used to report "Password changed." having revoked nothing. It calls the same
# operation now -- clitest.sh pins that in the source -- and this pins that the
# BEHAVIOUR is the same, which is the half a source grep cannot see.
#
# The session and the first CSRF token are taken while the server is still
# fault-free on purpose: `DELETE FROM session WHERE user_id=?` is also the
# statement session_new uses to trim an account back to the cap, so with the
# fault armed there is no logging in.
rm -f web.jar
curl -s -c web.jar -o /dev/null -X POST \
     -d 'email=pwtx@example.com&password=secondpassword' $U/login
WSESS=$(dbq "$DB" "SELECT count(*) FROM session WHERE user_id=$PWUID")
ck "a browser session exists for the web case" "^[1-9][0-9]*$" "$WSESS"
kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; SRVPID=

: > srvpw.log
FAULT_LOG=srvpw.log
if fault_serve "the settings page with its revocation broken" \
     DB_FAIL_PREPARE_SQL 'DELETE FROM session WHERE user_id=?'; then
  WCSRF=$(curl -s -b web.jar $U/settings |
          grep -o 'name=csrf value="[0-9a-f]*"' | head -1 |
          sed 's/.*value="//;s/"//')
  ck "the settings page still renders for that session" "^[0-9a-f]\{8,\}$" \
     "$WCSRF"
  WOUT=$(curl -s -b web.jar -X POST \
         -d "csrf=$WCSRF&old=secondpassword&new=thirdpassword" \
         $U/settings/password)
  ck "the settings page says the password was NOT changed" "was not changed" \
     "$WOUT"
  nk "...and does not claim the change was made" "Password changed" "$WOUT"
  if grep -q "INJECTED FAULT" srvpw.log; then
    t_ok "...and the fault fired inside the SERVER, not somewhere else"
  else
    t_bad "the fault never fired in the server: the web case proves nothing"
  fi
  kill "$SRVPID" 2>/dev/null; wait "$SRVPID" 2>/dev/null; SRVPID=
else
  t_bad "the server did not start with the revocation fault armed"
fi

FAULT_LOG=/dev/null
if fault_serve "the fault-free server, to ask what the browser form left"; then
  ck "THE OLD PASSWORD STILL SIGNS IN after the browser form failed too" "sid" \
     "$(rm -f web2.jar
        curl -s -c web2.jar -o /dev/null -X POST \
             -d 'email=pwtx@example.com&password=secondpassword' $U/login
        cat web2.jar)"
  ck "...and the one the form tried to set does not" "Wrong email or password" \
     "$(curl -s -X POST \
         -d 'email=pwtx@example.com&password=thirdpassword' $U/login)"
  ck "...and the session that made the request survived it" "pwtx@example.com" \
     "$(curl -s -b web.jar $U/)"
else
  t_bad "the fault-free server did not come back"
fi

t_end
if [ "$fail" = 0 ]; then
  printf '\033[1;32mfaulttest\033[0m: a statement that did not finish is never an answer\n'
else
  printf 'faulttest: FAILED\n'
fi
exit $fail
