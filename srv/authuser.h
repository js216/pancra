/* SPDX-License-Identifier: GPL-3.0
 * authuser.h --- the account: its address, its password, its removal
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
#ifndef AUTHUSER_H
#define AUTHUSER_H

struct db; /* db.h: every storage call names its database */

#include "proto.h"  /* PW_*: the KDF parameters this server writes */
#include "pwhash.h" /* pw_hash: arithmetic over a password, not an account */
/* The account's id, or 0. `failed` (may be NULL) is set when the lookup did
 * not run, which is not the same as "no such account". */
int64_t user_by_email(struct db *d, const char *email, int *failed);

/* ---- WHAT HAPPENED WHEN AN ACCOUNT WAS ASKED FOR -----------
 *
 * user_create answered 1 or 0, and 0 covered five different things: an empty
 * password, a non-canonical address, a duplicate, a KDF that refused, and a
 * database that did not answer. The callers had to guess, and both guessed
 * the same way -- "already exists, or the password is empty" from the CLI, a
 * 400 "Need a real email address and a password" from the invitation page.
 * On a board with a full disk, the operator is told their password is wrong
 * and the person accepting an invitation is told their address is.
 *
 * A failure the caller cannot name is a failure they cannot act on, so the
 * three that mean different things are separated:
 *
 *   USER_CREATE_INVALID   the request cannot be granted as asked. The caller
 *                         may say so to whoever typed it.
 *   USER_CREATE_EXISTS    an account with that address is already there --
 *                         the ONLY case that means "already exists", and it
 *                         is confirmed by the constraint rather than inferred
 *                         from a failed insert.
 *   USER_CREATE_FAIL      the request was fine and the server could not carry
 *                         it out. Nothing was created; say so as a server
 *                         failure and do not blame the input. */
enum user_create_result {
   USER_CREATE_OK,
   USER_CREATE_INVALID,
   USER_CREATE_EXISTS,
   USER_CREATE_FAIL
};

/* Create an account. `uid` is filled only on USER_CREATE_OK. */
enum user_create_result user_create(struct db *d, const char *email,
                                    const char *pw, int64_t *uid);
/* Set the password AND sign every session out, in one transaction. 1 only
 * when both are committed; 0 leaves the account untouched.
 *
 * Revocation is not a separate step a caller can forget -- the browser path
 * forgot it and reported success, so a stolen cookie outlived the password it
 * was issued against. See the comment on the definition. */
int user_set_password(struct db *d, int64_t uid, const char *pw);
/* 1 = matches, 0 = does not, -1 = could not be checked (a storage failure,
 * which must not be reported to the user as a wrong password). */
int user_check_password(struct db *d, int64_t uid, const char *pw);

/* ---- DELETING AN ACCOUNT, FROM EITHER SURFACE --------------
 *
 * The web has a delete button behind a typed confirmation; the CLI had
 * nothing, so an operator whose user could not sign in -- a lost password on
 * a server with no reset by email, which is this server -- had to open the
 * database with sqlite3 and work out for themselves what a delete has to
 * touch. That is the operation most worth having in one place: every other
 * table references user(id) with ON DELETE CASCADE, so it is ONE statement,
 * and a hand-written one that misses the transaction leaves half an account.
 *
 *   USER_DELETED     the account and everything cascading from it are gone.
 *   USER_NO_SUCH     there was no account with that id. Nothing was written,
 *                    and this is NOT a failure -- it is the answer to
 *                    "delete something that is not there".
 *   USER_DELETE_FAIL the store could not do it. Nothing was written.
 *
 * THE THREE ARE SEPARATE for the same reason session_verify's are: telling an
 * operator "no such account" when the database was merely busy sends them to
 * check the address of an account that exists. */
enum user_delete { USER_DELETED = 0, USER_NO_SUCH, USER_DELETE_FAIL };

enum user_delete user_delete_account(struct db *d, int64_t uid);

#endif
