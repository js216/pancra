#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0
# glucoserve.py --- dead-simple receiver/viewer for Pancra's REMOTE push
# Copyright 2026 Jakob Kastelic

"""Receives glucose datapoints from the app and shows them as plain text.

The app (SETTINGS -> REMOTE) POSTs one line per new datapoint to this server:

    <epoch seconds> <glucose mg/dL>\n

Every accepted line is appended VERBATIM to data.txt (next to this script) --
epoch stays the canonical stored form. A GET returns the LAST 500 lines as
text/plain, one datapoint per line, oldest first, with the timestamp shown
human-readable in local time (no spaces inside it): "2026-07-24T13:48:41 106".

Run (port 80 needs root, or a port-forward / CAP_NET_BIND_SERVICE):

    sudo python3 glucoserve.py [port]

Stdlib only; no dependencies.
"""

import os
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data.txt")
SHOW = 500  # datapoints served to a GET
PORT = 80
RATE_S = 60  # crude DoS guard: accept at most one datapoint per this many s

# The board has no tzdata and runs UTC, so displayed times would be hours off
# the wall clock. A POSIX TZ rule needs no zoneinfo files and handles DST.
# Honour an externally set TZ; default to US Pacific.
os.environ.setdefault("TZ", "PST8PDT,M3.2.0,M11.1.0")
try:
    time.tzset()
except AttributeError:  # non-POSIX platform: local time is what it is
    pass

# One lock around all file access: the appends are tiny, and correctness
# beats concurrency at 288 datapoints a day.
lock = threading.Lock()

# monotonic() of the last ACCEPTED datapoint (None = none yet) -- monotonic,
# not wall clock, so an NTP step at boot cannot open or jam the rate gate.
last_accept = None


class Handler(BaseHTTPRequestHandler):
    server_version = "glucoserve/1.0"

    def _text(self, code, body):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        if n <= 0 or n > 64:  # a datapoint line is ~15 bytes; refuse blobs
            self._text(400, "expected a short '<epoch> <mg/dL>' body\n")
            return
        body = self.rfile.read(n).decode("utf-8", "replace").strip()
        try:
            t, glu = (int(x) for x in body.split())
        except ValueError:
            self._text(400, "expected '<epoch> <mg/dL>'\n")
            return
        if t <= 0 or not 0 < glu < 2000:
            self._text(400, "implausible values refused\n")
            return
        global last_accept
        with lock:
            # Crude DDoS protection: a CGM produces one datapoint per five
            # minutes, so anything faster than one per minute is not glucose.
            # Rejected points are simply lost here -- the phone's own log
            # remains the record of truth, this page is a mirror.
            now = time.monotonic()
            if last_accept is not None and now - last_accept < RATE_S:
                self._text(429, "rate limited: one datapoint per minute\n")
                return
            last_accept = now
            with open(DATA, "a", encoding="utf-8") as f:
                f.write(f"{t} {glu}\n")
        self._text(200, "OK\n")

    @staticmethod
    def _fmt(line):
        """'<epoch> <glu>' -> '2026-07-24T13:48:41 <glu>' (local time, no
        spaces inside the timestamp). Anything unparseable passes verbatim."""
        try:
            t, glu = line.split()
            stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(int(t)))
            return f"{stamp} {glu}\n"
        except (ValueError, OverflowError, OSError):
            return line if line.endswith("\n") else line + "\n"

    def do_GET(self):
        with lock:
            try:
                with open(DATA, encoding="utf-8") as f:
                    lines = f.readlines()
            except FileNotFoundError:
                lines = []
        self._text(200, "".join(self._fmt(l) for l in lines[-SHOW:]))

    def log_message(self, fmt, *args):  # quiet: one line per request is noise
        pass


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    srv = ThreadingHTTPServer(("", port), Handler)
    print(f"glucoserve: listening on port {port}, appending to {DATA}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
