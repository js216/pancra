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

BIN=${1:-build/srv/sync}
PORT=${2:-18443}
D=$(mktemp -d)
FAIL=0

ok()   { printf '   ok   %s\n' "$1"; }
bad()  { printf '   FAIL %s\n' "$1"; FAIL=1; }
have() { command -v "$1" >/dev/null 2>&1; }

cleanup() {
   [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
   [ -n "${SRV:-}" ] && wait "$SRV" 2>/dev/null
   rm -rf "$D"
}
trap cleanup EXIT

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

# A throwaway P-256 certificate. It must be EC: tls_init finds the private key
# by scanning the DER for a 32-byte OCTET STRING, so an RSA key does not load
# at all -- which is itself worth asserting, further down.
openssl ecparam -name prime256v1 -genkey -noout -out "$D/key.pem" 2>/dev/null
openssl req -new -x509 -key "$D/key.pem" -out "$D/cert.pem" -days 2 \
    -subj "/CN=localhost" 2>/dev/null

"$BIN" "$PORT" "$D" "$D/cert.pem" "$D/key.pem" > "$D/srv.log" 2>&1 &
SRV=$!
sleep 3
if ! kill -0 "$SRV" 2>/dev/null; then
   printf 'tlstest: server did not start\n'; cat "$D/srv.log"; exit 1
fi

U="https://127.0.0.1:$PORT"

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

# 4. NEGATIVE: a forged record must be refused. This is the assertion whose
#    absence let the deleted tag check pass. We complete a handshake with
#    python, then flip one bit of an application record's ciphertext; the
#    server must not answer it.
#    It must be the TAG that is wrong and the ciphertext that is right -- see
#    tlsforge.py for why a record of random bytes proves nothing at all.
if have python3; then
   python3 "$(dirname "$0")/tlsforge.py" 127.0.0.1 "$PORT" \
      && ok "refuses a record whose authentication tag is wrong" \
      || bad "refuses a record whose authentication tag is wrong"
elif [ "${ALLOW_SKIP:-0}" = "1" ]; then
   printf '   skip python3 absent: forged-record case not run (ALLOW_SKIP=1)\n'
else
   bad "python3 absent: the forged-record case could not run"
fi

# 5. NEGATIVE: an RSA key must be refused outright rather than half-working.
#    tls_init scans for a 32-byte EC scalar, so this is the one configuration
#    error that silently produces a server nobody can reach.
openssl genrsa -out "$D/rsa.pem" 2048 2>/dev/null
openssl req -new -x509 -key "$D/rsa.pem" -out "$D/rsacert.pem" -days 2 \
    -subj "/CN=localhost" 2>/dev/null
"$BIN" "$((PORT + 1))" "$D" "$D/rsacert.pem" "$D/rsa.pem" > "$D/rsa.log" 2>&1 &
RSAPID=$!
sleep 2
if kill -0 "$RSAPID" 2>/dev/null; then
   bad "refuses an RSA key (it started anyway)"; kill "$RSAPID" 2>/dev/null
else
   ok "refuses an RSA key instead of starting unreachable"
fi

# 6. REGRESSION: a peer that completes a handshake and then RESETS must not
#    wedge a worker. full_write used to retry on every errno, so with SIGPIPE
#    ignored the close alert spun on EPIPE forever -- ten sockets pinned every
#    worker on a one-core board and the server stopped answering entirely.
if have python3; then
   python3 - "$PORT" <<'PY'
import socket, ssl, struct, time, sys
port = int(sys.argv[1])
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
for _ in range(10):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=5)
        t = ctx.wrap_socket(s, server_hostname="localhost")
        t.send(b"GET /login HTTP/1.1\r\nHost: localhost\r\n\r\n")
        time.sleep(0.1)
        t.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        t.close()
    except Exception:
        pass
PY
   T0=$(awk '{print $14+$15}' /proc/$SRV/stat 2>/dev/null || echo 0)
   sleep 4
   T1=$(awk '{print $14+$15}' /proc/$SRV/stat 2>/dev/null || echo 0)
   HZ=$(getconf CLK_TCK)
   PCT=$(awk -v a="$T0" -v b="$T1" -v hz="$HZ" 'BEGIN{printf "%.0f", (b-a)*100.0/(hz*4)}')
   [ "$PCT" -lt 25 ] \
      && ok "no CPU spin after 10 reset connections (${PCT}%)" \
      || bad "worker spinning after reset connections (${PCT}%)"
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
if have python3; then
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

[ "$FAIL" -eq 0 ] && printf 'tlstest: the TLS server answers real clients\n'
exit "$FAIL"
