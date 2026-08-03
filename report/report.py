#!/usr/bin/env python3
"""Build a PDF report from the app's readings.csv.

Usage: python3 report/report.py [data/readings.csv] [report/report.pdf]

Three sections:
  1. one page per month, every day's 24 h trace superposed
  2. time in range, day by day
  3. one plot per day, for every day with data
"""
import sys
from collections import defaultdict
from datetime import datetime, timedelta, timezone

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.backends.backend_pdf import PdfPages

LO, HI = 70, 180  # the in-range band stats.c uses
SRC = sys.argv[1] if len(sys.argv) > 1 else "data/readings.csv"
OUT = sys.argv[2] if len(sys.argv) > 2 else "report/report.pdf"


def load(path):
    """-> {date: [(hour_of_day_float, mg/dL), ...]}, local time, CGM only."""
    days = defaultdict(list)
    for line in open(path):
        if line.startswith("#"):
            continue
        f = line.rstrip("\n").split(",")
        if len(f) < 2 or not f[0] or not f[1]:
            continue
        # Columns after the 4th were added later; old rows simply lack them.
        # kind 1 is a fingerstick -- excluded, exactly as stats.c does, so the
        # numbers here agree with the ones on the phone.
        if len(f) > 8 and f[8] == "1":
            continue
        t, glu = int(f[0]), int(f[1])
        tz = int(f[7]) if len(f) > 7 and f[7] else 0
        if glu <= 0 or t <= 0:
            continue
        d = datetime.fromtimestamp(t + tz, timezone.utc)  # tz-shifted = local
        days[d.date()].append((d.hour + d.minute / 60 + d.second / 3600, glu))
    for v in days.values():
        v.sort()
    return days


def band(ax):
    """The in-range band, drawn the same way on every plot."""
    ax.axhspan(LO, HI, color="0.85", zorder=0)
    ax.set_ylim(40, 400)
    ax.set_xlim(0, 24)
    ax.set_xticks(range(0, 25, 6))
    ax.grid(alpha=0.3)


def main():
    days = load(SRC)
    if not days:
        sys.exit(f"no readings in {SRC}")
    print(f"{len(days)} days, {sum(len(v) for v in days.values())} readings")

    with PdfPages(OUT) as pdf:
        # 1. per month, every day superposed -- thin alpha lines pile up into
        #    a density map, so the habitual shape of a month is visible.
        months = defaultdict(list)
        for d in sorted(days):
            months[(d.year, d.month)].append(d)
        for (y, m), ds in sorted(months.items()):
            fig, ax = plt.subplots(figsize=(11, 6))
            for d in ds:
                xs, ys = zip(*days[d])
                ax.plot(xs, ys, color="tab:blue", alpha=0.25, lw=1)
            band(ax)
            ax.set_title(f"{y}-{m:02d}   {len(ds)} days superposed")
            ax.set_xlabel("hour of day")
            ax.set_ylabel("mg/dL")
            pdf.savefig(fig)
            plt.close(fig)

        # 2. time in range, day by day
        ds = sorted(days)
        tir = [
            100 * sum(LO <= g <= HI for _, g in days[d]) / len(days[d]) for d in ds
        ]
        fig, ax = plt.subplots(figsize=(11, 6))
        ax.bar(ds, tir, color="tab:green", width=1.0)
        ax.axhline(70, color="0.4", ls="--", lw=1)  # the usual 70 % target
        ax.set_ylim(0, 100)
        ax.set_title(f"time in range {LO}-{HI} mg/dL   (mean {sum(tir)/len(tir):.0f} %)")
        ax.set_ylabel("% of readings in range")
        ax.grid(alpha=0.3, axis="y")
        fig.autofmt_xdate()
        pdf.savefig(fig)
        plt.close(fig)

        # 3. one plot per day, 6 to a page
        per = 6
        for i in range(0, len(ds), per):
            chunk = ds[i : i + per]
            fig, axes = plt.subplots(3, 2, figsize=(11, 8.5))
            for ax, d in zip(axes.flat, chunk):
                xs, ys = zip(*days[d])
                ax.plot(xs, ys, color="tab:blue", lw=1.2)
                band(ax)
                t = 100 * sum(LO <= g <= HI for _, g in days[d]) / len(days[d])
                ax.set_title(f"{d}   TIR {t:.0f} %", fontsize=9)
                ax.tick_params(labelsize=7)
            for ax in axes.flat[len(chunk) :]:
                ax.axis("off")
            fig.tight_layout()
            pdf.savefig(fig)
            plt.close(fig)

    print(f"wrote {OUT}")


main()
