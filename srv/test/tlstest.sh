#!/bin/sh
# tlstest.sh --- drive srv/tls.c with a real TLS client.
#
# Until this existed, the 1000 lines of hand-written TLS 1.3 in srv/tls.c were
# reachable by NO test: srv/test/synctest.sh starts the server with two
# arguments, which selects plain HTTP, so every assertion in the suite went
# over a cleartext socket. An adversarial review proved the cost by deleting
# the aes128_gcm_open() return check -- forged records accepted as plaintext --
# and watching `make check` still print "all tests passed".
#
# So the point here is not to re-test the crypto primitives (cryptotest pins
# those to the published vectors); it is to make the SERVER answer a real
# client, and to make the failure paths fail. Everything below runs against
# the actual `sync` binary over a real socket.
set -u

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/test/testlib.sh"

BIN=${1:-build/srv/sync}
D=$(mktemp -d)
T_TMP=$D

# The verdict is testlib's `fail`, and it is the only one. This file kept its
# own FAIL alongside the helpers it shared with nothing; two variables that can
# disagree about whether a run passed is one variable too many.
ok()   { t_ok "$1"; }
bad()  { t_bad "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }

cleanup() {
   [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
   [ -n "${SRV:-}" ] && wait "$SRV" 2>/dev/null
   rm -rf "$D"
}
trap cleanup EXIT INT TERM

# A SKIP IS A FAILURE UNLESS SOMEBODY ASKED FOR ONE.
#
# These assertions are the only thing exercising a thousand lines of
# hand-written TLS 1.3, and they used to vanish silently when a tool was
# absent: no openssl and the whole file exited 0, no python3 and the forged-
# record case -- the one assertion this file exists for -- was skipped with
# FAIL still 0. A green `make check` then meant "the TLS layer was not
# tested", which is the same class of lie as the CRLF gate that scanned a
# directory that no longer existed.
#
# Set ALLOW_SKIP=1 to opt out deliberately (a machine without openssl that
# genuinely cannot run this); anything else is a failure with a name.
if ! have openssl; then
   if [ "${ALLOW_SKIP:-0}" = "1" ]; then
      printf 'tlstest: openssl not installed, SKIPPED (ALLOW_SKIP=1)\n'
      exit 0
   fi
   printf 'tlstest: openssl is REQUIRED to test the TLS server.\n'
   printf '  install it, or re-run with ALLOW_SKIP=1 to accept an untested\n'
   printf '  TLS layer in this build.\n'
   exit 1
fi
# python3 drives the forged-record case, the reset-connection case and the
# dribbling-client case -- three of the seven things this file tests, and the
# three that no other test in the tree can reach. They used to sit inside bare
# `if have python3` with no else, so on a machine without it the file printed
# four passes and exited 0 while the assertions it exists for never ran.
# `need` has already made the verdict red (or printed a deliberate skip), so
# the flag below only decides whether to attempt the cases, never whether the
# run passes.
HAVE_PY=1
need python3 "the forged-record, reset and dribble cases" || HAVE_PY=0

# The port is asked for rather than chosen: `make check -j4` runs tlstest and
# tlsasan at once, and every case below that starts a server asks for one of
# its own.
PORT=${2:-}

# A throwaway P-256 certificate. It must be EC: tls_init finds the private key
# by scanning the DER for a 32-byte OCTET STRING, so an RSA key does not load
# at all -- which is itself worth asserting, further down.
openssl ecparam -name prime256v1 -genkey -noout -out "$D/key.pem" 2>/dev/null
openssl req -new -x509 -key "$D/key.pem" -out "$D/cert.pem" -days 2 \
    -subj "/CN=localhost" 2>/dev/null

# Poll for a real handshake rather than sleeping a guess. `sleep 3` was both
# too long for a fast machine and, under the sanitizer build on a loaded one,
# occasionally too short -- which failed as "server did not start".
#
# ...and the pid that answers must be the pid we started. tlstest and tlsasan
# run at once under `make check`, so "something completed a TLS handshake on
# that port" was never the same claim as "the binary under test is serving it"
# -- and the whole file, forged records included, would have been aimed at the
# other run's server.
tls_up() { # tls_up <port>
   "$BIN" "$1" "$D" "$D/cert.pem" "$D/key.pem" > "$D/srv.log" 2>&1 &
   T_PID=$!
}
if [ -n "$PORT" ]; then
   tls_up "$PORT"
   SRV=$T_PID
   if ! wait_ready "$SRV" "https://127.0.0.1:$PORT/login" --insecure; then
      printf 'tlstest: server did not become ready on port %s\n' "$PORT"
      cat "$D/srv.log"; exit 1
   fi
elif serve "tlstest" https /login tls_up --insecure; then
   PORT=$T_PORT
   SRV=$T_PID
else
   printf 'tlstest: server did not become ready on any port\n'
   cat "$D/srv.log"; exit 1
fi
U="https://127.0.0.1:$PORT"
ck_owns "the TLS server under test owns the port the attacks are aimed at" \
   "$SRV" "$PORT"

# 1. A real handshake, and the parameters this server is documented to offer.
OUT=$(printf 'GET /login HTTP/1.1\r\nHost: localhost\r\n\r\n' \
      | openssl s_client -connect "127.0.0.1:$PORT" -servername localhost \
        -tls1_3 2>&1)
echo "$OUT" | grep -q "Protocol *: *TLSv1.3" \
   && ok "negotiates TLS 1.3" || bad "negotiates TLS 1.3"
echo "$OUT" | grep -q "TLS_AES_128_GCM_SHA256" \
   && ok "negotiates TLS_AES_128_GCM_SHA256" || bad "negotiates the cipher suite"

# 2. It actually serves over TLS, not merely handshakes.
CODE=$(curl -sk -o "$D/body" -m 15 -w '%{http_code}' "$U/login" 2>/dev/null)
[ "$CODE" = "200" ] && ok "serves a page over TLS (200)" || bad "serves over TLS (got $CODE)"
grep -qi "sign in" "$D/body" 2>/dev/null \
   && ok "the page is the real one" || bad "page content"

# 3. Session resumption. It is advertised (a ticket is sent), so it has to work
#    -- a ticket that never resumes is a slow handshake pretending to be fast.
#    The sleep is load-bearing: the ticket arrives AFTER the handshake, so a
#    client that hangs up the instant it has sent its request never sees one
#    and -sess_out writes a file with nothing to resume from.
{ printf 'GET /login HTTP/1.1\r\nHost: localhost\r\n\r\n'; sleep 2; } \
   | openssl s_client -connect "127.0.0.1:$PORT" -servername localhost \
     -sess_out "$D/sess.pem" >/dev/null 2>&1
RES=$({ printf 'GET /login HTTP/1.1\r\nHost: localhost\r\n\r\n'; sleep 2; } \
      | openssl s_client -connect "127.0.0.1:$PORT" -servername localhost \
        -sess_in "$D/sess.pem" 2>&1)
echo "$RES" | grep -q "Reused" \
   && ok "resumes from a session ticket" || bad "session resumption"

# 3b/3c. RESUMPTION ON BOTH SIDES OF A HELLO RETRY REQUEST, and the number of
#    ClientHellos ASSERTED rather than assumed.
#
#    The two are different code paths and only one of them was ever exercised
#    here -- by accident, and not the one anybody would have guessed. A modern
#    OpenSSL client leads with X25519 (or a hybrid), so the plain `s_client`
#    in test 3 above ALREADY provokes a retry: every resumption this file has
#    ever tested went through the retry path, and the direct path had no test
#    at all. That is exactly the shape of coverage that reports a rule as
#    pinned while the rule is never consulted, so the group list is now named
#    and the hello count is checked. A case that did not produce the handshake
#    it claims to test is a FAILURE, not a pass.
#
#    After a retry the binder must be verified over message_hash(CH1) ||
#    HelloRetryRequest || Truncate(CH2) (RFC 8446 4.2.11.2 with 4.4.1). Get
#    that transcript wrong in any way -- the obvious wrong answer is to hash
#    the truncated CH2 alone -- and 3c stops resuming while 3b still does,
#    which is what makes 3c the isolating case for the synthetic message_hash.
resume_case() { # resume_case <label> <groups> <expected ClientHellos>
   OUT=$({ printf 'GET /login HTTP/1.1\r\nHost: localhost\r\n\r\n'; sleep 2; } \
         | openssl s_client -connect "127.0.0.1:$PORT" -servername localhost \
           -groups "$2" -sess_in "$D/sess.pem" -msg 2>&1)
   NCH=$(printf '%s\n' "$OUT" | grep -ac ", ClientHello")
   if [ "$NCH" != "$3" ]; then
      bad "$1: $NCH ClientHello(s), expected $3 -- this case did not run"
   elif printf '%s\n' "$OUT" | grep -qa "Reused"; then
      ok "$1"
   else
      bad "$1"
   fi
}
resume_case "resumes with no retry (one ClientHello)" "P-256" 1
resume_case "resumes across a HelloRetryRequest (two ClientHellos)" \
   "X25519:P-256" 2

# 3d. NEGATIVE: THE SECOND CLIENTHELLO'S BINDER MUST BE VERIFIED.
#
#     The binder is the only thing proving the client holds the pre-shared
#     key, and everything it authenticates travels in the clear. The server
#     used to verify it once, in ClientHello 1, and carry the verdict across
#     the retry -- so a replayed ClientHello 1 authenticated any ClientHello 2
#     at all. srv/test/tlsretry.py replays a captured hello and then presents
#     a binder that does not validate, twice: once in CH2 and once in CH1. It
#     runs its own CONTROL first (a byte-perfect replay must resume), so a run
#     where the fixture quietly failed reports itself instead of passing.
if [ "$HAVE_PY" = 1 ]; then
   python3 "$(dirname "$0")/tlsretry.py" 127.0.0.1 "$PORT" "$D/sess.pem" \
      && ok "refuses a retried ClientHello whose PSK binder is wrong" \
      || bad "refuses a retried ClientHello whose PSK binder is wrong"
fi

# 3e. NEGATIVE: EVERY VECTOR IN A CLIENTHELLO MUST EXHAUST ITS DECLARED
#     LENGTH, and no two extensions may be the same one.
#
#     srv/test/cryptotest.c drives these against tls_handshake directly, over
#     a socketpair, which is where they can watch the parser refuse before a
#     byte reaches the wire. They are ALSO here, against the real listening
#     server, for two reasons the in-process suite cannot cover:
#
#       - `make tlsasan` runs this file against build/srv-asan/sync, so these
#         are the cases that put the new nested cursors under AddressSanitizer.
#         A parser that reads one byte past an attacker-chosen vector is the
#         defect class here, and a read that does not happen to crash is
#         indistinguishable from a pass without ASan behind it.
#       - the binary under test is the one that SHIPS: no -DTLS_FAULTS, no
#         test hooks, the real record layer and the real socket.
#
#     ONE CASE PER RUN, each with its own verdict. A single "malformed hellos
#     are rejected" assertion cannot say which cursor is loose -- loosen the
#     cipher-suite loop and the run still fails, on some other case, and the
#     report exonerates the parser. The controls come first: a hand-built
#     hello that is right in every particular must be ANSWERED, or every
#     silence below would be the silence of a server nobody reached.
if [ "$HAVE_PY" = 1 ]; then
   hello_case() { # hello_case <case> <what it is>
      python3 "$(dirname "$0")/tlshello.py" 127.0.0.1 "$PORT" "$1" \
         && ok "$2" || bad "$2"
   }
   hello_case control     "CONTROL: a hand-built ClientHello is answered"
   hello_case psk-control "CONTROL: ...and so is one offering a ticket we cannot open"
   hello_case cs-tail     "refuses a cipher_suites vector with a trailing byte"
   hello_case comp-two    "refuses legacy_compression_methods other than one zero byte"
   hello_case ver-tail    "refuses a supported_versions vector with a trailing byte"
   hello_case sig-tail    "refuses a signature_algorithms vector with a trailing byte"
   hello_case ks-tail     "refuses a client_shares vector with a trailing byte"
   hello_case ks-over     "refuses a client_shares length that overruns key_share"
   hello_case modes-tail  "refuses a psk_key_exchange_modes body with a trailing byte"
   hello_case ext-tail    "refuses a trailing byte after the last extension"
   hello_case ext-over    "refuses an extension block that overruns the hello"
   hello_case hello-tail  "refuses a trailing byte after the extension block"
   hello_case dup         "refuses a duplicated extension"
   hello_case ident-tail  "refuses a PSK identity list with a trailing byte"
   hello_case binder-tail "refuses a PSK binder list with a trailing byte"
   hello_case binders-short "refuses a binder list SHORTER than the identity list"
   hello_case binders-long  "refuses a binder list longer than the identity list"
   hello_case binder-short  "refuses a binder below the RFC's 32-byte minimum"
fi

# 4. NEGATIVE: a forged record must be refused. This is the assertion whose
#    absence let the deleted tag check pass. We complete a handshake with
#    python, then flip one bit of an application record's ciphertext; the
#    server must not answer it.
#    It must be the TAG that is wrong and the ciphertext that is right -- see
#    tlsforge.py for why a record of random bytes proves nothing at all.
if [ "$HAVE_PY" = 1 ]; then
   python3 "$(dirname "$0")/tlsforge.py" 127.0.0.1 "$PORT" \
      && ok "refuses a record whose authentication tag is wrong" \
      || bad "refuses a record whose authentication tag is wrong"
fi

# 5. NEGATIVE: an RSA key must be refused outright rather than half-working.
#    tls_init scans for a 32-byte EC scalar, so this is the one configuration
#    error that silently produces a server nobody can reach.
openssl genrsa -out "$D/rsa.pem" 2048 2>/dev/null
openssl req -new -x509 -key "$D/rsa.pem" -out "$D/rsacert.pem" -days 2 \
    -subj "/CN=localhost" 2>/dev/null
# ITS OWN DIRECTORY, and therefore its own database. This ran in $D alongside
# the server that is still up and holding $D/sync.db, so the two contended for
# the same file -- and the assertion is about a KEY, which makes any dependence
# on sqlite contention pure flakiness. It was: one run in a dozen had the RSA
# process still alive when the window closed and reported "it started anyway",
# a failure naming the wrong thing entirely.
mkdir -p "$D/rsa"
rsa_up() { # rsa_up <port>
   "$BIN" "$1" "$D/rsa" "$D/rsacert.pem" "$D/rsa.pem" > "$D/rsa.log" 2>&1 &
   T_PID=$!
}
# Polls for the exit rather than sleeping a flat two seconds and looking once:
# it returns as soon as the answer is known, and a slow machine does not turn a
# correct refusal into a failure. A port lost to another suite is retried, not
# reported as a credential failure.
serve_refuses "the RSA case" "$D/rsa.log" rsa_up
case $? in
1) bad "refuses an RSA key (it started anyway)"; kill "$T_PID" 2>/dev/null ;;
2) bad "refuses an RSA key: no port could be had, so nothing was tested" ;;
*)
   # ...and refuses it for the RIGHT REASON. A process that exited because the
   # port was taken, or the directory unwritable, satisfies "it did not start"
   # while proving nothing about the key.
   # EITHER refusal, because which one fires is a coin toss on the key bytes.
   # find_ec_scalar looks for a 32-byte OCTET STRING (04 20) anywhere in the
   # DER, and an RSA key is ~1200 bytes of modulus and primes, so roughly one
   # key in fifty happens to contain that pair. When it does, tls_init takes
   # those 32 bytes as a scalar and gets one step further before refusing --
   # at key_matches_cert, which cannot find a P-256 public key in an RSA
   # certificate. Both are the credential refusal this case is about; pinning
   # only the first made a green suite red about two runs in a hundred, with a
   # message that named the wrong thing.
   ck "refuses an RSA key instead of starting unreachable" \
      "no EC private key\|cannot read a P-256 public key" \
      "$(cat "$D/rsa.log" 2>/dev/null)" ;;
esac

# 5b. NEGATIVE: a certificate and a key that are both VALID and UNRELATED.
#
#     The worst-behaved configuration error there is. Both files parse, so the
#     server used to come up, print "listening", and then fail EVERY handshake
#     at CertificateVerify -- because the signature is made with a key the
#     presented certificate does not vouch for. Nothing local looks wrong: the
#     pid is alive and the log says it is serving. Only clients see it, as a
#     TLS error they cannot act on, and the phone retries for ever.
#
#     It is also the likeliest rotation mistake: two files, two copies, and
#     copying one of them leaves exactly this.
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "$D/other.pem" -out "$D/othercert.pem" -days 2 -nodes \
    -subj "/CN=localhost" 2>/dev/null
mkdir -p "$D/mix"
# The REAL certificate with the OTHER key: each is valid, and they are not a
# pair. Its own directory, for the reason the RSA case above documents.
mix_up() { # mix_up <port>
   "$BIN" "$1" "$D/mix" "$D/cert.pem" "$D/other.pem" > "$D/mix.log" 2>&1 &
   T_PID=$!
}
serve_refuses "the mismatched-pair case" "$D/mix.log" mix_up
case $? in
1) bad "refuses a mismatched cert/key pair (it started anyway)"
   kill "$T_PID" 2>/dev/null ;;
2) bad "refuses a mismatched cert/key pair: no port could be had" ;;
*)
   ck "refuses a mismatched cert/key pair before listening" \
      "is NOT the key for the certificate" "$(cat "$D/mix.log" 2>/dev/null)"
   # ...and it never got as far as a listener, which is the point: a refusal
   # that happens after the port is open is a window in which clients fail.
   if grep -q "listening on port" "$D/mix.log" 2>/dev/null; then
      bad "...but it had already announced a listener"
   else
      ok "...and never announced one"
   fi ;;
esac

# 5c. NEGATIVE: credential files that are not WHOLE.
#
#     slurp did one read into a 16 KiB buffer and took whatever came back. A
#     short read is LEGAL and on a regular file usually does not happen -- which
#     is worse than always, because the failure is intermittent and looks like a
#     corrupt certificate. And a file larger than the buffer was silently
#     prefix-truncated: a chain with several intermediates parsed to however
#     many fit, and the server presented a partial chain.
#
#     The short read cannot be arranged from a shell. The other three can, and
#     each must produce its OWN diagnostic rather than a generic "cannot read":
#     the operator's next move differs for a missing file, an empty one, one too
#     large, and one cut mid-block.
CRED_LOG=$D/cred.log
cred_up() { # cred_up <port> -- reads CRED_CERT/CRED_KEY, set by cred_case
   cdir="$D/cred$1"
   mkdir -p "$cdir"
   "$BIN" "$1" "$cdir" "$CRED_CERT" "$CRED_KEY" > "$CRED_LOG" 2>&1 &
   T_PID=$!
}

cred_case() { # cred_case <what> <certfile> <keyfile> <expected text>
   CRED_CERT=$2
   CRED_KEY=$3
   serve_refuses "$1" "$CRED_LOG" cred_up
   case $? in
   1) bad "refuses $1 (it started anyway)"
      kill "$T_PID" 2>/dev/null ;;
   2) bad "refuses $1: no port could be had, so nothing was tested" ;;
   *)
      ck "refuses $1" "$4" "$(cat "$CRED_LOG" 2>/dev/null)"
      if grep -q "listening on port" "$CRED_LOG" 2>/dev/null; then
         bad "...but it had already announced a listener for $1"
      fi ;;
   esac
}

cred_case "a certificate file that is not there" \
   "$D/absent.pem" "$D/key.pem" "cannot be opened"
: > "$D/empty.pem"
cred_case "an EMPTY certificate file" \
   "$D/empty.pem" "$D/key.pem" "is empty"
# A block that begins and never ends: what an interrupted copy leaves.
head -3 "$D/cert.pem" > "$D/cut.pem"
cred_case "a TRUNCATED certificate (a PEM block with no END)" \
   "$D/cut.pem" "$D/key.pem" "is TRUNCATED"
head -2 "$D/key.pem" > "$D/cutkey.pem"
cred_case "a TRUNCATED private key" \
   "$D/cert.pem" "$D/cutkey.pem" "is TRUNCATED"
# Larger than the ceiling. The prefix here is a COMPLETE, VALID certificate, so
# the old code would have parsed it happily and started -- the truncation is
# invisible precisely when the beginning of the file is good.
{ cat "$D/cert.pem"; head -c 20000 /dev/zero | tr '\0' 'x'; } > "$D/big.pem"
cred_case "a certificate file larger than the ceiling" \
   "$D/big.pem" "$D/key.pem" "is larger than this server reads"

# 6. REGRESSION: a peer that completes a handshake and then RESETS must not
#    wedge a worker. full_write used to retry on every errno, so with SIGPIPE
#    ignored the close alert spun on EPIPE forever -- ten sockets pinned every
#    worker on a one-core board and the server stopped answering entirely.
if [ "$HAVE_PY" = 1 ]; then
   RESETS=$(python3 - "$PORT" <<'PY'
import socket, ssl, struct, time, sys
port = int(sys.argv[1])
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
done = 0
for _ in range(10):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        t = ctx.wrap_socket(s, server_hostname="localhost")
        t.send(b"GET /login HTTP/1.1\r\nHost: localhost\r\n\r\n")
        time.sleep(0.1)
        t.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        t.close()
        done += 1
    except Exception:
        pass
print("RESET %d" % done)
PY
)
   # THE ATTACK HAPPENED. Every connection is wrapped in try/except, so a run
   # where all ten failed to connect printed nothing and the CPU measurement
   # below then confirmed that a server nobody had touched was not spinning.
   [ "$RESETS" = "RESET 10" ] \
      && ok "ten handshakes were completed and reset" \
      || bad "the reset fixture did not run (got '${RESETS:-nothing}')"

   # THE MEASUREMENT MUST HAVE BEEN TAKEN.
   #
   # This was `awk ... /proc/$SRV/stat 2>/dev/null || echo 0` at both ends, so
   # an unreadable /proc -- a different pid namespace, a kernel without procfs,
   # a server that had already died -- gave T0 = T1 = 0, a computed 0% and a
   # cheerful "no CPU spin (0%)". The one number that decides this assertion
   # was the one number allowed to be missing.
   #
   # awk's exit status does not report a failed read either, so the field is
   # checked for being a number rather than the command for being successful.
   read_ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null; }
   # EACH value separately. Concatenating them and testing the join for digits
   # is the same fail-open shape one level up: with T0 and T1 both empty the
   # joined string is just the clock tick, which IS all digits, and awk then
   # computes (""-"")/hz = 0 -- a green assertion over two readings that were
   # never taken.
   is_num() { case "${1:-}" in "" | *[!0-9]*) return 1 ;; *) return 0 ;; esac; }
   T0=$(read_ticks "$SRV")
   sleep 4
   T1=$(read_ticks "$SRV")
   HZ=$(getconf CLK_TCK 2>/dev/null)
   if ! is_num "$T0" || ! is_num "$T1" || ! is_num "$HZ"; then
      bad "the CPU measurement could not be taken (/proc/$SRV/stat: '$T0' '$T1', CLK_TCK '$HZ')"
   else
      PCT=$(awk -v a="$T0" -v b="$T1" -v hz="$HZ" \
            'BEGIN{printf "%.0f", (b-a)*100.0/(hz*4)}')
      [ "$PCT" -lt 25 ] \
         && ok "no CPU spin after 10 reset connections (${PCT}%)" \
         || bad "worker spinning after reset connections (${PCT}%)"
   fi
   CODE=$(curl -sk -o /dev/null -m 10 -w '%{http_code}' "$U/login" 2>/dev/null)
   [ "$CODE" = "200" ] && ok "still serving after the resets" \
                       || bad "stopped serving after the resets (got $CODE)"
fi

# 7. REGRESSION: a peer that dribbles must not hold a worker forever.
#    full_read consulted the deadline ONLY when a read failed, so a client
#    sending one byte at a time -- every read succeeding, none timing out --
#    never had the deadline evaluated at all. Unlike the spin above this costs
#    no CPU, so it is invisible: the worker is simply gone. HTTPS_DEADLINE_S is
#    8s, so a server that enforces it hangs up well inside 25.
if [ "$HAVE_PY" = 1 ]; then
   python3 - "$PORT" <<'PY' && ok "hangs up on a byte-at-a-time client" \
                            || bad "hangs up on a byte-at-a-time client"
import socket, sys, time
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=30)
s.settimeout(30)
# A plausible record header, then one byte at a time, forever.
s.send(b"\x16\x03\x01\x02\x00")
t0 = time.time()
closed = False
try:
    while time.time() - t0 < 25:
        s.send(b"\x00")
        if not s.recv(1):      # server hung up
            closed = True
            break
        time.sleep(0.5)
except OSError:
    closed = True              # RST/EPIPE is also the server letting go
s.close()
print("   held for %.1fs%s" % (time.time() - t0, "" if closed else " (STILL OPEN)"))
sys.exit(0 if closed else 1)
PY
fi

# 8. POST-HANDSHAKE KEYUPDATE, in both directions.
#
#    The protected-record loop used to clear and ignore every record that was
#    not application data or an alert, KeyUpdate included. That is the worst
#    of the available answers: RFC 8446 4.6.3 has the sender switch keys AFTER
#    sending the message, so the peer's very next record is encrypted under a
#    key this server had thrown away the announcement of. The connection then
#    died at the AEAD, indistinguishable from a forged record.
#
#    `K` makes s_client send a KeyUpdate asking for one back; `k` sends one
#    that does not. The two cases pin different halves and each names the
#    fixture it depends on, because "the client never sent a KeyUpdate" would
#    otherwise look exactly like "the server handled it":
#
#      K -- we must send our own KeyUpdate (<<<) and the request that follows
#           must still be answered. A server that skips the reply, or that
#           advances its own write keys without sending it, loses the
#           RESPONSE; one that never advances its read keys loses the request.
#      k -- no reply is due, and sending one anyway is its own bug, so the
#           absence of a <<< KeyUpdate is asserted rather than ignored.
ku_case() { # ku_case <label> <k|K> <expect reply: yes|no>
   OUT=$({ sleep 1; printf '%s\n' "$2"; sleep 1
           printf 'GET /login HTTP/1.1\r\nHost: localhost\r\n\r\n'; sleep 2; } \
         | openssl s_client -connect "127.0.0.1:$PORT" -servername localhost \
           -msg 2>&1)
   SENT=$(printf '%s\n' "$OUT" | grep -ac '>>>.*KeyUpdate')
   GOT=$(printf '%s\n' "$OUT" | grep -ac '<<<.*KeyUpdate')
   CODES=$(printf '%s\n' "$OUT" | grep -ac '^HTTP/1.1 200')
   if [ "$SENT" -lt 1 ]; then
      bad "$1: the client sent no KeyUpdate -- this case did not run"
   elif [ "$3" = yes ] && [ "$GOT" -lt 1 ]; then
      bad "$1: the server sent no KeyUpdate of its own"
   elif [ "$3" = no ] && [ "$GOT" -gt 0 ]; then
      bad "$1: the server sent a KeyUpdate nobody asked for"
   elif [ "$CODES" = "1" ]; then
      ok "$1"
   else
      bad "$1: $CODES responses after the key change, expected 1"
   fi
}
ku_case "serves a request after a KeyUpdate that asks for one back" K yes
ku_case "serves a request after a KeyUpdate that does not" k no

# 9. NEGATIVE: a post-handshake record that is NOT a KeyUpdate must be refused
#    deterministically, not skipped.
#
#    Same loop, same habit: a change_cipher_spec after the peer's Finished is
#    an unexpected record type (RFC 8446 5.1), and skipping it was also an
#    unbounded free loop -- five bytes a peer can send for as long as it likes,
#    each costing a read. The control run through the same proxy proves the
#    proxy is not what breaks the connection.
if [ "$HAVE_PY" = 1 ]; then
   python3 - "$PORT" <<'PY' && ok "refuses a change_cipher_spec after the handshake" \
                            || bad "refuses a change_cipher_spec after the handshake"
import socket, ssl, sys, threading
port = int(sys.argv[1])
CCS = b"\x14\x03\x03\x00\x01\x01"          # a legal record, in an illegal place

def pump(src, dst, inject_after):
    """Forward records; after the inject_after'th application record from the
    client -- the first is its Finished -- slip in a change_cipher_spec."""
    seen, buf = 0, b""
    try:
        while True:
            chunk = src.recv(65536)
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 5:
                n = (buf[3] << 8) | buf[4]
                if len(buf) < 5 + n:
                    break
                rec, buf = buf[:5 + n], buf[5 + n:]
                dst.sendall(rec)
                if rec[0] == 0x17:
                    seen += 1
                    if seen == inject_after:
                        dst.sendall(CCS)
    except OSError:
        pass
    try:
        dst.shutdown(socket.SHUT_WR)
    except OSError:
        pass

def relay(listener, inject_after):
    client, _ = listener.accept()
    up = socket.create_connection(("127.0.0.1", port), timeout=10)
    threading.Thread(target=pump, args=(up, client, 0), daemon=True).start()
    pump(client, up, inject_after)

def run(inject_after):
    l = socket.socket()
    l.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    l.bind(("127.0.0.1", 0))
    l.listen(1)
    threading.Thread(target=relay, args=(l, inject_after), daemon=True).start()
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    try:
        raw = socket.create_connection(("127.0.0.1", l.getsockname()[1]),
                                       timeout=10)
        t = ctx.wrap_socket(raw, server_hostname="localhost")
        t.send(b"GET /login HTTP/1.1\r\nHost: localhost\r\n\r\n")
        t.settimeout(5)
        return t.recv(64)
    except Exception:
        return b""

control, attack = run(0), run(1)
if not control.startswith(b"HTTP"):
    print("   the control did not get a page, so nothing was tested")
    sys.exit(1)
if attack.startswith(b"HTTP"):
    print("   the server answered a request after an unexpected record")
    sys.exit(1)
sys.exit(0)
PY
fi

# NOT TESTED HERE: a ticket write that fails part way (TICKET_WRITE_FAILED).
#
# tls_handshake now refuses the connection when send_ticket's WRITE fails,
# rather than reporting success and letting http_handle_conn run a request for
# a peer whose record layer is torn. I could not build a case that fails
# without that change, and the reason is worth writing down because it is not
# "I did not try hard enough" -- it is that the two ways a write can fail both
# take the connection with them BEFORE the difference becomes visible:
#
#   - A DEAD SOCKET. A reset is what makes send() fail with EPIPE/ECONNRESET,
#     and an arriving RST also flushes the receive queue -- so the request the
#     buggy server would have executed is gone by the time it would read it.
#     Both builds do nothing. A clean close is worse for the test, not better:
#     the first write into a closed peer SUCCEEDS and the RST comes back
#     afterwards.
#   - THE DEADLINE. give_up() is consulted at the top of every full_read pass
#     as well as every full_write pass, and accepted sockets carry a 1 s
#     SO_RCVTIMEO so a blocked read really does come back to check it. The
#     handshake's last read is the client's Finished, microseconds before the
#     ticket write, so any budget that expires in time to fail the write has
#     already failed that read. Landing between the two means hitting a window
#     of a few microseconds, which is a race to arrange, not a test.
#
# What that leaves is a rule that is right on inspection and unexercised here.
# Everything downstream of it IS covered: tests 3 and 3b/3c prove the ticket is
# written and resumes on the ordinary path, so a version of this that refused
# every connection would not survive the file.
#
# NOT TESTED HERE: the write-side stall.
#
# full_write now checks the deadline on every pass, mirroring full_read, and
# web_route sends the response after releasing the page lock. I could not build
# a case that FAILS without those changes, and a test that passes either way is
# worse than none -- it is the CPU-percentage assertion in test 6 all over
# again. The reason is that this server cannot produce a response big enough to
# block: a page is ~600 bytes and the largest thing it ever sends is a 256 kB
# GIF, both of which disappear into an autotuned send buffer. Ten clients
# pipelining 400 requests each and never reading did not hold a single worker.
#
# So the reported denial of service is not reachable at these response sizes.
# The deadline stays because tls_send is a public interface whose header
# promises it "moves all n bytes or fails", and because a future large response
# would make it reachable -- but it is defence, not a fixed exploit.

# Anything raised from inside a command substitution, where `fail=1` could not
# escape, becomes the verdict here.
t_end
[ "$fail" -eq 0 ] && printf 'tlstest: the TLS server answers real clients\n'
exit "$fail"
