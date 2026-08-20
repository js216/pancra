#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
# pairproxy.py --- an interloper wedged into the middle of a live pairing
# Copyright 2026 Jakob Kastelic
#
# WHY THIS EXISTS AT ALL.
#
# The rule under test is "a round-one request naming an account must not
# displace an exchange that account is already in the middle of". Proving it
# needs three things to happen in this order, from outside the server:
#
#   1. an honest client completes round 1, so an exchange is LIVE;
#   2. a second round 1 naming the same account arrives while it is live;
#   3. the honest client goes on to rounds 2, 3 and 4 and still succeeds.
#
# Step 2 has to land BETWEEN the honest client's round 1 and its round 2, and
# synccli runs all four rounds back to back inside one process -- so a shell
# script cannot get a request in edgeways, and every attempt to test this from
# the shell alone ends up testing a sequence the attack does not use.
#
# So the honest client is pointed at this instead of at the server. It forwards
# every request unchanged, and when it has just forwarded a POST /v1/pair/1 --
# after the server answered it, which is the moment the exchange became live --
# it sends that request AGAIN on a connection of its own before handing the
# reply back. The honest client never knows; from the server's side, a second
# round one arrived mid-exchange.
#
# THE REPLAY IS THE POINT, and it is why there is no J-PAKE in this file.
#
# The interloper's packet is a byte-for-byte copy of the honest client's own
# round-1 body. That makes it a WELL-FORMED round one for exactly the right
# account: the zero-knowledge proofs in an EC-J-PAKE round 1 cover two
# ephemeral public keys and nothing else -- no server challenge, no nonce, no
# timestamp -- so they verify again, forever, on any exchange. A round one
# built out of 320 zeros would also be refused, but it would be refused by the
# packet check as well as by the rule this is aimed at, and a case that two
# rules both refuse pins neither. The replay can only be refused by the rule.
#
# It is also a fair model of the attacker in the threat: someone who has seen
# one round-1 packet (or generated one, which needs no secret) and knows the
# email address. That is the whole of what the old code required.
#
#   pairproxy.py <listen-port> <server-port> <status-file>
#
# Every injected attempt's HTTP status is appended to <status-file>, one per
# line, so the suite can assert on what the server told the interloper. Runs
# until killed. Single-threaded on purpose: synccli is sequential, and a thread
# pool here would let the injection race the very request it is meant to follow.
import socket
import sys


def read_request(conn):
    """The WHOLE request: headers, then exactly Content-Length bytes.

    A proxy that forwards a prefix makes the server answer 400, and a 400 in
    the middle of this test reads as a protocol failure rather than as this
    file's own bug -- the same trap srv/synccli.c documents at length.
    """
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = conn.recv(65536)
        if not chunk:
            return None
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    want = 0
    for line in head.split(b"\r\n")[1:]:
        if line.lower().startswith(b"content-length:"):
            want = int(line.split(b":", 1)[1].strip())
    while len(rest) < want:
        chunk = conn.recv(65536)
        if not chunk:
            break
        rest += chunk
    return head + b"\r\n\r\n" + rest


def force_close(raw):
    """Rewrite the request so the reply ends at end-of-stream.

    This proxy reads an upstream reply until EOF, which is the whole of its
    framing -- and that is only correct when the server has been told to close.
    synccli already sends `Connection: close`; curl does not, so a readiness
    probe through here blocked until its own timeout and the proxy looked dead
    while working perfectly. Rather than teach this file Content-Length,
    chunked encoding and keep-alive, every forwarded request is made a
    close-delimited one.

    The BODY is never touched, which is what the replay depends on: the
    interloper's round-1 packet stays byte-for-byte the honest client's.
    """
    head, sep, body = raw.partition(b"\r\n\r\n")
    lines = [ln for ln in head.split(b"\r\n")
             if not ln.lower().startswith(b"connection:")]
    lines.append(b"Connection: close")
    return b"\r\n".join(lines) + sep + body


def to_server(port, raw):
    """Send one request, read the whole reply. force_close has made every
    request close-delimited, so end-of-stream IS end-of-reply."""
    s = socket.create_connection(("127.0.0.1", port))
    try:
        s.sendall(raw)
        out = b""
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
        return out
    finally:
        s.close()


def status_of(rsp):
    """0 rather than an exception: a status of 0 fails every assertion in the
    suite, whereas a traceback here would kill the proxy and make the honest
    client's NEXT round look like the failure."""
    try:
        return int(rsp.split(b" ", 2)[1])
    except (IndexError, ValueError):
        return 0


def main():
    if len(sys.argv) != 4:
        sys.stderr.write("usage: pairproxy.py <listen> <server> <statusfile>\n")
        return 2
    listen_port = int(sys.argv[1])
    server_port = int(sys.argv[2])
    status_file = sys.argv[3]

    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", listen_port))
    srv.listen(8)
    while True:
        conn, _ = srv.accept()
        try:
            raw = read_request(conn)
            if raw is None:
                continue
            line = raw.split(b"\r\n", 1)[0]
            fwd = force_close(raw)
            rsp = to_server(server_port, fwd)
            # AFTER the reply, BEFORE it is handed back. The exchange is live
            # from the moment the server answered round 1; injecting before
            # the forward would race it and test nothing in particular.
            if b"/v1/pair/1" in line:
                inject = to_server(server_port, fwd)
                with open(status_file, "a") as f:
                    f.write("%d\n" % status_of(inject))
            conn.sendall(rsp)
        except OSError as e:
            sys.stderr.write("pairproxy: %s\n" % e)
        finally:
            conn.close()


if __name__ == "__main__":
    sys.exit(main())
