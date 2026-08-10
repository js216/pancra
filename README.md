# Pancra

Pancra is a readout app for Dexcom G7, Stelo and OneTouch Verio devices. With
several CGMs paired at the same time, a new sensor can warm up while the old one
is still reporting. It supports two independent alarm levels, a one time nudge
and a persistent alarm, both for high and for low glucose levels.

## Safety Disclaimer

Pancra is a **wellness app**. It is not a medical device, it is not approved by
any regulator, and it has not been tested to any clinical standard. Readings
can arrive late, wrong, or not at all.

**Never use it to decide a dose, a correction, or any other treatment.** For
that, use the approved sensor manufacturer's readout device.

Pancra is *NOT* affiliated with, endorsed by or supported by Dexcom or
LifeScan. Dexcom, G7 and Stelo are trademarks of Dexcom, Inc.; OneTouch and
Verio are trademarks of LifeScan.

Cloud sync is optional and off until you set it up. When it is on, the record
travels over HTTPS and every request is signed with a key established by
pairing; the server holds it behind a login. Nobody but you sees it unless you
share it, and sharing is a link you create and can revoke.

## What it does

- **Several sensors at once, so sessions can overlap.** Start the replacement
  sensor before the current one expires and it warms up while the old one is
  still reporting -- no blind gap at the changeover. Each device keeps its own
  connection, session and colour.
- **Two alert levels.** The nudge sits outside the alarm and fires once, so the
  alarm can stay at a conservative value instead of being edited up and down
  all day.
- **Meters sync themselves.** A OneTouch is awake for a second or two after a
  fingerstick, so Pancra holds a connection open for every registered meter and
  catches it without you touching the phone.
- **You can hear the trend.** Each reading can chirp at a pitch that bends with
  how fast you are moving, and which way.
- **Insulin and weight logs**, editable, with a weight trend you can scrub.
- **Optional sync to a server you run.** Nothing leaves the phone until you
  both switch it on and pair, and it stays off until then. The phone is the
  authoritative copy: it pairs once with a 6-digit code, and from then on the
  two are kept in step -- corrections and deletions included -- over HTTPS,
  with every request signed. See [glucoserve](../glucoserve).

## Building

Prerequisites:

    sudo apt-get install clang-19 llvm-19 default-jdk adb aapt \
                         android-sdk-build-tools android-framework-res \
                         clang-format clang-tidy

`android.jar` and `r8.jar` are not packaged; put them in `tmp/tools/`.

    make            # builds and signs build/pancra.apk
    make install    # adb install
    make check      # format, static analysis, test suites

Native C against the NDK, no third-party libraries.

## Licence

Copyright 2026 Jakob Kastelic. GPL-3.0.
