# SPDX-License-Identifier: GPL-3.0
# pancra: plain-C Android app, no Gradle. `make run` builds, installs, launches.
# DISCOVERED, not hardcoded. These were absolute Debian paths with no ?= and
# no guard, so on any other distribution make died with a bare "No such file or
# directory" naming a compiler the developer never asked for. `make duo` and
# `make aab` already check their tool and point at the README; these do too.
# Override any of them on the command line: `make CLANG=/path/to/clang`.
# THE COMPILER CACHE, IF THERE IS ONE.
#
# Every host test recipe compiles its whole source list in ONE cc invocation,
# from scratch, on every invocation -- `make modeltest` rebuilds sixty
# translation units whether or not anything changed, and `make uitest` and
# `make modeltest` recompile the eighteen files they share TWICE between them.
# Measured on this tree: 35 s and 32 s respectively, with no single unit
# dominating (the spread is 0.2 s to 1.7 s, so there is nothing to optimise
# file by file).
#
# ccache turns the repeat into a hash lookup. Measured on app/dexdriver.c, the
# slowest unit here: 1863 ms cold, 16 ms warm -- and almost every compile in a
# test run is warm, because the same files are compiled again by the next
# target and again by the next run.
#
# ?= so it can be turned off (`make CCACHE=` ) and empty when ccache is not
# installed, in which case the recipes below run the compiler directly and
# nothing changes. It is a PREFIX, not a compiler: it wraps whatever compiler
# the recipe already chose, so the app's clang and the server's gcc both get
# it without either of them being renamed here.
CCACHE    ?= $(shell command -v ccache 2>/dev/null)
CLANG     ?= $(shell command -v clang-19 2>/dev/null || command -v clang)
STRIP     ?= $(shell command -v llvm-strip-19 2>/dev/null || \
                     command -v llvm-strip 2>/dev/null || echo strip)
# THE JDK, ASKED FOR RATHER THAN INFERRED FROM A SYMLINK.
#
# This was `dirname $(dirname $(readlink -f $(command -v javac)))`. `readlink
# -f` is a GNU extension -- it does not exist on macOS or the BSDs, where the
# whole expression silently yields nothing and the build stops with "no JDK
# found" on a machine that has one. And the two dirnames encode a LAYOUT
# (bin/javac under the home), which is not something a JDK promises.
#
# The JVM already knows where it lives, and every JDK answers the same way. No
# symlink walk, no layout assumption, no GNU-only tool.
JAVA_HOME ?= $(shell java -XshowSettings:properties -version 2>&1 | \
                     sed -n 's/^ *java\.home = *//p')
TARGET    := aarch64-linux-android29
# WHICH PLATFORM SUBDIRECTORY jni_md.h lives in.
#
# A JDK ships <jni.h> beside a per-platform header in include/<platform>. The
# directory names are the JDK's own -- linux, darwin, win32, aix, solaris --
# and they are NOT `uname -s` lower-cased: a JDK under MSYS or Cygwin reports
# `mingw64_nt-10.0` or `cygwin_nt-10.0` and ships `include/win32`, so deriving
# the path from uname builds an -I that names a directory no JDK has. FreeBSD
# and the other BSDs ship `include/freebsd` etc., which do match, but matching
# by accident is not a mapping.
#
# So: an explicit table, and an unknown host is an ERROR with the one thing
# the person can do about it. It was hard-coded to `linux` before this, which
# at least failed loudly on macOS; a wrong-but-plausible path fails as
# "jni_md.h: No such file", three includes deep, in a file nobody edited.
JNI_UNAME := $(shell uname -s)
JNI_PLATFORM := $(strip \
  $(if $(filter Linux,$(JNI_UNAME)),linux, \
  $(if $(filter Darwin,$(JNI_UNAME)),darwin, \
  $(if $(filter FreeBSD,$(JNI_UNAME)),freebsd, \
  $(if $(filter NetBSD,$(JNI_UNAME)),netbsd, \
  $(if $(filter OpenBSD,$(JNI_UNAME)),openbsd, \
  $(if $(filter AIX,$(JNI_UNAME)),aix, \
  $(if $(filter SunOS,$(JNI_UNAME)),solaris, \
  $(if $(filter CYGWIN_NT% MINGW% MSYS%,$(JNI_UNAME)),win32,)))))))))
# THE JDK INCLUDES, IN ONE PLACE. The app build, the host tests and clang-tidy
# each used to spell this out, and two of them spelled it
# /usr/lib/jvm/default-java -- so a developer who set JAVA_HOME built the app
# against one JDK and linted it against another, or against none.
JAVA_INC  := -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/$(JNI_PLATFORM)

# An unknown host, said once and in full, rather than an -I naming a
# directory that does not exist. Only when the APP is being built: the server
# needs no JDK at all (see require-jdk below).
# -I, not -iquote: the app builds freestanding and its own <stdio.h>,
# <string.h> and friends in app/ are what angle-bracket includes must resolve
# to. lib/ joins them because the shared sources live there now.
JNI_INC   := -Iapp -Ilib $(JAVA_INC)
# Every warning on, warnings are errors. Keep this list in sync with what the
# code actually satisfies -- the rule is to fix the cause, never to silence.
WARN      := -Werror -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
             -Wmissing-prototypes -Wwrite-strings -Wvla -Wformat=2 -Wundef \
             -Wdouble-promotion -Wcast-qual -Wswitch-enum -Wredundant-decls
# -std=gnu11 EXPLICITLY. There was no -std= anywhere, so the language was
# whatever the installed compiler happened to default to -- and a toolchain
# upgrade that moves the default to gnu23 would quietly change `bool`, empty
# parameter lists and `auto` under 35k lines. gnu, not c11: srv/ needs the
# POSIX surface (clock_gettime, strncasecmp, gmtime_r, timegm, PATH_MAX), and
# under a strict -std=c11 the server does not compile at all.
STD       := -std=gnu11
CFLAGS    := $(STD) -Os -ffreestanding -fno-stack-protector -fno-unwind-tables \
             -fno-asynchronous-unwind-tables -fvisibility=hidden $(WARN) $(JNI_INC)
FRAMEWORK ?= /usr/share/android-framework-res/framework-res.apk
# Recursive (=, not :=) so both can name the jars through the variables that
# the packaging rules DECLARE as prerequisites, which are defined further
# down. Spelling `tools/r8.jar` here and $(R8_JAR) in a prerequisite list is
# two spellings of one input, and they only stay equal by luck.
D8         = java -cp $(R8_JAR) com.android.tools.r8.D8
# javac: only -target 8 still allows -bootclasspath; d8 desugars 8 fine
JAVACFLAGS = -Xlint:-options -source 8 -target 8 -bootclasspath $(ANDROID_JAR)

# One sentence per missing prerequisite, rather than a failure from inside a
# recipe that names a path nobody chose.
#
# ...BUT ONLY FOR THE HALF THAT NEEDS THEM. These two were unconditional, and
# make evaluates the whole file before it looks at the goal -- so on a server
# with gcc, sqlite and no Android SDK, `make srv` died with "no clang found"
# and `make duo` with "no JDK found", both for a build that uses neither. The
# README documents that half as needing nothing but build-essential and curl,
# and it was documenting something that could not happen.
#
# WHICH TOOLS ARE NEEDED IS A PROPERTY OF THE RULE, not of the command line.
#
# This used to be decided at PARSE time: every requested goal was tested
# against a hand-maintained SRV_ONLY_GOALS allowlist, and $(error) fired if
# any goal was not on it. It failed in both directions, and each failure was
# an edit in a distant place that nobody making the change would think of:
#
#   - a new SERVER test or deployment target was absent from the list, so it
#     demanded Clang and a JDK for a build that uses neither -- the exact
#     complaint the README says cannot happen;
#   - the list was the only thing keeping the app half honest, so it had to be
#     revisited whenever a target was added on EITHER side.
#
# The requirement now hangs off the rules that run the tool, as an ORDER-ONLY
# prerequisite: `| require-clang` on what invokes $(CLANG), `| require-jdk` on
# what invokes javac, d8, or a JNI include path. Order-only so it can never
# count as newer than a target and force a rebuild.
#
# So the check runs when the tool is about to be used and not before, it names
# itself, and a new target on either side needs no second edit anywhere.
# `make srv` on a machine with no Android toolchain never reaches either rule.
#
# srvonlycheck is what holds this honest, and it had to change with it: a
# recipe-based guard is INVISIBLE to `make -n`, which prints recipes without
# running them, so the old "`make -n app` must fail" test would have passed
# over a guard that had been deleted. It now proves the guard both WORKS (a
# real `make require-clang CLANG=` fails) and is WIRED (`make -n app` prints
# it), which is strictly more than the parse-time version could establish.
.PHONY: require-clang require-jdk

require-clang:
	@[ -n "$(strip $(CLANG))" ] || { \
	   echo "no clang found: install clang, or run \`make CLANG=/path/to/clang\`"; \
	   echo "  -- see README. (The SERVER builds without it: try \`make srv\`.)"; \
	   exit 1; \
	 }

require-jdk:
	@[ -n "$(strip $(JAVA_HOME))" ] || { \
	   echo "no JDK found: install a JDK (javac must be on PATH), or run"; \
	   echo "  \`make JAVA_HOME=/path/to/jdk\` -- see README. (The SERVER"; \
	   echo "  builds without it: try \`make srv\`.)"; \
	   exit 1; \
	 }
	@[ -n "$(strip $(JNI_PLATFORM))" ] || { \
	   echo "unknown host '$(JNI_UNAME)': a JDK keeps jni_md.h in"; \
	   echo "  include/<platform> (linux, darwin, win32, freebsd, ...) and"; \
	   echo "  this build does not know which one that is here -- add it to"; \
	   echo "  JNI_PLATFORM in the Makefile, or run \`make JNI_PLATFORM=<dir>\`"; \
	   exit 1; \
	 }

APK  := build/app/pancra.apk
LIB  := build/app/apk/lib/arm64-v8a/libpancra.so
DEX  := build/app/apk/classes.dex
# kept OUTSIDE build/ so `make clean` can't wipe it -- a regenerated key changes
# the APK signature and blocks in-place updates (forcing an uninstall/data loss)
KEY  := tools/debug.keystore

# stub .so's satisfy the linker; the phone's dynamic linker binds the real
# bionic libs at runtime (stub_*.c hold only the symbol names we reference)
STUBS := build/app/stub/libc.so build/app/stub/libandroid.so build/app/stub/liblog.so

# launcher icon + any future resources (compiled into the APK via aapt -S)
# EVERY file under app/res, not just the .xml ones. aapt is handed the whole
# directory with -S, so a .png launcher icon or a .9.png is every bit as much
# an input as an .xml -- and `*/*.xml` declared none of them. Android's
# resource layout is exactly one level deep (res/<type>/<file>), so `*/*` is
# the complete set rather than a wider guess.
RES := $(wildcard app/res/*/*)

# The manifest ships release-safe (no android:debuggable). Dev builds flip it on
# with aapt --debug-mode so crash.log is retrievable on-device; the release
# artifact below never passes it at all.
AAPT_DEBUG := --debug-mode

# =====================================================================
# THE FOUR NUMBERS THAT IDENTIFY A PACKAGE, AS EXPLICIT BUILD INPUTS
# ---------------------------------------------------------------------
# versionCode, versionName, minSdkVersion and targetSdkVersion used to live
# as literals in app/AndroidManifest.xml -- versionCode="1" and
# targetSdkVersion="34" -- and the AAB rule ALSO passed
# `--min-sdk-version 29 --target-sdk-version 34` to aapt2, which looked like
# a second place the target SDK was configured and was not one.
#
# MEASURED, because the whole design turns on it. aapt v0.2-debian and
# aapt2 2.19-debian both treat --version-code, --version-name,
# --min-sdk-version and --target-sdk-version as INJECT-IF-ABSENT:
#
#   attribute present in the manifest -> the flag is parsed, accepted, and
#                                        SILENTLY DISCARDED; the manifest wins
#   attribute absent from the manifest -> the flag is what lands in the package
#
# So those AAB flags were dead text. Anybody retargeting this app by editing
# them would have rebuilt, seen no error, and shipped 34 -- and `aapt dump
# badging` on the result was the only thing in the world that would have said
# so. Which is why the manifest now carries NONE of the four (see the comment
# at the top of it), every path passes all four, and `versioncheck` refuses
# the manifest if one comes back: a returning attribute does not conflict
# with these variables, it replaces them without a word.
#
# WHY THE VERSION CODE IS THE ONE THAT CANNOT BE FIXED LATER. Android will
# not install a package whose version code does not exceed the installed
# one. With "1" nailed into the manifest, artifact #2 could not update
# artifact #1 on any phone; the user is shown an install failure, and the
# only route past it is uninstall, which deletes this app's private storage
# -- their entire glucose, insulin and meal history. app/published.mk is the
# ledger of what has actually been distributed, and versioncheck requires
# every release to exceed it.
include app/published.mk

# Overridable so a release can name its own; defaulted from the ledger so the
# ordinary case is right without anybody typing a number.
VERSION_CODE ?= $(NEXT_VERSION_CODE)
VERSION_NAME ?= 0.1

# THE SDK LEVELS.
#
# MIN_SDK is a compatibility floor and a code contract at once: it is what
# aapt writes as minSdkVersion, what d8 is told with --min-api, and what
# app/Ble.java's version guards are written against (see the API-33 note
# there). One variable so those three cannot drift.
MIN_SDK ?= 29
#
# TARGET_SDK is what the app tells Android it was written for, and Play makes
# it a condition of publishing: an app targeting too low an API cannot be
# updated on the store at all. It was 34, which stopped satisfying that
# requirement on 2025-08-31 when the floor moved to 35.
#
# THIS NUMBER HAS AN EXPIRY DATE AND THIS BUILD CANNOT LOOK IT UP. Play's
# rule is "target the API level from the previous year's release", so the
# floor moves every August. 35 satisfies it as of this writing; the next step
# is 36 (Android 16), from 2026-08-31. Before a release, check the current
# requirement in the Play Console -- no gate in this repository can, and a
# check that guesses would be a check that lies.
#
# AND RAISING IT IS NOT ONLY A NUMBER. targetSdkVersion is how an app opts in
# to a platform generation's behaviour changes -- at 35, Android 15 enforces
# edge-to-edge window insets on the activity, which for this app is where the
# glucose number and the buttons are drawn. That must be looked at on a real
# phone running that release. It has not been. See TODO 117.
TARGET_SDK ?= 35
PLAY_TARGET_SDK_MIN ?= 35
#
# COMPILE_SDK is not ours to choose: aapt stamps compileSdkVersion into the
# package from whatever framework-res.apk it was given with -I, and javac/d8
# compile against tools/android.jar. It is declared anyway, and asserted
# against the PACKAGED value, so that replacing either input shows up as a
# failure naming this line instead of as a silently different package. It is
# below TARGET_SDK here (Debian's framework-res is API 33), which is normal
# -- an app may target an API it has no stubs for, it just cannot call the
# new methods -- and is exactly the sort of thing that should be visible.
COMPILE_SDK ?= 33

# What every packaging path passes, so there is one spelling of it.
SDK_FLAGS  := --min-sdk-version $(MIN_SDK) --target-sdk-version $(TARGET_SDK)
VER_FLAGS  := --version-code $(VERSION_CODE) --version-name $(VERSION_NAME)

# =====================================================================
# THE PACKAGING TOOLCHAIN, DECLARED RATHER THAN ASSUMED
# ---------------------------------------------------------------------
# Every rule below this point consumes tools and SDK files that were named
# only inside recipes: tools/android.jar, tools/r8.jar, tools/bundletool.jar,
# framework-res.apk, aapt, aapt2, zipalign, apksigner. Make cannot see a path
# that appears only in a command line, so replacing any of them left the APK,
# the DEX and the AAB reported up to date -- and the thing you then tested was
# not the thing your tools would now build. Every one of them is a prerequisite
# now.
#
# THERE ARE TWO KINDS OF INPUT HERE, and only one of them is a file:
#
#   FILES. The jars, framework-res.apk, and the resolved path of each
#   executable. `command -v aapt` yields /usr/bin/aapt, make stats it, and a
#   package upgrade that rewrites the binary in place moves its mtime. That
#   covers the common case at zero cost.
#
#   VERSIONS. It does NOT cover the case this project actually has.
#   /usr/bin/aapt, /usr/bin/aapt2 and /usr/bin/zipalign are symlinks into
#   /usr/lib/android-sdk/build-tools/debian/, and /usr/bin/apksigner is a
#   thirty-line bash wrapper around a jar somewhere else entirely. Upgrade the
#   SDK and the wrapper script does not change -- same path, same size, same
#   mtime, different tool. The only thing that moves is what the tool SAYS when
#   you ask it, so that is what gets recorded and compared.
#
# The stamps are FORCE targets on the .aapt-mode pattern already used above:
# the recipe runs on every build, but rewrites the file only when the text
# differs, so the mtime -- and therefore every rebuild decision -- moves
# exactly when the toolchain moved.
#
# Overridable, and discovered rather than hardcoded, for the same reason CLANG
# and STRIP are: `make APKSIGNER=/opt/sdk/build-tools/34.0.0/apksigner`.
ANDROID_JAR    := tools/android.jar
R8_JAR         := tools/r8.jar
BUNDLETOOL_JAR := tools/bundletool.jar
AAPT      ?= $(shell command -v aapt 2>/dev/null)
AAPT2     ?= $(shell command -v aapt2 2>/dev/null)
ZIPALIGN  ?= $(shell command -v zipalign 2>/dev/null)
APKSIGNER ?= $(shell command -v apksigner 2>/dev/null)
JAVA      ?= $(shell command -v java 2>/dev/null)
JAVAC     ?= $(shell command -v javac 2>/dev/null)
# Snapshotted with := because a prerequisite list is expanded WHEN THE RULE IS
# READ, and these are recursive: without this each `command -v` would run once
# per rule that mentions it, on every make invocation including `make srv`.
# An empty one (tool absent) contributes no prerequisite and the recipe fails
# naming the tool, which is what it did before.
APKTOOL_BINS  := $(AAPT) $(ZIPALIGN) $(APKSIGNER)
AABTOOL_BINS  := $(AAPT2)
# THE JDK NEEDS NO VERSION PROBE: it ships one. Every JDK since 9 writes a
# `release` file at the top of JAVA_HOME carrying JAVA_VERSION and the build
# id, and an in-place upgrade rewrites it. Asking `javac -version` and
# `java -version` instead costs two JVM startups -- 1.5s -- on every single
# incremental build, to learn what a stat of one file already tells us.
# $(wildcard) so a JDK that does not ship it degrades to the two binary paths
# rather than stopping the build with "No rule to make target".
JDK_INPUTS := $(JAVAC) $(JAVA) $(wildcard $(JAVA_HOME)/release)

# WHAT EACH STAMP ASKS. One shell per stamp; stderr is folded in because
# `java -version` and zipalign's banner go there, and zipalign has no version
# flag at all -- its usage banner is the most identity it will give up, and
# the symlink's mtime in APKTOOL_BINS is what really covers it.
VERCMD_apktools = $(AAPT) version; $(ZIPALIGN); $(APKSIGNER) --version
VERCMD_aabtools = $(AAPT2) version; zip -h 2>&1 | head -2; unzip -v | head -1
# ...and the same trick for the five numbers stamped into the package. They
# are variables, not files, so without this `make VERSION_CODE=7` over an
# existing build reports the APK up to date and installs the old code -- the
# identical hazard .aapt-mode exists for, with a worse symptom: the artifact
# is wrong in exactly the field nobody re-reads, and it is signed and shipped.
VERCMD_pkgmeta  = echo '$(VERSION_CODE) $(VERSION_NAME) $(MIN_SDK) $(TARGET_SDK) $(COMPILE_SDK)'
# $* is an automatic variable, so $(VERCMD_$*) resolves per target when the
# RECIPE is expanded. That is not the $(call)/$(eval) hazard elsewhere in this
# file, where the text is expanded once at read time and a single-dollar
# variable silently becomes the empty string; a pattern rule's recipe is
# expanded per invocation with $* already set.
build/app/.ver-%: FORCE
	@mkdir -p $(@D); v=$$( { $(VERCMD_$*) ; } 2>&1 ); \
	 printf '%s' "$$v" | cmp -s - $@ 2>/dev/null || printf '%s' "$$v" > $@

# The three jars and the framework are inputs nobody can build, so a missing
# one is an instruction, not a "No rule to make target" naming a path the
# reader never chose. A rule with a recipe and no prerequisites never runs
# while the file is there, so this costs nothing when the tree is complete.
# (The AAB rule used to carry the bundletool half of this inline, which helped
# exactly one of its three consumers.)
$(ANDROID_JAR):
	@echo "$@ missing -- the Android API stubs javac and d8 compile against."; \
	 echo "  Copy platforms/android-34/android.jar out of an SDK install."; exit 1

$(R8_JAR):
	@echo "$@ missing -- R8/D8 turns the class files into classes.dex."; \
	 echo "  Get r8.jar from https://maven.google.com/ (com/android/tools/r8)."; exit 1

$(BUNDLETOOL_JAR):
	@echo "$@ missing -- get it from https://github.com/google/bundletool/releases"; \
	 exit 1

$(FRAMEWORK):
	@echo "$@ missing -- aapt needs framework-res.apk to resolve @android:"; \
	 echo "  references. Debian ships it in android-framework-res; elsewhere"; \
	 echo "  point at your own: \`make FRAMEWORK=/path/to/framework-res.apk\`."; \
	 exit 1

# THIS FILE, AS AN INPUT TO THE THINGS IT PACKAGES.
#
# Items 90-92 made every SDK tool a prerequisite because a rule that names a
# tool only inside a recipe leaves make blind to it. The recipe TEXT is the
# same kind of input and was still invisible. Demonstrated: edit the aapt
# link flags in the packaging recipe below to say --target-sdk-version 34,
# run `make app`, and make reports the APK up to date -- the edit does not
# reach the artifact, the build prints nothing, and the package that
# installs is the old one. The version and SDK VARIABLES are covered by
# build/app/.ver-pkgmeta; this covers changing the recipe itself, which is
# what somebody retargeting the app by hand would actually do.
#
# Costs one aapt run per Makefile edit for the APK. The AAB pays five
# minutes, and pays it exactly when its own recipe changed.
MAKEFILE_SELF := $(firstword $(MAKEFILE_LIST))

# Both halves. `make app` or `make srv` for one of them.
all: app srv
app: $(APK)

build/app/stub/lib%.so: app/stub_%.c | require-clang
	@mkdir -p $(@D)
	$(CCACHE) $(CLANG) --target=$(TARGET) -Iapp -shared -nostdlib -fuse-ld=lld \
	    -Wl,-soname,lib$*.so -o $@ $<

# native sources: UI/JNI core, BLE transport, protocol driver, self-contained crypto
# THE SOURCE GROUPS, by what a file IS -- and reusable, because the same sets
# are linked by the tests.
#
# SRC was one nine-line list in file order, and the model test's link line
# repeated it by hand: adding a file meant editing both, and forgetting the
# second one showed up as an undefined symbol in a test that had been passing.
# Both are built from these now.
#
# The grouping is the app's own layering, not a filing convention:
#   UI_SRC     draws and maps taps -- pure, and driven offline by uitest
#   LOGIC_SRC  decisions with no I/O at all: each has its own host test
#   DATA_SRC   the files on disk and the state read back from them
#   SHELL_SRC  the workflows: threads, JNI, the driver, the screens' actions
#   DEX_SRC    the sensor and meter protocols
#   CRYPTO_SRC lib/: one algorithm per file, shared with the server
UI_SRC := app/uirender.c app/uidraw.c app/uimain.c app/uidev.c app/uiconfirm.c \
          app/uimenu.c app/uikeypad.c app/uilog.c app/uifood.c app/font.c
LOGIC_SRC := app/alarmlogic.c app/scanlogic.c app/ingest.c app/senslogic.c \
             app/meterlogic.c app/syncstat.c app/plotdata.c app/keypad.c \
             app/gesturelogic.c app/civil.c
DATA_SRC := app/store.c app/settings.c app/stats.c app/insulin.c \
            app/weight.c app/exercise.c app/food.c app/sensors.c app/meterstore.c app/metersess.c app/util.c app/clock.c app/selection.c app/sesscache.c
SHELL_SRC := app/main.c app/jbridge.c app/notify.c app/alarm.c app/nav.c \
             app/forms.c app/meter.c app/scan.c app/pairing.c app/menu.c \
             app/device.c app/input.c app/model.c app/reading.c \
             app/reconcile.c app/remote.c app/calib.c app/crashlog.c \
             app/tzoff.c
DEX_SRC := app/dexble.c app/bondtable.c app/dexdriver.c app/dexcom.c app/dexdata.c app/otble.c
CRYPTO_SRC := lib/jpake.c lib/rand.c lib/randunix.c lib/p256.c lib/sha256.c lib/aes.c \
              lib/ecdsa.c lib/hmac.c lib/ct.c
SYNC_SRC := app/sync.c app/syncrow.c app/syncjni.c

SRC := $(UI_SRC) $(LOGIC_SRC) $(DATA_SRC) $(SHELL_SRC) $(DEX_SRC) \
       $(CRYPTO_SRC) $(SYNC_SRC) lib/plot.c

HDR := $(wildcard app/*.h lib/*.h)

# ONE OBJECT PER SOURCE, not one clang invocation over all of them. The single
# invocation could not be parallelised -- `make -j4` had exactly one recipe to
# run -- and could not be incremental, so a one-line edit to ui.c recompiled
# main.c's nine thousand lines with it.
#
# Header dependencies come from -MMD, which is STRICTER than the old trick of
# listing every header as a prerequisite of everything: that rebuilt the world
# on any header edit (correct but slow), and would have missed a header nobody
# remembered to add to HDR. Now each object knows exactly which headers it
# read.
# The object path keeps the source's directory (build/app/obj/app/main.o,
# build/app/obj/lib/plot.o) so a file in app/ and one in lib/ can never write
# to the same object, and so every product of a build lives under build/.
OBJ := $(patsubst %.c,build/app/obj/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

build/app/obj/%.o: %.c | require-clang require-jdk
	@mkdir -p $(@D)
	$(CCACHE) $(CLANG) --target=$(TARGET) $(CFLAGS) -MMD -MP -c -o $@ $<

# READ what -MMD writes. Without this the .d files are generated and ignored,
# so editing a header rebuilt NOTHING in the app: a change to a struct in
# ui.h would leave every object that was not itself edited holding the old
# layout, and the linker would join them without complaint. The server half
# has always had its `-include $(SRVDEP)`; the app half never did, while the
# comment above claimed "each object knows exactly which headers it read".
-include $(DEP)

$(LIB): $(OBJ) $(STUBS) | require-clang
	@mkdir -p $(@D)
	$(CCACHE) $(CLANG) --target=$(TARGET) -shared -nostdlib -fuse-ld=lld \
	    -Wl,--no-undefined -Lbuild/app/stub -lc -landroid -llog -o $@ $(OBJ)
	$(STRIP) $@

# THE CLASS DIRECTORY IS EMPTIED FIRST, because $(DEX) below GLOBS it: a class
# that has been renamed, moved to another file, or deleted leaves its .class
# behind, and d8 packages the ghost into the APK. That is not hypothetical --
# moving one inner class from PancraService to BoundaryLogic shipped a dead
# PancraService$$Notif alongside the live BoundaryLogic$$Notif, which is a second
# copy of exactly the rule that change existed to make single-definition. javac
# overwrites what it recompiles and knows nothing about what it no longer emits.
#
# ...AND IT IS EMPTIED BY BUILDING SOMEWHERE ELSE FIRST, not by deleting it.
# `rm -rf build/app/classes` immediately before javac fixes the ghost and
# introduces a worse failure, because it destroys the good output BEFORE
# knowing whether a replacement can be produced:
#
#   A javac that FAILS leaves no classes at all. The tree is now less built
#   than it was before a command that did nothing but report an error.
#
#   A javac that is INTERRUPTED -- ^C, a full disk, the OOM killer -- leaves
#   the classes it had already written, and Ble.class is the first of the five
#   sources on the command line. That partial directory carries a marker
#   NEWER THAN EVERY SOURCE, so the next `make` asks javac for nothing, d8
#   packages whatever survived, and the APK installs and dies on the phone at
#   the first reference to a class that never got written. Nothing in the
#   build reports anything; the previous build's error scrolled past hours ago.
#
# So: compile into a fresh directory that nothing reads, and put it in place
# only after javac has succeeded. The good output survives every failure, and
# the marker this rule is named for appears at a rename -- meaning it exists
# only when the whole set behind it does. `.new` is removed first rather than
# reused, so the set is exactly this javac's output and cannot inherit a
# straggler from an interrupted run either.
#
# The swap is two renames, not one, because a directory cannot be renamed onto
# a non-empty directory. The window between them is a rename apart and neither
# leaves the marker pointing at a partial set: after the first, `classes` does
# not exist (make rebuilds); after the second it is complete.
CLASSDIR := build/app/classes
JAVA_SRC := app/Ble.java app/PancraService.java app/Alarm.java app/PancraFiles.java app/BoundaryLogic.java app/ExportSnapshot.java
$(CLASSDIR)/com/jk/pancra/Ble.class: $(JAVA_SRC) $(ANDROID_JAR) $(JDK_INPUTS) | require-jdk
	rm -rf $(CLASSDIR).new $(CLASSDIR).old
	mkdir -p $(CLASSDIR).new
	javac $(JAVACFLAGS) -d $(CLASSDIR).new $(JAVA_SRC)
	{ [ ! -d $(CLASSDIR) ] || mv $(CLASSDIR) $(CLASSDIR).old; } && \
	    mv $(CLASSDIR).new $(CLASSDIR) && rm -rf $(CLASSDIR).old

# EVERY class javac emitted, found rather than globbed. The glob was
# `$(CLASSDIR)/com/jk/pancra/*.class`, which is one package deep: a source
# that declares any other package -- or a future com.jk.pancra.sync -- compiles
# cleanly, lands in a subdirectory, and is silently left out of classes.dex.
# That failure has no build symptom at all; it is a NoClassDefFoundError on the
# phone. The directory is freshly built by the rule above and holds nothing but
# this javac's output, so "everything under it" is the exact set.
#
# The count is asserted because a find that matches nothing would otherwise
# hand d8 no inputs and produce a valid, EMPTY classes.dex.
$(DEX): $(CLASSDIR)/com/jk/pancra/Ble.class $(R8_JAR) $(ANDROID_JAR) $(JDK_INPUTS)
	@mkdir -p $(@D)
	@n=$$(find $(CLASSDIR) -name '*.class' | wc -l); [ "$$n" -gt 0 ] || \
	   { echo "$(DEX): no .class files under $(CLASSDIR)"; exit 1; }
	$(D8) --release --min-api $(MIN_SDK) --lib $(ANDROID_JAR) --output $(@D) \
	    $$(find $(CLASSDIR) -name '*.class')

$(KEY):
	keytool -genkeypair -keystore $@ -alias debug -keyalg RSA -keysize 2048 \
	    -validity 10000 -storepass android -keypass android -dname CN=debug

# Mode stamp: AAPT_DEBUG isn't a file, so without this a `make` after `make
# release` (or vice-versa) would NOT rebuild the APK -- leaving the wrong
# debuggable flag installed. The stamp's contents change with the mode, forcing
# a rebuild exactly when it flips.
FORCE:
build/app/.aapt-mode: FORCE
	@mkdir -p $(@D); printf '%s' '$(AAPT_DEBUG)' | cmp -s - $@ 2>/dev/null \
	    || printf '%s' '$(AAPT_DEBUG)' > $@

# THE DEV APK. Debuggable, signed with the checked-in public-password debug
# key, and it is the only thing `make install`/`make run` ever puts on a phone.
$(APK): app/AndroidManifest.xml $(LIB) $(DEX) $(KEY) $(RES) build/app/.aapt-mode \
        build/app/.ver-pkgmeta $(MAKEFILE_SELF) $(FRAMEWORK) $(APKTOOL_BINS) build/app/.ver-apktools
	aapt package -f -M app/AndroidManifest.xml -S app/res -I $(FRAMEWORK) $(AAPT_DEBUG) \
	    $(VER_FLAGS) $(SDK_FLAGS) -F build/app/pancra.unaligned.apk
	cd build/app/apk && aapt add ../pancra.unaligned.apk lib/arm64-v8a/libpancra.so classes.dex
	zipalign -f -p 4 build/app/pancra.unaligned.apk $@
	apksigner sign --ks $(KEY) --ks-pass pass:android $@

# =====================================================================
# THE RELEASE ARTIFACT, AND WHY IT IS NOT THE DEV ONE WITH A FLAG FLIPPED
# ---------------------------------------------------------------------
# `make release` used to rebuild build/app/pancra.apk with AAPT_DEBUG empty,
# sign it through $(KEY) -- the debug keystore this repository CONTAINS, whose
# password is the word "android" and whose subject is CN=debug -- and then
# print a reminder to sign it with the real key. It printed the reminder in
# green, next to the word "release", after the build had succeeded.
#
# WHAT THAT SHIPS. An APK's signer is its identity to Android. Every one of
# these follows from the wrong one, and none of them is visible in the file:
#
#   - Installed over the real app, it is refused: INSTALL_FAILED_UPDATE_
#     INCOMPATIBLE, signatures do not match. The user's glucose history is
#     behind that failure, and the only way past it is uninstall -- which
#     deletes the history.
#   - Installed on a clean phone it works perfectly, and is then a DIFFERENT
#     application that can never be updated by, or share data with, the one
#     the user's other devices are running.
#   - Uploaded to Play it is rejected, but only after upload, and only if
#     somebody reads the error.
#   - Anybody at all can sign an update with the key in this repository.
#
# And the artifact carried the same PATH as the dev build, so the reminder was
# the only thing distinguishing them: `make release` followed by `make install`
# installed the release, and `make install` on its own could rebuild it back
# into a debuggable one without saying so.
#
# SO THE RELEASE PATH FAILS CLOSED. It has its own artifact, it will not run
# without the three production inputs, it refuses the debug keystore by name,
# and what it produced is verified against the fingerprint that was demanded
# of it BEFORE the file is allowed to survive. A reminder is not a gate.
#
#   RELEASE_KEY     path to the production keystore. Kept outside this tree.
#   RELEASE_ALIAS   the key alias inside it.
#   RELEASE_SIGNER  the SHA-256 certificate digest the signed APK must have.
#                   This is the whole point: it is what makes the check say
#                   "signed by the production signer" rather than "signed".
#                   Read it once from your own keystore and record it where
#                   your release procedure lives:
#                     keytool -list -v -keystore <ks> -alias <alias> | grep SHA256
#   RELEASE_KS_PASS / RELEASE_KEY_PASS
#                   optional, passed to apksigner verbatim, so `file:...` or
#                   `env:...`. Omitted, apksigner PROMPTS, which is the right
#                   default for a key that is meant to be protected. `pass:`
#                   is refused: a command line is world-readable in ps(1), and
#                   a password that reaches a Makefile has already leaked into
#                   the shell history of whoever typed it.
RELEASE_APK   := build/app/pancra-release.apk
RELEASE_KEY   ?=
RELEASE_ALIAS ?=
RELEASE_SIGNER ?=
RELEASE_KS_PASS ?=
RELEASE_KEY_PASS ?=
# Which APK releasecheck inspects. Its own by default; overridable so the
# verification can be pointed at a candidate artifact from anywhere -- and so
# that this gate can be tested against APKs it MUST refuse.
VERIFY_APK ?= $(RELEASE_APK)
# A sub-make that re-reads THIS makefile. Plain $(MAKE) re-reads whichever
# file make picks by default, which is a different file the moment anyone
# runs `make -f something-else` -- and the two would then disagree about what
# releasecheck means while reporting on the same artifact. -f is not inherited
# through MAKEFLAGS, so it has to be said.
MAKESELF = $(MAKE) -f $(firstword $(MAKEFILE_LIST))


# VERIFIED EVEN WHEN NOTHING HAD TO BE BUILT. The verification lived only in
# the recipe at first, and a recipe does not run for a target make considers
# up to date -- so a second `make release` over an existing artifact printed
# nothing, exited 0, and had inspected nothing. An artifact left behind by an
# earlier invocation with different variables is exactly the case that most
# needs looking at.
release: $(RELEASE_APK)
	@$(MAKESELF) --no-print-directory releasecheck VERIFY_APK=$(RELEASE_APK) \
	    RELEASE_SIGNER='$(RELEASE_SIGNER)'

# ...AND THE CONFIGURATION IS CHECKED FOR THE SAME REASON, in a target of its
# own rather than at the top of the recipe. Order-only, on the require-clang /
# require-jdk pattern above: it runs whenever the release APK is CONSIDERED,
# including when it is up to date, and being phony it still cannot make the
# artifact look out of date.
release-config:
	@[ -n "$(RELEASE_KEY)" ] && [ -n "$(RELEASE_ALIAS)" ] && [ -n "$(RELEASE_SIGNER)" ] || { \
	   echo "release: production signing is not configured, so there is nothing"; \
	   echo "  to build. A release APK signed with anything else is not a"; \
	   echo "  release APK -- see the comment above this rule for what it costs."; \
	   echo "  make release RELEASE_KEY=/path/to/upload.jks \\"; \
	   echo "               RELEASE_ALIAS=upload \\"; \
	   echo "               RELEASE_SIGNER=<sha256 digest of that key's cert>"; \
	   exit 1; \
	 }
	@[ -f "$(RELEASE_KEY)" ] || { \
	   echo "release: RELEASE_KEY=$(RELEASE_KEY) is not a file."; exit 1; }
	@[ "$$(cd $$(dirname $(RELEASE_KEY)) && pwd)/$$(basename $(RELEASE_KEY))" \
	   != "$$(cd $$(dirname $(KEY)) && pwd)/$$(basename $(KEY))" ] || { \
	   echo "release: RELEASE_KEY is $(KEY) -- the debug keystore that ships"; \
	   echo "  in this repository, password 'android', subject CN=debug."; \
	   echo "  It is a development key by construction and cannot be a"; \
	   echo "  production one by being named in a different variable."; \
	   echo "  (Resolved through the directory, so a relative path, an"; \
	   echo "  absolute one and a symlink to it are all the same file.)"; \
	   exit 1; \
	 }
	@case "$(RELEASE_KS_PASS) $(RELEASE_KEY_PASS)" in pass:*|*" pass:"*) \
	   echo "release: a keystore password on the command line is visible to"; \
	   echo "  every process on the machine (ps) and stays in shell history."; \
	   echo "  apksigner reads 'file:<path>', 'env:<var>' and 'stdin'; with"; \
	   echo "  neither variable set it prompts, which is what you want."; \
	   exit 1;; esac

# No .aapt-mode here, and no $(AAPT_DEBUG): the release mode is not a mode,
# it is this rule. Depending on the stamp would also flip it under the dev
# APK's feet and force that to rebuild every time the two are alternated.
$(RELEASE_APK): app/AndroidManifest.xml $(LIB) $(DEX) $(RES) \
                build/app/.ver-pkgmeta $(MAKEFILE_SELF) $(FRAMEWORK) $(APKTOOL_BINS) \
                build/app/.ver-apktools | release-config versioncheck
	aapt package -f -M app/AndroidManifest.xml -S app/res -I $(FRAMEWORK) \
	    $(VER_FLAGS) $(SDK_FLAGS) -F build/app/pancra-release.unaligned.apk
	cd build/app/apk && aapt add ../pancra-release.unaligned.apk lib/arm64-v8a/libpancra.so classes.dex
	zipalign -f -p 4 build/app/pancra-release.unaligned.apk $@
	@# ...and if SIGNING itself fails, the aligned-but-unsigned file zipalign
	@# just wrote does not stay. apksigner exits non-zero for a wrong
	@# password, a missing alias, or (with no --ks-pass and no terminal) a
	@# prompt it cannot ask -- and every one of those left a
	@# pancra-release.apk on disk with no signature at all, NEWER than its
	@# prerequisites. The next `make release` would call that up to date and
	@# skip the recipe entirely. releasecheck still refuses it, so nothing
	@# ships; but the artifact a failed build leaves behind should be no
	@# artifact.
	apksigner sign --ks $(RELEASE_KEY) --ks-key-alias $(RELEASE_ALIAS) \
	    $(if $(RELEASE_KS_PASS),--ks-pass $(RELEASE_KS_PASS)) \
	    $(if $(RELEASE_KEY_PASS),--key-pass $(RELEASE_KEY_PASS)) $@ \
	    || { rm -f $@; exit 1; }
	@$(MAKESELF) --no-print-directory releasecheck VERIFY_APK=$@ \
	    RELEASE_SIGNER='$(RELEASE_SIGNER)' || { rm -f $@; exit 1; }
	@printf '\033[1;32mrelease\033[0m: %s signed by %s and verified.\n' "$@" "$(RELEASE_SIGNER)"

# WHAT A SIGNED APK HAS TO SURVIVE BEFORE IT COUNTS AS A RELEASE.
#
# Deliberately a separate target taking VERIFY_APK, because a check that only
# ever runs on output the same recipe just produced cannot be shown to refuse
# anything. Point it at the debug-signed dev APK and it must say no.
#
# It now also runs the full packaged-metadata pass (app/test/apkcheck.sh)
# with REQUIRE_NEWER=1. That is not duplication of `make apkcheck`: apkcheck
# looks at the DEV APK, and the artifact that has to have a version code
# exceeding the published one, a current target SDK and the backup
# exclusions is THIS one -- the file about to be handed to people. Signed
# and correctly identified is not the same as installable over what they
# already have.
#
# The debug-keystore refusal is UNCONDITIONAL and does not consult
# RELEASE_SIGNER, so it still holds when RELEASE_SIGNER is wrong, empty, or
# has been set -- by accident or by a helpful script -- to the digest of the
# key in this repository. That is the one failure mode the fingerprint
# comparison cannot catch, because it is the comparison being satisfied.
releasecheck:
	@set -e; apk="$(VERIFY_APK)"; \
	 [ -f "$$apk" ] || { echo "releasecheck: no such APK: $$apk"; exit 1; }; \
	 [ -n "$(RELEASE_SIGNER)" ] || { \
	   echo "releasecheck: RELEASE_SIGNER is empty, so there is no signer to"; \
	   echo "  verify against and this check would pass on anything."; exit 1; }; \
	 out=$$(apksigner verify --print-certs -v "$$apk" 2>&1) || { \
	   echo "releasecheck: $$apk is not validly signed:"; echo "$$out"; exit 1; }; \
	 echo "$$out" | grep -q '^Verified using v2 scheme.*true' || \
	 echo "$$out" | grep -q '^Verified using v3 scheme.*true' || { \
	   echo "releasecheck: $$apk carries no APK Signature Scheme v2/v3 block."; \
	   echo "  A v1-only (jar-signed) package is verified by reading a"; \
	   echo "  MANIFEST inside the zip, which does not cover the zip's own"; \
	   echo "  structure; Android has required v2 for new uploads since"; \
	   echo "  2021. apksigner emits it by default, so its absence means"; \
	   echo "  somebody turned it off."; \
	   exit 1; }; \
	 got=$$(echo "$$out" | sed -n 's/^Signer #1 certificate SHA-256 digest: *//p' \
	        | tr 'A-F' 'a-f' | tr -d ': '); \
	 [ -n "$$got" ] || { echo "releasecheck: apksigner printed no SHA-256 digest"; \
	   echo "  for signer #1, so nothing was compared:"; echo "$$out"; exit 1; }; \
	 want=$$(printf '%s' '$(RELEASE_SIGNER)' | tr 'A-F' 'a-f' | tr -d ': '); \
	 dbg=$$(keytool -list -v -keystore $(KEY) -storepass android -alias debug 2>/dev/null \
	        | sed -n 's/.*SHA256: *//p' | head -1 | tr 'A-F' 'a-f' | tr -d ': '); \
	 if [ -n "$$dbg" ] && [ "$$got" = "$$dbg" ]; then \
	   echo "releasecheck: $$apk is signed with $(KEY) -- the DEBUG key that"; \
	   echo "  ships in this repository. Installed over the real app it is"; \
	   echo "  refused as a signature mismatch and the only way past that is"; \
	   echo "  an uninstall, which deletes the user's history; installed on a"; \
	   echo "  clean phone it is a different app wearing the same name."; \
	   exit 1; \
	 fi; \
	 [ "$$got" = "$$want" ] || { \
	   echo "releasecheck: $$apk is signed by the wrong key."; \
	   echo "  expected (RELEASE_SIGNER): $$want"; \
	   echo "  actual   (signer #1):      $$got"; \
	   exit 1; }; \
	 badge=$$(aapt dump badging "$$apk") || { \
	   echo "releasecheck: aapt could not read $$apk"; exit 1; }; \
	 if printf '%s' "$$badge" | grep -q "^application-debuggable"; then \
	   echo "releasecheck: $$apk declares android:debuggable."; \
	   echo "  A debuggable release lets any app with RUN_AS, and anybody"; \
	   echo "  with adb, read this app's private files -- which are the"; \
	   echo "  user's glucose history and the sync credentials for it."; \
	   exit 1; \
	 fi; \
	 EXPECT_VERSION_CODE='$(VERSION_CODE)' EXPECT_VERSION_NAME='$(VERSION_NAME)' \
	 EXPECT_MIN_SDK='$(MIN_SDK)' EXPECT_TARGET_SDK='$(TARGET_SDK)' \
	 EXPECT_COMPILE_SDK='$(COMPILE_SDK)' \
	 PUBLISHED_VERSION_CODE='$(PUBLISHED_VERSION_CODE)' REQUIRE_NEWER=1 \
	 ./app/test/apkcheck.sh "$$apk"; \
	 printf '\033[1;32mreleasecheck\033[0m: %s -- v2/v3 signed by %s, not debuggable,\n' \
	   "$$apk" "$$got"; \
	 printf '  version code exceeds the published ledger, target SDK %s verified in the package\n' \
	   '$(TARGET_SDK)'

# THE RELEASE INPUTS, CHECKED BEFORE ANYTHING IS BUILT WITH THEM.
#
# Order-only on the release APK and the AAB, on the release-config pattern
# above: it runs whenever either artifact is CONSIDERED, including when make
# thinks it is up to date, and being phony it cannot make one look stale.
#
# This is the SOURCE side. It cannot prove what a package contains -- only
# aapt/bundletool reading the finished artifact can, which is what
# app/test/apkcheck.sh and app/test/aabcheck.sh do -- and the two halves
# catch DIFFERENT mistakes, which is why both exist:
#
#   a stale link flag  -> the packaged value is wrong -> the packaged check
#   a stale manifest   -> the packaged value is RIGHT, because an attribute
#                         in the manifest wins over the flag silently, and
#                         the packaged check sees nothing wrong at all. Only
#                         a look at the manifest source finds this one.
#
# The first assertion below is that second case, and it is the reason the
# manifest is inspected here rather than trusted.
#
# ...AND IT READS THE MARKUP, NOT THE PROSE. The first cut of this grepped
# the file whole and failed on its own documentation: the comment at the top
# of the manifest EXPLAINS that android:versionCode must not be there, and
# the check called that an occurrence. An assertion that a word is absent
# from a file cannot be an assertion about XML, because the one place the
# word is certain to appear is the note saying why it must not. XMLCODE
# strips comment spans -- including a comment opened partway along a line,
# so a `<application ...> <!-- note -->` cannot hide a real attribute by
# sharing a line with a comment.
XMLCODE = awk '{ s=$$0; out=""; while (length(s)) { \
	    if (inc) { i=index(s,"-->"); if(!i){s="";break} s=substr(s,i+3); inc=0 } \
	    else { i=index(s,"<!--"); if(!i){out=out s; s=""; break} \
	           out=out substr(s,1,i-1); s=substr(s,i+4); inc=1 } } print out }'

versioncheck:
	@set -e; m=app/AndroidManifest.xml; \
	 [ -f $$m ] || { echo "versioncheck: $$m is missing"; exit 1; }; \
	 code=$$($(XMLCODE) $$m); \
	 printf '%s' "$$code" | grep -q '<manifest' || { \
	   echo "versioncheck: after stripping comments $$m has no <manifest>"; \
	   echo "  element left, so the assertions below would all pass on an"; \
	   echo "  empty string. The comment stripper is broken, not the file."; \
	   exit 1; }; \
	 for a in android:versionCode android:versionName; do \
	   if printf '%s' "$$code" | grep -q "$$a"; then \
	     echo "versioncheck: $$m carries $$a."; \
	     echo "  aapt and aapt2 treat --version-code/--version-name as"; \
	     echo "  inject-IF-ABSENT: with the attribute here, the build's"; \
	     echo "  VERSION_CODE=$(VERSION_CODE) is accepted on the command"; \
	     echo "  line and thrown away, with no diagnostic, and whatever is"; \
	     echo "  written here ships instead. Remove it -- the numbers come"; \
	     echo "  from app/published.mk and the Makefile."; \
	     exit 1; \
	   fi; \
	 done; \
	 if printf '%s' "$$code" | grep -q '<uses-sdk'; then \
	   echo "versioncheck: $$m carries a <uses-sdk> element."; \
	   echo "  Same mechanism: it silently overrides --min-sdk-version and"; \
	   echo "  --target-sdk-version, so MIN_SDK=$(MIN_SDK) and"; \
	   echo "  TARGET_SDK=$(TARGET_SDK) would stop reaching the package"; \
	   echo "  while every flag stayed in place and every build succeeded."; \
	   exit 1; \
	 fi; \
	 case "$(VERSION_CODE)" in \
	   ""|*[!0-9]*) echo "versioncheck: VERSION_CODE='$(VERSION_CODE)' is not"; \
	     echo "  a non-negative integer. Android's version code is an int;"; \
	     echo "  aapt would take '1.2' or '' and produce a package nobody"; \
	     echo "  can compare against the last one."; exit 1;; \
	 esac; \
	 case "$(PUBLISHED_VERSION_CODE)" in \
	   ""|*[!0-9]*) echo "versioncheck: PUBLISHED_VERSION_CODE ="; \
	     echo "  '$(PUBLISHED_VERSION_CODE)' in app/published.mk is not an"; \
	     echo "  integer. An unreadable ledger must not compare true: that"; \
	     echo "  is how a gate ends up passing on \"\" == \"\"."; exit 1;; \
	 esac; \
	 exp=$$(expr $(PUBLISHED_VERSION_CODE) + 1); \
	 [ "$(NEXT_VERSION_CODE)" = "$$exp" ] || { \
	   echo "versioncheck: app/published.mk is inconsistent."; \
	   echo "  PUBLISHED_VERSION_CODE=$(PUBLISHED_VERSION_CODE), so"; \
	   echo "  NEXT_VERSION_CODE must be $$exp; it says"; \
	   echo "  $(NEXT_VERSION_CODE). A bumped PUBLISHED with a stale NEXT"; \
	   echo "  defaults every later build to a code that already shipped."; \
	   exit 1; }; \
	 [ "$(VERSION_CODE)" -gt "$(PUBLISHED_VERSION_CODE)" ] || { \
	   echo "versioncheck: VERSION_CODE=$(VERSION_CODE) does not exceed the"; \
	   echo "  last published version code, $(PUBLISHED_VERSION_CODE)."; \
	   echo "  Android refuses to install a package whose version code does"; \
	   echo "  not exceed the installed one. This artifact could not update"; \
	   echo "  the one already on people's phones; they would be shown an"; \
	   echo "  install failure, and the only way past it is uninstall --"; \
	   echo "  which deletes this app's storage, i.e. every glucose"; \
	   echo "  reading, dose and meal they have logged."; \
	   echo "  Build with VERSION_CODE=$$exp or higher, and see"; \
	   echo "  app/published.mk for what the ledger is."; \
	   exit 1; }; \
	 [ -n "$(VERSION_NAME)" ] || { \
	   echo "versioncheck: VERSION_NAME is empty. It is the only one of"; \
	   echo "  these numbers a human ever sees -- in Settings, and in the"; \
	   echo "  bug report they send us."; exit 1; }; \
	 for v in MIN_SDK:$(MIN_SDK) TARGET_SDK:$(TARGET_SDK) \
	          COMPILE_SDK:$(COMPILE_SDK) PLAY_TARGET_SDK_MIN:$(PLAY_TARGET_SDK_MIN); do \
	   case "$${v#*:}" in ""|*[!0-9]*) \
	     echo "versioncheck: $${v%%:*} is not an integer ('$${v#*:}')"; exit 1;; \
	   esac; \
	 done; \
	 [ "$(TARGET_SDK)" -ge "$(PLAY_TARGET_SDK_MIN)" ] || { \
	   echo "versioncheck: TARGET_SDK=$(TARGET_SDK) is below the required"; \
	   echo "  target API level, PLAY_TARGET_SDK_MIN=$(PLAY_TARGET_SDK_MIN)."; \
	   echo "  Play refuses updates from an app targeting too low an API,"; \
	   echo "  and the floor moves every August. This build cannot look the"; \
	   echo "  current value up -- check the Play Console and raise both"; \
	   echo "  numbers together, having looked at the behaviour changes the"; \
	   echo "  new target opts this app in to."; \
	   exit 1; }; \
	 [ "$(MIN_SDK)" -le "$(TARGET_SDK)" ] || { \
	   echo "versioncheck: MIN_SDK=$(MIN_SDK) exceeds TARGET_SDK=$(TARGET_SDK)"; \
	   exit 1; }; \
	 printf '\033[1;32mversioncheck\033[0m: code %s > published %s, name %s, sdk %s..%s (compile %s)\n' \
	   '$(VERSION_CODE)' '$(PUBLISHED_VERSION_CODE)' '$(VERSION_NAME)' \
	   '$(MIN_SDK)' '$(TARGET_SDK)' '$(COMPILE_SDK)'

# Play App Bundle (.aab), built without Gradle: aapt2 links resources in
# protobuf format, we assemble bundletool's module layout, then build-bundle.
# debuggable is off (an .aab is a release artifact). The result is UNSIGNED --
# sign it with your upload key (jarsigner -keystore upload.jks build/app/pancra.aab)
# or let Play App Signing handle it. Requires tools/bundletool.jar.
AAB        := build/app/pancra.aab
BUNDLETOOL := java -jar $(BUNDLETOOL_JAR)

aab: $(AAB)
# bundletool.jar is a PREREQUISITE now, not a `test -f` in the recipe: the
# inline test could only say it was missing, never that it had been replaced
# by a different version -- which is the whole of item 92. The instructions it
# used to print live on the $(BUNDLETOOL_JAR) rule, where all three consumers
# of that jar reach them instead of just this one.
$(AAB): app/AndroidManifest.xml $(LIB) $(DEX) $(RES) $(BUNDLETOOL_JAR) \
        build/app/.ver-pkgmeta $(MAKEFILE_SELF) $(FRAMEWORK) $(AABTOOL_BINS) $(JDK_INPUTS) \
        build/app/.ver-aabtools | versioncheck
	rm -rf build/app/aab && mkdir -p build/app/aab/module/manifest build/app/aab/module/dex build/app/aab/module/lib/arm64-v8a
	aapt2 compile --dir app/res -o build/app/aab/res.zip
	aapt2 link --proto-format -o build/app/aab/base-proto.apk -I $(FRAMEWORK) \
	    --manifest app/AndroidManifest.xml $(VER_FLAGS) $(SDK_FLAGS) \
	    -R build/app/aab/res.zip --auto-add-overlay
	cd build/app/aab/module && unzip -qo ../base-proto.apk
	@# WHERE aapt2 PUT THE MANIFEST, asked rather than assumed. This was a
	@# bare `mv module/app/AndroidManifest.xml module/manifest/`, and aapt2
	@# 2.19 writes it at the ROOT of the proto APK, not under app/ -- so
	@# `make aab` died on this machine with "mv: cannot stat", after five
	@# minutes of aapt2 and before bundletool was ever reached. (Verified
	@# against the pre-existing rule: this is not a regression from the
	@# prerequisite work around it.) Both layouts are accepted, and neither
	@# being present is an error that says what it looked for -- the failure
	@# it replaces named one path and left the reader to guess the other.
	@m=build/app/aab/module; \
	 if [ -f $$m/app/AndroidManifest.xml ]; then mv $$m/app/AndroidManifest.xml $$m/manifest/; \
	 elif [ -f $$m/AndroidManifest.xml ]; then mv $$m/AndroidManifest.xml $$m/manifest/; \
	 else echo "aab: aapt2 produced no AndroidManifest.xml at $$m/ or $$m/app/"; exit 1; fi
	cp $(DEX) build/app/aab/module/dex/classes.dex
	cp $(LIB) build/app/aab/module/lib/arm64-v8a/
	cd build/app/aab/module && rm -f ../base.zip && zip -qr ../base.zip manifest dex res lib resources.pb
	@# THE OUTPUT IS REMOVED FIRST, because bundletool REFUSES to overwrite:
	@# "Error: File 'build/app/pancra.aab' already exists." So `make aab`
	@# succeeded exactly once per tree and failed for ever after, five minutes
	@# of aapt2 and zip later, on a message about a file the build had just
	@# decided to replace. Which made every prerequisite this rule declares
	@# moot -- the rebuild they exist to trigger could not complete. (--overwrite
	@# exists in current bundletool and not in older ones; rm does not care.)
	rm -f $@ && $(BUNDLETOOL) build-bundle --modules=build/app/aab/base.zip --output=$@
	@# ...AND THE FINISHED BUNDLE IS READ BACK, for the reason the whole of
	@# items 116-118 exists: the .aab is a RELEASE artifact -- it is the one
	@# Play distributes -- and everything above this line is what the build
	@# INTENDED. aapt2 accepting a flag is not the same as the bundle
	@# carrying the value, and this toolchain has already been caught
	@# accepting --target-sdk-version and discarding it. Nothing about this
	@# bundle is trusted until bundletool has been asked what is in it.
	@BUNDLETOOL='$(BUNDLETOOL)' EXPECT_VERSION_CODE='$(VERSION_CODE)' \
	 EXPECT_VERSION_NAME='$(VERSION_NAME)' EXPECT_MIN_SDK='$(MIN_SDK)' \
	 EXPECT_TARGET_SDK='$(TARGET_SDK)' EXPECT_COMPILE_SDK='$(COMPILE_SDK)' \
	 PUBLISHED_VERSION_CODE='$(PUBLISHED_VERSION_CODE)' REQUIRE_NEWER=1 \
	 ./app/test/aabcheck.sh $@
	@printf '\033[1;32maab\033[0m: %s built (debuggable=false, UNSIGNED). Sign with your upload key before upload.\n' "$@"

install: $(APK)
	adb install -r $(APK)

run: install
	adb shell am start -n com.jk.pancra/android.app.NativeActivity

uninstall:
	adb uninstall com.jk.pancra

clean:
	rm -rf build

# Static code analysis
# ---------------------
# `make check` runs the same gate offline: no CRLF, ASCII-only, clang-format
# clean, and clang-tidy clean (rules in .clang-tidy, warnings-as-errors). No
# compilation database is needed -- the whole app is one clang invocation, so we
# hand clang-tidy the exact compile flags after `--`.
# app/test/ IS the behavioural gate, so it gets the same formatting and encoding
# checks as app/. It was excluded, which meant the ~1400 lines that decide
# whether every other check means anything could rot unnoticed.
# Everything first-party, in all three trees: the app, the shared code and
# the server. The server had no formatting gate of its own before the merge.
# ...INCLUDING srv/test/. The app's tests were formatted and the server's were
# not, which is how srv/test/cryptotest.c came to be the one first-party file
# nothing checked.
FMT_SRC   := $(wildcard app/*.c app/*.h app/test/*.c app/test/*.h lib/*.c \
                        lib/*.h srv/*.c srv/*.h srv/test/*.c)
TIDY_ARGS := --target=$(TARGET) -ffreestanding $(JNI_INC)

# uitest and drivertest are part of the gate: `make check` alone runs neither,
# and both have caught defects that clang-tidy structurally cannot see (gcc's
# -Wformat-truncation, and every protocol/crypto path).
# $(LIB) and $(DEX) are gate dependencies, NOT incidental build products.
#
# Without them `make check` never compiled main.c with the project's own WARN
# set and never compiled the Java at all -- so a green check did not mean the
# APK builds. Proven by an adversarial probe: an unused variable added to
# main.c passed the entire gate and failed only the library link, and
# appending garbage to Alarm.java left the gate green as well. That left every
# one of the 14 -Werror flags ungated across ~3900 lines of main.c, and the
# whole alarm actuation end (Alarm.java, PancraService.java, Ble.java)
# unchecked by anything.
# javacheck is IN the gate, not beside it. It pins four things that exist
# only on the Java side of the boundary -- silence() staying synchronized and
# actually stopping the player, stop() actually calling stopScan, a recreated
# service going through BoundaryLogic.serviceAction rather than dying quietly,
# and BoundaryLogic.NET_* agreeing with enum sync_net_fail -- and every one of
# them fails on a phone and nowhere else. A check that has to be remembered is
# a check that is not run: it was reachable only by typing `make javacheck`,
# so nothing stopped a Java edit that silently broke alarm silencing from
# passing a full green gate.
#
# ONE gate for both halves. srvcheck is last because it is the slowest and
# the only one that builds a second toolchain's worth of code; put it earlier
# and a formatting slip costs a minute to discover.
check: manifestcheck versioncheck inclusions stalecheck symcheck synclogcheck harnesstest bondtabletest phonycheck format tidy crosscheck clockcheck sizecheck lockcheck actioncheck initcheck settingscheck javacheck boundaryjavatest exportjavatest apkcheck $(LIB) $(DEX) uitest plottest drivertest alarmtest storetest statstest notifytest metertest metersesstest jbridgetest registrytest settingstest scantest senstest gesturetest syncstattest durabilitytest meterstoretest remotetest pairingtest insulintest weighttest exercisetest foodtest calibtest ingesttest crashtest threadtest cryptotest giftest rowtest wiretest httptest datetest modeltest interoptest appasan apptsan srvtsan srvcheck srvasan tlsasan tlstest faulttest dbctxtest dbmigtest clitest emailtest srvonlycheck deploycheck deploydrill restoredrill adbdrill duocheck done

# The CRLF scan named `src test res` -- the pre-merge directory layout. Those
# have not existed since the trees became app/ lib/ srv/, so grep failed with
# "No such file or directory", the `|| true` swallowed it, and the check
# scanned NOTHING for months while reporting success. A gate that passes
# because its target is gone is worse than no gate: it is a gate you trust.
#
# It then scanned exactly what the FORMATTER scans -- the C, the headers, the
# Makefile and the Android manifest -- which is the set clang-format has an
# opinion about and NOT the set that breaks when a carriage return gets into
# it. Every executable text file in the tree was outside it: the 26 shell
# scripts, the Java, the Python gates, the deployment conf, the XML. The shell
# scripts are the ones that cost an incident, because a shebang ending in a
# carriage return names an interpreter that does not exist -- so it passes the
# whole gate here and fails on the board with "no such file or directory"
# about a file that is plainly there.
#
# So the CRLF pass is app/test/crlfcheck.py now. It builds ONE manifest of
# everything first-party -- tracked plus untracked-but-not-ignored, the same
# enumeration stalecheck and symcheck use, minus a stated list of binary and
# generated paths -- fails on a file it cannot READ rather than skipping it,
# and runs its own negative cases first so that a green run means the gate
# could still have refused a CRLF shebang.
#
# The non-ASCII scan below is still the formatter's file set, and still uses
# PORTABLE grep. `-P` is GNU-only, and on a host whose grep lacks it grep
# exits 2 -- which `if grep ...` reads as "no matches", so the scan would have
# passed silently on exactly the machine least likely to be producing clean
# files. The exit status is inspected: 0 means matches (fail), 1 means clean,
# anything else means the scan itself did not run.
format:
	@python3 app/test/crlfcheck.py
	@out=$$(LC_ALL=C grep -rln '[^[:print:][:space:]]' $(FMT_SRC)); st=$$?; \
	 if [ $$st -gt 1 ]; then \
	   echo "format: the non-ASCII scan could not run (grep exit $$st)"; exit 1; \
	 fi; \
	 if [ -n "$$out" ]; then \
	   echo "$$out"; echo "Non-ASCII characters found (see above)"; exit 1; \
	 fi
	clang-format --dry-run -Werror $(FMT_SRC)

format-fix:
	clang-format -i $(FMT_SRC)

# Constants that must agree ACROSS LANGUAGES. A _Static_assert cannot see Java,
# so nothing else can catch this: raise LINK_MAX without raising MAX_LINKS and
# Ble.link(id) returns null for the new link, silently dropping every GATT
# operation on it -- no exception, no log, no diagnostic on either side.
crosscheck:
	@c=$$(sed -n 's/^#define LINK_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/dexdriver.h); \
	 j=$$(sed -n 's/.*MAX_LINKS[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' app/Ble.java); \
	 if [ -z "$$c" ] || [ -z "$$j" ]; then \
	   echo "crosscheck: could not read LINK_MAX ('$$c') or MAX_LINKS ('$$j')"; \
	   echo "  the check compares two greps; if BOTH miss it would pass on \"\" == \"\"."; \
	   exit 1; \
	 fi; \
	 if [ "$$c" != "$$j" ]; then \
	   echo "LINK_MAX ($$c in dexdriver.h) != MAX_LINKS ($$j in Ble.java):"; \
	   echo "  every GATT op on a link above $$j would be silently dropped."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mcrosscheck\033[0m: LINK_MAX == MAX_LINKS (%s)\n' "$$c"; \
	 a=$$(sed -n 's/^#define AL_FRESH_S[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/alarmlogic.h); \
	 w=$$(sed -n 's/^#define WEB_FRESH_S[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' srv/home.c); \
	 if [ -z "$$a" ] || [ -z "$$w" ]; then \
	   echo "crosscheck: could not read AL_FRESH_S ('$$a') or WEB_FRESH_S ('$$w')"; \
	   echo "  the check compares two greps; if BOTH miss it would pass on \"\" == \"\"."; \
	   exit 1; \
	 fi; \
	 if [ "$$a" != "$$w" ]; then \
	   echo "AL_FRESH_S ($$a in alarmlogic.h) != WEB_FRESH_S ($$w in srv/home.c):"; \
	   echo "  the phone blanks its big number to --- at one age and the web"; \
	   echo "  page at another, so for the difference between them the page"; \
	   echo "  shows a reading the device itself refuses to. One rule for"; \
	   echo "  \"too old to be now\", in two binaries that cannot share a header."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mcrosscheck\033[0m: AL_FRESH_S == WEB_FRESH_S (%s)\n' "$$a"; \
	 n=$$(sed -n 's/^#define NHIST[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/store.h); \
	 u=$$(sed -n 's/^#define UI_PLOT_GLU[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/uirender.c app/uidraw.c app/uimain.c app/uidev.c app/uiconfirm.c app/uimenu.c app/uikeypad.c app/uilog.c app/uifood.c); \
	 if [ -z "$$n" ] || [ -z "$$u" ]; then \
	   echo "crosscheck: could not read NHIST ('$$n') or UI_PLOT_GLU ('$$u')"; \
	   exit 1; \
	 fi; \
	 if [ "$$n" != "$$u" ]; then \
	   echo "NHIST ($$n in store.h) != UI_PLOT_GLU ($$u in uirender.c):"; \
	   echo "  the shell sends up to NHIST plot points but the renderer draws"; \
	   echo "  UI_PLOT_GLU of them -- a smaller UI cap silently truncates the"; \
	   echo "  oldest in-window points, shrinking the 7D plot below a week."; \
	   echo "  (its own cap is UI_PLOT_GLU + NINS: the shell appends the"; \
	   echo "  insulin doses after the glucose points in the same array.)"; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mcrosscheck\033[0m: NHIST == UI_PLOT_GLU (%s)\n' "$$n"; \
	 gmin=$$(sed -n 's/^#define STORE_GLU_MIN[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/ingest.h); \
	 gmax=$$(sed -n 's/^#define STORE_GLU_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/ingest.h); \
	 rmin=$$(sed -n 's/^#define ROW_GLU_MIN[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' srv/rowdec.h); \
	 rmax=$$(sed -n 's/^#define ROW_GLU_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' srv/rowdec.h); \
	 if [ -z "$$gmin" ] || [ -z "$$gmax" ] || [ -z "$$rmin" ] || [ -z "$$rmax" ]; then \
	   echo "crosscheck: could not read the glucose band from both sides"; \
	   exit 1; \
	 fi; \
	 if [ "$$gmin" != "$$rmin" ] || [ "$$gmax" != "$$rmax" ]; then \
	   echo "STORE_GLU_MIN/MAX ($$gmin/$$gmax in app/ingest.h) !=" \
	        "ROW_GLU_MIN/MAX ($$rmin/$$rmax in srv/rowdec.h):"; \
	   echo "  the phone WRITES the rows the server READS. A narrower band on"; \
	   echo "  the server silently drops readings the phone stored; a wider"; \
	   echo "  one draws what the phone itself refused. One band, two files."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mcrosscheck\033[0m: the stored glucose band agrees (%s..%s)\n' "$$gmin" "$$gmax"; \
	 p=$$(sed -n 's/^#define PCELL_GLU[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/plotdata.c); \
	 s=$$(sed -n 's/^#define STORE_GLU_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/ingest.h); \
	 if [ -z "$$p" ] || [ -z "$$s" ]; then \
	   echo "crosscheck: could not read PCELL_GLU ('$$p') or STORE_GLU_MAX ('$$s')"; \
	   echo "  the check compares two greps; if BOTH miss it would pass on \"\" == \"\"."; \
	   exit 1; \
	 fi; \
	 if [ "$$p" -le "$$s" ]; then \
	   echo "PCELL_GLU ($$p in plotdata.c) <= STORE_GLU_MAX ($$s in ingest.h):"; \
	   echo "  plotdata drops any reading at or above PCELL_GLU, and a dropped"; \
	   echo "  reading is not clamped to the top of the plot -- it is ABSENT."; \
	   echo "  A severe high would be stored, alarmed on and counted in TIR, and"; \
	   echo "  then silently missing from the 30- and 90-day plots."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mcrosscheck\033[0m: PCELL_GLU (%s) > STORE_GLU_MAX (%s)\n' "$$p" "$$s"; \
	 ar=$$(sed -n 's/^#define SYNC_ROW_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/sync.h); \
	 sr=$$(sed -n 's/^#define ROW_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' srv/proto.h); \
	 if [ -z "$$ar" ] || [ -z "$$sr" ]; then \
	   echo "crosscheck: could not read SYNC_ROW_MAX ('$$ar') or ROW_MAX ('$$sr')"; \
	   echo "  the check compares two greps; if BOTH miss it would pass on \"\" == \"\"."; \
	   exit 1; \
	 fi; \
	 if [ "$$ar" != "$$sr" ]; then \
	   echo "SYNC_ROW_MAX ($$ar in app/sync.h) != ROW_MAX ($$sr in srv/proto.h):"; \
	   echo "  the two halves disagree about how long a row may be. The longer"; \
	   echo "  side accepts rows the shorter one refuses, so a bucket hashes"; \
	   echo "  differently on each end and the sync can never converge."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mcrosscheck\033[0m: SYNC_ROW_MAX == ROW_MAX (%s)\n' "$$ar"
# (This one is now the CHEAP copy of a check the compiler also makes:
#  srv/test/wiretest.c includes app/sync.h and srv/proto.h together and
#  compares this and every other mirrored limit with _Static_assert. Kept
#  because it runs in the first seconds of `make check`, where a mismatch is
#  cheapest to hear about; wiretest is the authority.)

# ---------------------------------------------------------------------
# STALE NAMES: a comment that points at a file which does not exist.
#
# A wrong comment is worse than none: it is read, believed, and acted on. The
# ones this catches are all real and were all in the tree -- driverpriv.h (a
# header that was deleted), ui.c (one file, since split into eight), the
# server's old sync[.]h
# (renamed proto.h), and `glucoserve`, the server's name before it was Pancra.
# Each was cited from several files, years after the thing it named was gone.
#
# The rule is mechanical: a retired name may not appear in the sources at all.
# When a name is retired, add it here in the same commit -- the list IS the
# record of what no longer exists.
#
# EVERY FIRST-PARTY FILE, not a hand-written list of C sources: the names were
# still in app/AndroidManifest.xml and app/Ble.java, which no list of *.c and
# *.h could ever have caught, and a check that covers the files somebody
# remembered is a check that covers the files that were already right.
#
# TRACKED *AND* UNTRACKED, which is the half that was missing. `git ls-files`
# alone lists only what is COMMITTED, so every file a refactor has just
# created is invisible to this gate -- and a refactor is exactly when a
# retired name gets typed, by somebody writing about what a thing used to be.
# That was not hypothetical: a new header explaining why it is not named after
# its .c did it, by citing the server's retired sync header as the precedent,
# and the gate reported success over it because the file was one `git add`
# away from existing. `--others --exclude-standard` adds the untracked files
# that are not ignored, so build output and editor droppings stay out and
# anything a person has just written is in.
#
# (That header is named here only as "the retired sync header". Spelling it
# out would make manifestcheck read this comment as a rule naming a file that
# does not exist -- the trap STALE_NAMES documents below.)
#
# READING THE LIST IS ITSELF CHECKED, because every way this gate has been
# wrong before was a way of scanning nothing and saying so cheerfully:
#
#   - ONE FILE AT A TIME, QUOTED. The list was interpolated unquoted into a
#     single grep, so a path containing a space became two paths, grep failed
#     on both, and the failure went to /dev/null. `app/my file.h` could hold
#     any retired name and this gate stayed green.
#   - EVERY grep's EXIT STATUS is inspected. 0 is a match (fail), 1 is clean,
#     anything else means the scan did not run. The format rule above learned
#     this same lesson; its comment says why.
#   - A FILE THAT NO LONGER EXISTS is skipped before grep, deliberately: with
#     --cached the list holds every tracked file, including one deleted in
#     the working tree, and a pending deletion is not an error.
#   - BINARY FILES are skipped with -I. GNU grep announces "binary file
#     matches" on STDERR, which the old redirect discarded, so a match inside
#     one was silently lost. Skipping is a decision; losing is not.
#   - AN EMPTY LIST IS A FAILURE. It used to fall back to `ls app/*.c app/*.h
#     srv/*.c srv/*.h` -- the narrow list this check exists to replace -- so
#     outside a git repository the gate quietly went back to seeing almost
#     nothing, and still printed success.
#
# Four files are excluded, each for a stated reason:
#
#   TODO.md                quotes the defects it asks to be fixed
#   NOTES.md               IS THE ARCHIVE. Its entire purpose is to record
#                          designs that were removed, by the names they had,
#                          so the code beside the live invariant does not have
#                          to. Every name in STALE_NAMES below is exactly the
#                          sort of thing it exists to be about, so scanning it
#                          would forbid the one file that is supposed to name
#                          dead ones. symcheck skips it for the identical
#                          reason; keep the two lists agreeing.
#   Makefile               holds this rule, which must name the names
#   srv/deploy/README.md   documents the override a board still on the old
#                          tree needs, and the procedure to move it
#
# That last one is the "minimal migration-documentation exception", and it is
# the only one of the four that is migration documentation.
#
# srv/deploy/pancra.conf USED to be a fourth exception and is not one now: it
# contains none of these names, and deploycheck already FAILS the build if
# `glucoserve` appears in it. One gate exempting a file from a name another
# gate forbids in it is not an exception, it is a hole with a comment over it.
empty :=
space := $(empty) $(empty)
# The retired names, spelled so manifestcheck does not read them as paths
# this Makefile is naming: `srv/sync[.]h` matches the same text and is not a
# filename. (manifestcheck's whole job is that a rule may not name a file that
# does not exist -- and this rule names two on purpose.)
STALE_NAMES := driverpriv[.]h srv/sync[.]h glucoserve
# STALECHECK FOR SYMBOLS. stalecheck catches a comment naming a FILE that is
# gone; nothing caught the commoner version -- a comment naming a function or
# global that was renamed or deleted while the paragraph explaining it stayed.
# The comments here carry the reasons, so one naming a dead symbol is a false
# explanation of live code that reads exactly as authoritatively as a true one.
# Twenty-two of them were live when this gate was written, including two the
# driver refactor had left behind.
symcheck:
	@python3 app/test/symcheck.py

synclogcheck:
	@python3 app/test/synclogcheck.py

stalecheck:
	@mkdir -p build; \
	 git ls-files --cached --others --exclude-standard 2>/dev/null | \
	   grep -vE '^(TODO\.md|NOTES\.md|Makefile|srv/deploy/README\.md)$$' \
	   > build/stale-files.txt; \
	 if [ ! -s build/stale-files.txt ]; then \
	   echo "stalecheck: git listed NO files, so this gate scanned nothing."; \
	   echo "  It used to fall back to app/*.c app/*.h srv/*.c srv/*.h --"; \
	   echo "  the narrow list this check exists to replace -- and report"; \
	   echo "  success over it. An empty list is a failure now."; \
	   exit 1; \
	 fi; \
	 bad=""; uic=""; broke=""; n=0; \
	 while IFS= read -r f; do \
	   [ -f "$$f" ] || continue; \
	   n=$$((n + 1)); \
	   out=$$(LC_ALL=C grep -IHnE '$(subst $(space),|,$(STALE_NAMES))' "$$f"); \
	   st=$$?; \
	   if [ $$st -eq 0 ]; then bad="$$bad$$out"'\n'; \
	   elif [ $$st -gt 1 ]; then broke="$$broke $$f"; fi; \
	   out=$$(LC_ALL=C grep -IHn '\bui\.c\b' "$$f" | grep -v 'uirender\.c:'); \
	   if [ -n "$$out" ]; then uic="$$uic$$out"'\n'; fi; \
	 done < build/stale-files.txt; \
	 if [ -n "$$broke" ]; then \
	   echo "stalecheck: the scan could not READ:$$broke"; \
	   echo "  A grep that cannot read its input is a failure, not a shrug:"; \
	   echo "  it is indistinguishable from a file with nothing wrong in it."; \
	   exit 1; \
	 fi; \
	 if [ -n "$$bad" ]; then \
	   printf '%b' "$$bad"; \
	   echo "stalecheck: a comment names something that no longer exists."; \
	   echo "  driverpriv.h was deleted, ui.c became app/ui*.c, the"; \
	   echo "  server's sync.h became srv/proto.h, and the server stopped"; \
	   echo "  being called glucoserve. A comment that points at a"; \
	   echo "  missing file is read, believed and acted on -- which is"; \
	   echo "  worse than no comment."; \
	   exit 1; \
	 fi; \
	 if [ -n "$$uic" ]; then \
	   printf '%b' "$$uic"; \
	   echo "stalecheck: ui.c is eight files now (app/ui*.c)."; \
	   echo "  Name the one that holds what the comment is about, or say"; \
	   echo "  \"the renderer\". uirender.c may mention it: its header"; \
	   echo "  comment explains what it was split out of."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mstalecheck\033[0m: no comment names a file that is gone\n'

# A STOPGAP, and labelled as one. There is no Java test binary -- Ble, Alarm and
# PancraService are almost entirely Android API calls, so exercising them needs
# a device or Robolectric. javac + d8 therefore gate syntax and types only, and
# an adversarial review proved two severe mutants survive the whole check:
# Alarm.silence() made a no-op (the C side records the alarm as dismissed while
# the MediaPlayer keeps looping -- a hypo that rings and cannot be silenced),
# and Ble.stop() unconditionally reporting success (duplicate scan clients,
# which is what stop_scan's retry logic exists to prevent).
#
# These are pattern checks on load-bearing lines, not behaviour. They are worth
# having because the alternative here is nothing at all, but they only pin the
# specific shapes that were proven to slip through -- do not mistake them for
# coverage.
# A REDUNDANT driver_lock() ABOVE A driver_enter() IS A LEAKED LOCK.
#
# driver_enter takes the lock itself. Leaving the old driver_lock() above it
# acquires the RECURSIVE lock twice and releases it once: the depth returns to
# 1, the owner stays set, and that thread holds the driver lock for the rest of
# the process. Every GATT callback then spins on it -- and because the Android
# side of a connection is all Java, the phone shows a connected sensor that has
# simply stopped producing readings. It shipped exactly once and cost three CGM
# cycles before it was noticed.
#
# Nothing else can see it: the lock/unlock counts balance (the extra
# acquisition is inside driver_enter), the tests cannot drive start_scan (it
# needs a live JNIEnv), and the UI stays responsive because the holder is the
# thread that wants it. So it is checked as a SHAPE, the way crosscheck and
# javacheck check theirs -- a grep for the pattern, and a build error if it
# reappears.
# NO FILE OVER 2000 LINES. Not a style rule: this codebase had a main.c of
# 8790 and a ui.c of 5783, and both had reached the size where the only way to
# find anything was to grep -- so state that belonged to one workflow was read
# and written by five, and the same fact ended up recorded in two places that
# then drifted. Every split since is held in place by this number.
#
# If a file crosses it, the answer is a MODULE, not a smaller font: find the
# workflow that owns the state, give it a header that says what it is
# responsible for, and move both.
# TWO CLOCKS, AND THEY MUST NOT SWAP. realtime_s() identifies an INSTANT (a
# reading's timestamp, a last-sync time, anything persisted or shown);
# mono_s() measures an INTERVAL (watchdogs, retries, backoffs, holds). A
# wall-clock correction -- a phone coming back from being off, or finding a
# network -- moves the first and not the second, which is the whole point: on
# realtime, a jump forward fired the meter's 90-second sync watchdog at once
# and tore down a working exchange, and a jump backward postponed it so a
# wedged link was never recovered.
#
# This checks the deadline STATE is never stamped from the wall clock. It is a
# grep, not a proof, but it catches the reflex -- realtime_s() is the older
# call and the one fingers type.
#
# THE LIST IS HAND-MAINTAINED, which is its one weakness: a new deadline is
# covered only when someone adds its name. g_scan_retry_after (app/scan.c, the
# throttle a refused scan enters) was missing -- it is the deadline that
# decides how long the app waits before trying to scan again, so a wall-clock
# stamp there means a phone that finds a network mid-backoff either retries at
# once (into the "scanning too frequently" block the throttle exists to avoid)
# or waits out the correction with no scan at all, which is monitoring stopped
# with nothing on screen. It is an _Atomic long written through atomic_store,
# so it needs the second shape as well as the first: the plain-assignment
# pattern below cannot see through the wrapper.
#
# TWO MORE, ADDED WITH THE ALARM'S CLOCK SPLIT. g_launch_mono (app/main.c) is
# when the process started, and the DISCONNECT alarm's launch grace is measured
# from it -- on the wall clock an NTP step could end the grace early and alarm
# over data that was merely waiting for the first sync, or extend it and
# thereby disable that alarm outright for the length of the skew. g_link_pred
# (app/alarm.c) carries, in its low 48 bits, the monotonic second a CGM's hypo
# PREDICTION arrived, and the freshness gate on it is what stops the one alarm
# the user cannot silence from wedging on a value frozen at disconnect; a
# backward correction made every stored prediction permanently fresh. Neither
# is ever persisted or displayed, so neither has any claim on realtime_s().
# alarm_note_pred is listed in the CALL shape as well, and that is the one that
# actually bites: the stamp is passed IN, so the store inside alarm.c would
# look innocent while the caller in reading.c supplied the wall clock.
#
# AND ONE PATTERN RATHER THAN FOUR NAMES: g_cal_*_at (app/calib.c) is the
# calibration module's four in-process deadlines -- the queue's give-up window,
# the 0x34 resend throttle, the 0x32 probe throttle and a pending rescale's
# expiry. They are matched by shape because a list is a thing to forget, and
# this file has been forgotten before; anything named g_cal_<something>_at is a
# deadline by construction, so a fifth one is covered the day it is written.
# The realtime stamps beside them (g_calq_t, g_rescale_pend_t) are PERSISTED
# and deliberately wall-clock -- they are what a restart reconciles against --
# so they are not, and must not be, in this list.
#
# last_clock_m (app/dexdriver.c) is the receipt stamp of the sensor's own
# session clock, and the base the warmup and session-end countdowns are
# projected from between the sensor's ~5-minute responses. It used to be
# stamped from realtime_s(), and a backward wall-clock correction of an hour
# made the projection's unsigned delta wrap: a sensor twenty minutes into
# warmup read as finished AND as expired at the same instant. Named here so it
# cannot go back to the wall clock without this gate saying so.
clockcheck:
	@bad=$$(grep -nE '(g_meter_start|g_link_idle_t\[[^]]*\]|synced_t|g_scan_hold_until|g_scan_retry_after|g_rem_next|g_rem_safety|last_warn|last_scan_retry|g_launch_mono|g_link_pred\[[^]]*\]|last_clock_m|g_cal_[a-z_]*_at)[[:space:]]*=[[:space:]]*realtime_s' app/*.c; \
	       grep -nE '(meter_link_idle|msess_idle_set|msess_claim|msess_begin|msess_end|alarm_note_pred)\(.*realtime_s' app/*.c; \
	       grep -nE 'atomic_store\(&(g_scan_retry_after)[^;]*realtime_s' app/*.c); \
	 if [ -n "$$bad" ]; then \
	   echo "$$bad"; \
	   echo "clockcheck: a DEADLINE was stamped from the wall clock."; \
	   echo "  Use mono_s(). realtime_s() is for instants that are persisted"; \
	   echo "  or shown to a person; a clock correction moves it, and every"; \
	   echo "  watchdog and backoff measured against it moves with it."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mclockcheck\033[0m: deadlines measure elapsed time, not wall time\n'


sizecheck:
	@big=$$(wc -l app/*.c app/*.h lib/*.c lib/*.h srv/*.c srv/*.h | \
	          awk '$$1 > 2000 && $$2 != "total" { print "  " $$2 " (" $$1 " lines)" }'); \
	 if [ -n "$$big" ]; then \
	   echo "sizecheck: over the 2000-line ceiling:"; \
	   echo "$$big"; \
	   echo "  Split by WORKFLOW -- see app/shell.h for how the last one went."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32msizecheck\033[0m: no file over 2000 lines (largest: %s)\n' \
	   "$$(wc -l app/*.c app/*.h lib/*.c srv/*.c | awk '$$2 != "total"' | sort -rn | head -1 | awk '{print $$2 " at " $$1}')"


# THE INCLUSION GRAPH -- and the header cycle it would otherwise hide.
#
# inclusions.py merges each .c with its own .h into ONE node and follows the
# quoted includes between them, so what it draws is the dependency shape of
# the MODULES rather than of the files. A cycle in that graph is the thing
# worth failing on: two modules that include each other cannot be read,
# tested or reused apart, and the compiler will never complain -- the include
# guards make it build perfectly.
#
# TWO GRAPHS, NOT ONE, and that is the adaptation this repository needs. The
# script keys a node on the FILE STEM, and app/ and srv/ each have a
# settings, a sync and a util. Handed every tree at once it would fuse
# app/util with srv/util into a single node and invent edges -- and possibly
# a cycle -- between two programs that do not include one line of each
# other. The phone build and the server build are therefore drawn separately;
# lib/ appears in both, because both link it.
#
# The PDF is written even when the check FAILS, on purpose: the cycle edges
# are drawn in red, and the picture is most useful exactly when there is one.
# That is why the script's exit status is captured before dot runs. Graphviz
# is optional -- a gate about C headers must not depend on it being
# installed.
#
# It cannot pass by scanning nothing: an empty file list makes the script
# print its usage and exit 1, so a renamed tree fails loudly here rather than
# reporting success over an empty graph (which is exactly how the CRLF scan
# above came to check nothing for months).
INCL_APP := $(wildcard app/*.c app/*.h lib/*.c lib/*.h)
INCL_SRV := $(wildcard srv/*.c srv/*.h lib/*.c lib/*.h)

manifestcheck:
	@miss=""; \
	 for f in $(sort $(SRC) $(SRVSRC) $(TEST_SRC) $(FMT_SRC) $(INCL_APP) $(INCL_SRV)); do \
	   [ -f "$$f" ] || miss="$$miss $$f"; \
	 done; \
	 for f in $$(grep -ohE '\b(app|srv|lib)/[A-Za-z0-9_./-]+\.[ch]\b' Makefile | sort -u); do \
	   [ -f "$$f" ] || miss="$$miss $$f"; \
	 done; \
	 if [ -n "$$miss" ]; then \
	   echo "manifestcheck: named but MISSING:"; \
	   for f in $$miss; do echo "  $$f"; done; \
	   echo "  Every source and header this Makefile names by hand must"; \
	   echo "  exist. A rule that reads a constant out of a file that was"; \
	   echo "  renamed does not fail -- sed finds nothing, the variable is"; \
	   echo "  empty, and the check passes on \"\" == \"\". Moving one header"; \
	   echo "  left crosscheck reading a file that no longer existed and the"; \
	   echo "  client binary depending on one too."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mmanifestcheck\033[0m: every named source and header exists\n'

inclusions: inclusions.py $(INCL_APP) $(INCL_SRV)
	@mkdir -p build
	@rc=0; \
	 python3 inclusions.py $(INCL_APP) > build/incl-app.dot || rc=1; \
	 python3 inclusions.py $(INCL_SRV) > build/incl-srv.dot || rc=1; \
	 for g in app srv; do \
	   if command -v dot >/dev/null 2>&1; then \
	     dot -Tpdf build/incl-$$g.dot -o build/incl-$$g.pdf || \
	       echo "inclusions: graphviz could not draw build/incl-$$g.dot"; \
	   fi; \
	 done; \
	 if [ $$rc -ne 0 ]; then \
	   echo "inclusions: the include graph was REFUSED (see above)."; \
	   echo "  A cycle means two modules include each other: neither can be"; \
	   echo "  read, tested or built without the other, and no compiler will"; \
	   echo "  ever say so. The offending edges are red in build/incl-app.pdf"; \
	   echo "  and build/incl-srv.pdf. Break it by moving the shared"; \
	   echo "  declaration into a header of its own -- app/uimodel.h and"; \
	   echo "  app/menuview.h exist for exactly that reason."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32minclusions\033[0m: no header cycles (graphs in build/incl-{app,srv}.pdf)\n'

# Settings are OWNED by settings.c: the preferences, the paired identity and
# the five file paths all live behind read-only views (prefs(), sync_creds(),
# code_path()...), and every change goes through a settings_set_* call that
# stores AND persists in one step. The alternative -- write the global,
# remember to save -- is a rule nothing can enforce, and a forgotten save is
# invisible until the app restarts and reverts the user's choice.
#
# This checks the shape that makes it true: settings.h exports no OBJECT at
# all. A new `extern int g_something;` there would quietly reopen the hole,
# and the compiler would not say a word.
# NO HEADER HANDS OUT A WRITABLE GLOBAL.
#
# settings.h exported the preferences, so anything that included it could
# change a setting without saving it -- which shows up only as a setting that
# reverts on the next launch. menu.h exported seven: the sampled permission
# states, the EXPORT checkboxes and the two list page numbers, so the FRAME
# read them directly and any caller could set them. Both are behind an
# accessor now (prefs(), menu_view_get), and this is what keeps them there.
#
# The check is deliberately crude -- `extern <something>;` that is not a
# function -- because the property is crude: a header either hands out an
# object or it does not.
# EVERY FAILED INITIALISATION STEP MUST FAIL THE INITIALISATION.
#
# init_java sets up the Java side: the Ble class, the JNI bind, the pairing
# natives, the BLE transport. Its contract is that any step failing fails the
# whole thing, because there is no degraded mode -- the caller publishes
# g_inited only if it returns 1.
#
# dexble_register's failure was LOGGED and stepped over, and init_java
# returned success. The result is not a visible crash: the screen comes up,
# the menus work, and nothing ever arrives. No sensor connects, no reading is
# logged, no alarm can fire. A glucose monitor that looks healthy and monitors
# nothing is the worst state this app can reach.
#
# The shape is checkable: every guard in that function ends in `return 0`, so
# the counts must match. A future step added with a log and no return is what
# this catches.
initcheck:
	@body=$$(awk '/^static int init_java/,/^\}/' app/main.c); 	 g=$$(printf '%s\n' "$$body" | grep -c 'if (!'); 	 r=$$(printf '%s\n' "$$body" | grep -c 'return 0;'); 	 if [ "$$g" != "$$r" ]; then 	   echo "initcheck: init_java has $$g guards but $$r failure returns."; 	   echo "  Every step that can fail must RETURN 0: the caller sets"; 	   echo "  g_inited on success, and an app that reports itself"; 	   echo "  initialised without the BLE transport monitors nothing while"; 	   echo "  looking perfectly healthy."; 	   exit 1; 	 fi; 	 printf '\033[1;32minitcheck\033[0m: all %s init_java steps fail the init\n' "$$g"

settingscheck:
	@for h in app/settings.h app/menu.h app/sensors.h app/store.h \
	          app/weight.h app/insulin.h app/forms.h; do \
	   bad=$$(grep -nE '^extern [^(]*;' $$h); \
	   if [ -n "$$bad" ]; then \
	     echo "$$h: $$bad"; \
	     echo "settingscheck: $$h exports a writable object."; \
	     echo "  State belongs behind an accessor that validates and (for a"; \
	     echo "  preference) persists what it changes; a frame reads it as ONE"; \
	     echo "  snapshot. Anything that can write one directly can change it"; \
	     echo "  without saving, or change it halfway through a frame."; \
	     exit 1; \
	   fi; \
	 done; \
	 ptr=$$(grep -nE '^(struct|const struct) [a-z_]+ \*sensor_(rec|slot)' app/sensors.h); \
	 if [ -n "$$ptr" ]; then \
	   echo "app/sensors.h: $$ptr"; \
	   echo "settingscheck: the registry is handing out a POINTER again."; \
	   echo "  The record cache is memmoved by a binder thread minting a"; \
	   echo "  sensor, so a pointer that leaves this module is a row that can"; \
	   echo "  MOVE under its holder -- and a writable one is a device the"; \
	   echo "  user renames with no validation and nothing saved. Copies and"; \
	   echo "  indexes only (sensor_rec_of, sensor_slot_of)."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32msettingscheck\033[0m: no header hands out writable state or internal pointers\n'


lockcheck:
	@bad=$$(awk 'FNR == 1 { held = 0 } \
	             /^[[:space:]]*[*]/ { next } /^[[:space:]]*\/\*/ { next } \
	             /driver_lock\(\);/ { held = FNR } \
	             /driver_enter\(/ { if (held && FNR - held <= 8) \
	                 print FILENAME ":" FNR ": driver_enter() under a live driver_lock() from line " held } \
	             /driver_leave\(|driver_unlock\(\);/ { held = 0 }' \
	         app/*.c); \
	 if [ -n "$$bad" ]; then \
	   echo "$$bad"; \
	   echo "lockcheck: a driver_lock() above a driver_enter() LEAKS the lock."; \
	   echo "  driver_enter takes it too; the recursive depth never returns to"; \
	   echo "  zero, and every GATT callback spins forever. Delete the"; \
	   echo "  driver_lock() -- driver_enter/driver_leave are the whole pair."; \
	   exit 1; \
	 fi; \
	 e=$$(cat app/*.c | grep -v '^[[:space:]]*[*]' | grep -c 'driver_enter('); \
	 l=$$(cat app/*.c | grep -v '^[[:space:]]*[*]' | grep -c 'driver_leave('); \
	 if [ "$$l" -lt "$$e" ]; then \
	   echo "lockcheck: $$e driver_enter() but only $$l driver_leave() across app/"; \
	   echo "  every enter needs a leave; an unmatched one holds the driver"; \
	   echo "  lock forever and every GATT callback then spins on it."; \
	   exit 1; \
	 fi; \
	 pub=$$(grep -rlnE '^(void|int) driver_(lock|unlock)\(' app/*.h); \
	 if [ -n "$$pub" ]; then \
	   echo "$$pub"; \
	   echo "lockcheck: the driver lock is in a HEADER again."; \
	   echo "  Public or 'private header', it is the same thing: another"; \
	   echo "  module reasoning about when the driver's state is safe to"; \
	   echo "  touch. One redundant lock() above a call that takes it"; \
	   echo "  internally is what once held it for the life of the process,"; \
	   echo "  with the phone looking connected and never producing another"; \
	   echo "  reading. Callers name an operation: driver_route_*,"; \
	   echo "  driver_link_*, driver_snapshot, driver_cal_*."; \
	   exit 1; \
	 fi; \
	 who=$$(grep -lnE '^[^*/]*driver_(lock|unlock)\(\)' app/*.c app/test/*.c \
	        2>/dev/null | grep -v '^app/dexdriver\.c$$'); \
	 if [ -n "$$who" ]; then \
	   echo "$$who"; \
	   echo "lockcheck: that file takes the DRIVER's lock by hand."; \
	   echo "  It belongs to dexdriver.c alone. Whatever the file needs is"; \
	   echo "  an operation on the driver -- see the four families named"; \
	   echo "  above; the calibration queue is the pattern for state that"; \
	   echo "  must be serialised with the driver but is not the driver's."; \
	   exit 1; \
	 fi; \
	 self=$$(awk 'FNR == 1 { held = 0 } \
	              /^[[:space:]]*[*]/ { next } /^[[:space:]]*\/\*/ { next } \
	              /store_lock\(\);/ { held = FNR } \
	              /store_unlock\(\);/ { held = 0 } \
	              /store_now\(/ { if (held) \
	                  print FILENAME ":" FNR ": store_now() while store_lock() is held (line " held ")" }' \
	          app/*.c); \
	 if [ -n "$$self" ]; then \
	   echo "$$self"; \
	   echo "lockcheck: store_now() TAKES the store lock, and that lock is"; \
	   echo "  not recursive -- taking it twice on one thread is a spin that"; \
	   echo "  never ends. Two of these froze the app on the phone: the main"; \
	   echo "  thread and the service tick both sat in store_now for ever and"; \
	   echo "  Android killed it as not responding. A caller that already"; \
	   echo "  holds it wants store_now_locked(), which exists for this."; \
	   exit 1; \
	 fi; \
	 pos=$$(grep -nE '(^|[^_a-zA-Z])(sensor_slot_at|sensor_slot_index|sensor_primary_slot|link_for_slot)\(' \
	          app/*.c | grep -v '^app/sensors\.c:' | \
	          grep -vE '^[^:]*:[0-9]+:[[:space:]]*[*]'); \
	 if [ -n "$$pos" ]; then \
	   echo "$$pos"; \
	   echo "lockcheck: a SLOT INDEX crossed a function boundary."; \
	   echo "  Every one of these turns a position into a device, and a"; \
	   echo "  position moves: a mint or a forget between the frame that drew"; \
	   echo "  the row and the tap that acts on it, or between a screen and"; \
	   echo "  its confirmation, renames the device under the workflow. The"; \
	   echo "  UI keeps an ID (menu_selected_id), a tapped row becomes one"; \
	   echo "  through model_snap_id -- against the frame the user actually"; \
	   echo "  touched -- and the radio is asked with link_for_sensor(id)."; \
	   exit 1; \
	 fi; \
	 idx=$$(grep -nE 'sensor_(set|cycle|retire|revive|forget)[a-z_]*\((menu_selected_slot\(\)|idx|slot|i)[,)]' \
	          app/*.c); \
	 if [ -n "$$idx" ]; then \
	   echo "$$idx"; \
	   echo "lockcheck: a sensor operation given a SLOT INDEX."; \
	   echo "  Every one of them names the device by its permanent id now,"; \
	   echo "  and resolves it under the lock it changes the slot under. An"; \
	   echo "  index is a position: a mint or a forget moves it, and the"; \
	   echo "  action then lands on a device the user was not looking at."; \
	   echo "  Read the slot once (sensor_slot_at) and pass slot.id."; \
	   exit 1; \
	 fi; \
	 python3 app/test/lockorder.py || exit 1; \
	 toc=$$(grep -nE '(^|[^_a-zA-Z])(slot_count\(\)|slot_at\(|srec_at\(|srec_count\(\))' \
	          app/*.c | grep -v '^app/sensors\.c:' | \
	          grep -vE '^[^:]*:[0-9]+:[[:space:]]*[*]' | \
	          grep -vE '^[^:]*:[0-9]+:[[:space:]]*/\*' | \
	          grep -vE 'sensor_slot_at\(|cgm_slot_count\(\)'); \
	 if [ -n "$$toc" ]; then \
	   echo "$$toc"; \
	   echo "lockcheck: a COUNT and an INDEX READ of the registry, outside"; \
	   echo "  sensors.c. An index is a POSITION: a mint or a forget on a"; \
	   echo "  binder thread moves every position after it, so \"is idx in"; \
	   echo "  range\" and \"what is at idx\" answered by two calls can"; \
	   echo "  describe two different devices -- and the caller then"; \
	   echo "  renames, recolours, CALIBRATES or disconnects the wrong one."; \
	   echo "  Ask once: sensor_slot_at(idx, &slot) for one device, or"; \
	   echo "  sensors_view_get() for a walk. Then key everything else by"; \
	   echo "  the id, which never moves."; \
	   exit 1; \
	 fi; \
	 rpub=$$(grep -rlnE '^(void|int) sensors_(lock|unlock)\(' app/*.h); \
	 if [ -n "$$rpub" ]; then \
	   echo "$$rpub"; \
	   echo "lockcheck: the REGISTRY lock is in a header again."; \
	   echo "  Eleven files once took it by hand around a count/index walk;"; \
	   echo "  several walked with no lock at all, and one held it across"; \
	   echo "  link_for_slot -- which takes the DRIVER's lock, inverting the"; \
	   echo "  documented driver -> registry order. Callers take a snapshot"; \
	   echo "  (sensors_view_get) or ask the registry a question"; \
	   echo "  (sensor_id_is_live, sensor_slot_of, sensor_rec_of)."; \
	   exit 1; \
	 fi; \
	 rwho=$$(grep -lnE '^[^*/]*sensors_(lock|unlock)\(\)' app/*.c app/test/*.c \
	        2>/dev/null | grep -v '^app/sensors\.c$$'); \
	 if [ -n "$$rwho" ]; then \
	   echo "$$rwho"; \
	   echo "lockcheck: that file takes the REGISTRY's lock by hand."; \
	   echo "  It belongs to sensors.c alone. A walk wants sensors_view_get:"; \
	   echo "  one copy of every slot AND its provenance at one instant, with"; \
	   echo "  the lock already released -- so it cannot be held across a"; \
	   echo "  call that takes another."; \
	   exit 1; \
	 fi; \
	 amb=$$(grep -HnE '^(void|int) dexble_(request_devinfo|subscribe|write|read|pair|reconnect|link_close)\(void\);' $(wildcard app/*.h)); \
	 if [ -n "$$amb" ]; then \
	   echo "$$amb"; \
	   echo "lockcheck: a GATT operation with no link is an AMBIENT one."; \
	   echo "  It reads whichever link a callback happened to select --"; \
	   echo "  which recorded a meter's model and firmware against a"; \
	   echo "  sensor's row, in a file that is never rewritten. Every one"; \
	   echo "  of these takes an explicit link."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mlockcheck\033[0m: %s driver_enter, %s driver_leave -- none unmatched\n' "$$e" "$$l"

# CHEAP SECONDARY DIAGNOSTICS, not the proof.
#
# These greps find a load-bearing line that has been DELETED, and say which
# one, which is worth having and costs nothing. What they cannot see is
# anything about behaviour: a call that is dead, in the wrong order, or
# skipped because an earlier statement threw satisfies every pattern below.
# Two such mutants were demonstrated -- release() folded into stop()'s try
# block (leaving a looping alarm nothing can reach), and runIndependent
# stopping at the first failing stage (silencing the sound but not the
# vibration) -- and both left this target green.
#
# boundaryjavatest is where those are caught. It runs the SAME policy code
# these files call, on the host JVM, and asserts call count, order, the
# outcome of each stage, and the retry state a refusal leaves behind.
javacheck:
	@if grep -nE '(notify|cancel|startForeground)\([0-9]' app/*.java; then \
	   echo "javacheck: a notification id is written as a NUMBER."; \
	   echo "  Every id belongs to BoundaryLogic's list. Two files each"; \
	   echo "  declared their own 2 -- Alarm's commented 'distinct from the"; \
	   echo "  service's id 1', PancraService's not commented at all -- so a"; \
	   echo "  glucose alarm and the 'no longer monitoring' warning shared one"; \
	   echo "  slot: either replaced the other, and either cancel took down"; \
	   echo "  the wrong one, including silencing the alarm's notification"; \
	   echo "  while the sound kept playing. Neither comment could catch that;"; \
	   echo "  only one list can, and a literal here is a second list."; \
	   exit 1; \
	 fi; \
	 f=app/Alarm.java; \
	 grep -q 'static synchronized void silence' $$f || \
	   { echo "$$f: silence() must stay synchronized"; exit 1; }; \
	 awk '/static synchronized void silence/,/^    \}/' $$f | grep -q 'stopSound()' || \
	   { echo "$$f: silence() must call stopSound() -- without it the C side"; \
	     echo "  records the alarm dismissed while the player keeps looping"; \
	     exit 1; }; \
	 awk '/static void stopSound/,/^    \}/' $$f | grep -q 'release()' || \
	   { echo "$$f: stopSound() must release() the player"; exit 1; }; \
	 g=app/Ble.java; \
	 awk '/static boolean stop\(/,/^    \}/' $$g | grep -q 'stopScan' || \
	   { echo "$$g: stop() must actually call stopScan -- reporting success"; \
	     echo "  without it lets the self-heal stack a second scan client"; \
	     exit 1; }; \
	 awk '/public static String scan\(/,/^    \}/' $$g | grep -q 'scanFailed(' || \
	   { echo "$$g: the ScanCallback's onScanFailed must hand the failure on."; \
	     echo "  It used to only Log.i it. Native has already latched"; \
	     echo "  'a scan is running' by then, and every recovery path it has"; \
	     echo "  starts only when that flag is CLEAR -- so a scan the platform"; \
	     echo "  refused asynchronously stopped the app scanning for the life"; \
	     echo "  of the process: no sensor reconnect after a dropout, no meter"; \
	     echo "  noticed when switched on, and nothing on screen to say so."; \
	     exit 1; }; \
	 awk '/static void scanFailed\(/,/^    \}/' $$g > build/scanfailed.txt; \
	 grep -q 'scanFailureIsCurrent' build/scanfailed.txt || \
	   { echo "$$g: scanFailed() must ask BoundaryLogic whose scan failed."; \
	     echo "  The failure arrives on a binder thread long after the call,"; \
	     echo "  by which time the scan may have been stopped and replaced;"; \
	     echo "  resetting on a superseded scan's behalf cancels a HEALTHY"; \
	     echo "  one -- the same outage, caused by the fix for it. The"; \
	     echo "  generation rule is host-tested (boundaryjavatest)."; \
	     exit 1; }; \
	 grep -q 'onScanFailed(gen, err)' build/scanfailed.txt || \
	   { echo "$$g: scanFailed() must reach native onScanFailed(gen, err)."; \
	     echo "  Releasing the Java callback alone leaves native latched on a"; \
	     echo "  scan that no longer exists, which is the original bug with"; \
	     echo "  one more moving part."; \
	     exit 1; }; \
	 awk '/@Override public void onServicesDiscovered/,/^            \}/' $$g \
	   > build/svcdisc.txt; \
	 grep -q 'BoundaryLogic.gattDiscovered' build/svcdisc.txt || \
	   { echo "$$g: onServicesDiscovered must ask BoundaryLogic.gattDiscovered"; \
	     echo "  whether the link is set up. It used to call native connected"; \
	     echo "  regardless of the discovery STATUS: the service table is then"; \
	     echo "  absent, every subscribe and read answers 'char not found', and"; \
	     echo "  the app blames a sensor that is physically fine while the"; \
	     echo "  driver advances on a connection that was never set up."; \
	     exit 1; }; \
	 grep -q 'onConnected(id)' build/svcdisc.txt || \
	   { echo "$$g: native onConnected(id) belongs INSIDE"; \
	     echo "  onServicesDiscovered -- a link is ready when it has a service"; \
	     echo "  table and at no earlier moment."; \
	     exit 1; }; \
	 nconn=$$(grep -c 'onConnected(id)' $$g); \
	 [ "$$nconn" = 1 ] || \
	   { echo "$$g: onConnected(id) is called $$nconn times, not once."; \
	     echo "  Successful discovery is the ONLY route to it. A second call"; \
	     echo "  site is a route that has not been gated, and the gate is the"; \
	     echo "  whole point: the driver's state machine must not start on a"; \
	     echo "  connection whose MTU or discovery step actually failed."; \
	     exit 1; }; \
	 for h in gattConnected gattMtuRequested gattMtuChanged \
	          gattDiscoverRequested; do \
	   grep -q "BoundaryLogic.$$h" $$g || \
	     { echo "$$g: the GATT setup must go through BoundaryLogic.$$h."; \
	       echo "  Each of the four setup signals -- the CONNECTED status, the"; \
	       echo "  return of requestMtu, the MTU completion status, the return"; \
	       echo "  of discoverServices -- was ignored. A REFUSED request means"; \
	       echo "  no callback ever arrives, so the link sits connected and"; \
	       echo "  idle, and during first-time pairing nothing recovers it"; \
	       echo "  (the stall watchdog skips links that are not paired yet)."; \
	       exit 1; }; \
	 done; \
	 if grep -n '"pancra\.csv"' app/*.java; then \
	   echo "javacheck: the SHARED, TRUNCATABLE export filename is back."; \
	   echo "  Every export wrote files/pancra.csv and shared a URI naming"; \
	   echo "  it; the recipient opens that URI when it SENDS, minutes or"; \
	   echo "  hours later, so a second export truncated the first"; \
	   echo "  recipient's data mid-read -- the doctor received an empty file"; \
	   echo "  or half of two exports, and the sender saw a normal share"; \
	   echo "  sheet. Names come from BoundaryLogic.exportName now."; \
	   exit 1; \
	 fi; \
	 awk '/public static void exportData/,/^    \}/' $$g > build/export.txt; \
	 grep -q 'newSnapshot(' build/export.txt || \
	   { echo "$$g: exportData must share a fresh SNAPSHOT, not a rewritten"; \
	     echo "  file. See the comment there for what the recipient saw."; \
	     exit 1; }; \
	 grep -q 'ExportSnapshot.write(' build/export.txt || \
	   { echo "$$g: exportData must write the snapshot through"; \
	     echo "  ExportSnapshot, which is where two invisible failures are"; \
	     echo "  answered: the URI is built only after the output stream"; \
	     echo "  CLOSED (a close is where a buffered stream writes its last"; \
	     echo "  block, so publishing before it shares a prefix), and a row"; \
	     echo "  is copied only once its own newline has been read (the C"; \
	     echo "  side appends to these files while the share sheet is up)."; \
	     echo "  Both are host-tested by exportjavatest; neither is visible"; \
	     echo "  on a phone until somebody else opens the attachment."; \
	     exit 1; }; \
	 if grep -n 'readLine' app/*.java | grep -v '^app/ExportSnapshot.java:'; then \
	   echo "javacheck: readLine() is back in the app's Java."; \
	   echo "  It cannot tell a final line with no trailing newline from a"; \
	   echo "  row native is half way through appending -- and the export"; \
	   echo "  then TERMINATED the fragment, shipping a row that no file"; \
	   echo "  ever held. ExportSnapshot.copyRows works in bytes and copies"; \
	   echo "  a row only when it has read the newline that ends it."; \
	   exit 1; \
	 fi; \
	 p=app/PancraFiles.java; \
	 awk '/private File resolve/,/^    \}/' $$p > build/provresolve.txt; \
	 grep -q 'BoundaryLogic.exportNameValid(' build/provresolve.txt || \
	   { echo "$$p: the provider must validate the name it was handed."; \
	     echo "  The last path segment arrives from another process, already"; \
	     echo "  URL-decoded, so '..', '%2F..' and absolute paths reach it as"; \
	     echo "  ordinary strings -- and stelo.key and paircode.txt live in"; \
	     echo "  the same tree. A provider that resolves whatever it is given"; \
	     echo "  is a path-traversal surface; the grammar is host-tested."; \
	     exit 1; }; \
	 s=app/PancraService.java; \
	 awk '/public int onStartCommand/,/^    \}/' $$s | grep -q 'serviceAction' || \
	   { echo "javacheck: onStartCommand must go through BoundaryLogic.serviceAction"; \
	     echo "  a recreated service that stops QUIETLY looks exactly like a"; \
	     echo "  healthy phone -- no notification -- while nothing is being"; \
	     echo "  monitored and no alarm can sound"; \
	     exit 1; }; \
	 awk '/void warnStopped/,/^    \}/' $$s | grep -q 'MONITORING STOPPED' || \
	   { echo "javacheck: the stopped-monitoring warning lost its text"; \
	     exit 1; }; \
	 n=app/BoundaryLogic.java; \
	 awk '/^enum sync_net_fail/,/^\};/' app/syncstat.h | \
	   sed -n 's/^ *\(SYNC_NET_[A-Z]*\).*/\1/p' | sed 's/^SYNC_NET_//' > build/netfail.c.txt; \
	 sed -n 's/^ *static final int NET_\([A-Z]*\) = \([0-9]*\);/\2 \1/p' $$n \
	   | sort -n | sed 's/^[0-9]* //' > build/netfail.java.txt; \
	 cmp -s build/netfail.c.txt build/netfail.java.txt || \
	   { echo "javacheck: BoundaryLogic.NET_* and enum sync_net_fail disagree."; \
	     echo "  Ble.syncFail() passes these numbers straight to native, so a"; \
	     echo "  renumbering on one side alone turns a timeout into a DNS"; \
	     echo "  failure on the screen -- silently, and only on a phone."; \
	     diff build/netfail.c.txt build/netfail.java.txt; exit 1; }; \
	 cbuf=$$(sed -n '/^#define SYNC_BUF_MAX/,/[^\\]$$/p' app/sync.h \
	         | tr -d '\n\\' | sed 's|/\*.*||' \
	         | sed -n 's/.*SYNC_BUF_MAX[^(]*(\([^)]*\)).*/\1/p' | tr -d ' L'); \
	 jbuf=$$(sed -n 's/.*SYNC_BODY_CAP = \(.*\);.*/\1/p' $$n | tr -d ' L'); \
	 cval=$$(awk "BEGIN{printf \"%d\", $$cbuf}" 2>/dev/null); \
	 jval=$$(awk "BEGIN{printf \"%d\", $$jbuf}" 2>/dev/null); \
	 case "$$cval$$jval" in ''|*[!0-9]*) \
	   echo "javacheck: the sync body-limit cross-check READ NOTHING."; \
	   echo "  It got C='$$cbuf' -> '$$cval' and Java='$$jbuf' -> '$$jval'."; \
	   echo "  One of the two declarations was reformatted out from under the"; \
	   echo "  greps. A gate that scans nothing passes for the same reason a"; \
	   echo "  working one does, so this refuses instead."; \
	   exit 1;; esac; \
	 [ "$$cval" = "$$jval" ] || \
	   { echo "javacheck: SYNC_BUF_MAX ($$cval) and BoundaryLogic's"; \
	     echo "  SYNC_BODY_CAP ($$jval) disagree about how big a sync reply"; \
	     echo "  may be. Ble.syncHttp stops reading at the JAVA number and"; \
	     echo "  syncjni.c's jni_http refuses at the C one, so drift is"; \
	     echo "  invisible until a real server sends a body between them:"; \
	     echo "  a Java limit ABOVE the C one puts back the allocate-then-"; \
	     echo "  refuse behaviour that could exhaust the heap of the process"; \
	     echo "  monitoring a CGM, and a Java limit BELOW it silently refuses"; \
	     echo "  replies the protocol allows -- a sync that never works, with"; \
	     echo "  no reason shown. There is one number; this is its copy."; \
	     exit 1; }; \
	 awk '/static byte\[\] syncHttp\(/,/^    \}/' $$g > build/synchttp.txt; \
	 grep -v '^ *\*' build/synchttp.txt > build/synchttp.code.txt; \
	 grep -q 'BoundaryLogic.runExchange(' build/synchttp.code.txt || \
	   { echo "$$g: syncHttp must run the exchange through"; \
	     echo "  BoundaryLogic.runExchange, which is where the bounded read,"; \
	     echo "  the refusal of every 3xx and the closing of both streams"; \
	     echo "  live. It used to loop until EOF into an unbounded"; \
	     echo "  ByteArrayOutputStream and let NATIVE refuse the result --"; \
	     echo "  correct, and three copies too late. The bound has to be on"; \
	     echo "  this side of the allocation, and it has to be where the host"; \
	     echo "  JVM can run it (boundaryjavatest drives it with a dribbling"; \
	     echo "  stream, a lying Content-Length and a redirect)."; \
	     exit 1; }; \
	 grep -q 'setInstanceFollowRedirects(false)' build/synchttp.code.txt || \
	   { echo "$$g: syncHttp must switch OFF automatic redirects."; \
	     echo "  HttpURLConnection follows 3xx by default. Native signs one"; \
	     echo "  method and one path (sync.c, sync_signing_string), so a"; \
	     echo "  followed redirect sends that signature somewhere else -- or,"; \
	     echo "  via 303, turns the PUT that carried a bucket of readings into"; \
	     echo "  a bodyless GET. The 2xx from the new location is then"; \
	     echo "  reported as the answer to the upload, sync.c believes the"; \
	     echo "  rows are on the server, and the phone deletes its copy."; \
	     exit 1; }; \
	 if grep -q 'while ((n = in.read(buf)) > 0)' build/synchttp.code.txt; then \
	   echo "$$g: the UNBOUNDED sync read loop is back."; \
	   echo "  A server answering a bucket fetch with a gigabyte gets this app"; \
	   echo "  to allocate until the heap gives out, and an OutOfMemoryError"; \
	   echo "  lands on the next allocation ANYWHERE -- as likely the BLE"; \
	   echo "  callback decoding a glucose reading as the sync itself."; \
	   exit 1; \
	 fi; \
	 grep -q 'System.nanoTime()' build/synchttp.code.txt || \
	   { echo "$$g: syncHttp's deadline must start from System.nanoTime()."; \
	     echo "  currentTimeMillis is the WALL clock: an NTP correction or the"; \
	     echo "  user setting the date mid-sync moves it, and a deadline"; \
	     echo "  computed from a clock that stepped BACKWARDS grows -- handing"; \
	     echo "  the wedged request this exists to kill even more time."; \
	     echo "  See item 70 for the first time this bug was paid for."; \
	     exit 1; }; \
	 if grep -q 'currentTimeMillis' build/synchttp.code.txt; then \
	   echo "$$g: syncHttp must not time an exchange by the wall clock."; \
	   exit 1; \
	 fi; \
	 grep -q 'syncWatch.schedule(' build/synchttp.code.txt || \
	   { echo "$$g: syncHttp must arm the watchdog that closes the socket."; \
	     echo "  Checking a deadline between reads cannot interrupt a thread"; \
	     echo "  already blocked inside one, and the request write has no"; \
	     echo "  timeout of any kind. Without the timer a server that accepts"; \
	     echo "  the connection and then says nothing parks the ONE sync/pair/"; \
	     echo "  restore thread for the life of the process."; \
	     exit 1; }; \
	 printf '\033[1;32mjavacheck\033[0m: load-bearing Java lines present\n'

# app/ is tidied with the real freestanding cross-compile flags. app/test/ is tidied
# too -- it is the behavioural gate and must not rot -- but as HOST binaries it
# needs its own flag set (glibc FILE/fopen/exit, host target, the drivertest
# macro), not TIDY_ARGS. app/test/.clang-tidy inherits the root config and turns off
# only the checks that are wrong for a hosted harness (see that file); everything
# else is enforced identically to app/.
# Project headers come in via -iquote (so "" includes resolve to app/) and NOT
# -Iapp: app/ ships freestanding stubs named stdio.h/stdlib.h/string.h, and -Iapp
# would put those on the <> path, hiding glibc's real FILE/fopen from the host
# harness (implicit-decl errors). -iquote never affects <> includes, so <stdio.h>
# stays glibc's -- exactly as the test build (JVM_INC, not JNI_INC) does it.
# modeltest and settingstest are EXCLUDED from clang-tidy, and only from
# clang-tidy: they are the two tests that reach INSIDE a translation unit
# rather than calling across a header. modeltest does it because build_model
# and the shell state it reads are static in main.c; settingstest does it
# because arranging a case means setting a preference to a value no user could
# reach -- a corrupt one, or half of an ordered pair -- which the public
# interface deliberately refuses now that the storage is private.
#
# That technique makes three checks fire on the technique itself rather than
# on any defect: the .c include, "no header provides this symbol" (there is
# none, by design), and dexlibc.h's freestanding declarations colliding with
# glibc's when the file is tidied with host headers. Both are still compiled
# with -Werror (TESTWARN, the same set every other host test gets), still
# format-checked, and still run by `make check`; only the linter skips them.
# WHAT THE MODEL TEST LINKS: the whole app except its entry point (modeltest
# INCLUDES main.c, because build_model and the shell state it reads are
# static) and except the BLE transport, which is stubbed. Derived from SRC so
# a new file cannot be added to the app and forgotten here -- which showed up
# as an undefined symbol in a test that had been passing for months.
# lib/ct.c IS LINKED NOW. It used to be filtered out with the other two,
# because nothing the model test reached called into it -- p256.c says so at
# its ct_eq comment ("several link lines compile this file without ct.c").
# jpake.c reaches it now, for the wipe that clears the password scalar and the
# derived key before free, so excluding it is an undefined symbol rather than
# a saving. The app links it too (it is in SRC), so keeping it here makes this
# test link what the app links, which is the rule the comment above states.
MODEL_SRC := $(filter-out app/main.c app/dexble.c,$(SRC)) \
             app/stub_android.c app/stub_log.c

TEST_SRC   := $(filter-out app/test/modeltest.c app/test/settingstest.c,\
                $(wildcard app/test/*.c))
# -DAPP_FAULTS for the same reason -DDEXDRIVER_TEST is here: durabilitytest is
# BUILT with it (it is the only way to make an fsync fail on demand), so
# linting without it hides the file's fault paths behind an undeclared
# identifier and reports the test itself as broken.
TIDY_TEST  := -iquote app -iquote lib -iquote app/test $(JAVA_INC) \
              -DDEXDRIVER_TEST -DAPP_FAULTS -Wall -Wextra
# The server was linted by NOTHING. $(SRC) is the app plus lib/, $(TEST_SRC)
# is the app's tests, and srv/ appeared in neither -- so the half that
# terminates TLS and parses HTTP for strangers had never been through
# clang-tidy, while the app had. Moving four files out of srv/ into lib/ was
# what exposed it: they picked up 27 real diagnostics the moment they crossed
# the line into a directory the linter could see.
# `=` not `:=`: SQLITE is defined with the server variables far below, so an
# immediate expansion here would silently bake in an empty -I and every srv
# file would fail to find sqlite3.h.
TIDY_SRV = -iquote srv -iquote lib -I$(SQLITE) -Wall -Wextra
tidy: | require-clang require-jdk
	clang-tidy $(SRC) -- $(TIDY_ARGS)
	clang-tidy $(TEST_SRC) -- $(TIDY_TEST)
	# synccli.c is the protocol's TEST CLIENT, not the server: it runs only
	# under synctest.sh, on a loopback socket, against input it produced
	# itself. Its remaining analyzer findings (a tainted index from a reply it
	# just received, a zero-length malloc) are worth fixing but are not the
	# server's exposure, and holding the server's gate hostage to them would
	# have meant shipping neither.
	clang-tidy $(filter srv/%,$(SRVSRC)) -- $(TIDY_SRV)

done:
	@printf '\033[1;32mSUCCESS\033[0m: all checks passed.\n'

# offline UI render harness: builds the renderer + font.c on the host and
# screens to $(TESTFIXDIR)/*.ppm, so the UI can be verified with no phone
# attached. uitest.c names each screen and nothing else: the directory reaches
# it through $(TESTENV) below, so the ASan build of this suite writes its
# screens into build/app/test-asan/fixtures/uitest/ instead of over the plain
# build's.
# -iquote so "" project headers come from app/ while <> headers stay glibc's
# (the freestanding shims lack FILE/fopen etc. the harness needs on the host).
# THE SAME JDK the app is built against (JAVA_INC), not whichever one the
# distribution's default-java symlink points at: those were two different JDKs
# on any machine with more than one installed, and the difference shows up as
# a host test that compiles against headers the app never sees.
JVM_INC := $(JAVA_INC)
# The harness built with only -Wall -Wextra and no -Werror, so it emitted live
# -Wformat-truncation warnings (a real one-character label truncation) and still
# passed. gcc catches truncation that clang-tidy does not, so this is the only
# gate that sees it -- it has to be fatal.
# -O2 is REQUIRED, not an optimisation choice: gcc only runs
# -Wformat-truncation / -Wformat-overflow / -Wstringop-* / -Wmaybe-uninitialized
# under optimisation. Without it this gate emitted ZERO truncation diagnostics
# and the -Werror was decorative -- a genuinely truncating snprintf shipped as
# silently as before. Clang (the real -Os build) does not implement the warning
# at all, so this is the only place in the project that can see it.
TESTWARN := -Wall -Wextra -Werror -Wformat=2 -O2

# ---- HOST TEST OBJECTS: COMPILED ONCE, NOT ONCE PER RUN -------------------
#
# Every host test recipe used to name its whole source list in ONE cc
# invocation that compiled AND linked. Three things followed from that, and
# they are the whole reason `make check` takes as long as it does:
#
#   - NOTHING WAS EVER REUSED. `make modeltest` recompiled sixty translation
#     units on every invocation whether or not a single byte had changed --
#     measured at 35 s, with no unit dominating (the spread is 0.2 s to 1.7 s,
#     so there was nothing to fix file by file).
#   - NOTHING RAN IN PARALLEL. One cc process compiles its inputs in sequence,
#     so `make -j4` bought nothing: the parallelism has to be between make
#     recipes, and there was only ever one.
#   - ccache COULD NOT HELP. It caches single-file `-c` compilations; a
#     compile-and-link of sixty files is a miss every time. Adding the prefix
#     changed the 35 s by 90 ms, which is how this was diagnosed.
#
# So the sources become .o files under a per-flavour directory, with -MMD -MP
# deps so make skips what has not changed, and the test target links them. Now
# an unchanged tree relinks in well under a second, one changed file rebuilds
# one object, `-j` uses every core, and ccache catches the rest.
#
# ONE DIRECTORY PER FLAG SET, and that is not optional: an object built with
# -DAPP_FAULTS is not the same object as one built without it, and sharing a
# directory between two flavours would hand a test the other one's code with
# nothing to say so. The directory name IS the flavour.
TESTOBJ := build/test

$(TESTOBJ)/plain/%.o: %.c
	@mkdir -p $(dir $@)
	$(CCACHE) cc -iquote app -iquote lib $(JVM_INC) $(TESTWARN) -MMD -MP -c $< -o $@

$(TESTOBJ)/faults/%.o: %.c
	@mkdir -p $(dir $@)
	$(CCACHE) cc -iquote app -iquote lib $(JVM_INC) $(TESTWARN) -DAPP_FAULTS \
	    -MMD -MP -c $< -o $@

# The object lists, derived from the source lists so a file added to the app
# cannot be forgotten here -- the rule MODEL_SRC already states.
MODEL_OBJ := $(patsubst %.c,$(TESTOBJ)/faults/%.o,app/test/modeltest.c $(MODEL_SRC))
UITEST_SRC := app/test/uitest.c app/uirender.c app/uidraw.c app/uimain.c \
              app/uidev.c app/uiconfirm.c app/uimenu.c app/uikeypad.c \
              app/uilog.c app/uifood.c app/font.c lib/plot.c app/sensors.c \
              app/util.c app/clock.c app/weight.c app/food.c app/exercise.c \
              app/syncstat.c app/keypad.c
UITEST_OBJ := $(patsubst %.c,$(TESTOBJ)/plain/%.o,$(UITEST_SRC))

# WILDCARD, NOT $(shell find). This line runs on EVERY parse of this file,
# including every sub-make, and spawning a process to walk a directory that
# grows an entry per object is a cost paid for nothing -- wildcard is
# in-process. Two levels deep covers app/, lib/ and app/test/.
-include $(wildcard $(TESTOBJ)/*/*/*.d $(TESTOBJ)/*/*/*/*.d)


# ---- WHERE A HOST TEST BINARY LIVES, PER BUILD MODE --------------------
#
# `make check` lists the ordinary host suites AND appasan as independent
# prerequisites, and appasan is a RECURSIVE make of those same suites with
# sanitizer flags. Both wrote and then executed build/app/test/<name> -- one
# path, two configurations -- so under `make -j check` the two builds raced:
# whichever finished last owned the file, and either side could execute a
# half-written binary or a complete one built with the other's flags. A
# sanitizer suite that silently ran the plain build reports "clean" having
# checked nothing, which is the failure that matters here.
#
# Keyed by mode instead, so the two trees cannot collide and neither can be
# mistaken for the other. TSAN gets its own for the same reason.
#
# DECLARED HERE, ABOVE THE TEMPLATE, AND REFERENCED AS $$(TESTDIR) INSIDE IT.
# Both halves of that sentence were learned the hard way. `define APPTEST` is
# instantiated with $(eval $(call ...)), and `call` expands the body ONCE,
# immediately -- so a single-dollar $(TESTDIR) written there is resolved while
# the makefile is still being read, not when the recipe runs. With the
# declaration further down the file it resolved to the EMPTY STRING for all
# fourteen generated suites: `mkdir -p` with no operand, `cc ... -o /alarmtest`,
# and `./` + `/alarmtest`. Every one of them died on the mkdir, which is to say
# `make check` did not run at all. It was invisible from `make appasan`, because
# a command-line TESTDIR=... assignment IS defined before the makefile is read,
# so the sanitizer paths were the only ones that worked.
#
# The double dollar defers it to recipe execution, the same way $$(TESTWARN)
# and $$(JNI_TESTINC) already do in that template, and then the position in the
# file no longer matters. It is declared up here anyway: a variable whose value
# is spliced into rules further down should be readable before them.
TESTDIR ?= build/app/test

# ---- WHERE A HOST TEST'S FIXTURES LIVE: PER MODE, AND PER SUITE --------
#
# TESTDIR above keys the BINARY. It did nothing about the other half of the
# same defect: the fixture directory was the literal string "build/app/test"
# inside app/test/*.c, so an ASan or TSan binary read and wrote the PLAIN
# tree's files. Six suites (calibtest, durabilitytest, metersesstest,
# meterstoretest, modeltest, threadtest) are in both sanitizer lists AND the
# plain one, so `make -j check` put two or three processes in one fixture
# directory; `make -j2 appasan apptsan` failed in calibtest on state another
# process had replaced under it, while each passed alone. The mirror case is
# `make appasan` on its own, which failed in storetest because the literal
# build/app/test need not exist when only the sanitizer tree has been built.
#
# PER SUITE AS WELL AS PER MODE, and the second half is not decoration. Keying
# by mode alone left the collision intact INSIDE one mode: `make -j8` gives the
# jobserver to the sanitizer submake too, so a dozen suites of the SAME mode run
# at once, and they do not have distinct filenames -- registrytest, statstest,
# modeltest and pairingtest all call sensors_paths() on their fixture directory
# and then unlink and re-mint sensors.csv and slots.csv; settingstest and
# modeltest share settings.cfg and remote.cfg; storetest, weighttest and
# insulintest each share a log with modeltest. `make -j8 uitest appasan apptsan`
# failed in statstest on "a sensor with a known activation is registered" --
# a mint into a registry file another process had just truncated. $@ is the
# suite's own name in every one of these recipes, so one directory per target
# costs one variable and needs nothing from the suites.
#
# The suites read the directory from the ENVIRONMENT, defaulting to
# build/app/test, so a binary run by hand behaves exactly as it did before --
# one definition, in app/test/testdir.h, which explains the choice. This is the
# launch side of it: every recipe that RUNS a host suite creates
# $(TESTFIXDIR) and prefixes the command with $(TESTENV), so the fixtures
# follow the binary without any recipe naming a path of its own.
#
# RECURSIVE (=), NOT SIMPLE (:=), AND REFERENCED AS $$(TESTENV) IN THE
# TEMPLATE. Both matter, for the reasons the TESTDIR comment above gives at
# length. A `:=` here would freeze TESTDIR at read time, before the
# command-line TESTDIR=build/app/test-asan of a sanitizer submake exists, and
# would freeze $@ to the empty string as well -- an automatic variable has no
# value while the makefile is being read. A single-dollar $(TESTENV) inside
# `define APPTEST` would be expanded by $(call ...) for the same reason, which
# is how fourteen suites once compiled to -o /alarmtest.
# UNDER fixtures/, NOT ALONGSIDE THE BINARIES. $(TESTDIR)/$@ is already taken:
# it is the executable this same recipe links, so a fixture directory of that
# name would be `mkdir` over a file (and `-o` over a directory).
TESTFIXDIR = $(TESTDIR)/fixtures/$@

TESTENV = APP_TESTDIR=$(TESTFIXDIR)

# The BINARY tree, for the one caller that needs to find a binary rather than a
# fixture directory: app/test/interop.sh runs $(TESTDIR)/interoptest, so it
# cannot be handed the per-suite fixture path.
TESTBINENV = APP_TESTBIN=$(TESTDIR)

# THE APP TEST TARGETS, from one template.
#
# There were fifteen of these, each a near-identical five lines: mkdir, one cc
# with the same flags and include paths, run the binary. That is ~165 lines
# saying one thing, and it is where a new test gets its flags subtly wrong by
# being copied from whichever neighbour was nearest. The recipe lives once;
# each test names only what it links.
#
# The JNI include path is here for every test, not only the three that need
# it: several app headers reach jni.h transitively, and a template whose flags
# depend on which header a source happens to pull in is the copy-paste problem
# again in a smaller box.
#
# $(1) = target name, $(2) = extra sources beyond app/test/$(1).c
JNI_TESTINC := $(JAVA_INC)

# PER-OBJECT, like modeltest and uitest above, so every suite built through
# this template gets incremental rebuilds, ccache hits and `-j` for free.
#
# JNI_TESTINC and JVM_INC are both $(JAVA_INC) and TESTWARN is shared, so these
# objects are byte-for-byte the ones the `plain` flavour already builds -- which
# is what makes sharing the directory correct rather than merely convenient. If
# either ever diverges, this needs its own flavour directory; an object built
# with different flags is a different object, and sharing would hand a suite
# somebody else's code with nothing to say so.
#
# $$ THROUGHOUT except the $(1)/$(2) call arguments: inside a `define`, a
# single-dollar $(VAR) expands when $(call) is READ, not when the rule runs.
# That mistake once silently broke fourteen suites here.
define APPTEST
$(1)_OBJ := $$(patsubst %.c,$$(TESTOBJ)/plain/%.o,app/test/$(1).c $(2))
# THE OBJECTS ARE PREREQUISITES, not a recursive $(MAKE). A sub-make re-reads
# this whole file, and reading it costs 1.5 s (measured) because of the tool
# probes at the top -- so invoking one per suite paid the parse twice and wiped
# out most of what per-object building had just won.
$(1): $$($(1)_OBJ) | require-jdk
	@mkdir -p $$(TESTFIXDIR)
	$(CCACHE) cc $$($(1)_OBJ) -o $$(TESTDIR)/$(1)
	@$$(TESTENV) ./$$(TESTDIR)/$(1)
endef

# alarmtest is NOT an APPTEST entry, for the same reason threadtest and
# bondtabletest are not: it starts threads, and the template links no -pthread.
#
# The threads are there because the imminent-hypo prediction and the second it
# arrived are published as ONE atomic word -- a GATT binder thread writes it
# holding the DRIVER lock while the alarm evaluation reads it holding the
# ALARM lock, and there is no common lock they may take (alarm_lk is held
# across blocking MediaPlayer JNI, so taking it under the driver lock is a
# freeze, not a fix). "One word" is a claim about concurrency, so the suite
# drives a writer and a reader at it and checks the pair is never mixed. It is
# in TSAN_TESTS for the same reason.
alarmtest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) \
	    app/test/alarmtest.c app/alarmlogic.c \
	    -o $(TESTDIR)/alarmtest -pthread
	@$(TESTENV) ./$(TESTDIR)/alarmtest
# notifytest is bespoke for the same reason alarmtest is: it starts a thread,
# and the template links no -pthread. The thread is the subject -- "the render
# slot is already taken" is a state one thread cannot be in, and it is the
# state the service tick and the activity's 1 Hz timer are in whenever they
# collide over the same refresh mark. It is in TSAN_TESTS for that reason.
notifytest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) \
	    app/test/notifytest.c app/notify.c \
	    -o $(TESTDIR)/notifytest -pthread
	@$(TESTENV) ./$(TESTDIR)/notifytest
$(eval $(call APPTEST,storetest,app/store.c app/util.c app/clock.c app/sensors.c app/stats.c))
$(eval $(call APPTEST,crashtest,app/crashlog.c))
# THE JNI BOUNDARY. jbridgetest is a plain APPTEST because its JNIEnv is a
# table of function pointers it builds itself -- no VM, no Android, no
# threads.
# It links syncjni.c and dexble.c WHOLE, and stubs everything they call.
# jni_http is static inside syncjni.c -- reachable only as the hook
# syncjni_wire installs -- and dexble_register's two global refs are
# file-scope statics, so there is no honest way to drive either except by
# linking the module that owns it. The stubs are in the suite, declared
# through the real headers, so a signature that moves is a compile error here
# rather than a silent link-time mismatch.
$(eval $(call APPTEST,jbridgetest,app/jbridge.c app/syncjni.c app/dexble.c app/util.c app/food.c app/exercise.c app/stub_log.c))
# -DAPP_FAULTS: every calibration and rescale transition is a transaction that
# must put the state back when its one-line file cannot be replaced, and a
# replace only fails on demand (see util.c). Nothing that ships sets it.
#
# AND NO app/clock.c. This module's subject is now WHICH CLOCK each of its
# comparisons is on: a retry window that a wall-clock correction can end
# instantly or postpone for ever is the defect the suite exists to pin, and it
# cannot be pinned by a test that has to wait real seconds and cannot move the
# wall clock at all. So calibtest supplies realtime_s / mono_s / now_ms itself
# and drives them independently -- a forward jump of an hour with no elapsed
# time, a backward jump with elapsed time -- which is exactly what an NTP step
# or a user fixing the date does to the phone.
#
# THE SAME NEED pairingtest has, met more cheaply. pairingtest recompiles
# util.c and clock.c with the three names REDEFINED, because it links modules
# that call them and must not get two definitions. Nothing in calibtest's link
# set does: app/util.c and app/stub_log.c name none of the three, and app/calib.c
# is the code under test. So dropping clock.c is enough here, and the -D dance
# would be ceremony. The real clock is still compiled for everything else and
# still gated by clockcheck; what this drops is one suite's dependence on it.
calibtest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) -DAPP_FAULTS \
	    app/test/calibtest.c app/calib.c app/util.c \
	    app/stub_log.c -o $(TESTDIR)/calibtest -pthread
	@$(TESTENV) ./$(TESTDIR)/calibtest
# statstest links app/store.c, and it is not a dependency of the code under
# test: it is the OTHER reader of readings.csv, and the last block drives both
# over one file so that a row the two judge differently fails HERE rather than
# on a phone showing a plot and a TIR that disagree about the same log.
$(eval $(call APPTEST,statstest,app/stats.c app/util.c app/clock.c app/sensors.c app/store.c))
$(eval $(call APPTEST,metertest,app/otble.c app/util.c app/clock.c app/meterlogic.c app/civil.c))
# -DAPP_FAULTS for registrytest: every registry mutation is a transaction that
# has to put the table back when slots.csv cannot be replaced, and the only
# way to make a replace fail on a healthy filesystem is to arm one. Nothing
# that ships carries the switch (see util.c).
registrytest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) -DAPP_FAULTS \
	    app/test/registrytest.c app/sensors.c app/util.c app/clock.c \
	    -o $(TESTDIR)/registrytest
	@$(TESTENV) ./$(TESTDIR)/registrytest
# -DAPP_FAULTS: every setter is a transaction that has to put the old value
# back when its file cannot be replaced, and the only way to make a replace
# fail on a healthy filesystem is to arm one (see util.c). Nothing that ships
# carries the switch. Its own target rather than an APPTEST entry for that
# reason.
settingstest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) -DAPP_FAULTS \
	    app/test/settingstest.c app/util.c app/clock.c -lpthread \
	    -o $(TESTDIR)/settingstest
	@$(TESTENV) ./$(TESTDIR)/settingstest
$(eval $(call APPTEST,weighttest,app/weight.c app/util.c app/clock.c app/civil.c))
$(eval $(call APPTEST,exercisetest,app/exercise.c app/util.c app/clock.c app/civil.c))
$(eval $(call APPTEST,foodtest,app/food.c app/util.c app/clock.c app/civil.c))
$(eval $(call APPTEST,insulintest,app/insulin.c app/util.c app/clock.c app/civil.c))
$(eval $(call APPTEST,ingesttest,app/ingest.c))
$(eval $(call APPTEST,scantest,app/scanlogic.c))
$(eval $(call APPTEST,senstest,app/senslogic.c))
# WHICH FINGER A TOUCH BELONGS TO. The host stub for libandroid reports exactly
# one pointer, with id 0, so no suite that drives on_input can express a second
# finger -- and a second finger is the entire subject: the shipped code masked
# the action word, dropped the pointer index, ignored POINTER_DOWN/POINTER_UP
# and read slot 0 every time, so a second touch inherited the first one's armed
# control and the release fired it from the wrong coordinates. The rule lives in
# gesturelogic.c precisely so this can hand it as many fingers as it likes, in
# any slot order.
$(eval $(call APPTEST,gesturetest,app/gesturelogic.c))
$(eval $(call APPTEST,syncstattest,app/syncstat.c))
# -pthread: the runtime table is written by binder callbacks while the
# watchdog, the renderer and the save read it, and a single-threaded test
# cannot tell a locked table from the unlocked one it replaced.
# app/sesscache.c app/senslogic.c: the session cache is the OTHER table in the
# app that a render path mutates while a service heartbeat renders it to a
# file, and it had no test of its own at all -- it was linked into other
# binaries and never called from them. It belongs here rather than in a suite
# of its own because this is the suite with a fixture directory, real threads
# and a persistence contract already, and because meterstoretest is in both
# the ASan and the TSan lists, which is where a lock is actually proved.
#
# -DAPP_FAULTS: both save paths render a table into a buffer and then write
# it, and the two rules that matters most for -- the render is ONE instant,
# and an older render never lands on a newer one -- are wrong for only a
# handful of instructions per row. A test cannot land inside that on its own:
# builds with each lock deleted wrote a perfectly coherent file on every run,
# so the assertions passed against the implementations they exist to reject.
# The flag turns on a sched_yield inside each render loop and nothing else;
# util.c's deliberate failures under the same flag are armed by environment
# variable and this suite arms none. Nothing that ships defines it.
meterstoretest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) -DAPP_FAULTS \
	    app/test/meterstoretest.c app/meterstore.c app/sensors.c app/util.c \
	    app/sesscache.c app/senslogic.c \
	    app/clock.c -o $(TESTDIR)/meterstoretest -pthread
	@$(TESTENV) ./$(TESTDIR)/meterstoretest
# lib/plot.c IS THE SUITE'S SECOND SUBJECT: plottest owns the long-span data
# AND the renderer's geometry boundary (the framebuffer whose stride nothing
# used to check), so the renderer has to be linked in. It is the same file
# uitest links; nothing else about this line changes.
$(eval $(call APPTEST,plottest,app/plotdata.c app/sensors.c app/util.c app/clock.c lib/plot.c))

# THE SYNC SCHEDULE, with both clocks and the dispatch faked.
#
# Its own target rather than an APPTEST entry for the same reason threadtest
# is: the single-flight gate has to be shown holding under real threads, so
# this one needs -pthread. Everything else it needs -- the two clocks, the
# state stamp, the request into Java, the preferences, the identity -- is
# faked in the test, which is what makes a schedule testable with no phone.
remotetest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) \
	    app/test/remotetest.c app/remote.c app/syncstat.c \
	    -o $(TESTDIR)/remotetest -pthread
	@$(TESTENV) ./$(TESTDIR)/remotetest

# ADVERT -> CANDIDATE -> REGISTERED DEVICE, with the radio and the driver
# faked.
#
# Its own target because it needs two things an APPTEST line cannot give it:
# -pthread (the candidate list is written by a binder thread and read by the
# main looper, and that is half of what is being tested), and a util.c
# compiled with the three clock functions RENAMED, so the test's own fake
# clock is the only one in the binary. The freshness window -- a candidate
# heard a minute ago must not be compared against one heard now -- cannot be
# tested against a clock that only moves forwards at one second per second.
pairingtest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) \
	    -Drealtime_s=util_realtime_s -Dmono_s=util_mono_s \
	    -Dnow_ms=util_now_ms -c app/util.c -o $(TESTDIR)/util_noclock.o
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) \
	    -Drealtime_s=util_realtime_s -Dmono_s=util_mono_s \
	    -Dnow_ms=util_now_ms -c app/clock.c -o $(TESTDIR)/clock_noclock.o
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) \
	    app/test/pairingtest.c app/pairing.c app/sensors.c app/scanlogic.c \
	    $(TESTDIR)/util_noclock.o \
	    $(TESTDIR)/clock_noclock.o -o $(TESTDIR)/pairingtest -pthread
	@$(TESTENV) ./$(TESTDIR)/pairingtest

# "SAVED" HAS TO MEAN SAVED, on all four append-only logs.
#
# Its own target because app/util.c app/clock.c is compiled with -DAPP_FAULTS for it: an
# fsync that fails, a close reporting a deferred error, a rename that fails at
# the last step and a rollback that cannot truncate are the moments the
# durability contract is about, and none of them can be arranged on a healthy
# filesystem. Nothing that ships carries the switch.
durabilitytest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) -DAPP_FAULTS \
	    app/test/durabilitytest.c app/util.c app/clock.c app/store.c app/weight.c \
	    app/insulin.c app/sensors.c app/stats.c -lpthread \
	    -o $(TESTDIR)/durabilitytest
	@$(TESTENV) ./$(TESTDIR)/durabilitytest

# THE CROSS-THREAD PRIMITIVES, under real threads.
#
# app/thread.h is the only place one thread hands anything to another now, and
# it is four inline functions -- which is exactly the size of code that gets
# reviewed by eye and shipped wrong. Its own target rather than an APPTEST
# entry because it is the one test that needs -pthread.
# THE BOND TABLE, read and written at once. It was inside the JNI bridge --
# one translation unit with a JavaVM in it -- so no host suite could link it
# and the lock guarding it was argued in a comment rather than demonstrated.
# In TSAN_TESTS below, which is where this one earns its keep: the assertion
# can pass on a racy build because the window is narrow, and the sanitizer
# does not depend on hitting the window.
bondtabletest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) \
	    app/test/bondtabletest.c app/bondtable.c \
	    -o $(TESTDIR)/bondtabletest -pthread
	@$(TESTENV) ./$(TESTDIR)/bondtabletest

threadtest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) \
	    app/test/threadtest.c app/util.c app/clock.c -o $(TESTDIR)/threadtest -pthread
	@$(TESTENV) ./$(TESTDIR)/threadtest

# THE METER SESSION, under the overlap it actually sees: binder callbacks
# claiming and ending an exchange while two watchdog threads snapshot it. Its
# own target rather than an APPTEST entry for the same reason as threadtest --
# it is the other test that needs -pthread.
metersesstest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -iquote app -iquote lib $(JNI_TESTINC) $(TESTWARN) \
	    app/test/metersesstest.c app/metersess.c app/meterlogic.c app/util.c \
	    app/clock.c app/civil.c -o $(TESTDIR)/metersesstest -pthread
	@$(TESTENV) ./$(TESTDIR)/metersesstest

uitest: $(UITEST_OBJ) | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc $(UITEST_OBJ) -o $(TESTDIR)/uitest
	$(TESTENV) ./$(TESTDIR)/uitest

# Behavioural gate for the LONG-SPAN plot data. The 30D plot is downsampled
# from the log, and downsampling is where a plot lies quietly: this asserts
# that every reading which would occupy its own pixel survives, that the span
# is filled end to end, and that ten times the history does not cost ten
# times the memory. The bug it pins: plot depth was tied to a point budget,
# so a 30-day plot showed ten days once four sources were logging.

# Behavioural gate for the alarm decision logic. Until this existed, NOTHING in
# main.c was covered by any test binary -- an adversarial review deleted the
# glucose alarm outright and `make check` stayed green. The LOW alarm was in
# fact dead at the time (see alarmlogic.h).

# Behavioural gate for the reading history / dedup model. store.c was in no
# test binary, and every caller persists only on a non-zero hist_insert result
# -- so a wrong return there drops a reading permanently and silently.

# WHAT THE APP DECIDES TO SHOW. uitest proves ui_render draws a struct screen
# correctly -- from structs it builds BY HAND. build_model is the only thing
# that fills one from real app state, it is in main.c, and NO test binary
# linked main.c at all: a model bound to the wrong sensor, or carrying a stale
# reading, drew perfectly and passed every gate here. main.c is included (not
# linked) because build_model and the state it reads are static; only the BLE
# transport is stubbed.
#
# -DAPP_FAULTS: what a CONFIRM does when the write FAILS is half of this
# suite's subject -- a dose whose append never reaches the disk, a weigh-in
# whose rewrite cannot be renamed -- and neither can be arranged on a healthy
# filesystem except on demand (see util.c). Nothing that ships carries it.
modeltest: $(MODEL_OBJ) | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc $(MODEL_OBJ) -lpthread -o $(TESTDIR)/modeltest
	@$(TESTENV) ./$(TESTDIR)/modeltest > $(TESTDIR)/modeltest.log 2>&1 \
	    && grep -q "modeltest: the model the app builds" $(TESTDIR)/modeltest.log \
	    && printf '\033[1;32mmodeltest\033[0m: build_model + screen mapping OK\n' \
	    || { cat $(TESTDIR)/modeltest.log; exit 1; }

# THE TLS PRIMITIVES, against the documents that define them. NIST's GCM and
# ECDSA vectors, RFC 5869's HKDF, RFC 4231's HMAC, RFC 8448's TLS 1.3 key
# schedule -- so a pass means the code agrees with the specification and not
# merely with itself. Nothing in srv/tls.c may reach a socket until this
# passes: a cipher that is wrong in a way only a peer notices is the worst
# kind of bug to chase.
# The crash logger's two formatters run inside a signal handler, so they may
# not call snprintf and are hand-rolled -- and while they sat inside main.c
# nothing could reach them. Splitting crashlog.c out is what made this
# possible; the cases are mostly hostile, because the one property that
# matters is that they never write past the buffer while recording a crash.

# ---- WHERE A SERVER HOST TEST BINARY LIVES, PER BUILD MODE -------------
#
# The same rule as TESTDIR, for the same reason, on the other half of the tree.
# srvtsan is a recursive make of httptest with ThreadSanitizer flags, and
# `check` lists httptest AND srvtsan as independent prerequisites -- so both
# wrote and then executed build/srv/test/httptest, one path holding two
# configurations, and under `make -j check` the plain build and the TSan build
# raced for it. A TSan pass that actually ran the -O2 binary reports "no data
# races" having instrumented nothing.
#
# Only httptest is built twice today. The variable covers every server host
# suite regardless, because the next one to acquire a sanitizer pass should not
# have to discover this again.
SRVTESTDIR ?= build/srv/test

# THE TRANSPORT CONTRACT, against a socket that really does push back.
#
# http.h promises write() moves every byte or fails. The plain transport did
# not: one write(2), and a client that stops reading makes it return a short
# count that the response path then treated as success. It needs a real
# BACKPRESSURED socket to show up at all, which is what this builds.
httptest:
	@mkdir -p $(SRVTESTDIR)
	$(CCACHE) cc -iquote srv -iquote lib $(TESTWARN) srv/test/httptest.c srv/http.c \
	    srv/util.c lib/rand.c lib/randunix.c lib/sha256.c -lpthread -o $(SRVTESTDIR)/httptest
	@./$(SRVTESTDIR)/httptest > $(SRVTESTDIR)/httptest.log 2>&1 \
	    && grep -q "ALL HTTP TESTS PASSED" $(SRVTESTDIR)/httptest.log \
	    && printf '\033[1;32mhttptest\033[0m: writes complete or fail, never short\n' \
	    || { cat $(SRVTESTDIR)/httptest.log; exit 1; }


# THE WIRE CONTRACT, against permanent vectors -- and the one place both
# halves' headers are included together, so the compiler compares their
# limits instead of a Makefile grep comparing one of them.
wiretest:
	@mkdir -p $(SRVTESTDIR)
	$(CCACHE) cc -iquote srv -iquote lib -iquote app $(TESTWARN) srv/test/wiretest.c \
	    lib/sha256.c lib/hmac.c srv/route.c srv/sigstr.c \
	    -o $(SRVTESTDIR)/wiretest
	@./$(SRVTESTDIR)/wiretest

# THE ROW DECODER, against the rows a network can deliver. Pure: no database,
# no sockets, no threads.
#
# srv/route.c is linked in as well, for the one thing it shares with the row
# decoder and with nothing else: a bounded decimal parse whose answer decides
# whether attacker-supplied text becomes a stored number. The two overflow
# rules have to agree, so they are checked side by side and in one place --
# srv/test/wiretest.c owns route CLASSIFICATION against the wire vectors,
# this owns the number grammar underneath it.
rowtest:
	@mkdir -p $(SRVTESTDIR)
	$(CCACHE) cc -iquote srv -iquote lib $(TESTWARN) srv/test/rowtest.c srv/rowdec.c \
	    srv/route.c \
	    -o $(SRVTESTDIR)/rowtest
	@./$(SRVTESTDIR)/rowtest

# THE GIF ENCODER, INTERLEAVED AND CONCURRENT. Its LZW dictionary used to be
# file-scope, so two renders at once emitted streams that decode to garbage --
# quietly, because a wrong GIF is still a GIF. Needs -pthread; the case that
# proves the test can tell the difference deliberately shares one workspace.
giftest:
	@mkdir -p $(SRVTESTDIR)
	$(CCACHE) cc -iquote srv -iquote lib $(TESTWARN) srv/test/giftest.c srv/gif.c \
	    lib/plot.c -o $(SRVTESTDIR)/giftest -pthread
	@./$(SRVTESTDIR)/giftest

# A NAMED DAY IS A DATE THE CALENDAR HAS. The day field was bounded to 1..31
# for every month and then handed to timegm, whose job is to NORMALISE: /day-
# 20250231 rendered March 3rd under a heading that said 2025-02-31, because the
# heading was printed from the digits asked for. Drives the real router with
# the database, the renderer and the page skeleton stubbed, so each case sees
# exactly what the router decided -- which handler, which window, which title.
datetest:
	@mkdir -p $(SRVTESTDIR)
	$(CCACHE) cc -iquote srv -iquote lib $(SQLINC) $(TESTWARN) srv/test/datetest.c \
	    srv/plotpages.c srv/util.c lib/rand.c lib/randunix.c lib/sha256.c \
	    -o $(SRVTESTDIR)/datetest
	@./$(SRVTESTDIR)/datetest > $(SRVTESTDIR)/datetest.log 2>&1 \
	    && grep -q "ALL DATE TESTS PASSED" $(SRVTESTDIR)/datetest.log \
	    && printf '\033[1;32mdatetest\033[0m: nonexistent dates are refused, not normalised\n' \
	    || { cat $(SRVTESTDIR)/datetest.log; exit 1; }

# lib/jpake.c is linked in as well as the TLS primitives. The pairing exchange
# is the other half of "keys agreed with a peer", it has published vectors of
# its own, and the property this test pins about it -- that a peer round is
# never marked accepted until its zero-knowledge proof passes -- is a STATE
# assertion no shell test over the wire can make.
#
# -DTLS_FAULTS: the ClientHello and ticket cases need srv/tls.c's injected
# monotonic clock, and its three doors onto sealing, opening and sending a
# ticket. Same shape as the -DAPP_FAULTS suites above and -DDB_FAULTS in
# faulttest: this is a test binary of its own, so nothing that ships is built
# with the hooks. "No monotonic clock, no ticket" is otherwise unreachable --
# clock_gettime(CLOCK_MONOTONIC) does not fail on a test machine.
cryptotest:
	@mkdir -p build/lib/test
	$(CCACHE) cc -iquote lib -iquote srv $(TESTWARN) -DTLS_FAULTS srv/test/cryptotest.c srv/tls.c lib/ct.c \
	    $(LIBCRYPTO) lib/jpake.c lib/aes.c lib/sha256.c lib/p256.c lib/rand.c lib/randunix.c \
	    -o build/lib/test/cryptotest
	@./build/lib/test/cryptotest > build/lib/test/cryptotest.log 2>&1 \
	    && grep -q "agree with the specs" build/lib/test/cryptotest.log \
	    && printf '\033[1;32mcryptotest\033[0m: GCM, HKDF, ECDSA and J-PAKE vs the specs OK\n' \
	    || { cat build/lib/test/cryptotest.log; exit 1; }

# THE CORRECTION PATH. calib.c decides whether the number shown is the number
# the sensor meant: an implausible factor must be REJECTED and not clamped, a
# stale fingerstick reference must EXPIRE and not apply, a backfilled point
# older than the correction must keep its raw value, and a confirmed
# calibration must never vanish without saying so. None of that was reachable
# by any test while it lived inside main.c.

# Behavioural gate for the rolling TIR/average buckets. The ring aliases an
# over-old reading onto a live bucket and ZEROES it, so the boundary guards are
# the whole safety of the statistics -- and they are exactly what a hand-check
# reads past.

# Behavioural gate for the OneTouch meter driver, which had none. It decides
# which fingersticks reach the append-only log, and its timestamp gate, its
# walk-advance rule and its counter handling have all been wrong at some point.

# Behavioural gate for the provenance registry. An id names one physical device
# forever and readings.csv cites those ids in rows that are never rewritten, so
# an id reused is one sensor's history permanently merged into another's.
# sensors.c was linked into other test binaries but nothing called mint, claim,
# forget or rebind.

# Behavioural gate for settings persistence. alarm_load's validation is the
# last thing standing between a corrupt file and a hypo alarm that cannot fire
# (an out-of-range low disables it; low > high latches both) -- while still
# accepting low == high, which alarm_step legitimately produces.
# The two implementations against each other. SKIPS when the server is not
# built, so pancra still builds on a machine that has never seen it.
interoptest: $(SRVBIN)
	@mkdir -p $(TESTFIXDIR)
# -DAPP_FAULTS: the restore path stages a file and publishes it by rename, and
# the two answers that matter -- refused with the original intact, and
# published-but-not-proven-durable -- are only reachable by making a rename or
# a directory fsync fail on demand. Without it the durability half of the
# restore is untested against a real server.
	$(CCACHE) cc -iquote app -iquote lib $(TESTWARN) -DAPP_FAULTS app/test/interoptest.c app/sync.c app/syncrow.c app/util.c app/clock.c \
	    lib/jpake.c lib/rand.c lib/randunix.c app/dexcom.c lib/p256.c lib/sha256.c lib/aes.c lib/ecdsa.c \
	    lib/hmac.c \
	    -o $(TESTDIR)/interoptest
	@$(TESTBINENV) ./app/test/interop.sh > $(TESTDIR)/interoptest.log 2>&1 \
	    || { cat $(TESTDIR)/interoptest.log; exit 1; }
	@if grep -q "ALL INTEROP TESTS PASSED" $(TESTDIR)/interoptest.log; then \
	    printf '\033[1;32minteroptest\033[0m: the app and the server agree\n'; \
	 elif grep -q "SKIP" $(TESTDIR)/interoptest.log; then \
	    printf '\033[1;33minteroptest\033[0m: skipped (the server is not built)\n'; \
	 else cat $(TESTDIR)/interoptest.log; exit 1; fi


# Behavioural gate for the insulin dose log. Doses are user-entered facts in an
# append-only file the app reloads at every launch: the load-time validation is
# what keeps a corrupt row from resurrecting forever, and last-units-per-type is
# what the LOG INSULIN form pre-populates with.


# Behavioural gate for the scan-lifecycle decision, which governs whether an
# already-paired CGM can reconnect at all. It has been wrong in both directions.


# Offline end-to-end protocol test: a simulated Stelo runs the real J-PAKE
# server side and answers the driver's writes, and the final glucose is decoded
# from REAL captured bytes. Covers subscribe sequencing, round
# request/reassembly/chunking, the 02/03/04/05 auth exchange, shared-key
# agreement + persistence, and EGV decode.
#
# This existed but was built by NO target, so nothing exercised dexdriver.c,
# jpake.c, dexcom.c, dexdata.c, p256.c, aes.c or sha256.c -- the crypto and protocol
# core had zero automated coverage, and the test had rotted (stale include, a
# typedef the tree no longer uses, four transport hooks added since). Wiring it
# in means a new hook breaks the build instead of silently going untested.
# NO app/clock.c. The harness defines mono_s/realtime_s/now_ms itself (the
# same pattern calibtest, pairingtest and remotetest already use), because the
# one thing this suite has to be able to express is the two clocks DISAGREEING:
# the wall clock stepping backwards an hour while the monotonic one carries on.
# That is what a phone does when it comes back from being off or finds a
# network, and it is what used to wrap the sensor's projected session time --
# see sens_project_clock in app/senslogic.h. Linked against the real clock.c
# the projection could only ever be observed agreeing with itself.
#
# app/senslogic.c is here for that projection: it is the pure half of the
# session-clock arithmetic, and the driver calls it.
DRVTEST_SRC := app/test/test_driver.c app/dexdriver.c lib/jpake.c lib/rand.c lib/randunix.c \
               app/dexcom.c app/dexdata.c lib/p256.c lib/sha256.c lib/aes.c \
               lib/ecdsa.c app/util.c app/senslogic.c

drivertest: | require-jdk
	@mkdir -p $(TESTFIXDIR)
	$(CCACHE) cc -DDEXDRIVER_TEST -iquote app -iquote lib -iquote app/test $(JVM_INC) $(TESTWARN) \
	    $(DRVTEST_SRC) -o $(TESTDIR)/drivertest
	$(TESTENV) ./$(TESTDIR)/drivertest > $(TESTDIR)/drivertest.log 2>&1 \
	    && grep -q "ALL DRIVER TESTS PASSED" $(TESTDIR)/drivertest.log \
	    && printf '\033[1;32mdrivertest\033[0m: pairing + auth + EGV decode OK\n' \
	    || { tail -20 $(TESTDIR)/drivertest.log; exit 1; }

.PHONY: calibtest duocheck ingesttest interoptest modeltest srvasan weighttest
# srvasanbuild is the shared sanitizer server build that srvasan and tlsasan
# both depend on. It has to be phony for the ordinary reason -- it is a verb,
# not a file -- and phonycheck would say so, but the consequence here is
# specific: were a directory named srvasanbuild ever to appear, make would call
# it up to date and BOTH sanitizer suites would run whatever binary happened to
# be in build/srv-asan/ from the last build, at whatever flags that was.
.PHONY: srvasanbuild datetest
.PHONY: versioncheck harnesstest bondtabletest symcheck synclogcheck initcheck stalecheck clitest emailtest deploydrill adbdrill wiretest dbmigtest appasan apptsan srvtsan metersesstest jbridgetest giftest rowtest manifestcheck inclusions lockorder actioncheck faulttest dbctxtest phonycheck srvonlycheck deploycheck duodeploy duorollback duorotate duobackup restoredrill pairingtest remotetest exercisetest foodtest clockcheck httptest app srv duo duosmoke devicecheck apkcheck lockcheck settingscheck sizecheck boundaryjavatest exportjavatest srvcheck tlstest tlsasan crashtest cryptotest FORCE all release release-config releasecheck aab install run uninstall clean check crosscheck javacheck format format-fix tidy done uitest plottest drivertest alarmtest storetest statstest metertest registrytest settingstest scantest senstest gesturetest syncstattest durabilitytest meterstoretest remotetest pairingtest insulintest threadtest notifytest

boundaryjavatest: app/BoundaryLogic.java app/test/BoundaryLogicTest.java | require-jdk
	@mkdir -p build/app/hostjava
	@javac -d build/app/hostjava $^
	@java -cp build/app/hostjava com.jk.pancra.BoundaryLogicTest

# The same arrangement for the export writer, and for the same reason: every
# interesting failure it has to survive -- a source that throws part way
# through a read, an output whose CLOSE is what fails, a CSV whose tail is a
# row native is still appending -- is a thing no phone will produce on demand,
# so on a phone this code is only ever exercised by the happy path. Here they
# are ordinary test inputs, and the assertions are about what is left ON DISK
# afterwards: no published file, no temporary, no manufactured row.
exportjavatest: app/BoundaryLogic.java app/ExportSnapshot.java \
                app/test/ExportSnapshotTest.java | require-jdk
	@mkdir -p build/app/hostjava
	@javac -d build/app/hostjava $^
	@java -cp build/app/hostjava com.jk.pancra.ExportSnapshotTest

# The expectations are PASSED IN rather than re-derived by the script, so
# there is exactly one place each number is written (app/published.mk and the
# variables above) and the script's job is purely to say what the package
# declares. REQUIRE_NEWER is off here: this is the debug-signed dev APK,
# which cannot be an update to anything -- Android refuses a signature
# change before it ever looks at the version code. The release paths turn it
# on.
apkcheck: $(APK)
	@EXPECT_VERSION_CODE='$(VERSION_CODE)' EXPECT_VERSION_NAME='$(VERSION_NAME)' \
	 EXPECT_MIN_SDK='$(MIN_SDK)' EXPECT_TARGET_SDK='$(TARGET_SDK)' \
	 EXPECT_COMPILE_SDK='$(COMPILE_SDK)' REQUIRE_NEWER=0 \
	 ./app/test/apkcheck.sh $(APK)

# THE PHONE-FACING SCRIPTS, RUN WITHOUT A PHONE.
#
# This tests app/fetch.sh and app/test/devicesmoke.sh, NOT the device: `adb` is
# a shell script on PATH that can be told to fail at a chosen point, to hand
# back half a file and then die, or to report a permission as requested-and-
# denied. Exactly the arrangement deploydrill uses for the board.
#
# It belongs in `check` and devicecheck does not, for the reason the two are
# different things: devicecheck needs hardware and says something about a
# phone, while this needs nothing and says whether the scripts would NOTICE.
# They would not -- a failed adb and a clean result were the same exit status
# in both of them, so a truncated pull and an unreachable phone each produced
# a green run. No device, no network, no build.
adbdrill:
	@./app/test/adbdrill.sh

# =====================================================================
# THE SERVER (srv/)
# ---------------------------------------------------------------------
# glucoserve: the multi-user HTTPS server the app syncs to. It was its own
# repository until the two were merged; what made that worth doing is lib/ --
# the J-PAKE pairing and the plot renderer are ONE copy now, compiled by both
# sides, instead of a copy in each that had already drifted apart in comments
# while staying byte-identical in code.
#
# Native build (what the tests run):   make srv
# For the Milk-V Duo (riscv64/musl):   make duo
#
# CROSS implies a fully static, stripped binary. -no-pie is required rather
# than preferred: that toolchain cannot produce a static PIE containing
# thread-local storage, which the worker pool needs.
CROSS ?=
DUOCROSS = tools/riscv64-linux-musl-cross/bin/riscv64-linux-musl-
# Where `make duo` leaves its binary, computed the same way the build computes
# it, so the deploy and the smoke test cannot be told a path the build does
# not use. (Recursive expansion, because SRVMODE/CROSSID are defined below.)
DUOID    = $(notdir $(patsubst %-,%,$(DUOCROSS)))
DUOBIN   = build/srv-$(DUOID)/sync
DUOARCH  = $(firstword $(subst -, ,$(DUOID)))
# The object tree (and the binaries in it) are keyed by build mode, so a
# native build and a cross build cannot overwrite each other's files -- see
# the .build-mode stamp below for the failure that motivated it. Native keeps
# the plain build/srv/ path so nothing else has to change; a cross build goes
# to build/srv-<target>/, named after the toolchain prefix.
# WHICH CROSS TARGET, from the prefix itself. `-cross` for all of them meant
# two different targets shared one object tree and one output binary: build
# for the Duo, then for anything else, and the second build silently reuses
# the first's objects for whatever it does not have to recompile. The prefix
# already names the target, so the tree is named after it -- and two cross
# builds can coexist the way native and cross now do.
CROSSID = $(notdir $(patsubst %-,%,$(CROSS)))
SRVMODE = $(if $(CROSS),-$(CROSSID),)
# ...and the machine, for the banner. This was the literal string "riscv64" for
# every cross build, which is a claim, not a fact.
ARCH = $(if $(CROSS),$(firstword $(subst -, ,$(CROSSID))),native)
SRVCC  = $(CCACHE) $(CROSS)gcc
# THE SAME WARNINGS AS THE APP, and errors for the same reason. This half is
# the one strangers can reach: it terminates TLS and parses HTTP on the open
# internet, and it was building with -Wall -Wextra and no -Werror while the
# app -- which talks only to a sensor over BLE -- ran fourteen flags and
# treated every one as fatal. The weaker gate was on the more exposed code.
#
# Turning it on cost three fixes in the whole tree (two signedness mismatches
# on an HMAC argument, one const cast in the test client), which is the real
# argument for it: the code already satisfied this standard, nothing was
# checking that it kept doing so. sqlite is compiled separately and is not
# subject to these.
SRVWARN := -Werror -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
           -Wmissing-prototypes -Wwrite-strings -Wvla -Wformat=2 -Wundef \
           -Wdouble-promotion -Wcast-qual -Wswitch-enum -Wredundant-decls
SRVCFLAGS ?= $(STD) -Os $(SRVWARN)
# WHAT `CROSS=` ACTUALLY SUPPORTS: an LP64 static Linux target whose toolchain
# is a gcc with this prefix, and whose compiler has `unsigned __int128`.
#
# THAT IS NARROWER THAN "another Linux target", and the difference is not
# stylistic -- it is what the code already refuses to compile without:
#
#   LP64. srv/proto.h asserts sizeof(long) >= 8 and sizeof(time_t) >= 8,
#   because every id, timestamp and bucket on this wire is a C long, printed
#   and parsed with %ld on both sides. On a 32-bit target that build does not
#   fail quietly and later: the static assertions stop it.
#
#   A 64x64->128 MULTIPLY. lib/p256.c asserts the presence of GCC/Clang's
#   `unsigned __int128` and provides no fallback, so P-256 -- and with it the
#   pairing and every signature -- cannot be built for a target without it.
#
# So an armv7 example was never buildable, and citing one invited somebody to
# find that out from a compiler error rather than from a sentence. Supporting
# 32-bit targets means a fixed-width port of the wire format and a portable
# 128-bit multiply: a substantially larger change, not a toolchain prefix.
#
# The flags below are a policy, not a deduction:
#   -static  because the board's libc is not the one this links against, and a
#            dynamically linked binary copied there does not start;
#   -no-pie  because the musl toolchain used for the Duo cannot produce a
#            static PIE containing thread-local storage, which the worker pool
#            needs -- this is required, not preferred;
#   -s       because the board has 56 MB of RAM and no debugger on it.
# A target that needs a different policy sets CROSS_LDFLAGS itself:
#   make srv CROSS=<prefix> CROSS_LDFLAGS='-static'
CROSS_LDFLAGS ?= -static -no-pie -s
ifneq ($(CROSS),)
SRVLDFLAGS += $(CROSS_LDFLAGS)
endif

# Third-party, NOT in git: everything downloaded rather than written lives in
# tools/, which .gitignore covers -- see README for what to fetch and where
# from. lib/sqlite wins if you would rather vendor it beside our own sources.
# TLS is OURS now: srv/tls.c is a TLS 1.3 server in about a thousand lines,
# on top of lib/gcm.c, lib/hkdf.c, lib/ecdsa.c and the P-256, SHA-256 and AES the
# pairing already needed. No library to fetch, no second build system, and no
# code for the options this server never offers.
SQLITE  ?= $(firstword $(wildcard lib/sqlite) tools/sqlite-amalgamation-3460100)
TLSINC  =
TLSSRC  =
SQLINC  = -I$(SQLITE)
SQLSRC  = $(SQLITE)/sqlite3.c
# THREADSAFE=1 (serialized): requests are served by a pool of threads, so
# several may be inside sqlite at once. Mode 1 lets them share one connection
# and takes the locking internally -- the alternative, a connection per
# thread, would multiply the page cache on a 56 MB board for no gain, since
# the queries are microseconds and it is the WAITING that needed to overlap.
SQLFLAGS = -DSQLITE_THREADSAFE=1 -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
           -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED \
           -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_AUTOINIT \
           -DSQLITE_LIKE_DOESNT_MATCH_BLOBS -DSQLITE_MAX_EXPR_DEPTH=0

# The shared half: one copy in lib/, compiled by the app too (see SRC above).
SHARED = lib/jpake.c lib/rand.c lib/randunix.c lib/ct.c lib/p256.c lib/sha256.c lib/aes.c lib/plot.c

# The rest of lib/. Same rule as above -- one algorithm per file, nothing in
# them that knows what a glucose reading or an HTTP request is -- these are
# just the ones the server links and the app mostly does not. `ecdsa` is the
# exception and appears in SRC too: app/dexcom.c signs the sensor's key
# challenge with it rather than carrying a second copy of the arithmetic.
LIBCRYPTO = lib/gcm.c lib/hmac.c lib/hkdf.c lib/pbkdf2.c lib/ecdsa.c

SRVSRC = srv/sync.c srv/db.c srv/util.c srv/auth.c srv/logs.c srv/pair.c \
         srv/web.c srv/page.c srv/oops.c srv/home.c srv/settings.c srv/invite.c \
         srv/plotpages.c srv/plots.c srv/gif.c srv/rowdec.c srv/route.c \
         srv/sigstr.c \
         srv/http.c \
         srv/https.c \
         srv/tls.c $(LIBCRYPTO) $(SHARED)

# Per-file objects, under build/ like everything else, and keeping the source
# directory so srv/util.c and app/util.c app/clock.c can never collide. -MMD writes the
# header dependencies as it goes, which is stricter than listing them by hand.
SRVOBJ = $(patsubst %.c,build/srv$(SRVMODE)/obj/%.o,$(SRVSRC))
SRVDEP = $(SRVOBJ:.o=.d)
SRVINC = -iquote srv -iquote lib
SRVOBJFLAGS = $(SRVCFLAGS) $(SRVINC) $(TLSINC) $(SQLINC) -MMD -MP

SRVBIN  = build/srv$(SRVMODE)/sync
SRVCLI  = build/srv$(SRVMODE)/synccli

srv: $(SRVBIN) $(SRVCLI)

# The board build, so the cross-compiler's path lives here once instead of in
# whoever's shell history. Recursive because CROSS has to be set before the
# rules above are evaluated.
# THE CONFIGURATION THAT ACTUALLY SHIPS, compiled by the gate.
#
# `check` built the aarch64 library and the NATIVE server and stopped there,
# so the riscv64/musl build that runs on the board was compiled by nothing and
# linted by nothing -- it happened to work, and nothing would have said if it
# stopped. This compiles it (into its own object tree, so it cannot disturb
# the native one) and, like the other tools, says so rather than vanishing
# when the cross-compiler is absent.
duocheck:
	@if [ ! -x $(DUOCROSS)gcc ]; then \
	   if [ "$${ALLOW_SKIP:-0}" = "1" ]; then \
	     printf 'duocheck: cross-compiler absent, SKIPPED (ALLOW_SKIP=1)\n'; \
	     exit 0; \
	   fi; \
	   echo "duocheck: $(DUOCROSS)gcc missing, so the board's own build is"; \
	   echo "  UNTESTED. See README, or re-run with ALLOW_SKIP=1."; \
	   exit 1; \
	 fi; \
	 $(MAKE) --no-print-directory srv CROSS=$(DUOCROSS) >/dev/null || exit 1; \
	 printf '\033[1;32mduocheck\033[0m: the riscv64 board build compiles\n'

duo:
	@test -x $(DUOCROSS)gcc || { echo "$(DUOCROSS)gcc missing -- see README"; exit 1; }
	$(MAKE) srv CROSS=$(DUOCROSS)
	@printf '\033[1;32mduo\033[0m: %s (%s)\n' "$(DUOBIN)" "$(DUOARCH)"

# Requires an already-authorized SSH login. It never deploys or mutates Duo:
# a smoke test that installed something could not answer the one question it
# exists for -- is what we deployed LAST time still alive. Deploying is
# `make duodeploy`, and it is the only thing that does.
duosmoke: duo
	@PANCRA_LOCAL_BIN=$(DUOBIN) ./srv/test/duosmoke.sh $(DUOBIN)

# THE DEPLOYMENT, in three verbs that all read srv/deploy/pancra.conf. None of
# them is reachable from `check`: they touch the running service.
# THE DEPLOYMENT CONTRACT IS ONE FILE, and this is what keeps it one.
#
# Every path on the board used to live in srv/test/duosmoke.sh and nowhere
# else -- the executable, the pid file and the log, spelled out three times,
# still naming the directory the service had before it was called Pancra. A
# test WAS the deployment contract, and there was nothing to check it against.
#
# So: the paths live in srv/deploy/pancra.conf, every script sources it, and
# this gate fails if any of them starts spelling one out again. It also
# parses every script (a deploy script with a syntax error is discovered
# during an incident) and refuses to let the smoke test grow the ability to
# install something -- a smoke test that deploys cannot answer the question it
# exists for.
# A SERVER-ONLY MACHINE, tested rather than described.
#
# The README says the server needs build-essential, curl and sqlite's
# amalgamation -- no Android SDK, no JDK, no clang. That was false for two
# years in the only way that matters: make evaluates the whole file before it
# considers the goal, so an unconditional $(error) for a missing clang or JDK
# rejected `make srv` on exactly the machine the server runs on.
#
# This runs make the way that machine would: with both tools declared absent
# (a command-line assignment beats the `?=` discovery, so nothing is found
# even if it is installed here). And it checks the other half too -- the app
# build must STILL refuse, with a sentence rather than a compiler error from
# inside a recipe -- because a guard that never fires is not a fixed guard, it
# is a deleted one.
srvonlycheck:
	@mkdir -p build
	@# THE SERVER REALLY BUILDS WITH NO ANDROID TOOLCHAIN -- built, not
	@# printed. This half used to be `make -n`, which is exactly the
	@# blindness the app half below was rewritten to escape: -n prints
	@# recipes without running them, so a require-clang wrongly added to a
	@# SERVER rule would be printed and never executed, and this check would
	@# report success over a server that no longer builds. Proven: adding
	@# `| require-clang` to the server object rule passed the -n form and
	@# failed the real one.
	@rm -rf build/srvonly && mkdir -p build/srvonly
	@$(MAKE) --no-print-directory srv CLANG= JAVA_HOME= JNI_PLATFORM= \
	     SRVMODE=only >build/srvonly.log 2>&1 \
	   || { echo "srvonlycheck: 'make srv' does not BUILD without clang/JDK:"; \
	        tail -20 build/srvonly.log; exit 1; }
	@$(MAKE) --no-print-directory -n duo CLANG= JAVA_HOME= JNI_PLATFORM= \
	     >/dev/null 2>build/srvonly.log \
	   || { echo "srvonlycheck: 'make duo' does not work without clang/JDK:"; \
	        cat build/srvonly.log; exit 1; }
	@# EACH GUARD WORKS: run it for real, with its own tool missing.
	@# `make -n` cannot establish this -- a deleted guard and a working one
	@# look identical under it.
	@for g in require-clang require-jdk; do \
	   if $(MAKE) --no-print-directory $$g CLANG= JAVA_HOME= JNI_PLATFORM= \
	        >/dev/null 2>&1; then \
	     echo "srvonlycheck: $$g accepted a missing tool."; \
	     echo "  It must say which tool is missing and point at the README"; \
	     echo "  -- otherwise the failure arrives from inside a recipe,"; \
	     echo "  naming a compiler path nobody chose."; exit 1; \
	   fi; \
	 done
	@# ...AND EACH IS WIRED to the app build, checked by ITS OWN sentence.
	@#
	@# This loop used to grep for "$$g\|no clang found\|no JDK found" -- an
	@# alternation in which the clang sentence matched during the require-jdk
	@# iteration too, so unwiring require-jdk from every rule left the check
	@# green. `$$g` never matches anything: -n prints recipe TEXT, not target
	@# names. Each guard is now looked for by the words only it prints.
	@$(MAKE) --no-print-directory -n app CLANG=cc JAVA_HOME=/nonexistent \
	     JNI_PLATFORM=linux >build/srvonly-app.log 2>/dev/null || true
	@grep -q 'no clang found' build/srvonly-app.log || { \
	   echo "srvonlycheck: the app build does not reach require-clang."; \
	   echo "  The guard exists but nothing depends on it, so a machine"; \
	   echo "  without clang would fail inside a recipe instead."; exit 1; }
	@grep -q 'no JDK found' build/srvonly-app.log || { \
	   echo "srvonlycheck: the app build does not reach require-jdk."; \
	   echo "  The guard exists but nothing depends on it, so a machine"; \
	   echo "  without a JDK would fail inside a recipe instead."; exit 1; }
	@# ...AND SO DOES EVERY HOST TEST THAT COMPILES A JNI INCLUDE PATH.
	@# `make remotetest JAVA_HOME=` used to die with "jni.h: No such file or
	@# directory" -- a failure from inside a recipe naming a path nobody
	@# chose, which is the thing this whole mechanism exists to remove.
	@#
	@# AND THE DRY RUN ITSELF HAS TO SUCCEED. This loop used to be
	@#
	@#     $$(MAKE) -n $$g ... 2>/dev/null | grep -q 'no JDK found' || fail
	@#
	@# and a pipeline carries only the LAST command's status, so the nested
	@# make's was discarded. A `make -n` that printed the guard's sentence and
	@# then died -- a prerequisite with no rule, an included fragment that no
	@# longer parses -- satisfied grep, and this check reported that every JNI
	@# host test reaches require-jdk on the strength of a dry run that had
	@# failed. Its stderr was thrown away too, so nothing anywhere said so.
	@#
	@# `set -o pipefail` is NOT the fix while `grep -q` is on the right-hand
	@# side: grep -q exits the instant it matches, make is then writing to a
	@# closed pipe, and a healthy dry run would start failing for that. So the
	@# two streams go to FILES, the status is taken from make itself, and the
	@# grep runs afterwards over stdout only -- stderr can never satisfy it.
	@# The captured output is printed only on failure: a passing dry run is
	@# thousands of lines of recipe text nobody wants in a green run.
	@for g in remotetest modeltest jbridgetest pairingtest drivertest uitest \
	          tidy; do \
	   $(MAKE) --no-print-directory -n $$g CLANG=cc JAVA_HOME=/nonexistent \
	       JNI_PLATFORM=linux >build/srvonly-jni.log 2>build/srvonly-jni.err; \
	   st=$$?; \
	   if [ $$st -ne 0 ]; then \
	     echo "srvonlycheck: \`make -n $$g\` FAILED (exit $$st), so whether it"; \
	     echo "  reaches require-jdk was never established -- a dry run that"; \
	     echo "  dies after printing the guard's sentence used to pass here."; \
	     tail -n 20 build/srvonly-jni.err build/srvonly-jni.log; \
	     exit 1; \
	   fi; \
	   grep -q 'no JDK found' build/srvonly-jni.log || { \
	     echo "srvonlycheck: $$g compiles a JNI include path without"; \
	     echo "  reaching require-jdk, so a machine with no JDK fails"; \
	     echo "  inside the recipe instead of being told what is missing."; \
	     exit 1; }; \
	 done
	@rm -rf build/srvonly build/srvonly-app.log build/srvonly-jni.log \
	        build/srvonly-jni.err
	 printf '\033[1;32msrvonlycheck\033[0m: the server builds with server tools only\n'

# EVERY PROCEDURAL TARGET IS .PHONY, and this is what says so.
#
# Seven were not: calibtest, ingesttest, weighttest, modeltest, interoptest,
# srvasan and duocheck. A non-phony target whose name is not a file works
# only by accident -- the day something creates a file or directory of that
# name in the tree, make declares it up to date and the test silently stops
# running. `make check` would still print its whole green list.
#
# The list is derived from the Makefile rather than maintained beside it,
# because a hand-kept list of what must be in a hand-kept list is two lists.
actioncheck:
	@bad=$$(grep -nE '(action|code)[[:space:]]*[!=]=[[:space:]]*[0-9]+' \
	          app/*.c app/*.h); \
	 if [ -n "$$bad" ]; then \
	   echo "$$bad"; \
	   echo "actioncheck: a UI action compared against a BARE INTEGER."; \
	   echo "  Every one of these was an MA_* value written out as a number,"; \
	   echo "  and the enum has been reordered since. Eleven of them shipped"; \
	   echo "  to the phone: MA_NONE joined the head of the list, so every"; \
	   echo "  settings toggle ran the NEXT row's handler -- ORIENT toggled"; \
	   echo "  the sound -- and the keypad's X, its DEL and the device"; \
	   echo "  list's CANCEL matched nothing at all, leaving no way off the"; \
	   echo "  screen. Name the action; the compiler cannot check a 113."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mactioncheck\033[0m: every UI action is named, not numbered\n'

phonycheck:
	@ph=$$(sed -n 's/^\.PHONY://p' Makefile | tr ' ' '\n' | grep -v '^$$' | sort -u); \
	 tg=$$( { grep -oE '^[A-Za-z][A-Za-z0-9_.-]*:' Makefile | tr -d ':'; \
	          grep -oE 'call APPTEST,[a-z0-9_]+' Makefile | cut -d, -f2; } \
	        | sort -u); \
	 bad=$$(printf '%s\n' "$$tg" | while read -r t; do \
	          printf '%s\n' "$$ph" | grep -qx "$$t" || echo "$$t"; \
	        done); \
	 for t in $$bad; do \
	   if [ -e "$$t" ]; then continue; fi; \
	   echo "phonycheck: '$$t' has a recipe, is not a file, and is not .PHONY."; \
	   echo "  The day something creates a file of that name, make will call"; \
	   echo "  it up to date and the target will silently stop running --"; \
	   echo "  while check still prints it in the green list."; \
	   exit 1; \
	 done; \
	 printf '\033[1;32mphonycheck\033[0m: every procedural target is .PHONY\n'

deploycheck:
	@set -e; \
	 for f in srv/deploy/deploy.sh srv/deploy/rollback.sh srv/deploy/backup.sh \
	          srv/deploy/restore.sh srv/deploy/rotate.sh srv/deploy/health.sh \
	          srv/deploy/start.sh srv/deploy/lock.sh srv/deploy/supervisor.sh \
	          srv/deploy/supervise.sh srv/test/duosmoke.sh \
	          srv/test/restoredrill.sh; do \
	   sh -n $$f || { echo "deploycheck: $$f does not parse"; exit 1; }; \
	   grep -q 'pancra.conf' $$f || [ "$$f" = srv/test/restoredrill.sh ] || \
	     { echo "deploycheck: $$f does not read the deployment contract"; exit 1; }; \
	 done; \
	 if grep -lnE '(^|&&|;|\|)[[:space:]]*setsid[[:space:]]' \
	      srv/deploy/*.sh srv/deploy/*.md srv/test/*.sh 2>/dev/null | \
	      grep -v '^srv/deploy/start.sh$$'; then \
	   echo "deploycheck: the start command is written out somewhere other"; \
	   echo "  than srv/deploy/start.sh. It existed inline four times --"; \
	   echo "  three scripts and a paste-this block in the deployment"; \
	   echo "  guide -- and the fourth spelled the port and both pem paths as"; \
	   echo "  literals and never recorded the new pid, so sync.pid kept"; \
	   echo "  naming the process that had just been killed."; \
	   exit 1; \
	 fi; \
	 for v in PANCRA_FRONT PANCRA_FRONT_OWNER PANCRA_PUBLIC_PORT \
	          PANCRA_SUPERVISOR PANCRA_BOOT_HOOK PANCRA_DOWN; do \
	   grep -q "^$$v=" srv/deploy/pancra.conf || \
	     { echo "deploycheck: the contract does not declare $$v."; \
	       echo "  The server listens on PANCRA_PORT and the health checks"; \
	       echo "  probe a public URL on another one. What maps the two is"; \
	       echo "  part of the deployment, and a procedure that never names"; \
	       echo "  it cannot restore, roll back or debug it."; exit 1; }; \
	 done; \
	 for f in deploy rollback backup restore rotate; do \
	   grep -q 'lock_take' srv/deploy/$$f.sh || \
	     { echo "deploycheck: $$f.sh does not take the board lock."; \
	       echo "  Deployments, rollbacks, backups, restores and rotations"; \
	       echo "  share the staged artifact names, the pid file, the running"; \
	       echo "  process and the live database. Two at once publish each"; \
	       echo "  other's binary, kill each other's server, or interleave a"; \
	       echo "  database move -- and each reports success. One lock, taken"; \
	       echo "  by every verb, or it is not a lock at all."; exit 1; }; \
	 done; \
	 grep -q 'PANCRA_FRONT' srv/deploy/health.sh || \
	   { echo "deploycheck: health.sh does not consult the front door."; \
	     echo "  Its checks go through it, so its failures must name it."; \
	     exit 1; }; \
	 sed -n '/^wait_healthy_since/,/^}/p' srv/deploy/health.sh | \
	   grep -q 'front_declared' || \
	   { echo "deploycheck: the health wait does not refuse an UNDECLARED"; \
	     echo "  front door. Defining the test is not running it: every"; \
	     echo "  check here traverses the front door, so a pass with nothing"; \
	     echo "  declared rests on a component nobody maintains."; exit 1; }; \
	 if grep -n 'glucoserve' srv/deploy/pancra.conf; then \
	   echo "deploycheck: the contract still names the server's PREVIOUS name."; \
	   echo "  A default that points at the old installation tree is how an"; \
	   echo "  obsolete path becomes permanent: every board keeps working, so"; \
	   echo "  nobody moves it, and the contract documents history instead of"; \
	   echo "  the service. Override PANCRA_ROOT for a board not yet moved."; \
	   exit 1; \
	 fi; \
	 if grep -n 'glucoserve\|/home/jk' srv/deploy/*.sh srv/test/duosmoke.sh; then \
	   echo "deploycheck: a board path is spelled out above."; \
	   echo "  srv/deploy/pancra.conf is the ONE place the deployment is"; \
	   echo "  described; a path written anywhere else is a second, unchecked"; \
	   echo "  contract -- which is how the smoke test came to be pointing at"; \
	   echo "  a directory named after the server's previous name."; \
	   exit 1; \
	 fi; \
	 if grep -nE '(^|[^a-z])(scp|rsync|mv .*sync\.new|setsid)' srv/test/duosmoke.sh; then \
	   echo "deploycheck: the smoke test must not install anything."; \
	   echo "  It exists to answer 'is what we deployed LAST time still alive',"; \
	   echo "  which a run that deploys first cannot answer."; \
	   exit 1; \
	 fi; \
	 sed -n '/^stop_block/,/^}/p' srv/deploy/start.sh | \
	   grep -q "> '\$$PANCRA_DOWN'" || \
	   { echo "deploycheck: the shared stop does not mark itself deliberate."; \
	     echo "  A supervisor cannot tell an operator's stop from a crash"; \
	     echo "  unless the stop says so, and one that guesses wrong starts"; \
	     echo "  the OLD binary in the middle of a deploy's swap -- two"; \
	     echo "  servers on one data directory, and the pid file naming"; \
	     echo "  whichever start wrote it last."; exit 1; }; \
	 sed -n '/^start_block/,/^}/p' srv/deploy/start.sh | \
	   grep -q "rm -f '\$$PANCRA_DOWN'" || \
	   { echo "deploycheck: the shared start does not clear the deliberate-stop"; \
	     echo "  marker. Raised and never cleared, it is a supervisor that"; \
	     echo "  ignores the NEXT crash -- and every one after it."; exit 1; }; \
	 grep -q 'supervisor_install' srv/deploy/deploy.sh || \
	   { echo "deploycheck: deploy.sh never installs supervision."; \
	     echo "  The board had none: an unexpected exit stayed exited and a"; \
	     echo "  reboot came up with no server at all, because the only thing"; \
	     echo "  that had ever started it was a deploy running on somebody"; \
	     echo "  else's laptop. Installing it anywhere but the deploy means"; \
	     echo "  the board that has been up longest is the one without it."; \
	     exit 1; }; \
	 if grep -nE 'PANCRA_(PORT|CERT|KEY|ORIGIN|DATA)' srv/deploy/supervise.sh; then \
	   echo "deploycheck: the watchdog names the server's own arguments."; \
	   echo "  It must not build a start command: srv/deploy/start.sh owns"; \
	   echo "  the one copy, and the watchdog gets a GENERATED copy of it"; \
	   echo "  (start_template) in the env file the deploy writes. A second"; \
	   echo "  hand-written start is how a board came back up on port 8443"; \
	   echo "  when it had been deployed on another one."; \
	   exit 1; \
	 fi; \
	 grep -q 'pancra_start_template' srv/deploy/supervise.sh || \
	   { echo "deploycheck: the watchdog does not use the generated start."; \
	     exit 1; }; \
	 printf '\033[1;32mdeploycheck\033[0m: one deployment contract, and every script reads it\n'

duodeploy: duo
	@PANCRA_LOCAL_BIN=$(DUOBIN) ./srv/deploy/deploy.sh $(DUOBIN)

duorollback:
	@./srv/deploy/rollback.sh $(HASH)

# CERT=<cert.pem> KEY=<key.pem>. Validates the pair here before anything on
# the board moves, keeps the old one, restarts through the same operation
# every other verb uses, and puts the old pair back if the new one does not
# serve. It replaces five paragraphs of paste-this in srv/deploy/README.md
# that hard-coded the port and both pem paths and never recorded the new pid.
duorotate:
	@./srv/deploy/rotate.sh $(CERT) $(KEY)

duobackup: srv
	@./srv/deploy/backup.sh

# The other half of a backup, and the only half that can be checked here: take
# one from a live server, restore it into a fresh directory, and make a second
# server serve the same phone's history out of it. Host-only and hermetic, so
# it belongs in the gate.
# THE DATABASE IS A VALUE, and this is the test that keeps it one.
#
# Links db.c against the sqlite object the server already built, and opens TWO
# databases in one process -- the thing the old implicit singleton made
# inexpressible. A static hiding behind a pointer fails its third assertion.
dbctxtest: build/srv/obj/srv/db.o build/srv/sqlite3.o
	@mkdir -p $(SRVTESTDIR)
	$(SRVCC) $(SRVCFLAGS) $(SRVINC) $(SQLINC) srv/test/dbctxtest.c \
	    build/srv/obj/srv/db.o build/srv/sqlite3.o -o $(SRVTESTDIR)/dbctxtest \
	    -lpthread -lm
	@./$(SRVTESTDIR)/dbctxtest

# THE SCHEMA VERSION AND ITS STEPS, including the upgrade the deployed board
# will actually perform: a checked-in pre-versioning schema, upgraded in
# place, with its rows still in it afterwards.
dbmigtest: build/srv/obj/srv/db.o build/srv/sqlite3.o
	@mkdir -p $(SRVTESTDIR)
	$(SRVCC) $(SRVCFLAGS) $(SRVINC) $(SQLINC) srv/test/dbmigtest.c \
	    build/srv/obj/srv/db.o build/srv/sqlite3.o -o $(SRVTESTDIR)/dbmigtest \
	    -lpthread -lm
	@./$(SRVTESTDIR)/dbmigtest

# THE CLI's help, the dispatch and the README, compared. They were three
# lists: `logout` was implemented and undocumented, and `--help` was an error.
clitest: $(SRVBIN)
	@./srv/test/clitest.sh $(SRVBIN)

# ONE ADDRESS RULE, ON EVERY SURFACE THAT TAKES AN ADDRESS.
#
# Its own suite rather than more of synctest, because what it asserts is not
# what the server ANSWERED: /login already refused an over-long address with a
# 401, and a 401 is also what a wrong password gets. The defect was in the rows
# written on the way to that answer -- a throttle row keyed on a string no
# later attempt can produce again, and (on the invitation form, which had no
# bound at all) an ACCOUNT under an address /login will never resolve. So every
# case here asserts against login_fail and user, not against the response.
#
# Needs the CLI binary too: `sync adduser` is the third surface, and synccli's
# pairing body is the fourth.
emailtest: $(SRVBIN) $(SRVCLI)
	@./srv/test/emailtest.sh $(SRVBIN)

# THE DEPLOY, RUN. deploycheck reads the scripts; this one runs deploy.sh
# against a fake board in a temp directory -- an ssh that is a shell, an scp
# that is cp, a file:// URL and a tiny static "server". It exists for the
# same-hash-dead-service case: the identical-hash path used to exit 0 before
# looking at the pid, the log or the URL, so re-deploying an unchanged build
# could not recover a service that had died. No network, no board.
deploydrill:
	@./srv/test/deploydrill.sh

restoredrill: $(SRVBIN) $(SRVCLI)
	@./srv/test/restoredrill.sh $(SRVBIN)

# Deliberately separate from `check`: this installs and exercises the APK on a
# connected phone/emulator, so it is run once after the offline work is green.
devicecheck: $(APK)
	@./app/test/devicesmoke.sh $(APK)

# Build-mode stamp. CROSS is not a file, so without this a plain `make` after
# a `make CROSS=...` rebuilds NOTHING and leaves riscv64 objects in place --
# which then fail as "Exec format error" under the tests, or worse, a native
# binary gets copied to the board.
#
# The stamp handles SEQUENTIAL mode switches. It cannot handle two make
# invocations running AT ONCE -- and that is the failure actually observed:
# a `make duo` in the background while a native `make srv` ran produced a
# half-riscv64 build/srv/ and a link that died on "Relocations in generic ELF
# (EM: 243)". No stamp can fix that, because both builds are writing the same
# object paths. So the object tree is now keyed by the mode as well: a native
# build and a cross build no longer share a single file, and running both at
# once is merely wasteful instead of wrong.
build/srv$(SRVMODE)/.build-mode: FORCE
	@mkdir -p $(@D); printf '%s' '$(CROSS)' | cmp -s - $@ 2>/dev/null \
	    || printf '%s' '$(CROSS)' > $@

# sqlite is a quarter of a million lines that never change; its own object
# stops every edit to srv/ from recompiling them.
#
# -Os ONLY, NOT $(SRVCFLAGS): our warning set is a statement about code we
# wrote and can fix, and sqlite is neither. It builds clean under -Wall
# -Wextra, but -Wundef fires on its own `#if SQLITE_DEBUG` idiom -- perfectly
# legal, and not ours to change. Holding a vendored amalgamation to a
# first-party standard only ever ends in editing it or silencing the check.
build/srv$(SRVMODE)/sqlite3.o: $(SQLSRC) build/srv$(SRVMODE)/.build-mode
	@mkdir -p $(@D)
	$(SRVCC) -Os $(SQLFLAGS) $(SQLINC) -c -o $@ $(SQLSRC)

build/srv$(SRVMODE)/obj/%.o: %.c build/srv$(SRVMODE)/.build-mode
	@mkdir -p $(@D)
	$(SRVCC) $(SRVOBJFLAGS) -c -o $@ $<

$(SRVBIN): $(SRVOBJ) build/srv$(SRVMODE)/sqlite3.o
	@mkdir -p $(@D)
	$(SRVCC) $(SRVCFLAGS) -o $@ $(SRVOBJ) build/srv$(SRVMODE)/sqlite3.o \
	    $(SRVLDFLAGS) -pthread

# The sync protocol's own test client (srv/synccli.c): pairing is a four-round
# J-PAKE and every call is signed, so synctest.sh cannot drive the server from
# curl alone. Test tool, not a reference implementation.
$(SRVCLI): srv/synccli.c srv/util.c $(filter-out lib/plot.c,$(SHARED)) srv/proto.h build/srv$(SRVMODE)/.build-mode
	@mkdir -p $(@D)
	$(SRVCC) $(SRVCFLAGS) $(SRVINC) $(TLSINC) -o $@ srv/synccli.c srv/util.c \
	    $(filter-out lib/plot.c,$(SHARED)) lib/hmac.c $(SRVLDFLAGS)

-include $(SRVDEP)

# THE TEST OF THE TEST HARNESS.
#
# srv/test/testlib.sh carries every shell assertion in the project -- ~750 in
# synctest, 57 in faulttest, 19 in tlstest, 17 in restoredrill, 9 in interop --
# and a defect in it makes all of them pass while testing nothing. Two were
# found by reading: a request whose curl failed returned the PREVIOUS
# response's body, and a port was released before the server that was to own it
# had bound it. Neither is visible from a suite that uses the helper, because a
# helper that reports success looks exactly like a helper that worked.
#
# So this one breaks each rule on purpose and requires the failure. It needs no
# server binary: it starts stub listeners of its own, because a broken server
# must not be able to look like a broken harness.
harnesstest:
	@./srv/test/harnesstest.sh

# The wire contract is the only thing standing between a stranger and the
# data, and "it stored the row" is not the same as "the two sides now agree".
srvcheck: $(SRVBIN) $(SRVCLI)
	./srv/test/synctest.sh

# THE TWO FAILURES A REAL DATABASE WILL NOT PERFORM ON REQUEST: a prepare that
# fails, and a COMMIT that fails with every row already in the transaction.
#
# Its own build (build/srv-fault/, like the sanitizer one) because the hooks
# are behind -DDB_FAULTS and nothing that ships may carry them. The suite
# refuses to run against a binary without them, so it cannot pass by injecting
# nothing.
faulttest:
	@$(MAKE) --no-print-directory build/srv-fault/sync build/srv-fault/synccli \
	    SRVMODE=-fault SRVCFLAGS="$(STD) -O1 -g -DDB_FAULTS $(SRVWARN)" >/dev/null
	@./srv/test/faulttest.sh build/srv-fault/sync

# THE APP'S HOST SUITES, UNDER AddressSanitizer AND UBSan.
#
# The server has had this gate for a while; the app half had none, so every
# parser, every persistence path and every registry mutation was exercised at
# -O2 with warnings only. Those are the suites that walk hand-written text,
# index tables by id, and roll transactions back -- exactly where a
# one-past-the-end read or a signed overflow lives, and exactly the class of
# defect that a passing test does not notice.
#
# The recipes take their flags from TESTWARN, so this is the same suites,
# rebuilt. The production cross-build flags are untouched: nothing here
# reaches the phone.
#
# NOT crashtest: it installs the crash handler and raises a real fault, which
# the sanitizer intercepts first -- the test would be testing ASan. NOT the
# suites that need a server (interoptest) or a device.
APPASAN_TESTS := alarmtest storetest statstest metertest registrytest \
                 settingstest scantest senstest gesturetest syncstattest meterstoretest \
                 insulintest weighttest calibtest ingesttest plottest \
                 drivertest modeltest jbridgetest uitest \
                 metersesstest threadtest durabilitytest
# -fno-sanitize-recover=all: a diagnostic that does not FAIL the build is a
# diagnostic nobody reads. -O1 -g so the traces name lines.
ASAN_TESTWARN := -Wall -Wextra -Werror -Wformat=2 -O1 -g \
                 -fsanitize=address,undefined -fno-omit-frame-pointer \
                 -fno-sanitize-recover=all
# ---- THREADSANITIZER --------------------------------------------------
#
# ASan and UBSan find memory and arithmetic faults; NEITHER diagnoses a C data
# race, and this codebase's whole difficulty is concurrency: a BLE binder
# thread, a service tick, a Java push worker and the main looper all reach the
# same state. The suites already exercise that overlap deliberately -- they
# were just never run under a tool that could see a race.
#
# It earned its place immediately. The first run reported three: the advert
# counter and the smart-pairing flag in app/pairing.c (plain objects shared
# with the binder callback), and the per-link reconnect throttle, whose
# read-compare-write let two adverts for one sensor both claim the same
# window and start two connects. See TODO item 14.
#
# TSAN_TESTS is every suite that actually starts a thread. A suite with no
# thread in it would only be paying for the instrumentation.
#
# giftest is EXCLUDED, and the reason is the fixture rather than the tool: its
# `shared` case hands ONE gif_ws to two threads on purpose, to prove that
# sharing a workspace corrupts the stream. TSan reports that race because it
# is real and deliberate. Running it here would mean either deleting the case
# or teaching the gate to expect a race, and both are worse than saying so.
#
# TSan cannot be combined with ASan, so this is a second pass rather than a
# flag added to the first.
TSAN_TESTS := threadtest bondtabletest metersesstest meterstoretest remotetest pairingtest \
              durabilitytest calibtest modeltest alarmtest notifytest
TSAN_TESTWARN := -Wall -Wextra -Werror -Wformat=2 -O1 -g \
                 -fsanitize=thread -fno-omit-frame-pointer

apptsan:
	@TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
	 $(MAKE) --no-print-directory $(TSAN_TESTS) \
	     TESTWARN="$(TSAN_TESTWARN)" TESTDIR=build/app/test-tsan \
	     >build/app/tsan.log 2>&1 \
	  || { cat build/app/tsan.log; exit 1; }
	@printf '\033[1;32mapptsan\033[0m: no data races in the app host suites\n'

# The server's own concurrency: httptest drives the worker pool, which is the
# representative path -- a request arriving on one thread while another is
# mid-response is what the pool exists to make safe.
srvtsan:
	@TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
	 $(MAKE) --no-print-directory httptest \
	     TESTWARN="$(TSAN_TESTWARN)" SRVTESTDIR=build/srv/test-tsan \
	     >build/srv-tsan.log 2>&1 \
	  || { cat build/srv-tsan.log; exit 1; }
	@printf '\033[1;32msrvtsan\033[0m: no data races in the server pool\n'

appasan:
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	 $(MAKE) --no-print-directory $(APPASAN_TESTS) \
	     TESTWARN="$(ASAN_TESTWARN)" TESTDIR=build/app/test-asan \
	     >build/app/asan.log 2>&1 \
	  || { cat build/app/asan.log; exit 1; }
	@printf '\033[1;32mappasan\033[0m: the app host suites are clean under ASan/UBSan\n'

# THE SANITIZER SERVER, BUILT ONCE FOR EVERY SUITE THAT RUNS IT.
#
# The warning set catches what the compiler can see; it cannot see an index
# computed at run time. A review mutated three length bounds in srv/tls.c and
# the guard in srv/home.c's hour_row -- all reachable overflows -- with the
# whole suite green, because an overflow that does not happen to crash reads
# exactly like a pass. ASan turns each of them into a failure with a stack.
#
# Its own object tree (SRVMODE=-asan) so it cannot disturb the shipping build,
# and detect_leaks is off: the server deliberately holds its per-worker sqlite
# connections and its certificate for the life of the process.
#
# TWO SUITES NEED THIS ONE BINARY -- srvasan over plain HTTP and tlsasan
# through the real handshake -- and each used to build it with a recursive
# $(MAKE) of its own. Two makes, one set of output paths, both named in
# `check`: under `make -j check` one was relinking build/srv-asan/sync while
# the other was executing it, and build/srv-asan/.build-mode is a FORCE target
# that both rewrote. That is the same defect the per-mode TESTDIR above
# removes from the app suites, with the same worst case -- a sanitizer suite
# that reports clean having run a half-written or half-configured binary.
#
# One phony target both depend on instead. make runs a target's recipe once per
# invocation however many goals need it, and every goal that names it waits for
# it to finish, so the tree is built exactly once and nobody executes it early.
#
# THE FLAGS LIVE IN ONE VARIABLE for the second half of the same problem: the
# two recipes each spelled the sanitizer set out in full, so the day one of
# them gained a flag the other did not, the two suites would be testing two
# different configurations out of one directory and whichever built last would
# decide which -- a wrong-configuration pass that no gate could see. They were
# byte-identical when this was written, which is exactly when to remove the
# duplicate rather than after it has drifted.
#
# faulttest above is deliberately NOT routed through here. It is the sole
# consumer of build/srv-fault/ and carries -DDB_FAULTS, which nothing that
# ships and nothing else that tests may have, so its single recursive make has
# no second writer to race with. If a second suite ever wants that tree, it
# needs this treatment, not a copy of that recipe.
SRVASANFLAGS := $(STD) -O1 -g -fsanitize=address,undefined \
                -fno-omit-frame-pointer $(SRVWARN)

srvasanbuild:
	@$(MAKE) --no-print-directory build/srv-asan/sync build/srv-asan/synccli \
	    SRVMODE=-asan SRVCFLAGS="$(SRVASANFLAGS)" >/dev/null

srvasan: srvasanbuild
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	 ./srv/test/synctest.sh build/srv-asan/sync \
	   && printf '\033[1;32msrvasan\033[0m: the suite is clean under ASan/UBSan\n'

# Exercise the real TLS handshake, forged-record, reset, and deadline cases
# against the sanitizer build too; srvasan itself speaks only plain HTTP.
#
# It needs only build/srv-asan/sync, not the test client, but it depends on the
# whole shared build anyway: a second target that builds a SUBSET of the same
# directory is how one path comes to hold two configurations again.
tlsasan: srvasanbuild
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	 ./srv/test/tlstest.sh build/srv-asan/sync \
	   && printf '\033[1;32mtlsasan\033[0m: TLS attacks are clean under ASan/UBSan\n'

# synctest.sh drives the server over PLAIN HTTP -- it passes two arguments, so
# no certificate is loaded -- which meant srv/tls.c, a thousand lines of
# hand-written TLS 1.3 on the open internet, was reachable by no test at all.
# This one hands it a throwaway P-256 certificate and points real clients at
# it: openssl for the handshake, curl for a page, and a tampering proxy for
# the case that matters, a record whose ciphertext is valid and whose
# authentication tag is not. It also pins the reset-connection regression that
# used to spin a worker at 100% CPU forever.
# NO PORT ARGUMENT. Each suite asks the kernel for a free one (srv/test/
# testlib.sh, pick_port), so `make check -j4` can run tlstest, tlsasan,
# srvcheck, srvasan and interoptest at once without them fighting over a
# listener -- which used to show up as one run's assertions all coming back
# empty and reading like a protocol break.
tlstest: $(SRVBIN)
	@./srv/test/tlstest.sh $(SRVBIN)
