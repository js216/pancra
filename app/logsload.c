// SPDX-License-Identifier: GPL-3.0
// logsload.c --- re-read every log from disk (see logsload.h)
// Copyright 2026 Jakob Kastelic

#include "logsload.h"
#include "exercise.h" /* exercise_load: a restore rewrote it too */
#include "food.h"     /* food_load: the entries AND their vocabulary */
#include "insulin.h"
#include "log.h"
#include "sensors.h"
#include "shell.h"
#include "stats.h" /* stat_reload: a restore rewrote the readings log */
#include "store.h"
#include "weight.h"

/* Re-read every log from disk.
 *
 * Only sync_restore needs this: it APPENDS rows to the log files behind the
 * app's back, and nothing in memory knows. Without a reload the rows are on
 * disk and the plot, the statistics and the history still show what they
 * showed before -- which looks exactly like a restore that did nothing.
 *
 * Same lock discipline as startup: sensors first (readings resolve their
 * source through the registry), then the history under hist_lock, because
 * store_load rewrites g_hist wholesale and a main-thread draw must not see it
 * half-shifted.
 *
 * THE STATISTICS ARE PART OF "EVERY LOG", which is easy to leave out because
 * stat_load also runs at startup. Reload the history without them and a
 * restore fills the history list and redraws the plot while the TIR and
 * AVERAGE printed against that same plot stay at whatever the fresh install
 * computed for itself -- two figures on one screen describing the same
 * readings and disagreeing, with no way for the user to tell which pair to
 * believe, until they restart the app.
 *
 * ...AND THE HISTORY THEY DESCRIBE HAS TO BE RIGHT FIRST. A store_load that
 * INSERTED into the live table rather than replacing it would leave the
 * history a union of what was there before and what came back: a reading the
 * user had deleted still on the plot, and the superseded value kept by the
 * dedup where the restored file corrects one. Statistics rebuilt from that
 * table match neither the file nor the plot -- a second wrong number, computed
 * carefully. store_load stages a whole table and swaps it (see store.c), and
 * the rebuild below is published AFTER that swap, from the same file, under
 * the same lock. */
void pancra_logs_reload(void)
{
   sensors_load();
   /* The primary BEFORE the history lock: registry -> history is the order,
    * and store_load needs the id to re-bind the big number. */
   int prime = sensor_primary_id();
   /* THE STATISTICS ARE PARSED HERE, BEFORE THE HISTORY LOCK, AND PUBLISHED
    * INSIDE IT. Both halves of that are lock order, not taste.
    *
    * The parse resolves every row's sensor through the REGISTRY (the warm-up
    * hour is per-sensor, anchored on its activation), and the order is
    * driver -> registry -> history. Parsing under the history lock would take
    * the registry inside it -- the inversion behind two phone freezes in one
    * day -- and test/app/lockorder.py refuses it. Publishing is a copy and
    * two stores, no I/O and no other lock, so it belongs inside the very hold
    * store_load already takes: the restored history and the restored
    * statistics then become visible in the same instant, and no frame can
    * ever draw the new plot beside the previous average.
    *
    * AFTER sensors_load(), which is not optional: the replay excludes a
    * sensor's warm-up hour, and the activation it measures that against comes
    * out of the registry this call has just re-read. Preparing first would
    * count uncalibrated readings the live path never counted, so the restored
    * numbers would differ from the ones the phone had produced from the very
    * same rows. */
   int prepared = stat_reload_prepare(store_path());
   store_lock();
   int rc = store_load(prime);
   stat_reload_publish();
   store_unlock();
   if (!prepared)
      LOGW("restore: the statistics could not be rebuilt; TIR and the "
           "average still describe the record from before the restore");
   if (rc < 0)
      LOGW("restore: the readings log could not be re-read whole");
   /* EVERY LOG THE RESTORE REWROTE, NOT TWO OF THEM.
    *
    * insulin and weight were reloaded here; food, its vocabulary and
    * exercise were not, so a restore rewrote those files and the screen went
    * on showing the pre-restore contents until the app was next started --
    * with no indication that what was displayed and what was on disk had
    * stopped being the same thing. The food TYPES matter most of that set:
    * an entry names its type by id, so a restored food.csv beside a stale
    * foodtypes.csv draws rows the app cannot name.
    *
    * ONE LOG AT A TIME, EACH COHERENT; THE SET IS NOT ATOMIC. Every loader
    * below builds its state separately and publishes it under its own leaf
    * lock, so no reader ever sees half a log. A frame drawn between two of
    * them shows the new insulin beside food that has not reloaded yet, and
    * that is the
    * deliberate trade: making the SET atomic means holding four leaf locks
    * across four file reads on this thread, and the frame builder takes
    * every one of them -- a repaint waiting on flash is the ANR this app has
    * been killed for twice (app/thread.h). The window is milliseconds and
    * every log in it is internally consistent.
    *
    * FOOD BEFORE ITS ENTRIES is food_load's own business: it reads the
    * vocabulary first, for the reason stated there. */
   if (insulin_load() < 0)
      LOGW("restore: the insulin log could not be re-read whole");
   if (weight_load() < 0)
      LOGW("restore: the weight log could not be re-read whole");
   if (food_load() < 0)
      LOGW("restore: the food log could not be re-read whole");
   if (exercise_load() < 0)
      LOGW("restore: the exercise log could not be re-read whole");
   shell_ui_dirty();
}
