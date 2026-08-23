// SPDX-License-Identifier: GPL-3.0
// food.c --- Food log: a named vocabulary plus editable entries
// Copyright 2026 Jakob Kastelic

/* See food.h. Freestanding like weight.c and insulin.c, whose shape the entry
 * log follows deliberately: hand parsers with digit caps, every row validated
 * on the way in, and the newest rows kept by TIME rather than by arrival. */
#include "food.h"

#include "csvcur.h" /* the shared CSV cursor; the grammar stays here */
#include "dexlibc.h"
#include "log.h"    /* LOGW: an edit that landed but could not be re-read */
#include "thread.h" /* food_lk: the log is published, not filled in place */
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h> /* snprintf */

/* ---- THE WHOLE LOG IS ONE OBJECT, AND IT IS PUBLISHED -------
 *
 * The vocabulary, the entries and the picker's order are one state: an entry
 * names a type by id, and the order is built from both. They were five bare
 * globals, rewritten in place by food_load -- which a RESTORE now calls on
 * the SYNC WORKER. A reader
 * landing mid-load would see entries whose types are not in the vocabulary
 * yet: unnameable rows, which is precisely the state fd_parse_rec refuses to
 * create and the loader would have been creating for real.
 *
 * So: one struct, one leaf lock, and a load that builds a SEPARATE state and
 * publishes it with a single assignment. The three parts become visible
 * together or not at all.
 *
 * (Cross-domain, a restore's four logs publish one at a time -- see
 * pancra_logs_reload in app/reading.c for why that is the trade.) */
struct food_state {
   struct food_type ft[NFOODTYPE];
   int nft;
   int next_id;
   struct food_rec fd[NFOOD];
   int nfd;
   /* THE PICKER'S ORDER, part of the state rather than beside it: it indexes
    * `ft`, so an order published against a different vocabulary names the
    * wrong food. */
   int order[NFOODTYPE];
   int order_n;
};

static struct food_state g_live = {.next_id = 1};

/* A LEAF: taken innermost, never held across another module's call and never
 * across file I/O. See app/thread.h. */
static struct mutex food_lk = MUTEX_INIT;

/* The live state's fields by their old names, so the rest of this file reads
 * as it did. Everything below that touches them runs under food_lk; the
 * LOADER does not use them at all -- it works on a state of its own. */
#define g_ft      g_live.ft
#define g_nft     g_live.nft
#define g_next_id g_live.next_id
#define g_fd      g_live.fd
#define g_nfd     g_live.nfd

static char g_food_path[256];
static char g_ftype_path[256];
static const char g_food_hdr[]  = "# unix_time,type_id,grams,tz_offset_s\n";
static const char g_ftype_hdr[] = "# type_id,name\n";

/* ---------------- the vocabulary ---------------- */

/* THE ORDER THE PICKER SHOWS, and it is not the order they were added.
 *
 * A vocabulary grows and never shrinks, so after a few months the foods
 * somebody actually eats are scattered through a list ordered by when they
 * first thought of them. Sorting by how often each has been logged puts
 * breakfast at the top, which is where the hand goes.
 *
 * ONE ORDER, SHARED BY BOTH ACCESSORS, and that is the part that matters
 * rather than the sorting. food_type_copy fills the frame's snapshot and
 * food_type_at resolves the row a finger landed on -- if they disagreed about
 * position 3, a tap would log a food nobody chose. They cannot disagree here
 * because there is one table and they both read it.
 *
 * BUILT ON EVERY CHANGE, AND ONLY THEN. It is O(types * entries), and the
 * picker asks per row, so it cannot be built per read -- but the answer to
 * that is not a lazy rebuild INSIDE the readers (order_at calling
 * order_build when a flag says the cache is stale). That would make
 * food_type_at() -- which reads as a question -- mutate three file statics,
 * so the cost of a read would depend on which reads came before it, a reader
 * on another thread could be rebuilding the very table this one is walking,
 * and a "const" answer would be nothing of the sort.
 *
 * Now every path that can change either input publishes the complete order as
 * part of the change, and the accessors do nothing but read what is already
 * valid. The inputs are: a new type, a logged entry, an edit, a delete, a
 * reload. There is no staleness to detect, so there is no flag to get wrong.
 *
 * TIES GO TO THE OLDER FOOD -- insertion order -- so a list of foods eaten
 * once each does not reshuffle itself between frames, and a food never eaten
 * sits at the bottom rather than nowhere. */
#define g_order   g_live.order
#define g_order_n g_live.order_n

/* (A LOAD IS STILL ONE CHANGE, not one per row -- a rebuild per row would
 * cost O(rows * types * rows) at startup, for a table nobody can look at
 * until the load returns. The loader works on a state of its own, whose
 * helpers do not publish, and builds the order once at the end -- rather than
 * a batch FLAG suppressing publication, because a flag that turns a rule off
 * for a while is a rule that can be left off.) */

static void order_build_in(struct food_state *s)
{
   int freq[NFOODTYPE] = {0};
   for (int i = 0; i < s->nfd; i++)
      for (int t = 0; t < s->nft; t++)
         if (s->ft[t].id == s->fd[i].type) {
            freq[t]++;
            break;
         }
   for (int i = 0; i < s->nft; i++)
      s->order[i] = i;
   /* INSERTION SORT, and deliberately: the vocabulary is at most NFOODTYPE
    * (64) entries and this runs when one of them changes, not per frame. It
    * is stable, which is what makes equal counts keep insertion order without
    * a second comparison. */
   for (int i = 1; i < s->nft; i++) {
      const int cur = s->order[i];
      int j         = i - 1;
      while (j >= 0 && freq[s->order[j]] < freq[cur]) {
         s->order[j + 1] = s->order[j];
         j--;
      }
      s->order[j + 1] = cur;
   }
   s->order_n = s->nft;
}

/* PUBLISH the order after a change to either input. Cheap to call, and
 * deliberately called on every such path rather than remembered as pending. */
static void order_publish(void)
{
   order_build_in(&g_live);
}

/* The table row shown at display position `i`, or -1. A READ, and nothing
 * else: it cannot build, cannot repair and cannot fail into rebuilding. If
 * the order and the vocabulary ever disagreed about how many there are, this
 * refuses the position rather than papering over it -- a blank row is a
 * visible bug, and a silently rebuilt table in the middle of a frame is the
 * kind that logs the wrong food. */
static int order_at(int i)
{
   if (i < 0 || i >= g_order_n || i >= g_nft)
      return -1;
   return g_order[i];
}

struct food_type food_type_at(int i)
{
   struct food_type z = {0, {0}};
   const int r        = order_at(i);
   if (r < 0)
      return z;
   return g_ft[r];
}

/* IS THIS ID IN THE VOCABULARY AT ALL, and where in the TABLE -- not on the
 * screen. The two are different questions: a display position needs the
 * published order, which a load deliberately does not build until it has read
 * every row, so validating an entry against a position would reject the whole
 * file. Existence is a property of the table alone. */
static int ft_row_in(const struct food_state *s, int id)
{
   for (int i = 0; i < s->nft; i++)
      if (s->ft[i].id == id)
         return i;
   return -1;
}

/* The live vocabulary. CALLER HOLDS food_lk. */
static int ft_row_of(int id)
{
   return ft_row_in(&g_live, id);
}

int food_type_copy(struct food_type *out, int cap)
{
   if (!out || cap <= 0)
      return 0;
   mutex_lock(&food_lk);
   int n = g_nft < cap ? g_nft : cap;
   for (int i = 0; i < n; i++) {
      const int r = order_at(i);
      out[i]      = (r >= 0) ? g_ft[r] : (struct food_type){0, {0}};
   }
   mutex_unlock(&food_lk);
   return n;
}

/* THE NAME AND THE LOOKUP IN ONE HOLD, and the returned pointer is INTO the
 * live table -- which is what this has always handed out (food.h). The table
 * is a fixed array whose rows only move on a load, so the pointer stays
 * valid; what the lock buys is that the row it points at is the row the
 * lookup found. */
const char *food_type_name(int id)
{
   mutex_lock(&food_lk);
   /* THE TABLE ROW, not the display position. The two are different orderings
    * of the same vocabulary -- the picker sorts by how often each food has
    * been logged -- so indexing the table with a position names whichever
    * food happens to sit there instead. */
   const int r      = ft_row_of(id);
   const char *name = (r < 0) ? "" : g_ft[r].name;
   mutex_unlock(&food_lk);
   /* "" RATHER THAN A NULL. Every caller of this draws the answer into a menu
    * row, and a null would be a crash at the one moment the data is already
    * known to be inconsistent. An empty name renders as a blank cell, which is
    * what an entry pointing at a type that is not there honestly looks like. */
   return name;
}

/* Two names are the same name when their bytes are. Deliberately NOT a
 * case-insensitive or whitespace-folding comparison: the keypad produces one
 * case, so a fold would only ever fire on a name that arrived from a file or a
 * sync -- and quietly merging two rows that a peer considered distinct is a
 * decision this file is not in a position to make. */
static int name_eq(const char *a, const char *b)
{
   int i = 0;
   while (a[i] && b[i] && a[i] == b[i])
      i++;
   return a[i] == 0 && b[i] == 0;
}

/* Is this a name that can be written to a CSV row and read back unchanged?
 *
 * ',' and '\n' are the field separator and the row terminator, so a name
 * holding either would not survive the round trip: the comma would split one
 * name into two fields and the newline would split one row into two. A name
 * holding a '#' at the start would be read back as the header comment.
 *
 * REFUSED, NOT REPAIRED. Stripping the character would store a name the user
 * did not type and cannot type again to match; escaping it would make this the
 * only field in the app's file formats that needs an unescaper. The keypad
 * offers no comma, so in practice this rejects only what arrives from a file
 * that something else wrote. */
static int name_ok(const char *n)
{
   if (!n || !n[0])
      return 0;
   if (n[0] == '#')
      return 0;
   int i = 0;
   for (; n[i]; i++) {
      if (n[i] == ',' || n[i] == '\n' || n[i] == '\r')
         return 0;
      if (i >= FOOD_NAME_MAX)
         return 0; /* longer than the field holds */
   }
   return 1;
}

/* Take a type into memory. Used by the loader and by food_type_add, so the
 * id bookkeeping has one home: g_next_id must always be past every id in the
 * table, or a load followed by an add would mint an id that already exists. */
static int ft_take_in(struct food_state *s, int id, const char *name)
{
   if (s->nft >= NFOODTYPE)
      return -1;
   s->ft[s->nft].id = id;
   int i            = 0;
   for (; name[i] && i < FOOD_NAME_MAX; i++)
      s->ft[s->nft].name[i] = name[i];
   s->ft[s->nft].name[i] = 0;
   s->nft++;
   if (id >= s->next_id)
      s->next_id = id + 1;
   return id;
}

/* Into the LIVE state, which also republishes the order. CALLER HOLDS
 * food_lk. The loader uses ft_take_in on its own state and builds the order
 * once at the end -- see food_load. */
static int ft_take(int id, const char *name)
{
   int r = ft_take_in(&g_live, id, name);
   if (r >= 0)
      order_publish(); /* a new food joins the list; see order_build_in */
   return r;
}

int food_type_add(const char *name)
{
   if (!name_ok(name))
      return -1;
   /* THE VOCABULARY IS READ UNDER THE LOCK AND THE FILE IS WRITTEN WITHOUT
    * IT: the leaf is never held across an append. A restore publishing in
    * between is why the take at the end re-checks rather than trusting the
    * id it read -- see there. */
   mutex_lock(&food_lk);
   int known = -1;
   for (int i = 0; i < g_nft && known < 0; i++)
      if (name_eq(g_ft[i].name, name))
         known = g_ft[i].id; /* already known: the same food, the same id */
   int full = g_nft >= NFOODTYPE;
   int id   = g_next_id;
   mutex_unlock(&food_lk);
   if (known >= 0)
      return known;
   if (full)
      return -1;
   char b[FOOD_NAME_MAX + 32];
   int n = snprintf(b, sizeof b, "%d,%s\n", id, name);
   n     = clampn(n, sizeof b);
   /* WRITTEN BEFORE IT IS TAKEN. A type that is in memory but not on disk
    * disappears at the next launch, and the entries logged against it in the
    * meantime become unnameable -- the exact damage food_load reports. The
    * other order costs nothing: a type on disk that failed to make it into
    * memory is picked up by the next load. */
   int rc =
       log_append(g_ftype_path, g_ftype_hdr, (int)sizeof g_ftype_hdr - 1, b, n);
   if (rc != LOG_OK)
      return -1;
   record_mutated(); /* a synced record changed: see util.h */
   mutex_lock(&food_lk);
   int r = ft_take(id, name);
   mutex_unlock(&food_lk);
   return r;
}

/* One row of the vocabulary: "<id>,<name>". */
static int ft_parse_line(struct food_state *s, const char *p, const char *e)
{
   if (p < e && *p == '#')
      return 0; /* the header */
   if (p >= e)
      return 0; /* a trailing newline, or an editor's blank line */
   struct csv_cur c;
   csv_open(&c, p, e);
   long id = csv_num(&c, 0);
   if (!csv_sep(&c))
      return -1; /* no separator: this is not a row, it is a fragment */
   char nm[FOOD_NAME_MAX + 1];
   csv_str(&c, nm, (int)sizeof nm);
   /* AN ID OF 0 IS NOT A TYPE (FOOD_TYPE_NONE), and a name that would not
    * survive a round trip through this format did not come from food_type_add
    * -- both mean the file has been edited or damaged. */
   if (id <= FOOD_TYPE_NONE || id > 0x7fffffffL)
      return -1;
   if (!name_ok(nm))
      return -1;
   /* A DUPLICATE ID IS DAMAGE, and it has to be caught here rather than
    * tolerated: entries reference types by id, so two types sharing one id
    * means every entry using it is ambiguous. The first is kept. */
   if (ft_row_in(s, (int)id) >= 0)
      return -1;
   if (ft_take_in(s, (int)id, nm) < 0)
      return -1; /* the vocabulary is full: the rest of the file is lost */
   return 1;
}

/* ---------------- the entries ---------------- */

int food_count(void)
{
   mutex_lock(&food_lk);
   int n = g_nfd;
   mutex_unlock(&food_lk);
   return n;
}

struct food_rec food_at(int i)
{
   struct food_rec z = {0, 0, 0};
   mutex_lock(&food_lk);
   if (i >= 0 && i < g_nfd)
      z = g_fd[i];
   mutex_unlock(&food_lk);
   return z;
}

long food_last_grams(int type_id)
{
   if (type_id == FOOD_TYPE_NONE)
      return 0;
   /* BACKWARDS, so the FIRST match is the most recent one. The tail is oldest
    * first (food.h states that as part of the contract), so walking forward
    * and keeping the last match would read the whole array to reach the same
    * answer. */
   /* ONE HOLD FOR THE WHOLE WALK, and NOT through food_count/food_at: those
    * take the lock themselves, so calling them from here would either
    * deadlock on a non-recursive leaf or -- worse, if it did not -- walk an
    * index range that a restore had already invalidated. */
   long g = 0;
   mutex_lock(&food_lk);
   for (int i = g_nfd - 1; i >= 0 && !g; i--)
      if (g_fd[i].type == type_id && g_fd[i].g > 0)
         g = g_fd[i].g;
   mutex_unlock(&food_lk);
   return g;
}

int food_copy(struct food_rec *out, int cap)
{
   if (!out || cap <= 0)
      return 0;
   mutex_lock(&food_lk);
   int n = g_nfd < cap ? g_nfd : cap;
   for (int i = 0; i < n; i++)
      out[i] = g_fd[i];
   mutex_unlock(&food_lk);
   return n;
}

/* The newest NFOOD rows BY TIME -- weight.c's wt_push carries the argument for
 * why that is not the same as "the last NFOOD seen". */
static void fd_push_in(struct food_state *s, const struct food_rec *r)
{
   if (s->nfd < NFOOD) {
      s->fd[s->nfd++] = *r;
      return;
   }
   int oldest = 0;
   for (int i = 1; i < s->nfd; i++)
      if (s->fd[i].t < s->fd[oldest].t)
         oldest = i;
   if (r->t < s->fd[oldest].t)
      return; /* the arriving row is the oldest: it evicts nobody */
   for (int i = oldest + 1; i < s->nfd; i++)
      s->fd[i - 1] = s->fd[i];
   s->fd[s->nfd - 1] = *r;
}

static void fd_sort_in(struct food_state *s)
{
   for (int i = 1; i < s->nfd; i++) {
      struct food_rec k = s->fd[i];
      int j             = i - 1;
      while (j >= 0 && s->fd[j].t > k.t) {
         s->fd[j + 1] = s->fd[j];
         j--;
      }
      s->fd[j + 1] = k;
   }
}

/* Into the LIVE state, republishing the order. CALLER HOLDS food_lk. */
static void fd_push(const struct food_rec *r)
{
   /* EVERY entry changes a frequency, and the picker's order is built from
    * frequencies -- so this is the one place that must say so. The type count
    * has not changed, so nothing about the table's SHAPE would reveal it.
    *
    * PUBLISHED AFTER THE ROW IS IN: an order built from the rows as they
    * were is exactly the stale table this exists to abolish. (fd_push_in
    * does not publish at all, which is what makes it usable on the loader's
    * own state, where the order is built once at the end.) */
   fd_push_in(&g_live, r);
   order_publish();
}

static void fd_sort(void)
{
   fd_sort_in(&g_live);
}

/* ONE READER FOR ONE ROW SHAPE.
 *
 * No `why` on any field: as in exercise.c, every answer the cursor could give
 * is already a rejection here. An absent or non-numeric field reads 0, and 0
 * fails `t <= 0`, `type == FOOD_TYPE_NONE` and `g < FOOD_MIN_G` alike; an
 * over-long digit run keeps its leading digits, which cannot land inside a
 * plausible epoch, a live type id, or a plausible portion. */
static int fd_parse_rec(const struct food_state *s, const char *p,
                        const char *e, struct food_rec *r)
{
   struct csv_cur c;
   csv_open(&c, p, e);
   r->t = csv_num(&c, 0);
   csv_sep(&c);
   long ty = csv_num(&c, 0);
   csv_sep(&c);
   r->g = csv_num(&c, 0);
   if (r->t <= 0 || r->t >= FOOD_T_MAX)
      return 0;
   if (r->g < FOOD_MIN_G || r->g > FOOD_MAX_G)
      return 0;
   if (ty <= FOOD_TYPE_NONE)
      return 0;
   /* THE TYPE MUST EXIST, which is why the vocabulary loads first. An entry
    * naming a type that is not there is a meal the app cannot name -- it would
    * draw as a blank row with a weight beside it, which reads as a rendering
    * bug rather than as the missing data it is. Reported as damage instead. */
   if (ft_row_in(s, (int)ty) < 0)
      return 0;
   r->type = (int)ty;
   return 1;
}

static int fd_parse_line(struct food_state *s, const char *p, const char *e)
{
   if (p < e && *p == '#')
      return 0;
   if (p >= e)
      return 0;
   struct food_rec r = {0, 0, 0};
   if (!fd_parse_rec(s, p, e, &r))
      return -1;
   fd_push_in(s, &r);
   return 1;
}

/* Stream one file a line at a time, handing each to `line_fn`. Shared by the
 * two loads below because they differ only in the parser and the path: the
 * over-long-line rule, the final-line-without-a-newline rule and the "a prefix
 * is kept and reported" rule are one policy, and two copies of it would be two
 * places for it to drift. */
static int slurp_lines(struct food_state *s, const char *path,
                       int (*line_fn)(struct food_state *, const char *,
                                      const char *))
{
   int fd = open(path, O_RDONLY, 0);
   if (fd < 0)
      return errno == ENOENT ? 0 : -1;
   char buf[1024];
   char line[128];
   int llen    = 0;
   int over    = 0; /* over-long line: skip, never parse a truncation */
   int damaged = 0;
   long n      = 0;
   while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (long i = 0; i < n; i++) {
         if (buf[i] == '\n') {
            if (over || line_fn(s, line, line + llen) < 0)
               damaged = 1;
            llen = 0;
            over = 0;
         } else if (llen < (int)sizeof line - 1) {
            line[llen++] = buf[i];
         } else {
            over = 1;
         }
      }
   }
   if (llen > 0) {
      /* A FINAL LINE WITH NO NEWLINE is a file cut while being written. Its
       * bytes may parse perfectly and still be half a record, because what
       * says a row is finished is the newline the file does not have. */
      damaged = 1;
   }
   close(fd);
   return (n < 0 || damaged) ? -1 : 0;
}

/* THE STAGING STATE, private to the loader. Static because it is large and
 * this runs on a service thread; never published as anything but a copy. */
static struct food_state g_stage;

int food_load(void)
{
   /* BUILT SEPARATELY, PUBLISHED AT ONCE. Nothing below touches
    * the live state until the assignment at the end, so a reader on the main
    * thread holds the food log from before this call or the one after it --
    * never a vocabulary without its entries, or entries whose types have not
    * been read yet.
    *
    * NO BATCH FLAG IS NEEDED: the loader's helpers do not publish at all, so
    * there is no order for ft_take and fd_push to republish. It is built
    * once, here, on the state it belongs to. */
   struct food_state *s = &g_stage;
   s->nft               = 0;
   s->next_id           = 1;
   s->nfd               = 0;
   s->order_n           = 0;
   /* THE VOCABULARY FIRST. fd_parse_rec rejects an entry whose type is not in
    * the table, so loading the entries first would reject every one of them --
    * a log that empties itself depending on the order two files are read. */
   int a = slurp_lines(s, g_ftype_path, ft_parse_line);
   int b = slurp_lines(s, g_food_path, fd_parse_line);
   fd_sort_in(s);
   order_build_in(s);
   mutex_lock(&food_lk);
   g_live = *s;
   mutex_unlock(&food_lk);
   /* EITHER file being short means what the user is shown is short. They are
    * reported as one answer because they describe one thing -- the food
    * history -- and a caller that could act on "the types are fine but the
    * entries are not" would have nothing different to do about it. */
   return (a < 0 || b < 0) ? -1 : 0;
}

int food_append(long t, int type, long g, long tz)
{
   if (t <= 0 || t >= FOOD_T_MAX)
      return -1;
   if (g < FOOD_MIN_G || g > FOOD_MAX_G)
      return -1;
   /* THE TYPE HAS TO BE ONE WE KNOW. An entry against an id that is not in the
    * vocabulary is unnameable the moment it is written, and this log is never
    * rewritten -- so it would stay unnameable. The picker cannot produce such
    * an id, which is exactly why the check belongs here rather than there. */
   mutex_lock(&food_lk);
   int known = (type > FOOD_TYPE_NONE) && ft_row_of(type) >= 0;
   mutex_unlock(&food_lk);
   if (!known)
      return -1;
   char b[80];
   int n = snprintf(b, sizeof b, "%ld,%d,%ld,%ld\n", t, type, g, tz);
   n     = clampn(n, sizeof b);
   int rc =
       log_append(g_food_path, g_food_hdr, (int)sizeof g_food_hdr - 1, b, n);
   if (rc != LOG_OK)
      return rc; /* LOG_DAMAGED travels: the file may hold a partial row */
   struct food_rec r = {t, type, g};
   /* THE FILE FIRST, THE TABLE UNDER THE LOCK. */
   mutex_lock(&food_lk);
   fd_push(&r);
   fd_sort();
   mutex_unlock(&food_lk);
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
}

/* WHAT MAKES A LINE THE ROW BEING EDITED, and what the replacement says.
 * The rewrite itself -- two passes, a temporary, a durable publish, and the
 * tombstone when a delete empties the file -- is log_edit_last's (util.h),
 * which is why there is no third copy of it here. */
struct fd_edit {
   struct food_rec orig; /* the row the user is editing */
   long t;               /* what it becomes (unused for a delete) */
   int type;
   long g, tz;
};

static int fd_edit_matches(const char *line, const char *end, void *ctx)
{
   const struct fd_edit *e = ctx;
   struct food_rec r;
   /* THREE FIELDS, because two entries of the same food at the same instant
    * differ only by their weight, and two of different foods differ only by
    * their type. The triple is what names one row. */
   /* AGAINST THE LIVE VOCABULARY, which is what the row being edited was
    * validated against. Its own hold: this is called per line from inside
    * log_edit_last, which is file I/O, and no lock is held across that. */
   mutex_lock(&food_lk);
   int is = fd_parse_rec(&g_live, line, end, &r);
   mutex_unlock(&food_lk);
   return is && r.t == e->orig.t && r.type == e->orig.type && r.g == e->orig.g;
}

static int fd_edit_format(char *out, int cap, void *ctx)
{
   const struct fd_edit *e = ctx;
   return snprintf(out, (size_t)cap, "%ld,%d,%ld,%ld", e->t, e->type, e->g,
                   e->tz);
}

/* APPLY A COMMITTED EDIT TO THE TAIL, without touching the file.
 *
 * The same reasoning as weight.c's wt_tail_patch: only ever
 * called when the file rewrite SUCCEEDED and the re-read did not, so the edit
 * it applies is the one already on disk. It finds the row by the same key the
 * matcher above uses, then edits or removes it.
 *
 * A DELETE LEAVES THE TAIL ONE ROW SHORT of what it could hold: the row that
 * should be pulled in is older than anything in memory and only the file has
 * it. The next successful load fills it. One missing old row for one session
 * is a different order of wrong from a table that contradicts the file. */
/* CALLER HOLDS food_lk. */
static void fd_tail_patch(const struct food_rec *orig, int del, long t,
                          int type, long g)
{
   for (int i = 0; i < g_nfd; i++) {
      if (g_fd[i].t != orig->t || g_fd[i].type != orig->type ||
          g_fd[i].g != orig->g)
         continue;
      if (del) {
         for (int j = i + 1; j < g_nfd; j++)
            g_fd[j - 1] = g_fd[j];
         g_nfd--;
      } else {
         g_fd[i].t    = t;
         g_fd[i].type = type;
         g_fd[i].g    = g;
         fd_sort(); /* the instant may have moved: keep oldest-first */
      }
      /* A deleted row and a retyped row both change a frequency, so the
       * picker's order is republished here as on every other mutation. */
      order_publish();
      return;
   }
   /* Older than the newest NFOOD rows: not in the tail, nothing to do. */
}

static int fd_rewrite(const struct food_rec *orig, int del, long t, int type,
                      long g, long tz)
{
   if (!orig)
      return -1;
   struct fd_edit e = {*orig, t, type, g, tz};
   struct log_edit ed;
   ed.matches = fd_edit_matches;
   ed.format  = del ? 0 : fd_edit_format;
   ed.ctx     = &e;
   if (log_edit_last(g_food_path, &ed) != 0)
      return -1;
   /* THE FILE IS RIGHT; NOW MAKE MEMORY SAY SO. food_load clears
    * the tail as its first act, so a reload that then fails leaves the screen
    * showing an EMPTY food log after an edit that succeeded -- and a partial
    * read can leave the edited row showing its old values with the edit
    * reported committed. The rewrite is already validated, so the tail is
    * published from it rather than re-read. */
   /* THE WHOLE STATE IS SAVED, not just the entries: food_load republishes
    * the vocabulary and the picker's order too, so restoring the rows alone
    * would leave them beside a table built from a load that failed. */
   struct food_state save;
   mutex_lock(&food_lk);
   save = g_live;
   mutex_unlock(&food_lk);
   if (food_load() != 0) {
      mutex_lock(&food_lk);
      g_live = save;
      fd_tail_patch(orig, del, t, type, g);
      mutex_unlock(&food_lk);
      LOGW("food: the log was rewritten but could not be re-read; the table "
           "was updated from the edit itself");
   }
   return 0;
}

int food_update(const struct food_rec *orig, long t, int type, long g, long tz)
{
   if (!orig || t <= 0 || t >= FOOD_T_MAX)
      return -1;
   if (g < FOOD_MIN_G || g > FOOD_MAX_G)
      return -1;
   /* The type must still exist, for the reason food_append refuses an unknown
    * one: an entry against an id no vocabulary holds is unnameable, and the
    * only writer that ever revisits a row is this one. */
   mutex_lock(&food_lk);
   int known = (type > FOOD_TYPE_NONE) && ft_row_of(type) >= 0;
   mutex_unlock(&food_lk);
   if (!known)
      return -1;
   return fd_rewrite(orig, 0, t, type, g, tz);
}

int food_delete(const struct food_rec *orig)
{
   if (!orig)
      return -1;
   return fd_rewrite(orig, 1, 0, 0, 0, 0);
}

int food_paths(const char *dir)
{
   int ok = 1;
   if (!(data_path(g_food_path, sizeof g_food_path, dir, "/food.csv")))
      ok = 0;
   if (!(data_path(g_ftype_path, sizeof g_ftype_path, dir, "/foodtypes.csv")))
      ok = 0;
   return ok;
}

const char *food_path(void)
{
   return g_food_path;
}

const char *food_types_path(void)
{
   return g_ftype_path;
}
