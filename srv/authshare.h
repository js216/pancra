/* SPDX-License-Identifier: GPL-3.0
 * authshare.h --- the invitation link, minted under a per-owner cap
 * Copyright 2026 Jakob Kastelic
 *
 * ONE CREDENTIAL PER HEADER. A header declaring five unrelated credentials
 * -- the account and its password, the browser session and its CSRF
 * derivative, the invitation link, the signed app request, and the login
 * throttle -- makes every page file inherit all
 * of them whether it touched one or not. They share no types, no tables and
 * no lifetimes; splitting them means a reader of any one of these files has
 * exactly one idea to hold, and a caller's includes say which credentials it
 * actually deals in.
 *
 * The declarations below are UNCHANGED, comments included: this is a move,
 * not a redesign, and the contracts they state were each paid for by an
 * incident.
 */
#ifndef AUTHSHARE_H
#define AUTHSHARE_H

struct db; /* db.h: every storage call names its database */

#include <stddef.h>
#include <stdint.h>

/* ---- MINTING AN INVITATION LINK: THREE ANSWERS, NOT TWO ---------------
 *
 * A share token is a credential, so it is minted here with the others, and
 * minting one also PRUNES the table and enforces the owner's quota -- in one
 * transaction, because a cap enforced against rows a half-applied prune left
 * behind is not a cap.
 *
 * "Could not create the link" and "you already have as many as you may" are
 * different sentences to the person pressing the button, and only one of them
 * has anything they can do about it:
 *
 *   SHARE_MINT_OK      the row is committed and `token` holds the hex.
 *   SHARE_MINT_FULL    the owner already holds MAX_LIVE_TOKENS unused links.
 *                      Nothing was minted and nothing was revoked; the way
 *                      forward is the Revoke button on the same page. Collapsed
 * into FAILED, it sends somebody to look at the server for a limit that is
 * working exactly as intended. SHARE_MINT_FAILED  storage. `token` is untouched
 * and no row exists.
 *
 * `owner` 0 means a plain signup link with no follow attached and no quota to
 * count against (see the definition). `email` may be NULL or "" for a link
 * addressed to nobody in particular. */
enum share_mint { SHARE_MINT_OK, SHARE_MINT_FULL, SHARE_MINT_FAILED };

enum share_mint share_token_mint(struct db *d, int64_t owner, const char *email,
                                 char *token, size_t cap);

#endif
