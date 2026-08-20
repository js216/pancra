// SPDX-License-Identifier: GPL-3.0
// food.c --- Food log: a named vocabulary plus append-only entries
// Copyright 2026 Jakob Kastelic

/* See food.h. Freestanding like weight.c and insulin.c, whose shape the entry
 * log follows deliberately: hand parsers with digit caps, every row validated
 * on the way in, and the newest rows kept by TIME rather than by arrival. */
#include "food.h"

#include "csvcur.h" /* the shared CSV cursor; the grammar stays here */
#include "dexlibc.h"
#include "util.h"
#if __STDC_HOSTED__
#include <errno.h> /* ENOENT: a missing file is not a read failure */
#endif
#include <stdio.h> /* snprintf */

static struct food_type g_ft[NFOODTYPE];
static int g_nft;
static int g_next_id = 1;

static struct food_rec g_fd[NFOOD];
static int g_nfd;

static char g_food_path[256];
static char g_ftype_path[256];
static const char g_food_hdr[]  = "# unix_time,type_id,grams,tz_offset_s\n";
static const char g_ftype_hdr[] = "# type_id,name\n";

/* ---------------- the vocabulary ---------------- */

int food_type_count(void)
{
   return g_nft;
}

struct food_type food_type_at(int i)
{
   struct food_type z = {0, {0}};
   if (i < 0 || i >= g_nft)
      return z;
   return g_ft[i];
}

int food_type_index(int id)
{
   for (int i = 0; i < g_nft; i++)
      if (g_ft[i].id == id)
         return i;
   return -1;
}

int food_type_copy(struct food_type *out, int cap)
{
   int n = g_nft < cap ? g_nft : cap;
   for (int i = 0; i < n; i++)
      out[i] = g_ft[i];
   return n;
}

const char *food_type_name(int id)
{
   int i = food_type_index(id);
   /* "" RATHER THAN A NULL. Every caller of this draws the answer into a menu
    * row, and a null would be a crash at the one moment the data is already
    * known to be inconsistent. An empty name renders as a blank cell, which is
    * what an entry pointing at a type that is not there honestly looks like. */
   return i < 0 ? "" : g_ft[i].name;
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
static int ft_take(int id, const char *name)
{
   if (g_nft >= NFOODTYPE)
      return -1;
   g_ft[g_nft].id = id;
   int i          = 0;
   for (; name[i] && i < FOOD_NAME_MAX; i++)
      g_ft[g_nft].name[i] = name[i];
   g_ft[g_nft].name[i] = 0;
   g_nft++;
   if (id >= g_next_id)
      g_next_id = id + 1;
   return id;
}

int food_type_add(const char *name)
{
   if (!name_ok(name))
      return -1;
   for (int i = 0; i < g_nft; i++)
      if (name_eq(g_ft[i].name, name))
         return g_ft[i].id; /* already known: the same food, the same id */
   if (g_nft >= NFOODTYPE)
      return -1;
   int id = g_next_id;
   char b[FOOD_NAME_MAX + 32];
   int n = snprintf(b, sizeof b, "%d,%s\n", id, name);
   n     = clampn(n, sizeof b);
   /* WRITTEN BEFORE IT IS TAKEN. A type that is in memory but not on disk
    * disappears at the next launch, and the entries logged against it in the
    * meantime become unnameable -- the exact damage food_load reports. The
    * other order costs nothing: a type on disk that failed to make it into
    * memory is picked up by the next load. */
   int rc = log_append(g_ftype_path, g_ftype_hdr, (int)sizeof g_ftype_hdr - 1,
                       b, n);
   if (rc != LOG_OK)
      return -1;
   record_mutated(); /* a synced record changed: see util.h */
   return ft_take(id, name);
}

/* One row of the vocabulary: "<id>,<name>". */
static int ft_parse_line(const char *p, const char *e)
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
   if (food_type_index((int)id) >= 0)
      return -1;
   if (ft_take((int)id, nm) < 0)
      return -1; /* the vocabulary is full: the rest of the file is lost */
   return 1;
}

/* ---------------- the entries ---------------- */

int food_count(void)
{
   return g_nfd;
}

struct food_rec food_at(int i)
{
   struct food_rec z = {0, 0, 0};
   if (i < 0 || i >= g_nfd)
      return z;
   return g_fd[i];
}

struct food_rec food_newest(void)
{
   struct food_rec z = {0, 0, 0};
   return g_nfd > 0 ? g_fd[g_nfd - 1] : z;
}

int food_copy(struct food_rec *out, int cap)
{
   int n = g_nfd < cap ? g_nfd : cap;
   for (int i = 0; i < n; i++)
      out[i] = g_fd[i];
   return n;
}

/* The newest NFOOD rows BY TIME -- weight.c's wt_push carries the argument for
 * why that is not the same as "the last NFOOD seen". */
static void fd_push(const struct food_rec *r)
{
   if (g_nfd < NFOOD) {
      g_fd[g_nfd++] = *r;
      return;
   }
   int oldest = 0;
   for (int i = 1; i < g_nfd; i++)
      if (g_fd[i].t < g_fd[oldest].t)
         oldest = i;
   if (r->t < g_fd[oldest].t)
      return; /* the arriving row is the oldest: it evicts nobody */
   for (int i = oldest + 1; i < g_nfd; i++)
      g_fd[i - 1] = g_fd[i];
   g_fd[g_nfd - 1] = *r;
}

static void fd_sort(void)
{
   for (int i = 1; i < g_nfd; i++) {
      struct food_rec k = g_fd[i];
      int j             = i - 1;
      while (j >= 0 && g_fd[j].t > k.t) {
         g_fd[j + 1] = g_fd[j];
         j--;
      }
      g_fd[j + 1] = k;
   }
}

/* ONE READER FOR ONE ROW SHAPE.
 *
 * No `why` on any field: as in exercise.c, every answer the cursor could give
 * is already a rejection here. An absent or non-numeric field reads 0, and 0
 * fails `t <= 0`, `type == FOOD_TYPE_NONE` and `g < FOOD_MIN_G` alike; an
 * over-long digit run keeps its leading digits, which cannot land inside a
 * plausible epoch, a live type id, or a plausible portion. */
static int fd_parse_rec(const char *p, const char *e, struct food_rec *r)
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
   if (food_type_index((int)ty) < 0)
      return 0;
   r->type = (int)ty;
   return 1;
}

static int fd_parse_line(const char *p, const char *e)
{
   if (p < e && *p == '#')
      return 0;
   if (p >= e)
      return 0;
   struct food_rec r = {0, 0, 0};
   if (!fd_parse_rec(p, e, &r))
      return -1;
   fd_push(&r);
   return 1;
}

/* Stream one file a line at a time, handing each to `line_fn`. Shared by the
 * two loads below because they differ only in the parser and the path: the
 * over-long-line rule, the final-line-without-a-newline rule and the "a prefix
 * is kept and reported" rule are one policy, and two copies of it would be two
 * places for it to drift. */
static int slurp_lines(const char *path, int (*line_fn)(const char *,
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
            if (over || line_fn(line, line + llen) < 0)
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

int food_load(void)
{
   g_nft     = 0;
   g_next_id = 1;
   g_nfd     = 0;
   /* THE VOCABULARY FIRST. fd_parse_rec rejects an entry whose type is not in
    * the table, so loading the entries first would reject every one of them --
    * a log that empties itself depending on the order two files are read. */
   int a = slurp_lines(g_ftype_path, ft_parse_line);
   int b = slurp_lines(g_food_path, fd_parse_line);
   fd_sort();
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
   if (type <= FOOD_TYPE_NONE || food_type_index(type) < 0)
      return -1;
   char b[80];
   int n = snprintf(b, sizeof b, "%ld,%d,%ld,%ld\n", t, type, g, tz);
   n     = clampn(n, sizeof b);
   int rc = log_append(g_food_path, g_food_hdr, (int)sizeof g_food_hdr - 1, b,
                       n);
   if (rc != LOG_OK)
      return rc; /* LOG_DAMAGED travels: the file may hold a partial row */
   struct food_rec r = {t, type, g};
   fd_push(&r);
   fd_sort();
   record_mutated(); /* a synced record changed: see util.h */
   return 0;
}

int food_paths(const char *dir)
{
   int ok = 1;
   ok &= data_path(g_food_path, sizeof g_food_path, dir, "/food.csv");
   ok &= data_path(g_ftype_path, sizeof g_ftype_path, dir, "/foodtypes.csv");
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
