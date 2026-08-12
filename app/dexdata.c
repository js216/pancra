// SPDX-License-Identifier: GPL-3.0
// dexdata.c --- Dexcom G7/Stelo glucose payload decoding
// Copyright 2026 Juggluco and xDrip contributors

/* Dexcom G7 / Stelo glucose decoding.
 *
 * Two on-wire shapes:
 *   - backfill / EGV record: 9 bytes  [tsU32LE | gluU16LE | 3 status bytes]
 *   - current-EGV control response (opcode 0x4e): >=19 bytes, richer layout
 *
 * Field layout ported from xDrip cgm/dex/g7/EGlucoseRxMessage.java (GPLv3).
 * Offline test build:
 *   cc -DDEXDATA_TEST dexdata.c -o dexdata_test && ./dexdata_test
 */

#include "dexdata.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
   return (uint32_t)p[0] | (uint32_t)p[1] << 8U | (uint32_t)p[2] << 16U |
          (uint32_t)p[3] << 24U;
}

static uint16_t le16(const uint8_t *p)
{
   return (uint16_t)((unsigned)p[0] | (unsigned)p[1] << 8U);
}

/* Decode one 9-byte backfill/EGV record. */
int dexdata_record(const uint8_t rec[9], struct dex_record *out)
{
   out->timestamp    = le32(rec);
   uint16_t g        = le16(rec + 4);
   out->display_only = ((unsigned)g & 0xf000U) != 0;
   out->glucose      = (unsigned)g & 0x0fffU;
   memcpy(out->status, rec + 6, 3);
   return 1;
}

/* Decode a stream of concatenated 9-byte records (a backfill notification may
 * hold several). Writes up to max records; returns the count. */
int dexdata_records(const uint8_t *buf, size_t len, struct dex_record *out,
                    int max)
{
   int n = 0;
   for (size_t off = 0; off + 9 <= len && n < max; off += 9)
      dexdata_record(buf + off, &out[n++]);
   return n;
}

/* Decode the current-EGV control response (opcode 0x4e). Returns 1 on success.
 */
int dexdata_egv(const uint8_t *p, size_t len, struct dex_egv *out)
{
   if (len < 19 || p[0] != 0x4e)
      return 0;
   out->status_raw = p[1];
   out->clock      = le32(p + 2);
   out->sequence   = le16(p + 6);
   /* p+8: unused/bogus */
   out->age          = le16(p + 10);
   uint16_t g        = le16(p + 12);
   out->display_only = ((unsigned)g & 0xf000U) != 0;
   out->glucose      = (unsigned)g & 0x0fffU;
   out->state        = p[14];
   out->trend        = (int8_t)p[15];
   out->predicted    = (unsigned)le16(p + 16) & 0x03ffU;
   return 1;
}

#ifdef DEXDATA_TEST
#include <stdio.h>

static int hex(const char *s, uint8_t *out)
{
   int n = 0;
   for (; s[0] && s[1]; s += 2) {
      unsigned v;
      sscanf(s, "%2x", &v);
      out[n++] = (uint8_t)v;
   }
   return n;
}

int main(void)
{
   int all = 1;

   struct {
      const char *hex;
      uint32_t ts;
      uint16_t glu;
   } recs[] = {
       {"9ffd07009e00060ffe", 523679, 158}, /* Stelo */
       {"f7ff07009b00060ffd", 524279, 155},
       {"2d080800a500060ffe", 526381, 165}, /* G7 */
       {"59090800a400060ffe", 526681, 164},
       {"850a0800a300060ffe", 526981, 163},
   };

   printf("== 9-byte records (vs captured values) ==\n");
   for (int i = 0; i < 5; i++) {
      uint8_t b[16];
      hex(recs[i].hex, b);
      struct dex_record r;
      dexdata_record(b, &r);
      int ok = (r.timestamp == recs[i].ts) && (r.glucose == recs[i].glu);
      printf("  [%s] %s -> ts=%u glu=%u\n", ok ? "PASS" : "FAIL", recs[i].hex,
             r.timestamp, r.glucose);
      all &= ok;
   }

   printf("== multi-record backfill notification ==\n");
   {
      uint8_t b[64];
      int nb = hex("2d080800a500060ffe59090800a400060ffe", b);
      struct dex_record r[4];
      int n  = dexdata_records(b, nb, r, 4);
      int ok = (n == 2) && r[0].glucose == 165 && r[1].glucose == 164 &&
               r[1].timestamp - r[0].timestamp == 300;
      printf("  [%s] 2 records, 165 & 164, +300s apart\n",
             ok ? "PASS" : "FAIL");
      all &= ok;
   }

   printf("== 0x4e current-EGV control response ==\n");
   {
      struct {
         const char *hex;
         uint16_t glu;
         uint16_t age;
      } egvs[] = {
          {"4e0031080800dd0600010400a50006fea5000f", 165, 4}, /* G7 */
          {"4e00a4fd0700d406000105009e0006fe9c000f", 158, 5}, /* Stelo */
      };

      for (int i = 0; i < 2; i++) {
         uint8_t b[32];
         int nb = hex(egvs[i].hex, b);
         struct dex_egv e;
         int ok = dexdata_egv(b, nb, &e) && e.glucose == egvs[i].glu &&
                  e.age == egvs[i].age;
         printf("  [%s] %s -> glu=%u age=%us trend=%d predicted=%u\n",
                ok ? "PASS" : "FAIL", egvs[i].hex, e.glucose, e.age, e.trend,
                e.predicted);
         all &= ok;
      }
   }

   printf("\n%s\n", all ? "ALL DATA TESTS PASSED" : "SOME TESTS FAILED");
   return all ? 0 : 1;
}
#endif
