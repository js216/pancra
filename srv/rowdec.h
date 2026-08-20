/* SPDX-License-Identifier: GPL-3.0
 * rowdec.h --- the ONE decoder for a persisted reading row
 * Copyright 2026 Jakob Kastelic
 *
 * FOUR READERS, FOUR PARSERS, FOUR ANSWERS.
 *
 * A reading row is one line of the app's append-only log, stored here as
 * generic text (see db.c) and parsed wherever it is read. There were four
 * copies of that parse -- the headline, the recent table, the plot, and the
 * timezone probe -- each walking commas by hand and each disagreeing:
 *
 *   - `strtol` STOPS WHERE IT LIKES and reports the prefix it managed. "12abc"
 *     came back as 12, "0junk" as kind 0 (a CGM reading), and an empty field
 *     as 0 -- which is a real glucose, a real kind, and a real offset. These
 *     rows arrive over the network from whatever wrote them.
 *   - the headline learned to require an exact kind; the plot and the table
 *     never did, so one page refused a row the next one drew.
 *   - a row that stops early (a truncated line, an older writer) was read as
 *     a row whose missing fields are zero.
 *
 * So: ONE decoder, pure and bounded, and every server reader uses it. A row
 * either decodes completely or it is not a row -- there is no partial answer,
 * because a partial answer is what put a fingerstick at the top of the page
 * and a dose in the plot.
 *
 * THE FORMAT, as app/store.c writes it (see app/store.h):
 *
 *     epoch,glucose,trend10,rssi,recv_lag,source_id,raw_time,tz_off,kind[,rescale]
 *
 * `rssi` is EMPTY when the sample carried none -- the one field that may be
 * blank, because the writer emits it blank. Trailing fields beyond `kind`
 * (today: the rescale factor) are provenance and are ignored, which is what
 * lets the format grow: a reader of an older build must keep reading rows a
 * newer one writes.
 *
 * Pure: no globals, no clock, no database. srv/test/rowtest.c holds the
 * malformed vectors.
 */
#ifndef ROWDEC_H
#define ROWDEC_H

/* What a row says. Only fields the server actually uses are exposed; the rest
 * are still REQUIRED to be well-formed, because a row with a corrupt field we
 * do not read is not a row we can vouch for.
 *
 * THE int FIELDS HOLD WHAT THE ROW SAID, or the row was refused. They are
 * narrower than the longs the decoder parses into, and a row whose field does
 * not fit an int is not a row with a different value in it -- see the narrow()
 * comment in rowdec.c for what the old bare casts stored instead. */
struct row_reading {
   long t;    /* epoch seconds, > 0 */
   long glu;  /* mg/dL, within the plausible band below */
   int trend; /* mg/dL per 10 min, as stored; any int */
   int kind;  /* ROW_KIND_*, or ROW_KIND_NONE when the row does not say */
   int tz;    /* the offset in force when it was taken, SECONDS east of UTC */
   int src;   /* the registry id of the device that produced it; any int */
};

/* The kinds a row may state. These are app/sensors.h's KIND_* values: they
 * are a FILE FORMAT, written into rows that are never rewritten, so they may
 * never be renumbered. `make crosscheck` pins them against the app's. */
/* A row that STOPS after the offset does not carry a kind at all. That is a
 * real shape -- the column was appended to a format that was already on
 * phones, and readings.csv is append-only, so rows written before it exist
 * for as long as the account does.
 *
 * It is reported rather than guessed, and each reader decides. The PLOT and
 * the table draw such a row: it is still a glucose, and refusing it would
 * blank a long-standing account's older months. The HEADLINE does not: the
 * number at the top of the page is the one a person reads before deciding
 * whether to eat or to inject, and a row that does not say what device made
 * it may not supply it. The old code made that decision once, in the
 * headline's favour and wrongly -- it took the absence of the field as proof
 * of CGM. */
#define ROW_KIND_NONE (-1)

#define ROW_KIND_CGM 0
#define ROW_KIND_BGM 1
#define ROW_KIND_INS 2
#define ROW_KIND_WT  3
#define ROW_KIND_MAX 3

/* The band a stored glucose may lie in. THE SAME NUMBERS as the app's
 * STORE_GLU_MIN / STORE_GLU_MAX (app/ingest.h), because they describe the
 * same rows: a band narrower than the writer's silently drops readings the
 * phone considers real, and a wider one admits what the phone already
 * refused. `make crosscheck` pins the two together. A value outside it is
 * not a reading -- it is a warm-up sentinel, a spliced row from a partial
 * write, or another log's number. */
#define ROW_GLU_MIN 15
#define ROW_GLU_MAX 750

/* Decode one row.
 *
 * 1 when the line is a COMPLETE, well-formed reading and `out` is filled; 0
 * otherwise, and `out` is left untouched. `len` bounds the line; a trailing
 * newline or carriage return is accepted and ignored.
 *
 * REFUSED: a missing field before the kind, an empty field other than rssi, a
 * numeric field with anything but digits (and a leading '-') in it, a field of
 * more than eighteen digits, a value that does not fit the field it is stored
 * in, a kind this build does not define, a timestamp of zero or less, a
 * glucose outside the band, and an offset that is not one.
 *
 * ACCEPTED with `kind` = ROW_KIND_NONE: a row that ends after the offset. It
 * is a well-formed reading that does not state its provenance; see above.
 *
 * warn_unused_result, as lib/hkdf.h and lib/pbkdf2.h carry it. This decoder
 * exists because four readers used to take a partial parse for an answer, and
 * `out` is UNTOUCHED on a refusal -- callers here scan thousands of rows
 * through one struct, so ignoring the status does not read zeroes, it reads
 * the PREVIOUS row and draws it twice. That is not a mistake to leave to
 * review. */
int row_decode(const char *line, int len, struct row_reading *out)
    __attribute__((warn_unused_result));

#endif
