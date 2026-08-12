#!/usr/bin/env python3
"""Decode a GIF the server rendered and assert the plot is really in it.

Every GIF assertion in synctest.sh checked the six magic bytes and the
Content-Type. That is not a check on srv/gif.c or on the plot: an adversarial
review corrupted one line of the LZW encoder -- `code ^ 1` in the emit path --
and every served plot became garbage while `make check` stayed green, because
garbage still begins with GIF89a and is still sent as image/gif.

So this decodes the image properly (LZW, from the file's own colour table) and
asserts three things a corrupted encoder cannot satisfy at once:

  1. the pixel stream decodes at all, and yields exactly width*height indices
     -- a wrong code, a wrong code width or a desynchronised dictionary runs
     out of data or overruns;
  2. every index is inside the colour table;
  3. the image is a PLOT: more than one colour is used, and the plotted ink is
     a small minority of a mostly-background field. A uniform image passes 1
     and 2 and is exactly what a broken encoder tends to produce.

Usage: gifcheck.py <file.gif>
Exit 0 on success; prints a one-line reason and exits 1 otherwise.
"""
import sys


def die(msg):
    print("   gifcheck: %s" % msg)
    sys.exit(1)


def lzw_decode(data, min_code_size, expected):
    """Standard GIF LZW. Returns the index stream."""
    clear = 1 << min_code_size
    end = clear + 1
    code_size = min_code_size + 1
    dictionary = {i: [i] for i in range(clear)}
    next_code = end + 1
    out = []
    prev = None

    bitpos = 0
    total_bits = len(data) * 8
    while bitpos + code_size <= total_bits:
        # least-significant-bit-first, as GIF specifies
        code = 0
        for i in range(code_size):
            byte = data[(bitpos + i) // 8]
            bit = (byte >> ((bitpos + i) % 8)) & 1
            code |= bit << i
        bitpos += code_size

        if code == clear:
            dictionary = {i: [i] for i in range(clear)}
            next_code = end + 1
            code_size = min_code_size + 1
            prev = None
            continue
        if code == end:
            break

        if code in dictionary:
            entry = dictionary[code]
        elif prev is not None and code == next_code:
            entry = prev + [prev[0]]
        else:
            die("LZW desynchronised: code %d is not in the dictionary "
                "(next would be %d)" % (code, next_code))

        out.extend(entry)
        if prev is not None:
            dictionary[next_code] = prev + [entry[0]]
            next_code += 1
            if next_code == (1 << code_size) and code_size < 12:
                code_size += 1
        prev = entry
        if len(out) > expected * 2:
            die("LZW produced more pixels than the image can hold")
    return out


def main():
    if len(sys.argv) != 2:
        die("usage: gifcheck.py <file.gif>")
    raw = open(sys.argv[1], "rb").read()
    if len(raw) < 14 or raw[:6] not in (b"GIF89a", b"GIF87a"):
        die("not a GIF")

    w = raw[6] | (raw[7] << 8)
    h = raw[8] | (raw[9] << 8)
    flags = raw[10]
    if not (flags & 0x80):
        die("no global colour table")
    gct_len = 1 << ((flags & 0x07) + 1)
    p = 13 + gct_len * 3

    # skip extension blocks to the image descriptor
    while p < len(raw) and raw[p] == 0x21:
        p += 2
        while p < len(raw) and raw[p]:
            p += raw[p] + 1
        p += 1
    if p >= len(raw) or raw[p] != 0x2C:
        die("no image descriptor")
    local = raw[p + 9]
    if local & 0x80:
        die("unexpected local colour table")
    if local & 0x40:
        die("interlaced image (the encoder does not produce these)")
    p += 10

    min_code_size = raw[p]
    p += 1
    sub = bytearray()
    while p < len(raw) and raw[p]:
        n = raw[p]
        sub += raw[p + 1:p + 1 + n]
        p += n + 1

    px = lzw_decode(bytes(sub), min_code_size, w * h)

    if len(px) != w * h:
        die("decoded %d pixels, image says %dx%d = %d" % (len(px), w, h, w * h))
    for v in px:
        if v >= gct_len:
            die("pixel index %d is outside the %d-entry colour table"
                % (v, gct_len))

    counts = {}
    for v in px:
        counts[v] = counts.get(v, 0) + 1
    if len(counts) < 2:
        die("the image is a single flat colour -- nothing was plotted")
    top = max(counts.values())
    if top == len(px):
        die("every pixel is identical")
    ink = len(px) - top
    if ink * 100 < len(px):      # ink under 1% of the field
        die("almost nothing is drawn (%d of %d pixels are not background)"
            % (ink, len(px)))

    print("   ok   the served GIF decodes: %dx%d, %d colours, %.1f%% ink"
          % (w, h, len(counts), 100.0 * ink / len(px)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
