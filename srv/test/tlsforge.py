#!/usr/bin/env python3
"""Corrupt the AUTHENTICATION TAG of one otherwise-valid TLS record.

Why this exists, and why the obvious version of it does not work:

The natural way to test that a server checks its AEAD tag is to send it a
record full of random bytes. That proves nothing. With the tag check deleted
the server decrypts the random bytes to random plaintext, fails to make sense
of them, and closes the connection -- which is exactly what a correct server
does. Both answers look identical from outside, so the assertion passes
against a server with no authentication at all. (Verified: a build with the
aes128_gcm_open() return value discarded passed that version of the test.)

What discriminates is a record whose CIPHERTEXT IS VALID and whose TAG IS NOT.
A server that checks the tag rejects it. A server that does not decrypts it
perfectly and answers the request inside -- so the difference becomes visible
as an HTTP response that should never have existed.

We cannot forge that from outside the session (we have no keys), so we sit in
the middle of our own connection: a proxy passes every byte through untouched
until the client's second application-data record -- the first is the client's
Finished, since TLS 1.3 wraps everything after the ServerHello as type 0x17 --
and flips one bit of its last 16 bytes, which is the GCM tag.

Exit 0 if the server refused it (correct), 1 if it answered (tag unchecked).
"""
import socket
import ssl
import sys
import threading

REC_APPDATA = 0x17
TAG_LEN = 16


def pump(src, dst, tamper_nth):
    """Forward records, flipping the tag of the tamper_nth application record."""
    seen = 0
    buf = b""
    try:
        while True:
            chunk = src.recv(65536)
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 5:
                rtype = buf[0]
                rlen = (buf[3] << 8) | buf[4]
                if len(buf) < 5 + rlen:
                    break
                rec = bytearray(buf[:5 + rlen])
                buf = buf[5 + rlen:]
                if rtype == REC_APPDATA:
                    seen += 1
                    if seen == tamper_nth and rlen >= TAG_LEN:
                        rec[-1] ^= 0x01   # one bit of the GCM tag
                dst.sendall(bytes(rec))
    except OSError:
        pass
    try:
        dst.shutdown(socket.SHUT_WR)
    except OSError:
        pass


def relay(server_host, server_port, listener, tamper_nth):
    client, _ = listener.accept()
    upstream = socket.create_connection((server_host, server_port), timeout=10)
    t = threading.Thread(target=pump, args=(upstream, client, 0), daemon=True)
    t.start()
    pump(client, upstream, tamper_nth)


def main():
    host, port = sys.argv[1], int(sys.argv[2])
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    proxy_port = listener.getsockname()[1]

    # 2 = the client's Finished is the first application-data record in TLS
    # 1.3; the request is the second.
    threading.Thread(target=relay, args=(host, port, listener, 2),
                     daemon=True).start()

    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    reply = b""
    try:
        raw = socket.create_connection(("127.0.0.1", proxy_port), timeout=10)
        tls = ctx.wrap_socket(raw, server_hostname="localhost")
        tls.send(b"GET /login HTTP/1.1\r\nHost: localhost\r\n\r\n")
        tls.settimeout(5)
        reply = tls.recv(64)
    except Exception:
        reply = b""          # a refused record shows up as an error here

    if reply.startswith(b"HTTP"):
        print("   the server ANSWERED a record whose tag was wrong")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
