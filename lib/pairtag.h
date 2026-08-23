/* SPDX-License-Identifier: GPL-3.0
 * pairtag.h --- the key-confirmation tag, constructed in ONE place
 * Copyright 2026 Jakob Kastelic
 *
 * WHAT A CONFIRMATION TAG IS FOR.
 *
 * Rounds 3 and 4 of the pairing exchange prove that the two sides derived the
 * SAME key from a J-PAKE the network could not read. Each takes an
 * HMAC-SHA256 over a fixed label with the derived key and sends the first 32
 * hex characters of it; the other side recomputes and compares. The labels
 * differ per direction so a tag cannot be replayed back at its sender.
 *
 * Both halves of the protocol had their own copy of that
 * construction and their own copy of the two labels -- app/sync.h's
 * SYNC_CONFIRM_LABEL_*, srv/proto.h's CONFIRM_LABEL_*, app/syncpair.c's
 * sync_confirm_tag and srv/pair.c's confirm_mac. FOUR places, and a protocol
 * change had to land identically in all of them: a truncation to 30
 * characters on one side, or a label with a trailing space, is a pairing that
 * fails with nothing to say why, on a screen that offers to try again.
 *
 * The DELIBERATE duplication in this codebase is the two independent
 * implementations of the sync protocol itself (see lib/wirevec.h), and it is
 * kept because each is written against what the wire says. The same twelve
 * lines twice is not that: neither copy is a check on the other, and they are
 * compared only by a pairing that either works or does not.
 *
 * The GOLDEN VECTORS STAY INDEPENDENT (lib/wirevec.h, checked by
 * test/srv/wiretest.c and test/app/interoptest.c): they pin the BYTES, and
 * they are what says this one implementation is right rather than merely
 * consistent with itself.
 *
 * FREESTANDING. The app half is built with no libc, so this takes its own
 * length and does its own hex -- neither is worth an #ifdef.
 */
#ifndef PANCRA_PAIRTAG_H
#define PANCRA_PAIRTAG_H

#include <stddef.h>
#include <stdint.h>

/* THE TWO LABELS, once. Per DIRECTION: the server proves itself with the
 * first and the client with the second, so a tag captured from one side
 * cannot be replayed as the other's. */
#define PAIR_TAG_LABEL_SERVER "pancra-confirm-server"
#define PAIR_TAG_LABEL_CLIENT "pancra-confirm-client"

/* Hex characters in a tag: HALF the digest, and all of the guess. 32 of them
 * is 128 bits, which is a great deal more than a network attacker gets to try
 * against a code that expires in minutes. */
#define PAIR_TAG_HEX 32

/* The tag for `label` under `key`, NUL-terminated, into `out`.
 *
 * CHECKED, and this is the point of it being a function rather than a macro:
 * 1 when `out` holds a full tag, 0 when it does not, and on 0 `out` is set to
 * the empty string rather than left holding whatever the stack had. A caller
 * that ignores the answer therefore compares against "", which no tag equals
 * -- the failure is a refusal to pair, never an accidental match.
 *
 * `outcap` must be at least PAIR_TAG_HEX + 1. `keylen` is the derived key's
 * length (16 for this protocol); a zero key length is refused, since an HMAC
 * with no key is a hash anybody can compute. */
int pair_tag(const uint8_t *key, size_t keylen, const char *label, char *out,
             size_t outcap);

#endif
