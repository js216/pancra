#!/bin/bash
# emailtest.sh --- one address rule, on every surface that takes an address
#
# WHAT THIS PINS, AND WHY IT IS ITS OWN SUITE.
#
# /login refused an address longer than RFC 5321 allows and every other
# surface did not. The invitation form -- the same two fields, the same
# throttle, the same account table, and the ONLY path in this program that
# CREATES an account -- read the field into a kilobyte of stack and went
# straight on to the throttle lookup, the account lookup, the failure record
# and user_create. `sync adduser` took whatever was on its command line. So an
# account could be made under an address /login will never resolve (there is no
# password reset here), and a stranger could write a login_fail row keyed on a
# string no later attempt can ever produce again -- a row the throttle can
# never match and no successful login can ever clear.
#
# The second half is not about length at all. `user.email` is COLLATE NOCASE,
# so the account lookup is case-insensitive; `login_fail.email` is a TEXT
# PRIMARY KEY with no collation, so the throttle is case-SENSITIVE. One account
# therefore had as many throttle rows as there are spellings of its address,
# and the counter never reached LOGIN_FAIL_MAX.
#
# WHAT AN ASSERTION HERE HAS TO SAY. A case that only checks the HTTP status
# proves nothing about what was RECORDED: a 401 is what a wrong password gets
# too, and the defect was never in the answer, it was in the rows written on
# the way to it. So every refusal below is asserted against the DATABASE --
# login_fail and user -- and not against the response.
#
# Run by `make emailtest`.
set -u

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/test/testlib.sh"

DIR=$(mktemp -d)
T_TMP=$DIR
DB=$DIR/sync.db
SRVPID=
cleanup() {
  if [ -n "$SRVPID" ]; then
    kill "$SRVPID" 2>/dev/null || true
    wait "$SRVPID" 2>/dev/null || true
  fi
  rm -rf "$DIR"
}
trap cleanup EXIT INT TERM

need python3 "the address-rule suite" || exit 1
need curl "the address-rule suite" || exit 1

SYNCBIN=${1:-$HERE/build/srv/sync}
CLIBIN=$(dirname "$SYNCBIN")/synccli
[ -x "$SYNCBIN" ] || { echo "emailtest: $SYNCBIN is not built"; exit 1; }
cp "$SYNCBIN" "$DIR/sync"
cp "$CLIBIN" "$DIR/synccli"
cd "$DIR" || exit 1

# 300 bytes: comfortably past the 254 the rule allows, and past nothing else --
# it still fits every buffer on the way in, so a refusal here is the rule
# working and not a length limit somewhere downstream doing it by accident.
LONGLOCAL=$(python3 -c 'print("a"*290)')
LONG="$LONGLOCAL@example.com"

echo "== the CLI, which is where an account comes from before there is a web =="

ck "adduser makes the account this suite signs in as" "created user 1" \
   "$(printf 'correcthorse\n' | ./sync adduser jk@example.com stdin . 2>&1)"

# THE CANONICAL FORM IS WHAT IS STORED. Mixed case worked before (the column
# is NOCASE) but was stored as typed, so the address on the settings page and
# in `sync invites` was a spelling no throttle row would ever match.
printf 'mixedpassword\n' |
  ./sync adduser MiXeD@Example.COM stdin . >/dev/null 2>&1
ck_db "adduser stores the address folded to lower case" \
   "SELECT email FROM user WHERE id=2" "^mixed@example.com$"
nk_db "...and not the spelling that was typed" \
   "SELECT email FROM user WHERE id=2" "MiXeD"

# AN OVER-LONG ADDRESS MAKES NO ACCOUNT. It used to make one: the row went in,
# and its owner could never sign in through /login, which refuses the address
# before it looks anything up.
printf 'longpassword\n' | ./sync adduser "$LONG" stdin . >/dev/null 2>&1
ck_db "adduser creates NO account for an over-long address" \
   "SELECT count(*) FROM user WHERE length(email) > 254" "^0$"
ck_db "...and the account table still holds only the two real accounts" \
   "SELECT count(*) FROM user" "^2$"

# The shapes that are not addresses at all. "@" cleared the old rule, which
# asked only that an '@' appear somewhere.
for bad in "@" "nobody" "a@" "@b" "two@at@signs" "has space@example.com"; do
  printf 'pw\n' | ./sync adduser "$bad" stdin . >/dev/null 2>&1
done
ck_db "no account is made for an address that is not one" \
   "SELECT count(*) FROM user" "^2$"

# ...and the verbs that RESOLVE an address reach the same account whatever the
# case, because a revocation aimed at the wrong spelling reports "no such user"
# and leaves every stolen cookie working.
ck "passwd finds the account through a different spelling" \
   "password changed for jk@example.com" \
   "$(printf 'newcorrecthorse\n' |
      ./sync passwd JK@Example.COM stdin . 2>&1)"
ck "logout finds it too" "signed out everywhere: jk@example.com" \
   "$(./sync logout jk@EXAMPLE.com . 2>&1)"
ck "...and an over-long address is refused rather than looked up" \
   "not an address this server will accept" \
   "$(./sync logout "$LONG" . 2>&1)"
ck "passwd refuses an over-long address too, rather than reporting no user" \
   "not an address this server will accept" \
   "$(printf 'pw\n' | ./sync passwd "$LONG" stdin . 2>&1)"

# `sync invite <owner>` resolves an owner the same way. A link minted against
# the wrong spelling is not a link that fails loudly -- user_by_email would
# find the account anyway under NOCASE -- so what this pins is that the
# resolution goes through the same rule as everything else, and that an address
# the rule refuses does not silently become a signup-only link with no owner.
./sync invite JK@Example.COM . >/dev/null 2>&1
ck_db "sync invite resolves the owner through a different spelling" \
   "SELECT count(*) FROM share_token t JOIN user u ON u.id=t.owner_id
      WHERE u.email='jk@example.com'" "^1$"
ck "...and refuses an address the rule will not accept" \
   "not an address this server will accept" \
   "$(./sync invite "$LONG" . 2>&1)"
ck_db "...minting nothing for it" \
   "SELECT count(*) FROM share_token" "^1$"

echo "== the server =="

sync_up() { # sync_up <port>
  ./sync "$1" . >/dev/null 2>&1 &
  T_PID=$!
}
if serve "sync" http / sync_up; then
  PORT=$T_PORT
  SRVPID=$T_PID
else
  echo "emailtest: server did not become ready on any port"; exit 1
fi
U="http://127.0.0.1:$PORT"
ck_owns "the server under test owns the port everything below uses" \
   "$SRVPID" "$PORT"

# A CLEAN THROTTLE TABLE IS THE BASELINE EVERY CASE BELOW MEASURES AGAINST.
dbx "$DB" "DELETE FROM login_fail"
ck_db "the throttle table starts empty" "SELECT count(*) FROM login_fail" "^0$"

# ---- /login -------------------------------------------------------------
#
# The bound was already here. What was not here is the guarantee that the
# refusal happens before the throttle is touched, and that the counter the
# throttle reads is keyed on one canonical string.

req 401 POST /login --data-urlencode "email=$LONG" --data "password=x" >/dev/null
ck_code "an over-long address is refused at /login" 401
ck_db "...and NOTHING was written to the throttle table" \
   "SELECT count(*) FROM login_fail" "^0$"
nk_db "...not even a row nobody could ever match again" \
   "SELECT email FROM login_fail" "$LONGLOCAL"

# THE ISOLATING PART. A refusal that happened AFTER the lookup would leave a
# row; a refusal that happened after user_create would leave an account. Both
# would still answer 401, which is why the two assertions above are about rows.

req 401 POST /login --data "email=%40" --data "password=x" >/dev/null
ck_db "an address that is only an at-sign writes no throttle row either" \
   "SELECT count(*) FROM login_fail" "^0$"

# ---- ONE COUNTER PER ACCOUNT, NOT ONE PER SPELLING ----------------------
#
# Five wrong passwords is LOGIN_FAIL_MAX. Spread across five spellings of the
# one address they used to be five rows of one, and the throttle never fired.
# Folded, they are one row of five and the sixth attempt is refused 429.
for spelling in jk@example.com JK@example.com jk@EXAMPLE.com Jk@Example.Com \
                JK@EXAMPLE.COM; do
  req 401 POST /login --data "email=$spelling" --data "password=wrong" >/dev/null
done
ck_db "five spellings of one address are ONE throttle row" \
   "SELECT count(*) FROM login_fail" "^1$"
ck_db "...and that row has counted all five" \
   "SELECT n FROM login_fail WHERE email='jk@example.com'" "^5$"
req 429 POST /login --data "email=jk@example.com" --data "password=wrong" \
   >/dev/null
ck_code "...so the sixth attempt is throttled, which is what the counter is for" \
   429
# And the throttle recognises the same account under any spelling, which is the
# other half: a counter that fires only for the exact string it was written
# under is a counter an attacker walks around by shifting a letter.
req 429 POST /login --data "email=JK@ExAmPlE.cOm" --data "password=wrong" \
   >/dev/null
ck_code "...under a spelling it has never seen before, too" 429

dbx "$DB" "DELETE FROM login_fail"

# ---- THE EDGES ARE TRIMMED, BOTH OF THEM --------------------------------
#
# A person pasting an address out of a message brings the spaces around it, and
# a form field is not always trimmed before it arrives. Refusing that would be
# gratuitous -- the address is exactly the one they meant -- and, worse, it
# would key a DIFFERENT throttle row from the same person typing it cleanly.
# Trimmed, it is the same string on every surface. A space in the MIDDLE is a
# different thing and stays refused: it cannot be tidied into one answer.
req 303 POST /login --data-urlencode "email= jk@example.com" \
   --data "password=newcorrecthorse" >/dev/null
ck_code "a LEADING space is trimmed, and the sign-in succeeds" 303
req 303 POST /login --data-urlencode "email=jk@example.com  " \
   --data "password=newcorrecthorse" >/dev/null
ck_code "...and so is a TRAILING one" 303
req 401 POST /login --data-urlencode "email=jk@ex ample.com" \
   --data "password=newcorrecthorse" >/dev/null
ck_code "a space in the MIDDLE is still refused" 401
ck_db "...and writes no throttle row, because it is refused before the lookup" \
   "SELECT count(*) FROM login_fail" "^0$"

# ---- WHAT THE ROW IS KEYED ON, WHICH IS THE ASSERTION WITH THE TEETH -----
#
# The two cases above ask that no row exists at all, and they were WEAKER than
# they looked. An over-long address has no account -- email_canon is what stops
# one being made -- so on the invitation form the lookup misses, the failure
# branch is never entered, and no row would be written even if the address rule
# ran last. That case pins "no ACCOUNT was created"; it does not pin the order.
#
# THE ORDER IS PINNED HERE INSTEAD, with an address that is refused by the rule
# and DOES resolve to an existing account: a spelling in the wrong case. Under
# the rule it is folded, the throttle counts against the canonical row, and the
# answer is 401. With the rule moved after the failure record, the row is keyed
# on the spelling as typed -- one more row per spelling, none of them ever
# matched again -- which is precisely the defect, and it is what the KEY of the
# row says, not the count and not the status.
req 401 POST /login --data "email=JK@Example.COM" --data "password=wrong" \
   >/dev/null
ck_code "a wrong password under a mixed-case spelling is an ordinary refusal" \
   401
ck_db "...and the failure is recorded, so the throttle really is counting" \
   "SELECT count(*) FROM login_fail" "^1$"
ck_db "...against the CANONICAL address, not the spelling that was typed" \
   "SELECT email FROM login_fail" "^jk@example.com$"
nk_db "...so no row carries a spelling no later attempt can reproduce" \
   "SELECT email FROM login_fail" "JK@Example"
dbx "$DB" "DELETE FROM login_fail"

# The same for the invitation form, which is the surface that had no rule at
# all. It reaches the failure record by the same door: an address that HAS an
# account, and a password that is not its.
WRONGPW_LINK=$(./sync invite . 2>/dev/null)
WRONGPW_TOKEN=${WRONGPW_LINK##*/}
req 401 POST "/invite/$WRONGPW_TOKEN" --data "action=go" \
   --data "email=Jk@ExAmPle.com" --data "password=wrong" >/dev/null
ck_code "the invitation form refuses a wrong password the same way" 401
ck_db "...and records the failure against the canonical address" \
   "SELECT email FROM login_fail" "^jk@example.com$"
nk_db "...never against the spelling the request happened to use" \
   "SELECT email FROM login_fail" "Jk@ExAmPle"
dbx "$DB" "DELETE FROM login_fail"

# ---- the invitation form ------------------------------------------------
#
# This is the surface the item was filed about: no bound at all, and the only
# path that creates an account.

LINK=$(./sync invite . 2>/dev/null)
TOKEN=${LINK##*/}
if [ -n "$TOKEN" ] && [ "$TOKEN" != "$LINK" ]; then
  t_ok "an invitation link was minted to redeem"
else
  t_bad "no invitation token to test with (got '$LINK')"
fi

USERS_BEFORE=$(dbq "$DB" "SELECT count(*) FROM user")

req 400 POST "/invite/$TOKEN" --data "action=go" \
    --data-urlencode "email=$LONG" --data "password=x" >/dev/null
ck_code "an over-long address is refused at the invitation form" 400
ck_db "...and NO account was created for it" \
   "SELECT count(*) FROM user WHERE length(email) > 254" "^0$"
ck_db "...the account table is exactly as it was" \
   "SELECT count(*) FROM user" "^$USERS_BEFORE\$"
ck_db "...and NOTHING was written to the throttle table" \
   "SELECT count(*) FROM login_fail" "^0$"
ck_db "...and the invitation was not spent on a request that did nothing" \
   "SELECT count(*) FROM share_token WHERE token='$TOKEN' AND used_at IS NULL" \
   "^1$"

# The same for a shape that is not an address. It used to reach user_create,
# whose only rule was "an '@' appears somewhere" -- so "@" made an account.
req 400 POST "/invite/$TOKEN" --data "action=go" --data "email=%40" \
    --data "password=x" >/dev/null
ck_db "an at-sign alone creates no account here either" \
   "SELECT count(*) FROM user" "^$USERS_BEFORE\$"
ck_db "...and writes no throttle row" "SELECT count(*) FROM login_fail" "^0$"

# AND THE ORDINARY PATH STILL WORKS -- a rule that refused everything would
# pass every assertion above. The address is given in mixed case on purpose:
# the account it creates must be the canonical one, or the person who just
# made it cannot sign in to it under the spelling they will type next time.
req 303 POST "/invite/$TOKEN" --data "action=go" \
    --data "email=NewPerson@Example.com" --data "password=apassword" >/dev/null
ck_code "a good address on the invitation form creates the account" 303
ck_db "...stored in the canonical form" \
   "SELECT count(*) FROM user WHERE email='newperson@example.com'" "^1$"
nk_db "...not as it was typed" "SELECT email FROM user" "NewPerson"

# ...and that account signs in through /login under a third spelling.
req 303 POST /login --data "email=NEWPERSON@example.COM" \
    --data "password=apassword" >/dev/null
ck_code "the new account signs in at /login whatever the case" 303

# ---- the pairing CLI ----------------------------------------------------
#
# synccli built its round-1 body with snprintf into a kilobyte and never
# checked the result: an over-long address TRUNCATED the J-PAKE packet and
# handed http_do a length past the end of the buffer, and the server answered
# with a complaint about the crypto.
CLIOUT=$(./synccli 127.0.0.1 "$PORT" pair "$LONG" 123456 2>&1)
ck "synccli refuses an over-long address before it builds a packet" \
   "not an address this server will accept" "$CLIOUT"
nk "...and does not complain about the crypto instead" \
   "ZKP\|round1 build" "$CLIOUT"

# ---- the pairing ROUTE --------------------------------------------------
#
# It writes nothing and counts nothing, so no row was ever at stake here; what
# is at stake is that "which account is this?" has one answer everywhere. Its
# own bound was 127 bytes, in its own file, for the same question.
PKT=$(python3 -c 'print("0"*320)')

# WITH NO CODE SHOWING, EVERY ADDRESS GETS THE SAME ANSWER, on purpose: "no
# such user", "no code showing" and "code expired" are one reply so the route
# cannot be used to test which addresses have accounts. That is also why a case
# built only on this reply proves NOTHING about which account was resolved --
# it is the same 403 either way. Asserted anyway, because it is the state the
# next case changes.
printf 'JK@Example.COM\n%s\n' "$PKT" > "$DIR/p1.txt"
req 403 POST /v1/pair/1 --data-binary "@$DIR/p1.txt" >/dev/null
ck_code "with no code showing, pairing round 1 refuses whatever the spelling" \
   403
ck_db "...and no pairing attempt wrote a throttle row" \
   "SELECT count(*) FROM login_fail" "^0$"

# NOW PUT A LIVE CODE ON THE ACCOUNT, which is what makes the resolution
# observable. A round 1 that FOUND the account gets as far as the J-PAKE proof
# and fails there ("round 1 rejected", 400, because the packet is 320 zeros); a
# round 1 that did not find it never gets there and answers 403. The two are
# distinguishable, so this is the case that says a mixed-case address reaches
# the same account the browser signs into -- and the ONLY one here that can.
dbx "$DB" "INSERT OR REPLACE INTO pairing(user_id,code,expires_at,tries)
           SELECT id, '123456', strftime('%s','now') + 600, 0
             FROM user WHERE email = 'jk@example.com'"
ck_db "a live pairing code is showing for the account" \
   "SELECT count(*) FROM pairing" "^1$"
req 400 POST /v1/pair/1 --data-binary "@$DIR/p1.txt" >/dev/null
ck_code "a mixed-case address in round 1 resolves to the SAME account, so the exchange gets as far as the proof" \
   400
# ...and the canonical spelling of course does too, so the case above is about
# the folding and not about the route working at all.
printf 'jk@example.com\n%s\n' "$PKT" > "$DIR/p1c.txt"
req 400 POST /v1/pair/1 --data-binary "@$DIR/p1c.txt" >/dev/null
ck_code "...the same as the canonical spelling" 400
# AND THE EDGES ARE TRIMMED, which at this route is the one thing the rule
# adds that nothing else was doing. Case is not it: `user.email` is NOCASE, so
# the lookup already folded case on its own. A trailing space or CR is
# different -- it is not part of the address, no collation removes it, and a
# first line that carried one used to resolve to NOTHING and be answered "no
# pairing code is active". That is the most confusing reply available: the code
# IS showing, on the screen, and the phone says it is not.
printf 'jk@example.com \r\n%s\n' "$PKT" > "$DIR/p1w.txt"
req 400 POST /v1/pair/1 --data-binary "@$DIR/p1w.txt" >/dev/null
ck_code "a first line with a trailing space and CR still names the account" 400
#
# An over-long address gets 400 here rather than 403, and NOT from this rule:
# split_body refuses a first line that does not fit its 128-byte buffer, so
# pairing's own bound (127) bites before the address rule's (254) can. Stated
# rather than left to be discovered: it means the LENGTH half of the rule is
# unreachable at this route, and the trimming and shape halves are what it
# contributes.
printf '%s\n%s\n' "$LONGLOCAL@e.com" "$PKT" > "$DIR/p1l.txt"
LONGBODY=$(req 400 POST /v1/pair/1 --data-binary "@$DIR/p1l.txt")
ck "an over-long first line is refused by pairing's own framing, before the address rule" \
   "expected" "$LONGBODY"
ck_db "...and pairing wrote no throttle row for any of it" \
   "SELECT count(*) FROM login_fail" "^0$"

t_end
if [ "$fail" = 0 ]; then
  printf '\033[1;32memailtest\033[0m: one address rule, on every surface\n'
else
  printf 'emailtest: FAILED\n'
fi
exit $fail
