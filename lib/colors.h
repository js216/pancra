// SPDX-License-Identifier: GPL-3.0
// colors.h --- every colour the app draws, in one place
// Copyright 2026 Jakob Kastelic

#ifndef PANCRA_COLORS_H
#define PANCRA_COLORS_H

// ABGR8888, so a literal reads backwards: 0xFF4466FF is RED, not blue

/* ---- text ------------------------------------------------------------- */
#define UI_TEXT     0xFFFFFFFFU /* primary: values, titles, active tabs      */
#define UI_TEXT_DIM 0xFFCCCCCCU /* body copy, timestamps, explanations       */
#define UI_MUTED    0xFF888888U /* labels, frames, inactive tabs             */
#define UI_FAINT    0xFFAAAAAAU /* present but not available: disabled rows  */
#define UI_RULE     0xFF555555U /* separators and the tracks under bars      */
#define UI_BLACK    0xFF000000U /* the screen itself, and reversed-out text  */
#define UI_DISCLAIM 0xFF777777U /* the gate's small print; plot date ticks   */

/* ---- what a control is saying ----------------------------------------- */
#define UI_OK     0xFF33FF88U /* confirmed, connected, in range              */
#define UI_WARN   0xFF00CCFFU /* pending, waiting, needs attention           */
#define UI_DANGER 0xFF4466FFU /* destructive: delete, forget, disconnect     */
#define UI_ALERT  0xFF0000FFU /* the alarm's own red                         */
#define UI_BUSY   0xFF44CCFFU /* amber: a request is in flight               */
#define UI_GO     0xFF00FF00U /* the affirmative half of a confirmation      */

/* ---- the four threshold bands: low is warm, high is cool, alarm louder -- */
#define UI_BAND_ALARM_LO 0xFF2020E0U /* red: below the low alarm             */
#define UI_BAND_NUDGE_LO 0xFF20A0FFU /* amber: below the low nudge           */
#define UI_BAND_NUDGE_HI 0xFFFFC890U /* light blue: above the high nudge     */
#define UI_BAND_ALARM_HI 0xFFFFC000U /* cyan: above the high alarm           */

/* ---- the big number's fixed medical scale ------------------------------ */
#define UI_GLU_LOW  0xFF0000FFU /* under 50: red, which no preference softens */
#define UI_GLU_SOFT 0xFF0080FFU /* 50 to 70: orange                          */
#define UI_GLU_MID  0xFF33FF88U /* 70 to 180: green, the in-range band       */
#define UI_GLU_HIGH 0xFFFFFFFFU /* over 180: white                           */

/* ---- exercise, by level; deepening blue as the effort rises ------------ */
#define UI_EX_LIGHT 0xFFFF9955U /* level 1                                   */
#define UI_EX_MOD   0xFFFF6622U /* level 2                                   */
#define UI_EX_HARD  0xFFFF3300U /* level 3                                   */

/* ---- the per-device trace palette (settings.h SET_NCOLORS) ------------- */
#define UI_SENS_GREEN  0xFF88FF33U /* also the accepted-calibration tick     */
#define UI_SENS_BLUE   0xFFFFAA44U
#define UI_SENS_PINK   0xFFAA66FFU
#define UI_SENS_CYAN   0xFFEEFF66U
#define UI_SENS_VIOLET 0xFFFF88BBU

/* ---- plot chrome (lib/plot.c and the log plots) ------------------------ */
#define UI_PLOT_FRAME 0xFF555555U /* the 50/max reference lines and the sides */
#define UI_PLOT_BAND  0xFF262626U /* the in-range shade behind 70-180        */
#define UI_PLOT_VGRID 0xFF2E2E2EU /* faint vertical gridlines                */
#define UI_PLOT_VTICK 0xFF666666U /* the brighter x tick along the bottom    */
#define UI_PLOT_EDGE  0xFFAAAAAAU /* a point clipped to the top or bottom    */
#define UI_PLOT_SCRUB 0xFFFFFFFFU /* the full-height rule under the finger   */
#define UI_LOG_FRAME  0xFF444444U /* the log plots' border and baseline      */
#define UI_LOG_GRID   0xFF2A2A2AU /* their horizontal divisions              */
#define UI_LOG_CURSOR 0xFF666666U /* their scrub rule                        */
#define UI_HILITE     0xFFAAAAAAU /* the scrubbed point, greyed out          */
#define UI_ORPHAN     0xFF8A8AA0U /* pre-registry points: no sensor to credit */

/* ---- markers for the things logged by hand ----------------------------- */
#define UI_MARK_FOOD 0xFF66DDFFU /* a meal on the glucose plot               */
#define UI_MARK_WT   0xFF88CCFFU /* a weighing on the glucose plot           */
#define UI_MARK_FAST 0xFFFFAA66U /* fast insulin: its rows, dots and trace   */

/* ---- bars -------------------------------------------------------------- */
#define UI_BAR_AGE    0xFF444444U /* the track the reading-age bar runs in   */
#define UI_BAR_STREAK 0xFF333333U /* the track under the in-range streak     */
#define UI_BAR_FILL   0xFF9A9A9AU /* how much of that streak has been run    */

/* ---- sync and status --------------------------------------------------- */
#define UI_SYNC_STALE 0xFFAA8844U /* pushed, but not lately                  */
#define UI_SYNC_WARN  0xFFFFCC44U /* amber: something for the user to fix    */
#define UI_SYNC_ERR   0xFFFF5555U /* a refusal, and the keypad's own errors  */
#define UI_BANNER     0xFF00D0FFU /* STALE over the big number; banner only  */

#endif
