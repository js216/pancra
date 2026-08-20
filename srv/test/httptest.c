// SPDX-License-Identifier: GPL-3.0
// httptest.c --- the transport contract: all the bytes, or an error
// Copyright 2026 Jakob Kastelic
//
/* THE ONE PROMISE http.h MAKES ABOUT WRITING, checked against a socket that
 * genuinely refuses to take everything at once.
 *
 * `plain_write` was a single write(2). On a blocking socket that usually
 * moves the whole buffer, which is exactly why the defect survived: it needs
 * BACKPRESSURE to show itself. Every accepted connection carries a one-second
 * SO_SNDTIMEO, so a client that stops reading makes write() return the bytes
 * it managed -- and the caller treated that as success, leaving the client a
 * body that stops in the middle under a Content-Length promising the rest.
 *
 * This test builds that state deliberately: a socket pair with a small send
 * buffer and a reader that drains slowly (or not at all).
 *
 * AND IT SETS NO SOCKET TIMEOUT. The write loop used to depend on the
 * one-second SO_SNDTIMEO that accepted connections carry: without it, write(2)
 * blocked in the kernel and the deadline it checks was never reached. A test
 * that sets the option itself proves the option works, not the code -- and it
 * hangs the moment the code stops setting it elsewhere. The case below leaves
 * the socket exactly as socketpair() made it, so the only thing that can end
 * the write is the deadline inside plain_write.
 *
 * THE WATCHDOG IS PART OF THE TEST. A regression here does not fail, it
 * hangs; alarm() turns that into a failure with a name, in bounded time,
 * instead of a suite that has to be killed from outside.
 */
#include "http.h"
#include "util.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

static int fails;

/* WHAT A HANG LOOKS LIKE FROM IN HERE. plain_write promises to give up after
 * HTTP_DEADLINE_S; anything that blocks for materially longer is the defect,
 * so it is caught and named rather than left to a `timeout` outside. */
static void watchdog(int sig)
{
   (void)sig;
   static const char msg[] =
       "  [FAIL] a write BLOCKED past its deadline (the loop is waiting on a\n"
       "         socket option instead of bounding the wait itself)\n"
       "HTTP TESTS FAILED\n";
   ssize_t ignored = write(2, msg, sizeof msg - 1);
   (void)ignored;
   _exit(1);
}

static void guard(unsigned seconds)
{
   signal(SIGALRM, watchdog);
   alarm(seconds);
}

static void sb_capacity_cases(void);
static void sb_builder_cases(void);
static void reqline_cases(void);
static void form_field_cases(void);
static void form_body_cases(void);

static void ck(int cond, const char *what)
{
   printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
   if (!cond)
      fails++;
}

/* The bytes to push: comfortably more than any socket buffer, so the write
 * cannot complete in one call however generous the kernel is feeling. */
#define BIG (2 * 1024 * 1024)
static unsigned char big[BIG];

struct drain {
   int fd;
   long got;
   int slow;
};

static void *drainer(void *p)
{
   struct drain *d = p;
   unsigned char buf[4096];
   for (;;) {
      ssize_t n = read(d->fd, buf, sizeof buf);
      if (n <= 0)
         break;
      d->got += n;
      if (d->slow) {
         struct timespec ts = {0, 1000000}; /* 1 ms per 4 kB */
         nanosleep(&ts, NULL);
      }
   }
   return NULL;
}

/* ---- THE STRING BUILDER'S CAPACITY ARITHMETIC (TODO item 103) ----------
 *
 * WHAT IT USED TO DO, MEASURED AGAINST THE CODE AS IT STOOD:
 *
 *   sb_room asked `if (s->n + need + 1 <= s->cap) return 1;`. With a fresh
 *   builder (n = 0, cap = 0) and need = SIZE_MAX, that sum is ZERO, zero is
 *   <= zero, and the function reported room without allocating anything --
 *   after which sb_raw ran memcpy(NULL, data, SIZE_MAX). Segmentation fault,
 *   with `err` still clear, so the one flag every page in this server checks
 *   said the page was fine. The same thing happens for any need in
 *   [SIZE_MAX - n - cap, SIZE_MAX]: with n = 10 and cap = 8192 the sum came to
 *   zero again, sb_room returned 1 with the capacity untouched, and the memcpy
 *   that followed wrote past an 8 KB heap block. That is a heap overflow in
 *   the request path, and sb_* builds every HTML page this server serves.
 *
 *   Then it grew with `while (cap < s->n + need + 1) cap *= 2;`. `cap` is a
 *   size_t holding a power of two: 4096, 8192, ... 2^63, and 2^63 doubled is
 *   0. Zero doubled is zero, so for any target above 2^63 that loop NEVER
 *   LEAVES. Measured: need = 2^63 spun until it was killed, and so did
 *   need = SIZE_MAX - 4000 on a builder that already had 4096 bytes. Not a
 *   wrong page -- a worker thread that never returns, holding the page mutex,
 *   with every other user's page queued behind it.
 *
 * TWO KINDS OF CASE, BECAUSE THE BOUNDARY WILL NOT FIT IN A MACHINE.
 *
 *   * PINNED BY THE PREDICATE. The interesting sizes are SIZE_MAX-adjacent
 *     and 2^63-adjacent; no test can allocate them to drive sb_raw there. So
 *     the rule is sb_cap_for, a pure function of four numbers, and the exact
 *     boundary and one either side of it are pinned by calling it. (lib/gcm.c
 *     splits aes128_gcm_limits out for exactly this reason.)
 *
 *   * PINNED BY DRIVING THE REAL BUILDER. sb_raw itself is called with
 *     SIZE_MAX and with 2^63 -- neither allocates, because neither gets that
 *     far -- and asserted to return, to set `err`, and to leave the buffer it
 *     was given exactly as it found it.
 *
 * AND THE REFUSALS ARE CHECKED AGAINST A SENTINEL. A builder is filled with a
 * known byte before each refusal and asserted unchanged after it. The return
 * value alone cannot tell a refusal from a partial append.
 *
 * THE WATCHDOG IS PART OF THIS TEST TOO. The doubling defect does not fail,
 * it hangs; alarm() turns that into a named failure in bounded time. */

/* A REQUEST FOR 2^63 BYTES MUST BE ALLOWED TO FAIL, NOT TO ABORT.
 *
 * One of the cases below hands sb_raw 2^63 bytes: the point is that the
 * builder RETURNS to find out it cannot have them, where the old doubling loop
 * spun forever instead. Getting there means reaching realloc with a size no
 * allocator will grant, and under a sanitizer that is not a NULL return -- it
 * is `allocation-size-too-big`, which aborts the process before the assertion
 * can be made. These are the documented hooks for saying "hand back NULL and
 * let the program cope", which is precisely the behaviour under test. They are
 * ordinary functions in a build without a sanitizer, called by nobody.
 *
 * srvtsan builds this file with ThreadSanitizer; nothing builds it with
 * AddressSanitizer today, but the case is written so that it could. */
const char *__asan_default_options(void);

const char *__asan_default_options(void)
{
   return "allocator_may_return_null=1";
}

const char *__tsan_default_options(void);

const char *__tsan_default_options(void)
{
   return "allocator_may_return_null=1";
}

static void sb_watchdog(int sig)
{
   (void)sig;
   static const char msg[] =
       "  [FAIL] sb_cap_for DID NOT RETURN (the doubling loop is spinning:\n"
       "         cap reached 2^63, doubled to 0, and 0 doubled is 0 forever)\n"
       "SOME HTTP TESTS FAILED\n";
   ssize_t ignored = write(2, msg, sizeof msg - 1);
   (void)ignored;
   _exit(1);
}

/* out == want, and the call said yes. */
static int capis(size_t n, size_t cap, size_t need, size_t want)
{
   size_t got = (size_t)-1;
   if (!sb_cap_for(n, cap, need, &got))
      return 0;
   return got == want;
}

/* The call said no, AND zeroed the answer -- a caller that drops the return
 * value must not be left holding a capacity it can pass to realloc. */
static int capno(size_t n, size_t cap, size_t need)
{
   size_t got = 0xDEADBEEF;
   int r      = sb_cap_for(n, cap, need, &got);
   return r == 0 && got == 0;
}

#define SB_2_63 ((size_t)1 << 63)

static void sb_capacity_cases(void)
{
   printf("== the capacity a page buffer would need (the predicate) ==\n");

   /* THE WATCHDOG COVERS THE WHOLE SECTION, and that is not where it started.
    * It was armed just before the three cases explicitly about the doubling
    * boundary -- but `need = SIZE_MAX - 1` asks for a capacity of SIZE_MAX,
    * which is also above 2^63 and also spins, and it is EARLIER in the list.
    * Mutating the guard away therefore hung the suite before the alarm was
    * ever set, and the run had to be killed from outside. Any request above
    * 2^63 reaches that loop, so the guard belongs around all of them. */
   signal(SIGALRM, sb_watchdog);
   alarm(20);

   /* ORDINARY GROWTH, unchanged, so the refusals below are not satisfied by a
    * function that refuses everything. */
   ck(capis(0, 0, 10, 4096), "a fresh builder starts at one 4096-byte page");
   ck(capis(0, 4096, 4095, 4096),
      "4095 more bytes fit in 4096 exactly -- the NUL is the 4096th");
   ck(capis(0, 4096, 4096, 8192),
      "...and 4096 more is one past that, so the page doubles");
   ck(capis(4095, 4096, 0, 4096),
      "a full-but-for-the-NUL buffer needs no more");
   ck(capis(4096, 4096, 0, 8192),
      "...and one byte fuller does, for the NUL alone");
   ck(capis(0, 4096, 1u << 20, 1u << 21),
      "a megabyte at once doubles straight past it rather than one page at a "
      "time");

   /* THE SIZE_MAX BOUNDARY. n + need + 1 must be a representable size_t;
    * these are the three sizes either side of that being true. */
   ck(capno(0, 0, SIZE_MAX),
      "need = SIZE_MAX is refused: n+need+1 wrapped to 0, which is <= any "
      "capacity, and THAT is what approved the memcpy that segfaulted");
   ck(capis(0, 0, SIZE_MAX - 1, SIZE_MAX),
      "...while need = SIZE_MAX-1 is the largest that IS representable, and "
      "asks for exactly SIZE_MAX");
   ck(capis(0, 0, SIZE_MAX - 2, SIZE_MAX - 1), "...and one below that, too");

   /* THE SAME BOUNDARY WITH BYTES ALREADY IN THE BUFFER -- the isolating
    * form, because it is the second addition rather than the first that
    * wraps. n = 10, cap = 8192 is the case that was MEASURED returning 1 with
    * the capacity left at 8192. */
   ck(capno(10, 8192, SIZE_MAX - 10),
      "10 bytes in and need = SIZE_MAX-10 is refused (this exact call used to "
      "return 'there is room' with the buffer still 8192 bytes)");
   ck(capis(10, 8192, SIZE_MAX - 11, SIZE_MAX),
      "...while one byte less is representable, and asks for SIZE_MAX");
   ck(capno(1, 0, SIZE_MAX - 1),
      "one byte in and need = SIZE_MAX-1 is refused: the sum is SIZE_MAX+1");
   ck(capno(SIZE_MAX, 0, 1), "a builder already at SIZE_MAX can take nothing");
   ck(capno(SIZE_MAX, 0, SIZE_MAX), "...least of all SIZE_MAX more");

   /* THE DOUBLING BOUNDARY. Above 2^63 no power of two exists to reach, so
    * the loop has to stop and take the exact size instead. With the old code
    * these three calls never returned at all -- the watchdog above is what
    * turns that into a failure with a name. */
   ck(capis(0, 0, SB_2_63 - 1, SB_2_63),
      "a target of exactly 2^63 is still reached by doubling");
   ck(capis(0, 0, SB_2_63, SB_2_63 + 1),
      "one byte past it takes the EXACT size instead -- the next doubling "
      "would be 0, and 0 doubled is 0 forever");
   ck(capis(0, 0, SIZE_MAX - 1, SIZE_MAX),
      "...and so does the largest request there is");
   ck(capis(0, SB_2_63, SB_2_63, SB_2_63 + 1),
      "a buffer ALREADY at 2^63 grows to the exact size, not to zero");

   /* A capacity that is already enough is reported as itself, so sb_room can
    * compare and skip the realloc. */
   ck(capis(0, SIZE_MAX, 100, SIZE_MAX),
      "a buffer that is already big enough needs no growth");

   /* NO OUTPUT PARAMETER IS NOT A QUESTION. */
   ck(sb_cap_for(0, 0, 10, NULL) == 0,
      "asking without somewhere to put the answer is refused");
   alarm(0);
}

/* ---- AND THE SAME LIMITS THROUGH THE REAL BUILDER ---------------------- */

/* A builder with a known allocation and a known byte in every slot of it, so
 * a refusal that touched anything is visible. */
#define SB_SENT 0x5A

/* Sentinel the space AFTER the terminator, so the buffer stays a valid C
 * string: the first thing these cases assert is that the text already in it
 * survived, and a sentinel written over the NUL would break that with the
 * fixture rather than with the code. (It did, the first time this ran.) */
static void sb_sentinel(struct sb *s)
{
   if (s->cap > s->n + 1)
      memset(s->p + s->n + 1, SB_SENT, s->cap - s->n - 1);
}

static int sb_untouched(const struct sb *s, char *p0, size_t n0, size_t cap0)
{
   if (s->p != p0 || s->n != n0 || s->cap != cap0)
      return 0;
   if (s->p[s->n] != '\0')
      return 0;
   for (size_t i = s->n + 1; i < s->cap; i++)
      if (s->p[i] != (char)SB_SENT)
         return 0;
   return 1;
}

static void sb_builder_cases(void)
{
   printf("== the same limits, driven through the real builder ==\n");

   /* IT STILL BUILDS PAGES. Everything below asserts a refusal; without this
    * they would all pass against a builder that refused everything. */
   {
      struct sb s = {0};
      ck(sb_add(&s, "<b>%s</b> %d", "hello", 42) == 1, "an append succeeds");
      ck(sb_raw(&s, "!!", 2) == 1, "...and so does a raw one");
      ck(!s.err && s.n == 17 && !strcmp(s.p, "<b>hello</b> 42!!"),
         "...and the buffer holds exactly what was appended, NUL-terminated");
      /* The path that formats straight into the buffer: longer than sb_add's
       * 1024-byte stack scratch, so it goes round again through sb_room. */
      char big3[4000];
      memset(big3, 'x', sizeof big3 - 1);
      big3[sizeof big3 - 1] = '\0';
      ck(sb_add(&s, "%s", big3) == 1,
         "an append longer than the stack scratch succeeds");
      ck(!s.err && s.n == 17 + 3999 && s.p[s.n] == '\0',
         "...and lands in the buffer at the right length");
      sb_free(&s);
      ck(s.p == NULL && s.n == 0 && s.cap == 0 && s.err == 0,
         "freeing leaves an empty builder, reusable");
   }

   /* THE WRAP, THROUGH sb_raw, WITH A SENTINEL. This exact call -- a builder
    * holding bytes, asked for SIZE_MAX more -- used to return 1 and then
    * memcpy SIZE_MAX bytes into it. */
   {
      struct sb s = {0};
      ck(sb_raw(&s, "0123456789", 10) == 1, "a builder with ten bytes in it");
      sb_sentinel(&s); /* every free byte after the terminator */
      char *p0    = s.p;
      size_t n0   = s.n;
      size_t cap0 = s.cap;
      ck(sb_raw(&s, "x", SIZE_MAX) == 0,
         "...refuses SIZE_MAX more (it used to segfault in memcpy)");
      ck(s.err == 1, "...and RECORDS it, which is the only thing pages check");
      ck(sb_untouched(&s, p0, n0, cap0),
         "...and did not move, resize, or write a single byte of the buffer");
      ck(!strcmp(s.p, "0123456789"),
         "...so what was already in it is still exactly what was in it");
      sb_free(&s);
   }

   /* THE DOUBLING LOOP, THROUGH sb_raw. 2^63 bytes is a request no allocator
    * will grant, but the point is that the function RETURNS to find that out.
    * With the old code it never reached realloc at all. */
   {
      struct sb s = {0};
      ck(sb_raw(&s, "0123456789", 10) == 1, "a builder with ten bytes in it");
      sb_sentinel(&s);
      char *p0    = s.p;
      size_t n0   = s.n;
      size_t cap0 = s.cap;
      signal(SIGALRM, sb_watchdog);
      alarm(10);
      int r = sb_raw(&s, "x", SB_2_63);
      alarm(0);
      ck(r == 0, "...RETURNS from a request for 2^63 bytes, refusing it");
      ck(s.err == 1, "...with the failure recorded");
      ck(sb_untouched(&s, p0, n0, cap0), "...and the buffer untouched");
      sb_free(&s);
   }

   /* THE STICKINESS, which is the contract every caller relies on: one
    * failure and every later append is a no-op, checked once at the end. */
   {
      struct sb s = {0};
      ck(sb_raw(&s, "abc", 3) == 1, "three bytes go in");
      ck(sb_raw(&s, "x", SIZE_MAX) == 0, "...an impossible append is refused");
      ck(sb_add(&s, "more") == 0, "...and every append after it is a no-op");
      ck(sb_raw(&s, "more", 4) == 0, "...raw ones too");
      ck(s.err == 1 && s.n == 3 && !strcmp(s.p, "abc"),
         "...with the buffer frozen at what it held when the failure "
         "happened");
      sb_free(&s);
   }

   /* NULL ARGUMENTS, IN EVERY COMBINATION THAT HAS A MEANING. */
   {
      printf("  -- null arguments --\n");
      ck(sb_raw(NULL, "x", 1) == 0, "sb_raw with no builder is refused");
      ck(sb_add(NULL, "x") == 0, "sb_add with no builder is refused");
      sb_free(NULL); /* must simply return */
      ck(1, "sb_free with no builder returns rather than freeing NULL->p");

      struct sb s = {0};
      ck(sb_raw(&s, NULL, 5) == 0,
         "a NULL source with a length is refused, not memcpy'd from");
      ck(s.err == 1, "...and recorded, so the page becomes a 500");
      sb_free(&s);

      struct sb t = {0};
      ck(sb_raw(&t, NULL, 0) == 1,
         "a NULL source of length zero appends nothing and is fine");
      ck(t.err == 0 && t.n == 0, "...leaving an empty, usable builder");
      ck(sb_add(&t, NULL) == 0, "a NULL format string is refused");
      ck(t.err == 1, "...and recorded");
      sb_free(&t);
   }

   /* A FORMAT vsnprintf CANNOT RENDER. It returned 0 and left `err` CLEAR, so
    * the text this call was supposed to contribute was simply missing from a
    * page that reported itself fine. In the "C" locale a non-ASCII wide
    * character has no multibyte form, so %ls fails with EILSEQ -- and if some
    * platform renders it anyway, this says so rather than failing. */
   {
      struct sb s                = {0};
      static const wchar_t bad[] = {0x00e9, 0};
      char probe[8];
      int n = snprintf(probe, sizeof probe, "%ls", bad);
      if (n < 0) {
         ck(sb_add(&s, "%ls", bad) == 0, "a format vsnprintf rejects fails");
         ck(s.err == 1,
            "...and RECORDS it -- it used to return 0 with err clear, so the "
            "page went out 200 with the text missing");
      } else {
         printf("  [SKIP] this platform renders %%ls in the C locale (%d "
                "bytes), so the vsnprintf-failure path is not exercised "
                "here\n",
                n);
      }
      sb_free(&s);
   }
}

/* ---- ONE EXACT REQUEST-LINE GRAMMAR (item 119) -------------------------
 *
 * The grammar is RFC 9112 3 and the reasoning is in http.h. What is asserted
 * here is the REFUSALS: a suite that only checks "GET / HTTP/1.1 still works"
 * proves nothing at all, because the old two-space split accepted that too.
 * Each case below is a shape the old split ACCEPTED, and each is named for
 * what a proxy in front of this server would have made of it. */
static void reqline_cases(void)
{
   printf("== the request line: one grammar, and every other shape refused "
          "==\n");
   char m[HTTP_METHOD_MAX], t[HTTP_TARGET_MAX];

   /* The control, and it is only a control: it exists so that a mutant which
    * refuses EVERYTHING is not mistaken for a working rule. */
   ck(http_reqline("GET /a?b=c HTTP/1.1\r\nHost: x\r\n\r\n", m, sizeof m, t,
                   sizeof t) == REQL_OK,
      "a well-formed line parses (the control, which proves nothing alone)");
   ck(!strcmp(m, "GET"), "...and the method is the method");
   ck(!strcmp(t, "/a?b=c"), "...and the target is the target, query included");

   /* AN EXTRA TOKEN. Everything after the second space used to be skipped to
    * the next newline, so this was served exactly like the control. A front
    * end that rejects it and an origin that serves it disagree about whether
    * a request happened at all. */
   ck(http_reqline("GET / HTTP/1.1 extra\r\n", m, sizeof m, t, sizeof t) ==
          REQL_BAD,
      "a fourth token on the request line is refused");
   ck(!m[0] && !t[0], "...and nothing is handed back to route on");

   /* AN UNSUPPORTED VERSION, well formed. 505, not 400: the message is fine,
    * the version is declined -- and told apart so a mutant that deletes the
    * version check cannot be masked by the grammar check. */
   ck(http_reqline("GET / HTTP/1.2\r\n", m, sizeof m, t, sizeof t) ==
          REQL_VERSION,
      "HTTP/1.2 is refused as a version, not accepted as 1.1");
   ck(http_reqline("GET / HTTP/9.9\r\n", m, sizeof m, t, sizeof t) ==
          REQL_VERSION,
      "...and so is a version that does not exist");
   ck(http_reqline("GET / HTTP/1.0\r\n", m, sizeof m, t, sizeof t) ==
          REQL_VERSION,
      "...and HTTP/1.0, whose connection defaults are the opposite of 1.1's");
   ck(http_reqline("GET / http/1.1\r\n", m, sizeof m, t, sizeof t) == REQL_BAD,
      "a lower-case version is not a version (RFC 9112 2.3 is literal)");
   ck(http_reqline("GET / HTTP/11\r\n", m, sizeof m, t, sizeof t) == REQL_BAD,
      "...and neither is one with no dot");

   /* NO VERSION AT ALL, which is the worst of them: the second space was
    * found INSIDE THE HOST HEADER, so the target became "/\r\nHost:" and the
    * request was routed on it. Two parsers reading one byte stream as
    * different numbers of requests is the whole of request smuggling. */
   ck(http_reqline("GET /\r\nHost: evil\r\n\r\n", m, sizeof m, t, sizeof t) ==
          REQL_BAD,
      "a request line with no version is refused");
   ck(!strchr(t, '\r') && !strchr(t, '\n'),
      "...and no CR or LF can reach the target that gets routed");

   /* A BARE LF. RFC 9112 2.2 PERMITS a recipient to accept one; this server
    * declines the permission, because taking it is what lets the front end
    * and the origin disagree about where the line ended. */
   ck(http_reqline("GET / HTTP/1.1\nHost: x\r\n\r\n", m, sizeof m, t,
                   sizeof t) == REQL_BAD,
      "a request line terminated by a bare LF is refused");

   /* WHITESPACE INSIDE THE THREE COMPONENTS, which RFC 9112 3 says a
    * recipient MUST reject. The double space used to produce an EMPTY
    * target. */
   ck(http_reqline("GET  / HTTP/1.1\r\n", m, sizeof m, t, sizeof t) == REQL_BAD,
      "two spaces between method and target are refused, not read as an "
      "empty target");
   ck(http_reqline("GET / HTTP/1.1 \r\n", m, sizeof m, t, sizeof t) == REQL_BAD,
      "a trailing space after the version is refused");
   ck(http_reqline("GET\t/ HTTP/1.1\r\n", m, sizeof m, t, sizeof t) == REQL_BAD,
      "a tab is not the separator");
   ck(http_reqline("GET /a b HTTP/1.1\r\n", m, sizeof m, t, sizeof t) ==
          REQL_BAD,
      "a space inside the target is refused rather than split on");
   ck(http_reqline(" GET / HTTP/1.1\r\n", m, sizeof m, t, sizeof t) == REQL_BAD,
      "a leading space is refused (RFC 9112 3 forbids it before the method)");
   ck(http_reqline("GET /a\177b HTTP/1.1\r\n", m, sizeof m, t, sizeof t) ==
          REQL_BAD,
      "a DEL inside the target is refused");
   ck(http_reqline("GE\001T / HTTP/1.1\r\n", m, sizeof m, t, sizeof t) ==
          REQL_BAD,
      "a control character in the method is refused: a method is a token");
   ck(http_reqline("", m, sizeof m, t, sizeof t) == REQL_BAD,
      "an empty request is refused");
   ck(http_reqline("GET  HTTP/1.1\r\n", m, sizeof m, t, sizeof t) == REQL_BAD,
      "an empty target is refused: request-target is 1*, not 0*");

   /* CAPACITY, and it comes AFTER the shape: an over-long target with a bad
    * version is a bad version, so neither answer can hide the other. */
   {
      char big[900];
      memset(big, 'x', sizeof big);
      big[0] = '/';
      char line[1024];
      (void)snprintf(line, sizeof line, "GET %.*s HTTP/1.1\r\n",
                     (int)sizeof big - 1, big);
      ck(http_reqline(line, m, sizeof m, t, sizeof t) == REQL_TARGET_LONG,
         "a target longer than the router can hold is refused, not truncated");
      (void)snprintf(line, sizeof line, "GET %.*s HTTP/1.2\r\n",
                     (int)sizeof big - 1, big);
      ck(http_reqline(line, m, sizeof m, t, sizeof t) == REQL_VERSION,
         "...and an over-long target with a bad VERSION is a bad version");
   }
   ck(http_reqline("VERYLONGMETHOD / HTTP/1.1\r\n", m, sizeof m, t, sizeof t) ==
          REQL_METHOD_LONG,
      "a method too long to implement is 501, not a truncated method name");
   /* The exact boundary, both sides, because an off-by-one here would let a
    * method through that http_method_bit then cannot name. */
   ck(http_reqline("ABCDEFG / HTTP/1.1\r\n", m, sizeof m, t, sizeof t) ==
          REQL_OK,
      "seven characters of method fit HTTP_METHOD_MAX exactly");
   ck(http_reqline("ABCDEFGH / HTTP/1.1\r\n", m, sizeof m, t, sizeof t) ==
          REQL_METHOD_LONG,
      "...and eight do not");
}

/* ---- FORM FIELDS: FIVE ANSWERS WHERE THERE WAS ONE BIT (item 120) ------
 *
 * Each case is one of the four behaviours util.h names, and each is written
 * so that only the rule under test can produce the answer asserted. */
static void form_field_cases(void)
{
   printf("== a form field is present, or it is one of four kinds of no ==\n");
   char v[16];

#define FF(body, name) form_field((body), sizeof(body) - 1, (name), v, sizeof v)

   ck(FF("tz=-480", "tz") == FORM_OK, "an ordinary field is FORM_OK");
   ck(!strcmp(v, "-480"), "...with the value it carried");
   ck(FF("a=1&tz=-480&b=2", "tz") == FORM_OK, "...wherever it sits");
   ck(FF("tz=", "tz") == FORM_OK, "an EMPTY value is present, not absent");
   ck(!v[0], "...and reads as the empty string");
   ck(FF("a=1&b=2", "tz") == FORM_ABSENT, "a field that is not there is 0");
   ck(!v[0], "...and leaves the buffer empty");
   ck(FF("mytz=-480", "tz") == FORM_ABSENT,
      "a name this one is a SUFFIX of is not this name");
   ck(FF("tzz=-480", "tz") == FORM_ABSENT, "nor one it is a prefix of");
   ck(FF("a=tz=9", "tz") == FORM_ABSENT,
      "nor a name-shaped string inside somebody else's VALUE");
   ck(FF("tz=%2D%34%38%30", "tz") == FORM_OK, "escapes decode");
   ck(!strcmp(v, "-480"), "...to the bytes they name");
   ck(FF("tz=a+b", "tz") == FORM_OK, "'+' is still a space (it is the "
                                     "encoding, not a leniency)");
   ck(!strcmp(v, "a b"), "...decoded as one");

   /* AN EMBEDDED NUL. The authentication-bypass shape: every caller reads the
    * result as a C string, so "%00" made one value on the wire read as a
    * shorter, different value here. csrf_ok is a strcmp. */
   ck(FF("csrf=abcd%00junk", "csrf") == FORM_MALFORMED,
      "a decoded NUL is MALFORMED, not a silent early terminator");
   ck(!v[0], "...and the caller is handed nothing to compare");
   ck(FF("csrf=%00", "csrf") == FORM_MALFORMED, "a value that is only a NUL "
                                                "is malformed too");

   /* AN INVALID ESCAPE. It used to survive as literal text, so the value this
    * server acted on was not the value a normalising front end saw. */
   ck(FF("tz=%zz", "tz") == FORM_MALFORMED, "'%zz' is malformed, not text");
   ck(FF("tz=%4", "tz") == FORM_MALFORMED,
      "an escape cut off by the end of the value is malformed");
   ck(FF("tz=%4&x=1", "tz") == FORM_MALFORMED,
      "...and it may not borrow the next field's bytes to complete itself");
   ck(FF("tz=%2D%3", "tz") == FORM_MALFORMED, "a trailing partial escape too");
   ck(FF("tz=100%", "tz") == FORM_MALFORMED, "a bare trailing '%' is not a "
                                             "percent sign");

   /* TOO LONG. Measured on the DECODED length and refused, where it used to
    * be clipped -- silently turning one value into another. */
   ck(FF("tz=0123456789abcdef0", "tz") == FORM_TOO_LONG,
      "a value too long for the caller's buffer is refused, not clipped");
   ck(!v[0], "...and the buffer holds no prefix of it");
   ck(FF("tz=%30%31%32%33%34%35%36%37%38%39%61%62%63%64%65%66%67", "tz") ==
          FORM_TOO_LONG,
      "...measured after decoding, so escapes cannot smuggle length past it");
   ck(FF("tz=012345678901234", "tz") == FORM_OK,
      "fifteen bytes fit a sixteen-byte buffer exactly");
   ck(FF("tz=0123456789012345", "tz") == FORM_TOO_LONG, "...and sixteen "
                                                        "do not");

   /* DUPLICATES. First-wins was the old behaviour, established by execution
    * in item 55 ("tz=-480&tz=5abc" stored -480). It is deliberately REVERSED
    * here -- not to last-wins, which is the same guess made differently, but
    * to a refusal, because neither end can know which one anything else
    * reading the same body chose. */
   ck(FF("tz=-480&tz=5abc", "tz") == FORM_DUPLICATE,
      "a repeated field is refused (this REPLACES the old first-wins)");
   ck(!v[0], "...and neither of the two values is handed back");
   ck(FF("tz=-480&tz=-480", "tz") == FORM_DUPLICATE,
      "even when the two values are identical: the AMBIGUITY is the defect");
   ck(FF("a=1&csrf=x&b=2&csrf=y", "csrf") == FORM_DUPLICATE,
      "...and wherever in the body the second one sits");

#undef FF
}

/* ---- AND THE SAME QUESTION ABOUT THE WHOLE BODY, BEFORE ANY NAME -------
 *
 * form_field answers about one name, so a handler has to ASK before a bad
 * field is noticed -- and by then the session has been read and the route
 * chosen. This is the pass that runs first; see web_route_locked. */
static void form_body_cases(void)
{
   printf("== the whole form, judged before anyone is authenticated ==\n");
#define FB(body) form_body_check((body), sizeof(body) - 1)
   ck(form_body_check(NULL, 0) == FORM_BODY_OK, "no body is not a bad body");
   ck(FB("") == FORM_BODY_OK, "and neither is an empty one");
   ck(FB("csrf=abc&tz=-480") == FORM_BODY_OK, "an ordinary form passes");
   ck(FB("csrf=a%2Db&tz=%2D480") == FORM_BODY_OK, "escapes and all");
   ck(FB("csrf=abc%00x&tz=-480") == FORM_BODY_MALFORMED,
      "a NUL anywhere in the body is refused");
   ck(FB("csrf=abc&tz=%zz") == FORM_BODY_MALFORMED,
      "an invalid escape anywhere in the body is refused");
   ck(FB("cs%zzrf=abc") == FORM_BODY_MALFORMED,
      "...including one in a NAME, which form_field would never look at");
   ck(FB("csrf=good&csrf=bad") == FORM_BODY_DUPLICATE,
      "a repeated name is refused before anything asks for it");
   ck(FB("csrf=good&tz=1&csrf=bad") == FORM_BODY_DUPLICATE,
      "...with other fields in between");
   ck(FB("csrf=good&csrfx=bad") == FORM_BODY_OK,
      "a name that merely starts the same is a different name");
   ck(FB("a&a") == FORM_BODY_DUPLICATE,
      "a repeated valueless field is still a repeat");
   ck(FB("csrf=good&CSRF=bad") == FORM_BODY_OK,
      "case matters: field names are bytes, and only 'csrf' is read");
#undef FB
}

int main(void)
{
   for (int i = 0; i < BIG; i++)
      big[i] = (unsigned char)i;

   printf("== a slow reader gets every byte, not a short write ==\n");
   {
      int sv[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
         printf("  [FAIL] socketpair\n");
         return 1;
      }
      /* The conditions a real connection is served under. */
      struct timeval tv = {1, 0};
      setsockopt(sv[0], SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
      int small = 4096;
      setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
      setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);

      struct drain d = {sv[1], 0, 1};
      pthread_t th;
      pthread_create(&th, NULL, drainer, &d);

      struct http_conn c = {.fd = sv[0], .tp = &http_transport_plain};
      ssize_t w          = http_transport_plain.write(&c, big, BIG);
      ck(w == (ssize_t)BIG, "the write reports every byte it was given");
      close(sv[0]);
      pthread_join(th, NULL);
      ck(d.got == BIG, "...and the reader received exactly that many");
      close(sv[1]);
   }

   /* THE DISCRIMINATING CASE. The one above passes even with a single
    * write(2) on this platform -- a blocking AF_UNIX write waits for the
    * whole buffer once someone is draining -- so it documents the contract
    * rather than testing it. A peer that never drains is what forces the
    * timeout, and with the old code that returned a positive short count
    * reported as success. */
   printf("== a reader that never reads is an ERROR, not a short count ==\n");
   {
      int sv[2];
      if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
         printf("  [FAIL] socketpair\n");
         return 1;
      }
      /* NO SO_SNDTIMEO: see the header. Only the send buffer is shrunk, so
       * the window fills quickly -- that is arranging backpressure, not
       * arranging the timeout. */
      int small = 4096;
      setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
      setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
      /* Nobody reads sv[1]: the window fills and stays full. */
      struct http_conn c = {.fd = sv[0], .tp = &http_transport_plain};
      guard(HTTP_DEADLINE_S + 7); /* generous, but finite */
      double t0 = http_mono_s();
      ssize_t w = http_transport_plain.write(&c, big, BIG);
      double el = http_mono_s() - t0;
      alarm(0);
      /* The contract is all-or-error. A positive count here would be a short
       * write reported as success, which is the defect this file exists for. */
      ck(w < 0, "a peer that never drains fails the write outright");
      ck(w != (ssize_t)BIG, "...and certainly does not claim success");
      /* ...AND IT GAVE UP ON TIME. Not merely "eventually": the deadline is
       * what stops one silent client from holding a worker, and a loop that
       * takes twice as long as it promises is a loop that is being stopped by
       * something else. */
      ck(el >= (double)HTTP_DEADLINE_S - 0.5,
         "...after waiting out its deadline, not before");
      ck(el < (double)HTTP_DEADLINE_S + 2.0,
         "...and not appreciably longer than its deadline");
      printf("       (gave up after %.2f s; the deadline is %d s)\n", el,
             HTTP_DEADLINE_S);
      close(sv[0]);
      close(sv[1]);
   }

   printf("== two servers in one process are two VALUES ==\n");
   /* WHAT THE OWNED POLICY BUYS. It used to be a process global filled by an
    * ordered setter call before the first accept -- so one process could hold
    * exactly one policy, "call this first" was a rule rather than a type, and
    * forgetting the listener-watch call silently disabled the fairness rule
    * for every connection.
    *
    * Two pools, two policies, and a connection carries its own: the answer to
    * "does this response keep the connection alive" comes from the connection
    * it is about, not from whatever was installed last. */
   {
      struct http_policy keep  = {1, 2.0, 500, HTTP_BODY_MAX};
      struct http_policy close = {0, 2.0, 500, HTTP_BODY_MAX};
      int last                 = 0;
      struct http_conn a       = {.fd           = -1,
                                  .tp           = &http_transport_plain,
                                  .pol          = &keep,
                                  .watch_fd     = -1,
                                  .last_on_conn = &last};
      struct http_conn b       = {.fd           = -1,
                                  .tp           = &http_transport_plain,
                                  .pol          = &close,
                                  .watch_fd     = -1,
                                  .last_on_conn = &last};
      ck(!strcmp(http_conn_value(&a), "keep-alive"),
         "a keep-alive server says so");
      ck(!strcmp(http_conn_value(&b), "close"),
         "...while a close-per-request server in the SAME process says close");
      /* And the per-connection decision still overrides its own policy. */
      last = 1;
      ck(!strcmp(http_conn_value(&a), "close"),
         "the last response on a connection closes it whatever the policy");
   }

   /* ---- THE PUBLIC ORIGIN A SHARE LINK IS BUILT FROM ------------------
    *
    * The settings page used to build invitation links out of the request's own
    * `Host` header. Escaped for HTML, so never an injection -- and worse than
    * that, because the link carries a LIVE SINGLE-USE TOKEN. An authenticated
    * request with `Host: evil.example`, through a permissive proxy or a second
    * name pointed at this address, rendered `https://evil.example/invite/<tok>`
    * on the owner's own page. They copy what the page shows, send it to the
    * person they meant to invite, and the token lands on another domain still
    * good.
    *
    * The origin is configured now, and the only thing standing between a
    * misconfigured value and a link with a hole in it is origin_ok. Its rules
    * are worth executing rather than reading. */
   printf("== the origin a share link may name ==\n");
   {
      ck(origin_ok("pancra.org"), "a plain host is an origin");
      ck(origin_ok("pancra.org:8443"), "...and so is host:port");
      ck(origin_ok("a-b.example.co.uk"), "hyphens and dots inside are fine");
      ck(origin_ok("127.0.0.1:443"), "an address with a port is fine");

      /* NOT AN ORIGIN. Each of these is a shape that would either produce a
       * broken link or smuggle something past a naive check. */
      ck(!origin_ok(""), "an empty value is not an origin");
      ck(!origin_ok(0), "and neither is a null one");
      ck(!origin_ok("evil.example/pancra.org"),
         "a path is refused -- a slash is how a host becomes a prefix");
      ck(!origin_ok("user@evil.example"),
         "userinfo is refused: the host is what follows the @, not precedes "
         "it");
      ck(!origin_ok("//evil.example"), "a scheme-relative form is refused");
      ck(!origin_ok("https://pancra.org"),
         "a full URL is refused -- the origin is host[:port], no scheme");
      ck(!origin_ok("pancra.org:"), "a colon with no port is refused");
      ck(!origin_ok(":8443"), "a port with no host is refused");
      ck(!origin_ok("pancra.org:80:80"), "two colons are refused");
      ck(!origin_ok("pancra.org:http"), "a non-numeric port is refused");
      ck(!origin_ok("pancra.org:123456"),
         "a port too wide to be a port is refused");
      ck(!origin_ok(".pancra.org"), "a leading dot is refused");
      ck(!origin_ok("pancra.org."), "a trailing dot is refused");
      ck(!origin_ok("-pancra.org"), "a leading hyphen is refused");
      ck(!origin_ok("pancra org"), "a space is refused");
      ck(!origin_ok("pancra.org\r\nX: y"),
         "CRLF is refused -- a header cannot be smuggled through the origin");

      /* AND THE CONFIGURED VALUE IS NEVER EMPTY. Every caller interpolates it
       * straight into a link, so "no origin" must be impossible rather than
       * merely unlikely. */
      const char *o = public_origin();
      ck(o != 0 && *o != 0, "the configured origin is never empty");
      ck(origin_ok(o), "...and is always one origin_ok accepts");
   }

   sb_capacity_cases();
   sb_builder_cases();
   reqline_cases();
   form_field_cases();
   form_body_cases();

   printf("\n%s\n", fails ? "SOME HTTP TESTS FAILED" : "ALL HTTP TESTS PASSED");
   return fails ? 1 : 0;
}
