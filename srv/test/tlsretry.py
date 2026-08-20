#!/usr/bin/env python3
"""Replay a captured ClientHello into a retry, then present a WRONG binder.

WHAT IS BEING TESTED, AND WHY THE OBVIOUS TEST DOES NOT TEST IT.

After a HelloRetryRequest the server reparsed ClientHello 2 but carried the
resumed-PSK decision forward from ClientHello 1, so ClientHello 2's binder was
never checked. The obvious way to catch that is a man in the middle: relay a
real resumption handshake and flip a bit of the second ClientHello's binder.
It proves nothing. Flipping a byte in flight also changes the SERVER's
transcript, so the two sides disagree at the Finished MAC and the handshake
fails on both a fixed and an unfixed server -- a green assertion over a rule
that was never consulted.

The real shape of the attack has nobody in the middle. Anything the binder
authenticates travels in the CLEAR, in ClientHello 1: the ticket, the
identity, the binder itself. So an attacker who watched one ticket-bearing
ClientHello go past can:

  1. REPLAY it verbatim. Its binder is a MAC over that exact hello, so it
     still validates, and the server concludes the peer holds the PSK.
  2. Take the HelloRetryRequest -- which browsers provoke as a matter of
     course, because they lead with X25519 and this server speaks P-256.
  3. Send ANY ClientHello 2 it likes, with a binder it cannot compute.

With the binder carried forward, step 3 was unchecked: the server entered the
resumed key schedule, announced selected_identity 0, skipped Certificate and
CertificateVerify, and answered a hello nobody who holds the pre-shared key
ever wrote. It could not read what came back -- the key schedule still folds
in a PSK the replayer does not have, so the client Finished fails -- but the
authentication had already been granted on the strength of a proof that was
never made for the message being answered. RFC 8446 4.2.11 requires the binder
of the selected PSK to validate over the complete transcript, which after a
retry is message_hash(ClientHello1) || HelloRetryRequest ||
Truncate(ClientHello2) (4.2.11.2 with 4.4.1's synthetic message).

The observable is the SERVERHELLO, which is plaintext: a server that resumed
sends one carrying the pre_shared_key extension. A server that refused sends
nothing at all.

Two runs, differing in ONE BIT:

  CONTROL  replay CH1, then CH2 exactly as the real client sent it. The
           HelloRetryRequest is a pure function of CH1 here (its random is
           fixed by the RFC and its session id is echoed), so the replayed
           transcript is byte-identical and CH2's binder is genuinely valid.
           The server MUST resume -- which is what proves this fixture
           reaches the code under test at all, rather than failing for some
           unrelated reason and looking like a pass.
  ATTACK   the same, with the last byte of CH2 flipped. That byte is the last
           byte of the last binder (pre_shared_key must be the final
           extension, and the binder list is its final field), and nothing
           else in the message reads it: the length checks in parse_hello are
           about widths, not contents. The server MUST NOT resume.
  FIRST    CH1 itself with its last byte flipped -- a live ticket this server
           sealed, presented with a binder that does not validate, in a FIRST
           ClientHello. RFC 8446 4.2.11 is a MUST abort, not a licence to
           quietly fall back to a full handshake, and the difference is
           visible from outside: a server that fell back would answer with a
           HelloRetryRequest. Nothing may come back at all.

Exit 0 if the control resumed and neither attack did.

Usage: tlsretry.py <host> <port> <session.pem>
"""
import socket
import subprocess
import sys
import threading

# RFC 8446 4.1.3: a HelloRetryRequest is a ServerHello with this random.
HRR_RANDOM = bytes.fromhex(
    "cf21ad74e59a6111be1d8c021e65b891c2a211167abb8c5e079e09e2c8a8339c")

REC_HANDSHAKE = 0x16
HS_CLIENT_HELLO = 1
HS_SERVER_HELLO = 2
EXT_PRE_SHARED_KEY = 41

REQUEST = b"GET /login HTTP/1.1\r\nHost: localhost\r\n\r\n"


def records(buf):
    """Split a byte stream into whole TLS records; return (records, rest)."""
    out = []
    while len(buf) >= 5:
        n = (buf[3] << 8) | buf[4]
        if len(buf) < 5 + n:
            break
        out.append(buf[:5 + n])
        buf = buf[5 + n:]
    return out, buf


def server_hello_kind(rec):
    """(is_hrr, selects_psk) for a ServerHello record, or None if it is not
    one. The message is plaintext, which is the whole reason this test can
    see the server's answer without holding any key."""
    if len(rec) < 11 or rec[0] != REC_HANDSHAKE or rec[5] != HS_SERVER_HELLO:
        return None
    b = rec[9:]                       # the ServerHello body
    if len(b) < 35:
        return None
    is_hrr = b[2:34] == HRR_RANDOM
    i = 34
    i += 1 + b[i]                     # legacy_session_id_echo
    i += 2 + 1                        # cipher_suite, legacy_compression
    if i + 2 > len(b):
        return (is_hrr, False)
    end = i + 2 + ((b[i] << 8) | b[i + 1])
    i += 2
    psk = False
    while i + 4 <= min(end, len(b)):
        etype = (b[i] << 8) | b[i + 1]
        elen = (b[i + 2] << 8) | b[i + 3]
        if etype == EXT_PRE_SHARED_KEY:
            psk = True
        i += 4 + elen
    return (is_hrr, psk)


def pump_down(src, dst):
    try:
        while True:
            chunk = src.recv(65536)
            if not chunk:
                break
            dst.sendall(chunk)
    except OSError:
        pass


def relay(listener, host, port, hellos):
    """One connection, forwarded untouched, keeping every ClientHello."""
    try:
        client, _ = listener.accept()
        upstream = socket.create_connection((host, port), timeout=10)
    except OSError:
        return
    threading.Thread(target=pump_down, args=(upstream, client),
                     daemon=True).start()
    buf = b""
    try:
        while True:
            chunk = client.recv(65536)
            if not chunk:
                break
            buf += chunk
            recs, buf = records(buf)
            for r in recs:
                if r[0] == REC_HANDSHAKE and len(r) > 5 and \
                        r[5] == HS_CLIENT_HELLO:
                    hellos.append(r)
                upstream.sendall(r)
    except OSError:
        pass


def capture(host, port, sess):
    """One honest resumption over a HelloRetryRequest, through the relay.
    Returns the ClientHello records the real client sent."""
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    hellos = []
    t = threading.Thread(target=relay,
                         args=(listener, host, port, hellos), daemon=True)
    t.start()
    # -groups X25519:P-256 offers a key share for X25519 only while naming
    # P-256 as acceptable, which is exactly what a browser does and what
    # makes the server retry. -sess_in puts the ticket, and its binder, in
    # ClientHello 1.
    cmd = ["openssl", "s_client", "-connect",
           "127.0.0.1:%d" % listener.getsockname()[1],
           "-servername", "localhost", "-groups", "X25519:P-256",
           "-sess_in", sess, "-ign_eof"]
    try:
        p = subprocess.run(cmd, input=REQUEST, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=30)
        out = p.stdout
    except (subprocess.TimeoutExpired, OSError):
        out = b""
    t.join(timeout=5)
    return hellos, out


def replay(host, port, ch1, ch2):
    """Send CH1, expect a retry, send CH2, and report what came back.
    Returns (saw_retry, saw_server_hello, resumed)."""
    saw_retry = saw_sh = resumed = False
    try:
        s = socket.create_connection((host, port), timeout=10)
    except OSError:
        return (False, False, False)
    try:
        s.settimeout(5)
        s.sendall(ch1)
        buf = b""
        while True:                      # the HelloRetryRequest
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
            recs, buf = records(buf)
            for r in recs:
                k = server_hello_kind(r)
                if k and k[0]:
                    saw_retry = True
            if saw_retry:
                break
        if saw_retry:
            s.sendall(ch2)
            s.settimeout(3)
            buf = b""
            try:
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        break            # refused: the server hung up
                    buf += chunk
                    recs, buf = records(buf)
                    for r in recs:
                        k = server_hello_kind(r)
                        if k and not k[0]:
                            saw_sh = True
                            resumed = resumed or k[1]
                    if saw_sh:
                        break
            except socket.timeout:
                pass
    except OSError:
        pass
    finally:
        s.close()
    return (saw_retry, saw_sh, resumed)


def main():
    host, port, sess = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    hellos, out = capture(host, port, sess)
    if len(hellos) < 2:
        print("   the fixture did not run: %d ClientHello(s) captured, "
              "so no retry happened" % len(hellos))
        return 1
    if b"Reused" not in out:
        print("   the fixture did not run: the captured handshake did not "
              "resume, so ClientHello 2 carried no binder to check")
        return 1
    ch1, ch2 = hellos[0], hellos[1]

    retry, sh, resumed = replay(host, port, ch1, ch2)
    if not retry:
        print("   the replayed ClientHello 1 did not draw a "
              "HelloRetryRequest")
        return 1
    if not resumed:
        print("   CONTROL FAILED: a byte-perfect replay did not resume "
              "(ServerHello seen: %s), so the attack below proves nothing"
              % sh)
        return 1

    bad = bytearray(ch2)
    bad[-1] ^= 0x01                      # one bit of the last binder
    retry, sh, resumed = replay(host, port, ch1, bytes(bad))
    if not retry:
        print("   the replayed ClientHello 1 did not draw a "
              "HelloRetryRequest the second time")
        return 1
    if resumed:
        print("   the server RESUMED a session for a ClientHello 2 whose "
              "binder is wrong")
        return 1
    if sh:
        print("   (a ServerHello came back, but it selected no PSK)")

    badfirst = bytearray(ch1)
    badfirst[-1] ^= 0x01                 # one bit of the FIRST hello's binder
    retry, sh, resumed = replay(host, port, bytes(badfirst), ch2)
    if retry or sh or resumed:
        print("   the server ANSWERED a first ClientHello whose binder is "
              "wrong (retry=%s hello=%s resumed=%s)" % (retry, sh, resumed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
