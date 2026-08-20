#!/usr/bin/env python3
"""Send ONE deliberately malformed ClientHello and require silence.

WHY THIS IS NOT A JOB FOR openssl s_client. Every case here is a message no
TLS library will ever produce: a cipher_suites vector carrying a byte that is
not half a suite, an extension block that claims more bytes than the hello
holds, a PSK offer with five identities and one binder. The point of a
hand-written parser's bounds is precisely the messages a correct client cannot
spell, so the fixture has to spell them itself.

WHY ONE CASE PER RUN. A single "malformed hellos are rejected" assertion
cannot tell you WHICH cursor is still loose: loosen the cipher-suite loop and
the run still fails, on somebody else's case, and the report says the parser
is fine. Each case below breaks exactly ONE vector's rule and leaves every
other byte of the message exact, and the shell runs them one at a time so each
gets its own named verdict.

WHAT COUNTS AS A REFUSAL. The ServerHello is plaintext, so it is visible from
here without holding any key. A server that accepted the hello answers with
one (or with a HelloRetryRequest, which is the same message); a server that
refused sends nothing at all and closes. So the observable is "did a handshake
record come back", and the CONTROL -- a hand-built hello that is correct in
every particular -- must draw one, or the fixture is asserting silence from a
server that was never reachable.

Usage: tlshello.py <host> <port> <case>
Exit 0 if the case behaved as its name says.
"""
import socket
import sys

REC_HANDSHAKE = 0x16
HS_SERVER_HELLO = 2

# The P-256 generator, uncompressed. Any point on the curve does; this one is
# written out rather than computed so the fixture needs no arithmetic.
P256_G = bytes.fromhex(
    "04"
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5")


def u8(v):
    return bytes([v & 0xFF])


def u16(v):
    return bytes([(v >> 8) & 0xFF, v & 0xFF])


def client_hello(case):
    """The record for one case. Every knob makes ONE vector wrong."""
    b = b"\x03\x03"                       # legacy_version: TLS 1.2
    b += bytes(range(0x40, 0x60))         # client random: fixed, unread
    b += u8(0)                            # legacy_session_id: empty

    # cipher_suites: TLS_AES_128_GCM_SHA256, plus a byte that is not a suite
    if case == "cs-tail":
        b += u16(3) + u16(0x1301) + u8(0)
    else:
        b += u16(2) + u16(0x1301)

    # legacy_compression_methods: RFC 8446 4.1.2 allows exactly one zero byte
    b += u8(2) + u8(0) + u8(0) if case == "comp-two" else u8(1) + u8(0)

    e = b""
    # supported_versions
    if case == "ver-tail":
        e += u16(43) + u16(4) + u8(3) + u16(0x0304) + u8(0)
    else:
        e += u16(43) + u16(3) + u8(2) + u16(0x0304)

    # signature_algorithms, twice for the duplicate case
    if case == "sig-tail":
        sig = u16(13) + u16(5) + u16(3) + u16(0x0403) + u8(0)
    else:
        sig = u16(13) + u16(4) + u16(2) + u16(0x0403)
    e += sig * 2 if case == "dup" else sig

    # key_share
    share = u16(0x0017) + u16(65) + P256_G
    inner = share + (u8(0) if case == "ks-tail" else b"")
    declared = len(inner) + (8 if case == "ks-over" else 0)
    e += u16(51) + u16(2 + len(inner)) + u16(declared) + inner

    if case.startswith(("psk", "modes", "ident", "binder")):
        # psk_key_exchange_modes: ke_modes<1..255>, then nothing
        modes = u8(1) + u8(1)             # psk_dhe_ke
        if case == "modes-tail":
            modes += u8(0)
        e += u16(45) + u16(len(modes)) + modes

        # pre_shared_key, and it must be the LAST extension
        idents = {"binders-short": 2, "binders-long": 1}.get(case, 1)
        binders = {"binders-short": 1, "binders-long": 2}.get(case, 1)
        blen = 20 if case == "binder-short" else 32
        ticket = bytes([0x77]) * 68       # a ticket this server never sealed
        ids = b""
        for _ in range(idents):
            ids += u16(len(ticket)) + ticket + u16(0) + u16(0)
        if case == "ident-tail":
            ids += u8(0)
        bl = (u8(blen) + bytes(blen)) * binders
        if case == "binder-tail":
            bl += u8(1)
        psk = u16(len(ids)) + ids + u16(len(bl)) + bl
        e += u16(41) + u16(len(psk)) + psk

    if case == "ext-tail":
        e += u8(0)                        # a stray byte inside the block
    b += u16(len(e) + (8 if case == "ext-over" else 0)) + e
    if case == "hello-tail":
        b += u8(0)                        # a stray byte after the block

    msg = u8(1) + bytes([0, (len(b) >> 8) & 0xFF, len(b) & 0xFF]) + b
    return bytes([REC_HANDSHAKE, 3, 1]) + u16(len(msg)) + msg


def answered(host, port, rec):
    """True if a handshake record came back. Anything else -- silence, a
    close, an alert -- is the server refusing."""
    try:
        s = socket.create_connection((host, port), timeout=10)
    except OSError:
        return None                        # the server is not there at all
    try:
        s.settimeout(4)
        s.sendall(rec)
        buf = b""
        while len(buf) < 5:
            chunk = s.recv(4096)
            if not chunk:
                return False
            buf += chunk
        return buf[0] == REC_HANDSHAKE and len(buf) > 5 and \
            buf[5] == HS_SERVER_HELLO
    except OSError:
        return False
    finally:
        s.close()


# Every case, and whether the server may answer it.
ANSWERS = {"control": True, "psk-control": True}


def main():
    host, port, case = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    got = answered(host, port, client_hello(case))
    if got is None:
        print("   could not connect: nothing was tested")
        return 1
    want = ANSWERS.get(case, False)
    if got != want:
        print("   %s: the server %s, expected %s" %
              (case, "answered" if got else "said nothing",
               "an answer" if want else "silence"))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
