# Pancra

Pancra is a readout app for Dexcom G7, Stelo and OneTouch Verio devices. With
several CGMs at once a new sensor can warm up while the old one is still
reporting. It supports two independent alarm levels for high and low glucose.

## Safety Disclaimer

Pancra is a **wellness app**. It is not a medical device, it is not approved by
any regulator, and it has not been tested to any clinical standard. Readings
can arrive late, wrong, or not at all.

**Never use it to decide a dose, a correction, or any other treatment.** For
that, use the approved sensor manufacturer's readout device.

Pancra is *NOT* affiliated with, endorsed by or supported by Dexcom or
LifeScan. Dexcom, G7 and Stelo are trademarks of Dexcom, Inc.; OneTouch and
Verio are trademarks of LifeScan.

The optional sensor push (off by default) is *unencrypted*: if enabled, anyone
can see your data. Secure cloud sync TBD.

## What it does

- **Several sensors at once, so sessions can overlap.** Start the replacement
  sensor before the current one expires and it warms up while the old one is
  still reporting — no blind gap at the changeover. Each device keeps its own
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
- **Optional push to a server you run.** Off by default.

## Building

Needs clang/LLVM 19 with the `aarch64-linux-android29` target, a JDK, the
Android SDK build tools (`aapt`, `zipalign`, `apksigner`), `adb`, and
`framework-res.apk`. Put `android.jar` and `r8.jar` in `tmp/tools/`.

    make            # builds and signs build/pancra.apk
    make install    # adb install
    make check      # format, static analysis, test suites

Native C against the NDK, no third-party libraries.

## Licence

Copyright 2026 Jakob Kastelic. GPL-3.0.
