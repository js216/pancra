# Pancra

Pancra is a readout app for Dexcom G7, Stelo and OneTouch Verio devices. With
several CGMs paired at the same time, a new sensor can warm up while the old one
is still reporting. It supports two independent alarm levels, a one time nudge
and a persistent alarm, both for high and for low glucose levels.

The other half of this repository is `srv/` -- a small glucose sync server the
app pairs with. One binary and one sqlite file: no daemons, no proxy, no
runtime.

It is written for this app, not as a general service: the pages render glucose
and insulin, the pairing is the app's EC-J-PAKE exchange, and the wire protocol
is the one `srv/sync.h` and `app/sync.h` define between them. What IS general is
underneath -- `lib/` is a self-contained set of primitives (P-256, AES-GCM,
SHA-256, HMAC, HKDF, PBKDF2, ECDSA, J-PAKE), and every file in it compiles
standalone with only `lib/` on the include path.

## What it Does

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
  with every request signed. See **The server** below.

## Building the App

Prerequisites:

    sudo apt-get install clang-19 llvm-19 default-jdk adb aapt \
                         android-sdk-build-tools android-framework-res \
                         clang-format clang-tidy zip

`make check` additionally runs the server's suite, which needs:

    sudo apt-get install gcc curl sqlite3 openssl python3

Those are not optional extras: without `openssl` the TLS tests cannot run, and
without `python3` neither can the forged-record case. Both now FAIL rather than
skip quietly -- set `ALLOW_SKIP=1` if you genuinely mean to accept an untested
TLS layer. The same applies to `duocheck`, which compiles the riscv64 build
that actually ships to the board.

The compiler, strip and JDK are discovered from `PATH`; override any of them
explicitly if you have several installed:

    make CLANG=/usr/bin/clang-19 JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64

`android.jar` and `r8.jar` are not packaged; put them in `tools/` along with
`bundletool.jar` if you want an `.aab`. Nothing in `tools/` is in git.

    make            # both halves
    make app        # builds and signs build/app/pancra.apk
    make install    # adb install
    make check      # format, static analysis, both test suites
    make clean      # rm -rf build

### The Server

Prerequisites:

    sudo apt-get install build-essential curl unzip

sqlite is the one library the server does not carry itself. It goes in
`tools/` with everything else that is downloaded rather than written:

    cd tools
    curl -O https://sqlite.org/2024/sqlite-amalgamation-3460100.zip
    unzip sqlite-amalgamation-3460100.zip

For the Milk-V Duo, the riscv64/musl cross-compiler goes beside it:

    cd tools
    curl -O https://musl.cc/riscv64-linux-musl-cross.tgz
    tar xf riscv64-linux-musl-cross.tgz

    make srv                            # native
    make duo                            # riscv64, static, for the board
    make srv CROSS=<prefix>             # any other cross target

Usage of the CLI:

    ./build/srv/sync 8444 [datadir] [cert.pem key.pem]
    ./build/srv/sync invite  [owner-email]  # print a signup link
    ./build/srv/sync invites                # list the live ones
    ./build/srv/sync revoke  <url|token|all>  # take one back
    ./build/srv/sync adduser <email> <password>  # the first account
    ./build/srv/sync passwd  <email> <password>  # the only password reset

## Legal

Pancra is a **wellness app**. It is not a medical device, it is not approved by
any regulator, and it has not been tested to any clinical standard. Readings
can arrive late, wrong, or not at all.

**Never use it to decide a dose, a correction, or any other treatment.** For
that, use the approved sensor manufacturer's readout device.

Pancra is *NOT* affiliated with, endorsed by, or supported by Dexcom or
LifeScan. Dexcom, G7 and Stelo are trademarks of Dexcom, Inc.; OneTouch and
Verio are trademarks of LifeScan.

Copyright 2026 Jakob Kastelic. GPL-3.0 only.
