#!/bin/sh
# rotate.sh --- install a new certificate and key, and put the old pair back
# if the service does not come up healthy on it.
#
# WHY THIS IS A SCRIPT AND NOT A LIST OF STEPS. Rotation used to be five
# paragraphs in srv/deploy/README.md that an operator pasted by hand, and the
# paste was wrong in three ways at once:
#
#   1. IT SPELLED THE PATHS OUT. `./sync 8443 . cert.pem key.pem`, with the
#      port and both pem names as literals -- in a repository whose deployment
#      guide opens by saying pancra.conf owns every path on the board and
#      "nothing else in the repository may name one". A board deployed on
#      another port came back up on 8443, listening where nothing forwards.
#   2. IT NEVER RECORDED THE NEW PID. The line ended at `&`, so sync.pid still
#      named the process that had just been killed. Every later health check
#      then asked after a dead pid -- and the next deploy's stop step killed
#      whatever the kernel had since given that number to.
#   3. IT SWAPPED FIRST AND CHECKED AFTERWARDS. "If the handshake fails, put
#      .prev back" is a correct instruction that arrives after the service is
#      already down, and it is the step most likely to be skipped by whoever
#      is now debugging a certificate at an unsociable hour.
#
# So: validate the pair BEFORE anything is moved, keep the old one, restart
# through the same operation every other procedure here uses, and restore
# automatically when the new pair does not serve.
#
# Usage:  ./srv/deploy/rotate.sh <cert.pem> <key.pem>
set -eu

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/deploy/pancra.conf"

SSH="$PANCRA_SSH"
. "$HERE/srv/deploy/health.sh"
. "$HERE/srv/deploy/start.sh"
. "$HERE/srv/deploy/lock.sh"
say() { printf 'rotate: %s\n' "$*"; }
fail() { printf 'rotate: FAILED: %s\n' "$*" >&2; exit 1; }
OP=$(op_id)

# BEFORE ANYTHING MOVES, for the reason deploy.sh states at length: the checks
# below all traverse the front door, so an undeclared one turns into "the new
# certificate did not serve" -- and this script answers that by swapping the
# old pair back and calling the board DOWN. A missing line in pancra.conf must
# not be reported as a bad certificate.
require_front || fail "nothing was changed; declare the front door and re-run"

NEWCERT=${1:-}
NEWKEY=${2:-}
[ -n "$NEWCERT" ] && [ -n "$NEWKEY" ] ||
   fail "usage: $0 <cert.pem> <key.pem>"
[ -f "$NEWCERT" ] || fail "no such certificate: $NEWCERT"
[ -f "$NEWKEY" ] || fail "no such key: $NEWKEY"

# ------------------------------------------------ is this pair even a pair?
#
# CHECKED HERE, WHERE A FAILURE COSTS NOTHING. A certificate with the wrong
# key produces a server that starts perfectly, logs that it is listening, and
# then fails every single handshake -- so the pid check passes, the readiness
# line appears, and only the public probe notices. Catching it before the swap
# turns an outage into an error message.
command -v openssl >/dev/null 2>&1 ||
   fail "openssl is needed to validate the pair before installing it"
cpub=$(openssl x509 -noout -pubkey -in "$NEWCERT" 2>/dev/null) ||
   fail "$NEWCERT is not a certificate openssl can read"
kpub=$(openssl pkey -pubout -in "$NEWKEY" 2>/dev/null) ||
   fail "$NEWKEY is not a private key openssl can read"
[ "$cpub" = "$kpub" ] ||
   fail "$NEWCERT and $NEWKEY are not a pair; every handshake would fail"

# Dates, for the same reason: an expired certificate is refused by the phone
# with the same symptom as a broken one, and a not-yet-valid one is the
# classic result of generating a replacement on a board with a wrong clock.
openssl x509 -noout -checkend 0 -in "$NEWCERT" >/dev/null 2>&1 ||
   fail "$NEWCERT has already expired"
openssl x509 -noout -dates -in "$NEWCERT" | sed 's/^/rotate:   /'
# Not fatal, only loud: rotating to something short-lived is sometimes exactly
# what is wanted, and the operator is standing right here.
openssl x509 -noout -checkend 604800 -in "$NEWCERT" >/dev/null 2>&1 ||
   say "WARNING: this certificate expires within seven days"

say "the pair matches; installing on $PANCRA_HOST"

# ---------------------------------------------------------- one at a time
#
# A ROTATION IS A DEPLOYMENT OF SOMETHING OTHER THAN THE BINARY, and it shares
# everything that matters: it stops the process every other procedure's pid
# file names, starts a new one, and waits on the same log for the same kind of
# readiness line. A deploy running beside it kills the server this one just
# started, so this one's health wait fails and it swaps a perfectly good
# certificate back out again -- and then reports the board DOWN about a board
# that is up on somebody else's build.
#
# Taken AFTER openssl has had its say: a mismatched pair is refused without
# ever touching the board, and a refusal that first stopped somebody else's
# deploy would be a worse answer to the same question.
lock_take rotate "$OP" || exit 1

# ---------------------------------------------------------------- the swap
#
# Copied beside, never over, exactly as the executable is: a transfer that
# dies halfway leaves the running pair untouched.
#
# STAGED UNDER THIS OPERATION'S ID: `cert.pem.new` and `key.pem.new` were fixed
# names, so two rotations could leave A's certificate beside B's key and the
# swap would install a pair that matches nothing -- the exact failure the
# openssl check above exists to make impossible, reintroduced by the filesystem.
#
# `.prev` is NOT given an id, and that is deliberate rather than an omission:
# it is a copy of whatever is installed at the moment of the swap, taken under
# the lock, so re-taking it is idempotent and there is only ever one rotation
# that could be rolled back. Giving it an id would also make it un-findable by
# an operator following the failure message, which names it.
CERT_NEW=$PANCRA_CERT.new-$OP
KEY_NEW=$PANCRA_KEY.new-$OP
$SSH "$PANCRA_HOST" "cat > '$CERT_NEW'" < "$NEWCERT" ||
   fail "could not copy the certificate to the board"
# UMASK, NOT A LATER chmod. The private key's permissions have to be right at
# the instant the file is CREATED: a `chmod 600` in the swap step below is a
# separate ssh round trip later, and for that whole window the key sits on the
# board with whatever the remote umask gives a new file -- 0644 on a stock
# system. A secret that was world-readable for a second was world-readable.
$SSH "$PANCRA_HOST" "umask 077; cat > '$KEY_NEW'" < "$NEWKEY" ||
   fail "could not copy the key to the board"

tag=$(start_tag)
mark=$(log_mark)

# ONE REMOTE SHELL, so the service is down for the swap and no other step can
# land between them. .prev is the whole recovery story and is written before
# anything is overwritten.
#
# `chmod 600` as well as the umask above: the umask is what makes the mode
# right from the first byte, this states the mode the server's key must have
# whatever the remote shell's umask turned out to be.
if ! $SSH "$PANCRA_HOST" "sh -s" <<EOF
set -eu
chmod 600 '$KEY_NEW'
cp '$PANCRA_CERT' '$PANCRA_CERT.prev'
cp '$PANCRA_KEY' '$PANCRA_KEY.prev'
$(stop_block)
mv '$CERT_NEW' '$PANCRA_CERT'
mv '$KEY_NEW' '$PANCRA_KEY'
$(start_block "$tag")
EOF
then
   # THE ONE OUTCOME WITH NO AUTOMATIC RECOVERY, so it has to be described
   # rather than summarised. Everything below this point in the script runs
   # after a start; this ran instead of one, and where it stopped decides
   # whether the board is serving:
   #
   #   before the stop  -- the old pair is still installed and the old process
   #                       is still running. Nothing happened; re-run.
   #   after the stop   -- THE SERVICE IS DOWN and was never started again,
   #                       and the pair on disk may be half swapped (a new
   #                       certificate against the old key serves nothing).
   #
   # Not restored automatically: a restore begins by stopping the service, and
   # in the first case that would take down a board that is fine on the
   # strength of a copy failing.
   echo "rotate: FAILED: the swap did not complete on $PANCRA_HOST." >&2
   echo "  The previous pair is at $PANCRA_CERT.prev and $PANCRA_KEY.prev if" >&2
   echo "  it was reached. THE SERVICE MAY BE STOPPED: if the step that failed" >&2
   echo "  came after the stop, nothing started it again. Check it, and put" >&2
   echo "  the pair you want back with '.prev' before re-running." >&2
   exit 1
fi

# ------------------------------------------------------------ did it serve?
#
# The same two questions every other procedure asks (health.sh): this start
# said it was listening, and the public URL answers. The second is the one
# that matters here -- a bad certificate is invisible to the first.
if wait_healthy_since "$mark" "$tag"; then
   say "the new certificate is live and $PANCRA_URL answers"
   $SSH "$PANCRA_HOST" \
      "openssl x509 -noout -enddate -in '$PANCRA_CERT' 2>/dev/null" || true
   exit 0
fi

# ------------------------------------------------------------- put it back
say "the service is NOT healthy on the new pair; restoring the previous one"
# THE MARK COMES FIRST, before the restart writes the line we are going to
# look for. Taken afterwards it would sit past the readiness line of the very
# start it is meant to witness, and the restore would report failure on a
# board that had just come back up perfectly.
backtag=$(start_tag)
back=$(log_mark)
$SSH "$PANCRA_HOST" "sh -s" <<EOF || fail "the restore itself failed; the board is DOWN"
set -eu
$(stop_block)
mv '$PANCRA_CERT.prev' '$PANCRA_CERT'
mv '$PANCRA_KEY.prev' '$PANCRA_KEY'
$(start_block "$backtag")
EOF

if wait_healthy_since "$back" "$backtag"; then
   fail "the new certificate did not serve; the previous pair is back and healthy"
fi
fail "the new certificate did not serve AND the previous pair did not come back; the board is DOWN"
