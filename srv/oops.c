/* SPDX-License-Identifier: GPL-3.0
 * oops.c --- see oops.h
 * Copyright 2026 Jakob Kastelic
 */
#include "oops.h"
#include "http.h"
#include "proto.h"

void oops(struct req *r)
{
   http_text(r->c, 500, "Internal Server Error", "server error\n");
}

/* TRANSIENT, and said so: see oops.h. */
void oops_busy(struct req *r)
{
   http_respond_hdr(r->c, 503, "Service Unavailable", "text/plain",
                    "Retry-After: 5", "storage unavailable; try again\n",
                    sizeof "storage unavailable; try again\n" - 1);
}
