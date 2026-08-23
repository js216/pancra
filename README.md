# Pancra

**A personal project, not a medical device. Never use it to decide a dose,
and never rely on its alerts to warn you of anything.** See [Legal](#legal).

Pancra is a readout app for Dexcom G7, Stelo and OneTouch Verio devices. With
several CGMs paired at the same time, a new sensor can warm up while the old one
is still reporting. It supports two independent alarm levels, a one time nudge
and a persistent alarm, both for high and for low glucose levels.

The other half of this repository is `srv/` -- a small glucose sync server the
app pairs with. One binary and one sqlite file: no daemons, no runtime, and no
proxy needed to *serve* -- it terminates TLS itself and speaks HTTP/1.1
directly.

## What it Does

- **Several sensors at once, so sessions can overlap.** Start the replacement
  sensor before the current one expires and it warms up while the old one is
  still reporting -- no blind gap at the changeover. Each device keeps its own
  connection, session and colour.
- **Two alert levels.** The nudge sits outside the alarm and fires once, so the
  alarm can stay at a conservative value rather than being edited up and down
  all day.
- **Meters sync themselves.** A OneTouch is awake for a second or two after a
  fingerstick, so Pancra holds a connection open for every registered meter and
  catches it without you touching the phone.
- **You can hear the trend.** Each reading can chirp at a pitch that bends with
  how fast you are moving, and which way.
- **Insulin and weight logs**, editable, with a weight trend plot.
- **Optional sync to a server you run.** Nothing leaves the phone until you
  both switch it on and pair, and it stays off until then.

## Building

Prerequisites:

    sudo apt-get install ccache clang-19 llvm-19 default-jdk adb aapt \
                         android-sdk-build-tools android-framework-res zip

`tools/` holds everything downloaded rather than written, and none of it is
in git:

    tools/android.jar                     platforms/android-34 from an SDK
    tools/r8.jar                          maven.google.com, com/android/tools/r8
    tools/debug.keystore                  what the debug APK is signed with
    tools/sqlite-amalgamation-3460100/    sqlite.org/2024
    tools/riscv64-linux-musl-cross/       musl.cc

sqlite is the one library the server does not carry itself, and the server is
built for the Milk-V Duo -- riscv64, static, no libc on the board to match:

    cd tools
    curl -O https://sqlite.org/2024/sqlite-amalgamation-3460100.zip
    unzip sqlite-amalgamation-3460100.zip
    curl -O https://musl.cc/riscv64-linux-musl-cross.tgz
    tar xf riscv64-linux-musl-cross.tgz

Then, from the top of the tree:

    make            # build/app/pancra.apk and build/srv/pancra_srv
    make install    # adb install, and start the activity
    make deploy     # stop, replace and start the server on the board
    make clean      # rm -rf build

## Legal

Pancra is a personal project published as source code. It is not a product, it
is not sold, and it is not supported.

It is not a medical device. It has no medical purpose, is not approved or
cleared by any regulator, has not been tested to any clinical standard, and is
not intended to diagnose, treat, cure, mitigate or prevent any disease.
Readings can arrive late, wrong, or not at all.

Its alerts are a convenience and are not designed or tested to be relied upon.
They can be late, wrong, or silent, and you must not depend on them to warn you
of anything.

**Never use it to decide a dose, a correction, or any other treatment.** For
that, use the approved sensor manufacturer's readout device.

There is no warranty of any kind. As set out in sections 15 and 16 of the
GPL-3.0, the program is provided as-is, and no copyright holder or conveyor of
it is liable for any damage arising out of its use.

Pancra is *NOT* affiliated with, endorsed by, or supported by Dexcom or
LifeScan. Dexcom, G7 and Stelo are trademarks of Dexcom, Inc.; OneTouch and
Verio are trademarks of LifeScan.

Copyright 2026 Jakob Kastelic. GPL-3.0 only.
