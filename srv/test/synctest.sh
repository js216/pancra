#!/bin/bash
# Exercise sync: the web login, pairing, the signed API, the digest/bucket
# protocol, and sharing -- against a throwaway instance on a spare port.
# Every case states what the contract promises, so a failure names the rule.
set -u
DIR=$(mktemp -d)
# The PORT is a parameter, not a constant.
#
# `make check -j4` runs srvcheck and srvasan concurrently, and both are this
# same script. With one hardcoded port they raced for the listener and, worse,
# each one's cleanup (`pkill -f "sync $PORT"`) killed the OTHER's server --
# which shows up as a run whose later assertions all get an empty response.
PORT=${2:-18091}
# The repo root is two levels up now (srv/test/), and the binaries are built
# under build/ like everything else.
HERE=$(cd "$(dirname "$0")/../.." && pwd)
# Which build to exercise. Defaults to the ordinary one; `make srvasan` passes
# the sanitizer build, which lives in its own object tree so the two cannot
# overwrite each other.
SYNCBIN=${1:-$HERE/build/srv/sync}
CLIBIN=$(dirname "$SYNCBIN")/synccli
cp "$SYNCBIN" "$DIR/sync"
cp "$CLIBIN" "$DIR/synccli"
cd "$DIR" || exit 1

fail=0
ck() { # ck <name> <expected-substring> <actual>
  if printf '%s' "$3" | grep -q -- "$2"; then
    echo "  ok   $1"
  else
    echo "  FAIL $1: wanted '$2', got: $(printf '%s' "$3" | tr '\n' '|' | cut -c1-300)"
    fail=1
  fi
}
empty() { # empty <name> <actual> -- grep cannot match "nothing"
  if [ -z "$2" ]; then echo "  ok   $1"; else
    echo "  FAIL $1: wanted nothing, got: $(printf '%s' "$2" | tr '\n' '|')"
    fail=1
  fi
}
nk() { # nk <name> <forbidden-substring> <actual>
  if printf '%s' "$3" | grep -q -- "$2"; then
    echo "  FAIL $1: did NOT want '$2', got: $(printf '%s' "$3" | tr '\n' '|' | cut -c1-200)"
    fail=1
  else
    echo "  ok   $1"
  fi
}
U="http://127.0.0.1:$PORT"
cli() { ./synccli 127.0.0.1 $PORT "$@"; }

echo "== accounts exist only by invitation, so the first one is made by hand =="
ck "adduser creates the first account" "created user 1" \
   "$(./sync adduser jk@example.com correcthorse . 2>&1)"
# No minimum any more: a short password is the account holder's choice. An
# EMPTY one is not a short password, it is no password, and stays refused.
ck "a short password is accepted" "created user" \
   "$(./sync adduser tiny@example.com x . 2>&1)"
ck "an empty password is refused" "could not create" \
   "$(./sync adduser empty@example.com "" . 2>&1)"
ck "the same email twice is refused" "could not create" \
   "$(./sync adduser jk@example.com correcthorse . 2>&1)"

./sync $PORT . >/dev/null 2>&1 &
SRVPID=$!
sleep 1

echo "== the browser half =="
ck "the root page is the login form when signed out" "Sign in" "$(curl -s $U/)"
ck "...titled with the app's name" "<h1>Pancra</h1>" "$(curl -s $U/)"
ck "and it says how accounts are made" "invitation link" "$(curl -s $U/)"
ck "a wrong password is refused" "Wrong email or password" \
   "$(curl -s -X POST -d 'email=jk@example.com&password=nope' $U/login)"
curl -s -c jar.txt -o /dev/null -X POST \
     -d 'email=jk@example.com&password=correcthorse' $U/login
ck "a good password sets a session cookie" "sid" "$(cat jar.txt)"
ck "the cookie is HttpOnly" "#HttpOnly_" "$(cat jar.txt)"

# A header lookup must NOT be able to read the request BODY. The header block
# used to be terminated only at the end of the whole buffer, so hdr_get()
# walked past the blank line and went on searching whatever had been POSTed.
# Session cookies are SameSite=Lax, so a cross-site POST carries no real
# Cookie header at all -- which made a body line the first and only match, and
# let an attacker pick the victim's session. This sends a genuine session id
# in the body with no cookie header, and must come back signed OUT.
SID=$(awk '/sid/ {print $NF}' jar.txt | tail -1)
ck "a session cookie smuggled in the request body does not sign you in" \
   "Sign in" "$(curl -s -X GET --data-binary "Cookie: sid=$SID" $U/)"

HOME_PAGE=$(curl -s -b jar.txt $U/)
ck "signed in, the root page is the data page" "No readings yet" "$HOME_PAGE"
nk "the app name is not repeated in the body" "<b>Pancra</b>" "$HOME_PAGE"
ck "...and the tab, before any data" "<title>Pancra" "$HOME_PAGE"
nk "an empty record does not print a bare 'stored:'" "stored:" "$HOME_PAGE"
ck "the page offers settings under the user's own name" "jk@example.com" \
   "$HOME_PAGE"

echo "== settings, and the pairing code =="
SET=$(curl -s -b jar.txt $U/settings)
CSRF=$(printf '%s' "$SET" | grep -o 'name=csrf value="[0-9a-f]*"' | head -1 |
       sed 's/.*value="//;s/"//')
ck "settings carries a CSRF token" "^[0-9a-f]\{32\}$" "$CSRF"
ck "a state-changing form without the token is refused" "expired" \
   "$(curl -s -b jar.txt -X POST -d 'csrf=bogus' $U/settings/pair)"
SET=$(curl -s -b jar.txt -X POST -d "csrf=$CSRF" $U/settings/pair)
CODE=$(printf '%s' "$SET" | grep -o 'class=code>[0-9]\{6\}' | sed 's/.*>//')
ck "a 6-digit pairing code is shown" "^[0-9]\{6\}$" "$CODE"

echo "== pairing (EC-J-PAKE, the same rounds a Dexcom sensor uses) =="
WRONG=$(printf '%06d' $(( (10#${CODE:-0} + 1) % 1000000 )))
# Both sides check. The app notices first, because the server's proof arrives
# with round 3, and it refuses to save a key from a server it cannot verify.
ck "a WRONG code is caught by the app, before it saves anything" \
   "server confirmation wrong" "$(cli pair jk@example.com "$WRONG" 2>&1)"
# ...and the server refuses the app's proof too, which is the path that
# actually costs an attacker a try.
ck "a WRONG confirmation is refused by the server" "wrong pairing code" \
   "$(cli pairbad jk@example.com "$WRONG" 2>&1)"
PAIR=$(cli pair jk@example.com "$CODE" 2>&1)
ck "the right code pairs and yields a uid and a 128-bit key" \
   "^1 [0-9a-f]\{32\}$" "$PAIR"
APPUID=$(echo "$PAIR" | cut -d' ' -f1)
KEY=$(echo "$PAIR" | cut -d' ' -f2)
ck "the code is spent: it cannot be used a second time" "no pairing code" \
   "$(cli pair jk@example.com "$CODE" 2>&1)"

echo "== three wrong attempts burn the code (what makes 6 digits enough) =="
SET=$(curl -s -b jar.txt $U/settings)
CSRF=$(printf '%s' "$SET" | grep -o 'name=csrf value="[0-9a-f]*"' | head -1 |
       sed 's/.*value="//;s/"//')
curl -s -b jar.txt -o /dev/null -X POST -d "csrf=$CSRF" $U/settings/unpair
SET=$(curl -s -b jar.txt -X POST -d "csrf=$CSRF" $U/settings/pair)
CODE2=$(printf '%s' "$SET" | grep -o 'class=code>[0-9]\{6\}' | sed 's/.*>//')
W2=$(printf '%06d' $(( (10#${CODE2:-0} + 7) % 1000000 )))
for i in 1 2 3; do cli pairbad jk@example.com "$W2" >/dev/null 2>&1; done
ck "after three wrong attempts the RIGHT code no longer works" \
   "no pairing code" "$(cli pair jk@example.com "$CODE2" 2>&1)"
# Re-pair for the rest of the run.
SET=$(curl -s -b jar.txt -X POST -d "csrf=$CSRF" $U/settings/pair)
CODE3=$(printf '%s' "$SET" | grep -o 'class=code>[0-9]\{6\}' | sed 's/.*>//')
PAIR=$(cli pair jk@example.com "$CODE3" 2>&1)
APPUID=$(echo "$PAIR" | cut -d' ' -f1)
KEY=$(echo "$PAIR" | cut -d' ' -f2)
ck "a fresh code pairs again" "^1 [0-9a-f]\{32\}$" "$PAIR"

echo "== the signed API refuses everything unsigned =="
ck "no Authorization header is 401" "bad or missing signature" \
   "$(curl -s $U/v1/digest)"
ck "a forged MAC is 401" "bad or missing signature" \
   "$(curl -s -H "Authorization: Pancra 1:$(date +%s):abcdefgh:$(printf '0%.0s' {1..64})" $U/v1/digest)"
nk "a signed request is accepted" "signature" \
   "$(cli req "$APPUID" "$KEY" GET /v1/digest 2>&1)"

echo "== the sync protocol: digest, bucket, replace =="
empty "an empty account has an empty digest" \
   "$(cli req "$APPUID" "$KEY" GET /v1/digest)"
DAY=20000
printf '1728000000,120,0,-70,3,7,1728000000,-420,0\n1728000300,124,2,-70,3,7,1728000300,-420,0\n' > b.txt
ck "a bucket PUT stores its rows" "stored 2 rows" \
   "$(cli req "$APPUID" "$KEY" PUT /v1/bucket/readings/$DAY b.txt)"
ck "the log now appears in the digest with its row count" "^readings 2 " \
   "$(cli req "$APPUID" "$KEY" GET /v1/digest)"
ck "the bucket digest reports the same count" "^$DAY 2 " \
   "$(cli req "$APPUID" "$KEY" GET /v1/digest/readings)"

# The hash the protocol specifies: rows sorted BYTEWISE, each newline
# terminated, SHA-256, first 16 hex. Recomputed here from the bytes on disk,
# so a change to how the server sorts or terminates rows fails this.
WANT=$(LC_ALL=C sort b.txt | sha256sum | cut -c1-16)
GOT=$(cli req "$APPUID" "$KEY" GET /v1/digest/readings | awk '{print $3}')
ck "the bucket hash is SHA-256 over the canonical text" "$WANT" "$GOT"

ck "GET returns the canonical text back" "1728000000,120" \
   "$(cli req "$APPUID" "$KEY" GET /v1/bucket/readings/$DAY)"
ck "re-PUTting the same bucket is idempotent" "stored 2 rows" \
   "$(cli req "$APPUID" "$KEY" PUT /v1/bucket/readings/$DAY b.txt)"
ck "...and the digest is unchanged" "$WANT" \
   "$(cli req "$APPUID" "$KEY" GET /v1/digest/readings | awk '{print $3}')"

# The property the whole design rests on: a PUT REPLACES, so a row the app
# deleted or corrected disappears here too.
head -1 b.txt > b2.txt
ck "a bucket PUT with one row fewer deletes the missing row" "stored 1 rows" \
   "$(cli req "$APPUID" "$KEY" PUT /v1/bucket/readings/$DAY b2.txt)"
nk "...the deleted row is really gone" "1728000300" \
   "$(cli req "$APPUID" "$KEY" GET /v1/bucket/readings/$DAY)"
ck "...and the digest followed it" "^readings 1 " \
   "$(cli req "$APPUID" "$KEY" GET /v1/digest)"
: > empty.txt
ck "an empty PUT deletes the bucket" "stored 0 rows" \
   "$(cli req "$APPUID" "$KEY" PUT /v1/bucket/readings/$DAY empty.txt)"
empty "...and an empty log vanishes from the digest" \
   "$(cli req "$APPUID" "$KEY" GET /v1/digest)"

echo "== rows are bytes: what the server must refuse =="
printf 'fine\n' > ok.txt
ck "an arbitrary log name is allowed (the app owns its own formats)" \
   "stored 1 rows" "$(cli req "$APPUID" "$KEY" PUT /v1/bucket/settings/0 ok.txt)"
printf 'trailing space \n' > bad.txt
ck "a row with a trailing space is refused" "malformed row" \
   "$(cli req "$APPUID" "$KEY" PUT /v1/bucket/readings/1 bad.txt)"
python3 -c "open('long.txt','w').write('x'*513+'\n')"
ck "a row over 512 bytes is refused" "malformed row" \
   "$(cli req "$APPUID" "$KEY" PUT /v1/bucket/readings/1 long.txt)"
ck "a bad log name is refused" "bad log" \
   "$(cli req "$APPUID" "$KEY" GET /v1/bucket/READINGS/1)"

# A TRUNCATED BODY MUST NOT READ AS A COMPLETE ONE. Whole-bucket replacement
# is DELETE-then-insert, so a PUT whose body is cut short used to commit as an
# authoritative deletion of every row that had not arrived -- turning a slow
# connection into data loss. Declares more than it sends, then stops.
BUCKET_BEFORE=$(cli req "$APPUID" "$KEY" GET /v1/bucket/settings/0)
SHORT=$(python3 - "$PORT" <<'PY'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), 5)
body = b"truncated\n"
s.sendall(b"PUT /v1/bucket/settings/0 HTTP/1.1\r\nHost: x\r\n"
          b"Content-Length: 5000\r\n\r\n" + body)
s.settimeout(12)
try:
    print(s.recv(4096).split(b"\r\n")[0].decode(), end="")
except OSError:
    print("no reply", end="")
PY
)
ck "a body shorter than Content-Length is refused" "400" "$SHORT"
ck "...and the bucket it targeted is untouched" "$BUCKET_BEFORE" \
   "$(cli req "$APPUID" "$KEY" GET /v1/bucket/settings/0)"
ck "nothing was stored by any of those" "^settings 1 " \
   "$(cli req "$APPUID" "$KEY" GET /v1/digest)"

echo "== data reaches the page it is stored for =="
printf '1728000000,142,0,-70,3,7,1728000000,-420,1\n' > r.txt
cli req "$APPUID" "$KEY" PUT /v1/bucket/readings/$DAY r.txt >/dev/null
PAGE=$(curl -s -b jar.txt $U/)
# Old data: the page shows WHEN, and blanks the big number rather than
# presenting a stale reading as current -- what the single-user page did.
# tz_off in the fixture is -420 SECONDS, so local time is 7 minutes behind UTC
# and the stamp lands on the 3rd. The point is that a year-old reading still
# shows WHEN, which is what the single-user page did.
ck "the newest reading's time is shown however old it is" "2024-10-03 23:53" \
   "$PAGE"
ck "...and a stale big number blanks to ---" ">---<" "$PAGE"
ck "the old page's one-line stylesheet, unchanged" \
   "font-family:monospace,monospace" "$PAGE"
ck "...and its self-refresh" 'http-equiv="refresh"' "$PAGE"
ck "the page lists what else is stored" "settings" "$PAGE"
ck "the tab is the old HH:MM value form" "<title>[0-9][0-9]:[0-9][0-9] " "$PAGE"

echo "== the dose log, replayed from append-only assertions =="
# Three rows, one dose: logged at 12, corrected to 10, and a second dose that
# is then retracted. The page must show ONE dose of 10 units.
printf '%s\n%s\n%s\n%s\n' \
  '1728000100,1,0,1728000090,0,12,-420' \
  '1728000700,1,0,1728000090,0,10,-420' \
  '1728000200,2,0,1728000190,1,4,-420' \
  '1728000900,2,1,1728000190,1,4,-420' > ins.txt
ck "the insulin log syncs" "stored 4 rows" \
   "$(cli req "$APPUID" "$KEY" PUT /v1/bucket/insulin/20000 ins.txt)"
UNITS=$(curl -s -b jar.txt $U/units)
ck "the corrected dose shows its NEW amount" "  10  slow" "$UNITS"
nk "...not the amount it was first entered as" "  12  slow" "$UNITS"
nk "a retracted dose is not shown at all" "fast" "$UNITS"
ck "the units page has a way back" "&lt;- Main" "$UNITS"

# A MALFORMED ASSERTION MUST NOT RENDER AS A DOSE.
#
# The server's replay is a clone of the app's, and it had dropped the app's
# range checks -- so a row the phone refuses to display was shown here as
# insulin the person had taken. row_ok admits these: they are well-formed
# TEXT, and only the replay knows what a dose may be.
printf '%s\n%s\n%s\n' \
  '# written,id,del,unix_time,type,units,tz_offset_s' \
  '1728001000,9,0,1728000500,0,999,-420' \
  '1728001100,10,0,1728000600,7,5,-420' > bad.txt
ck "a bucket of malformed doses is accepted as TEXT" "stored 3 rows" \
   "$(cli req "$APPUID" "$KEY" PUT /v1/bucket/insulin/20001 bad.txt)"
BADU=$(curl -s -b jar.txt $U/units)
nk "...but a 999-unit dose is not rendered" "999" "$BADU"
nk "...and neither is one of an unknown type" "  5  " "$BADU"
ck "the main page links to it" 'href="/units"' "$(curl -s -b jar.txt $U/)"
ck "...and to the plots" 'href="/plots"' "$(curl -s -b jar.txt $U/)"

echo "== the plots, drawn by the app's own renderer =="
PL=$(curl -s -b jar.txt $U/plots)
ck "the plots page offers the last 24 hours" "LAST 24 HOURS" "$PL"
ck "...and a month with data" "October 2024" "$PL"
ck "...and a way back" "&lt;- Main" "$PL"
# A GIF, not an error page: the bytes must start with the GIF89a magic.
G=$(curl -s -b jar.txt -o $DIR/p24.gif -w "%{http_code} %{content_type}" \
    $U/plot-24h.gif)
# No charset on a binary type: it is a claim about an encoding a GIF has not
# got.
ck "the 24h plot is served as a GIF, with no charset" "^200 image/gif$" "$G"
ck "...and is really a GIF" "GIF89a" "$(head -c 6 $DIR/p24.gif)"
# ...and it decodes to an actual plot. The magic bytes above are not a check on
# srv/gif.c: corrupting one line of the LZW encoder made every served plot
# garbage and left this whole suite green, because garbage still starts GIF89a.
if command -v python3 >/dev/null 2>&1; then
   if python3 "$HERE/srv/test/gifcheck.py" $DIR/p24.gif; then :; else
      printf '   FAIL the served GIF does not decode to a plot\n'; fail=1
   fi
elif [ "${ALLOW_SKIP:-0}" = "1" ]; then
   printf '   skip python3 absent: GIF not decoded (ALLOW_SKIP=1)\n'
else
   printf '   FAIL python3 absent: the GIF could not be decoded\n'; fail=1
fi
GD=$(curl -s -b jar.txt -o $DIR/pd.gif -w "%{http_code}" $U/plot-20241003.gif)
ck "a named day renders too" "200" "$GD"
ck "...as a GIF" "GIF89a" "$(head -c 6 $DIR/pd.gif)"
ck "the month page lists its days" "2024-10-04" \
   "$(curl -s -b jar.txt $U/plots-202410)"
ck "a day's datapoints are listed" "2024-10-04" \
   "$(curl -s -b jar.txt $U/day-20241004)"
# Signed out, a plot is not an image: it is the login page, like every other
# route. An unauthenticated GIF would be a record served to anyone with a URL.
nk "a signed-out request gets no plot" "GIF89a" \
   "$(curl -s $U/plot-24h.gif | head -c 6)"
ck "...it gets the login page instead" "Sign in" "$(curl -s $U/plot-24h.gif)"
ck "settings has a way back too" "&lt;- Main" "$(curl -s -b jar.txt $U/settings)"

echo "== sharing: the link IS the invitation =="
SET=$(curl -s -b jar.txt $U/settings)
CSRF=$(printf '%s' "$SET" | grep -o 'name=csrf value="[0-9a-f]*"' | head -1 |
       sed 's/.*value="//;s/"//')
SET=$(curl -s -b jar.txt -X POST -d "csrf=$CSRF&email=mum@example.com" \
      $U/settings/share)
TOKEN=$(printf '%s' "$SET" | grep -o '/invite/[0-9a-f]\{32\}' | head -1 |
        sed 's|/invite/||')
ck "a share link is minted" "^[0-9a-f]\{32\}$" "$TOKEN"
SET=$(curl -s -b jar.txt $U/settings)
ck "...and the settings page shows it as a full URL" \
   "https://127.0.0.1:$PORT/invite/$TOKEN" "$SET"
ck "...with a way to take it back" "Revoke" "$SET"
ck "the invitation page names who is sharing" "jk@example.com" \
   "$(curl -s $U/invite/$TOKEN)"
# Minted FOR an address: shown, not asked for, and not editable.
ck "an addressed invite prefills the email" 'value="mum@example.com"' \
   "$(curl -s $U/invite/$TOKEN)"
# Editable, because the owner typed it from memory and may have got it wrong.
ck "...but leaves it editable" "name=email type=email value" \
   "$(curl -s $U/invite/$TOKEN)"
ck "the login page says where accounts come from" "invitation link" \
   "$(curl -s $U/login)"
BADL=$(./sync invite . 2>/dev/null); BADT=${BADL##*/}
ck "the wrong password on an existing account is refused" "not its password" \
   "$(curl -s -X POST -d 'action=go&email=jk@example.com&password=wrongwrong' $U/invite/$BADT)"
ck "it offers one way in, whether or not they have an account" "Continue" \
   "$(curl -s $U/invite/$TOKEN)"
curl -s -c mum.txt -o /dev/null -X POST \
     -d "action=go&email=mum@example.com&password=hunter2hunter2" \
     $U/invite/$TOKEN
ck "following the link creates the account and signs it in" "sid" \
   "$(cat mum.txt)"
ck "the follower can open the owner's record" "2024-10-03 23:53" \
   "$(curl -s -b mum.txt "$U/?who=1")"
# Their page IS the shared record now, and the foot names its owner, so a
# "Shared with you: <same address>" line above would say it twice.
nk "the lone follower's page does not name the record twice" \
   "Shared with you" "$(curl -s -b mum.txt $U/)"
# A follower who will never pair an app still gets a sensible zone: the one
# belonging to whoever shares with them, rather than UTC.
ck "a follower inherits the owner's zone rather than UTC" \
   "Follow the data (currently" "$(curl -s -b mum.txt $U/settings)"
# With no app of their own and one person followed, the followed record IS
# their page: they should not have to know to type "?who=N".
MUMHOME=$(curl -s -b mum.txt $U/)
ck "a follower with no app of their own lands on the shared record" \
   "2024-10-03" "$MUMHOME"
ck "...showing the owner's data, not an empty page" "2024-10-03 23:53" \
   "$MUMHOME"
# No "back to mine" when there is no mine: the link would lead to an empty
# page wearing their own name.
nk "...and no back link, having no record of their own" "back to mine" \
   "$MUMHOME"
# Whose record it is belongs at the FOOT for a follower: the reading is what
# they came for, the attribution is a caption.
ck "...but they are told whose record it is" "Viewing <b>jk@example.com" \
   "$MUMHOME"

# THE SUBPAGES MUST FOLLOW THE RECORD, not fall back to the viewer's own.
# Units and Plots resolved the record separately from the main page, and the
# links to them carried no "who=", so a follower clicking either landed on
# their OWN empty record and saw nothing -- on both pages, every time.
ck "the follower's Units link keeps the record" "/units?who=" "$MUMHOME"
ck "...and so does the Plots link" "/plots?who=" "$MUMHOME"
MUMUNITS=$(curl -s -b mum.txt $U/units)
ck "a lone follower opening Units sees the owner's data" "2024-10-03" \
   "$MUMUNITS"
MUMPLOTS=$(curl -s -b mum.txt $U/plots)
ck "...and opening Plots sees the owner's months" "October 2024" "$MUMPLOTS"
# Every link OUT of the plots page has to carry it too, or the record is lost
# one click deeper instead of at the first.
ck "...whose day links keep the record" "who=" "$MUMPLOTS"
ck "...and whose images do too" ".gif?who=" "$MUMPLOTS"
ck "...and whose back link returns to the record" "&lt;- Main</a>" "$MUMPLOTS"
# An explicit who= must keep working for a follower who follows SEVERAL
# people, where there is no single record to default to. The id is read off
# the page rather than assumed, so adding an account cannot silently make
# this test pass against the wrong record.
OWNER_ID=$(printf '%s' "$MUMHOME" | grep -o '/units?who=[0-9]*' | head -1 |
           sed 's/.*=//')
ck "an explicit who= reaches Units as well" "2024-10-03" \
   "$(curl -s -b mum.txt "$U/units?who=$OWNER_ID")"
ck "...at the foot, below the plots link" \
   "Plots ...</a>.*Viewing <b>jk@example.com" \
   "$(printf '%s' "$MUMHOME" | tr '\n' ' ')"
# Reached explicitly, the link returns: they asked to look elsewhere.
ck "an explicit ?who= still offers the way back" "back to mine" \
   "$(curl -s -b mum.txt "$U/?who=1")"
ck "a spent invitation cannot be replayed" "not valid any more" \
   "$(curl -s $U/invite/$TOKEN)"

echo "== a signup link from the COMMAND LINE, with no owner attached =="
# How accounts are meant to be handed out: no owner, so it creates an account
# and shares nobody's data with anybody.
LINK=$(./sync invite . 2>/dev/null)
# The FULL url and nothing else, so it can be pasted or piped as-is.
ck "sync invite prints the full URL, alone" \
   "^https://pancra.org/invite/[0-9a-f]\{32\}$" "$LINK"
PLAIN=${LINK##*/}
ck "the invitation page is titled as one" "Pancra Invite" \
   "$(curl -s $U/invite/$PLAIN)"

# ONE form on that page, and unambiguously a registration: this is what makes
# a browser offer to generate a password, and two forms sharing a page is what
# stopped it.

# A link minted FOR an address does not ask for it again, and does not let it
# be changed: an editable field there could be aimed at somebody else.
ADDR=$(./sync invite . 2>/dev/null)   # (no owner; addressed one made below)
ck "the wrong password on an existing account is refused" \
   "not its password" \
   "$(curl -s -X POST -d 'action=go&email=jk@example.com&password=wrongwrong' $U/invite/$PLAIN)"
ck "...with a username field to file the credential under" \
   "autocomplete=username" "$(curl -s $U/invite/$PLAIN/new)"

curl -s -c dad.txt -o /dev/null -X POST \
     -d "action=go&email=dad@example.com&password=hunter2hunter2" \
     $U/invite/$PLAIN
ck "the link creates the account and signs it in" "sid" "$(cat dad.txt)"
ck "...on their own empty page" "No readings yet" "$(curl -s -b dad.txt $U/)"
nk "...with nobody else's data offered" "Shared with you" \
   "$(curl -s -b dad.txt $U/)"
ck "...and they cannot open the owner's record" "not shared" \
   "$(curl -s -b dad.txt "$U/?who=1")"
ck "a spent signup link cannot be replayed" "not valid any more" \
   "$(curl -s $U/invite/$PLAIN)"
ck "an invite for an unknown owner is refused" "no such user" \
   "$(./sync invite nobody@example.com . 2>&1)"
# With an owner it behaves like the web button: account AND follow.
LINK2=$(./sync invite jk@example.com . 2>/dev/null)
T2=${LINK2##*/}
ck "an owner's invite names them on the page" "jk@example.com" \
   "$(curl -s $U/invite/$T2)"

# Listing and revoking, which is how a link handed to the wrong person is
# taken back.
ck "invites lists the live link" "$T2" "$(./sync invites . 2>&1)"
ck "revoke accepts the whole URL" "revoked" \
   "$(./sync revoke "$LINK2" . 2>&1)"
nk "...and it is gone from the list" "$T2" "$(./sync invites . 2>&1)"
ck "a revoked link is dead" "not valid" "$(curl -s $U/invite/$T2)"

echo "== a follower is read-only, and only for what was shared =="
ck "the follower cannot open a record nobody shared" "not shared" \
   "$(curl -s -b mum.txt "$U/?who=99")"
ck "the follower has no app of their own paired" "Show pairing code" \
   "$(curl -s -b mum.txt $U/settings)"

echo "== revoking ends it =="
SET=$(curl -s -b jar.txt $U/settings)
CSRF=$(printf '%s' "$SET" | grep -o 'name=csrf value="[0-9a-f]*"' | head -1 |
       sed 's/.*value="//;s/"//')
ck "the follower is listed" "mum@example.com" "$SET"
# The follower's id read off the page, not assumed to be 2: adding an account
# anywhere earlier in this file used to shift it and silently revoke nobody.
WHO=$(printf '%s' "$SET" | grep -oE 'name=who value="[0-9]+"' | head -1 |
      grep -oE '[0-9]+')
curl -s -b jar.txt -o /dev/null -X POST -d "csrf=$CSRF&who=$WHO" $U/settings/revoke
ck "after revoking, the record is closed again" "not shared" \
   "$(curl -s -b mum.txt "$U/?who=1")"

echo "== passwords are salted per user =="
# Same password, two accounts: if the stored hashes matched, one cracked
# password would be every account with that password.
./sync adduser same1@example.com identicalpassword . >/dev/null 2>&1
./sync adduser same2@example.com identicalpassword . >/dev/null 2>&1
SALTS=$(python3 - <<'PY'
import sqlite3
c=sqlite3.connect("sync.db")
r=list(c.execute("SELECT hex(pw_salt),hex(pw_hash) FROM user WHERE email LIKE 'same%'"))
print("distinct" if len(r)==2 and r[0][0]!=r[1][0] and r[0][1]!=r[1][1] else "SHARED")
print(len(r[0][0])//2 if r else 0)
PY
)
ck "two accounts with the same password get different salt and hash" \
   "distinct" "$SALTS"
ck "...and the salt is 16 bytes" "^16$" "$(printf '%s' "$SALTS" | tail -1)"

echo "== not one byte of javascript, anywhere =="
# The pages are forms and links. Nothing here needs a script, and a page that
# grew one would also have grown a way for a bug in it to leak the record.
for page in / /login /settings "/invite/$PLAIN" "/invite/$PLAIN/new"; do
  nk "no script on $page" "<script" "$(curl -s -b jar.txt $U$page)"
  nk "no inline handler on $page" "on\(click\|load\|submit\|change\)=" \
     "$(curl -s -b jar.txt $U$page)"
done

echo "== the time zone is a list, and auto looks like auto =="
SET=$(curl -s -b jar.txt $U/settings)
ck "the zone is chosen from a list" "<select name=tz>" "$SET"
ck "...whose default is following the data" "Follow the data" "$SET"
# With nothing synced there is no offset to follow, and saying "+0:00" would
# report a fallback as though it were a measurement.
ck "...and says so honestly before anything has synced" "nothing to follow" \
   "$(curl -s -b dad.txt $U/settings)"

ck "...and which offers a real offset" "UTC-08:00" "$SET"
CSRF=$(printf '%s' "$SET" | grep -o 'name=csrf value="[0-9a-f]*"' | head -1 |
       sed 's/.*value="//;s/"//')
SET=$(curl -s -b jar.txt -X POST -d "csrf=$CSRF&tz=-480" $U/settings/tz)
ck "a chosen zone is kept selected" 'value="-480" selected' "$SET"
SET=$(curl -s -b jar.txt -X POST -d "csrf=$CSRF&tz=" $U/settings/tz)
ck "...and clearing it goes back to following the phone" \
   'value="" selected' "$SET"

echo "== deleting an account takes the whole record with it =="
# A second account to delete, so the rest of the run keeps its own.
DLINK=$(./sync invite . 2>/dev/null); DTOK=${DLINK##*/}
curl -s -c del.txt -o /dev/null -X POST \
     -d "action=go&email=gone@example.com&password=hunter2hunter2" \
     $U/invite/$DTOK
DSET=$(curl -s -b del.txt $U/settings)
DCSRF=$(printf '%s' "$DSET" | grep -o 'name=csrf value="[0-9a-f]*"' | head -1 |
        sed 's/.*value="//;s/"//')
ck "the settings page offers deletion" "Delete my account" "$DSET"
ck "the wrong email deletes nothing" "not your email" \
   "$(curl -s -b del.txt -X POST -d "csrf=$DCSRF&confirm=someone@else.com" \
      $U/settings/delete)"
ck "...and the account still works" "No readings yet" "$(curl -s -b del.txt $U/)"
curl -s -b del.txt -o /dev/null -X POST \
     -d "csrf=$DCSRF&confirm=gone@example.com" $U/settings/delete
ck "the right email deletes it" "Wrong email or password" \
   "$(curl -s -X POST -d 'email=gone@example.com&password=hunter2hunter2' $U/login)"
ck "...and the session went with it" "Sign in" "$(curl -s -b del.txt $U/)"

echo "== a long passphrase is not silently truncated =="
LONG=$(python3 -c "print('correct horse battery staple ' * 12, end='')")
./sync adduser long@example.com "$LONG" . >/dev/null 2>&1
ck "a 300-character passphrase creates an account" "sid" \
   "$(curl -s -c long.txt -o /dev/null -X POST \
      --data-urlencode "email=long@example.com" \
      --data-urlencode "password=$LONG" $U/login; cat long.txt)"
ck "...and a prefix of it does NOT sign in" "Wrong email or password" \
   "$(curl -s -X POST --data-urlencode "email=long@example.com" \
      --data-urlencode "password=correct horse battery staple " $U/login)"

echo "== passwords =="
SET=$(curl -s -b jar.txt $U/settings)
CSRF=$(printf '%s' "$SET" | grep -o 'name=csrf value="[0-9a-f]*"' | head -1 |
       sed 's/.*value="//;s/"//')
ck "the wrong current password changes nothing" "not the current password" \
   "$(curl -s -b jar.txt -X POST -d "csrf=$CSRF&old=wrong&new=newpassword1" \
      $U/settings/password)"
ck "a change is accepted with the right one" "Password changed" \
   "$(curl -s -b jar.txt -X POST \
      -d "csrf=$CSRF&old=correcthorse&new=newpassword1" $U/settings/password)"
ck "the old password no longer works" "Wrong email or password" \
   "$(curl -s -X POST -d 'email=jk@example.com&password=correcthorse' $U/login)"

# CONCURRENCY. The server used to serve one connection at a time, so a peer
# that connected and then said nothing held the WHOLE service for its
# handshake budget -- measured at 8 s, and two of them at 13. That is the
# thing this asserts against: with silent peers attached, ordinary requests
# must still be answered promptly, and simultaneous requests must overlap
# rather than queue.
echo "== many at once, and a silent peer holds nobody up =="
python3 - "$PORT" <<'PY' &
import socket, time, sys
# three peers that connect and never speak
socks = [socket.create_connection(("127.0.0.1", int(sys.argv[1]))) for _ in range(3)]
time.sleep(12)
for s in socks:
    s.close()
PY
SILENT=$!
sleep 1
# SUB-SECOND, because that is the whole difference. A request here costs about
# a millisecond, so serial service only shows up as the time spent waiting on
# the silent peers -- one second each, the grace before a silent connection is
# given away. Served concurrently it is immediate; served serially it is not.
T=$(curl -s -o /dev/null -m 20 -w '%{time_total}' "$U/login")
ck "a page is served immediately while three peers sit silent" "yes" \
   "$(awk -v t="$T" 'BEGIN{print (t < 0.5) ? "yes" : "no, took " t "s"}')"
# And ten at once, with those peers still attached, must all land inside the
# time a single silent connection would have held the server.
T0=$(date +%s)
PIDS=""
for i in $(seq 1 10); do
  curl -s -o /dev/null -m 20 "$U/login" &
  PIDS="$PIDS $!"
done
# ONLY the curls: a bare `wait` also waits on the silent-peer helper above,
# which sleeps for twelve seconds and would time this test's own fixture.
for p in $PIDS; do wait "$p"; done
T1=$(date +%s)
# (This one is a bound, not a proof of overlap: a request here costs a
# millisecond, so ten of them are quick even served one after another. The
# assertion above is the one that fails without a pool.)
ck "ten simultaneous requests all complete promptly" "yes" \
   "$([ $((T1 - T0)) -le 2 ] && echo yes || echo "no, took $((T1 - T0))s")"
kill $SILENT 2>/dev/null
wait $SILENT 2>/dev/null

echo "== security controls that nothing used to pin =="
# Three controls that an adversarial review mutated freely with the whole suite
# green: the replay window, the session expiry and the login throttle. Each one
# is a single line in srv/auth.c, and each is the only thing standing between a
# stranger and someone else\'s medical record.

# 1. A REPLAYED signed request must be refused. The nonce is spent on first use;
#    sending the identical request again must not work. (Deleting the
#    nonce_fresh() call made every signed request infinitely replayable.)
#    EXPORTED, not `VAR=x cli ...`: `cli` is a shell function, and a prefix
#    assignment on a function call sets the variable in this shell without
#    putting it in the environment of the process the function starts -- so
#    synccli generated a fresh nonce both times and the "replay" was two
#    different requests, which of course both succeeded.
export SYNCCLI_NONCE=aabbccddeeff0011
R1=$(cli req "$APPUID" "$KEY" GET /v1/digest 2>&1)
R2=$(cli req "$APPUID" "$KEY" GET /v1/digest 2>&1)
unset SYNCCLI_NONCE
ck "the first signed request with a fresh nonce works" "readings " "$R1"
# The refusal must be NAMED, not merely "some response": a check that the reply
# is non-empty passes whether or not the nonce was spent, which is exactly the
# kind of assertion that let this control be deleted with the suite green.
ck "a replayed signed request is refused" "bad or missing signature" "$R2"
nk "...and does not return a digest" "readings " "$R2"

# 3. The LOGIN THROTTLE. PBKDF2 on one core is exactly what an attacker would
#    like to make this board do, so after LOGIN_FAIL_MAX (5) failures in the
#    window the answer must be a refusal rather than another hash.
THR=""
i=0
while [ $i -lt 7 ]; do
   THR=$(curl -s -X POST -d 'email=throttle@example.com&password=wrong' $U/login)
   i=$((i + 1))
done
ck "repeated failed logins are throttled" "Too many attempts" "$THR"

echo "== logging out =="
# Logging out is a state change, so it needs the token like every other POST.
# Without one the server must refuse -- that is the assertion below.
LOGOUT_CSRF=$(curl -s -b jar.txt $U/ | grep -o 'name=csrf value="[0-9a-f]*"' |
              head -1 | sed 's/.*value="//; s/"//')
ck "logout without a CSRF token is refused" "settings" \
   "$(curl -s -b jar.txt -o /dev/null -X POST $U/logout; curl -s -b jar.txt $U/)"
curl -s -b jar.txt -c jar.txt -o /dev/null -X POST -d "csrf=$LOGOUT_CSRF" $U/logout
ck "after logout the session is gone" "Sign in" "$(curl -s -b jar.txt $U/)"

# A session can also be invalidated SERVER-SIDE, and the cookie must stop
# working. `sync logout` exists partly for this: dropping a user's sessions
# used to be reachable only as a side effect of changing their password, and
# there was no way to test the session check without an external sqlite3.
#
# Last in the file deliberately -- it signs the account out, so anything added
# after it would need to log back in.
#
# NOT covered: the TIME-BASED branch (expires_at < now). Ageing a row needs
# either a year or a database editor, and the suite has neither, so that one
# line stays unpinned.
# newpassword1: the settings section above changed it.
curl -s -c jar.txt -o /dev/null -X POST \
     -d 'email=jk@example.com&password=newpassword1' $U/login
ck "signed in again for the server-side logout case" "jk@example.com" \
   "$(curl -s -b jar.txt $U/)"
./sync logout jk@example.com . >/dev/null 2>&1
ck "a server-side logout invalidates the cookie" "Sign in" \
   "$(curl -s -b jar.txt $U/)"

# KILL THE PID WE STARTED, not a pattern.
#
# This was `pkill -f "sync $PORT"`, and once the port became an argument the
# script's OWN command line ("synctest.sh <binary> 18093") matched it -- so the
# cleanup killed the suite mid-run and make reported the target "Terminated".
# A pattern that can match the process running it is not a cleanup.
kill "$SRVPID" 2>/dev/null
wait "$SRVPID" 2>/dev/null
if [ $fail -eq 0 ]; then
  echo "sync: all tests passed"
else
  echo "sync: FAILURES above"
fi
rm -rf "$DIR"
exit $fail
