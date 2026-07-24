# SPDX-License-Identifier: GPL-3.0
# pancra: plain-C Android app, no Gradle. `make run` builds, installs, launches.
CLANG     := /usr/lib/llvm-19/bin/clang
STRIP     := /usr/lib/llvm-19/bin/llvm-strip
TARGET    := aarch64-linux-android29
JNI_INC   := -Isrc -I/usr/lib/jvm/default-java/include -I/usr/lib/jvm/default-java/include/linux
# Every warning on, warnings are errors. Keep this list in sync with what the
# code actually satisfies — the rule is to fix the cause, never to silence.
WARN      := -Werror -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
             -Wmissing-prototypes -Wwrite-strings -Wvla -Wformat=2 -Wundef \
             -Wdouble-promotion -Wcast-qual -Wswitch-enum -Wredundant-decls
CFLAGS    := -Os -ffreestanding -fno-stack-protector -fno-unwind-tables \
             -fno-asynchronous-unwind-tables -fvisibility=hidden $(WARN) $(JNI_INC)
FRAMEWORK := /usr/share/android-framework-res/framework-res.apk
D8        := java -cp tmp/tools/r8.jar com.android.tools.r8.D8
# javac: only -target 8 still allows -bootclasspath; d8 desugars 8 fine
JAVACFLAGS := -Xlint:-options -source 8 -target 8 -bootclasspath tmp/tools/android.jar

APK  := build/pancra.apk
LIB  := build/apk/lib/arm64-v8a/libpancra.so
DEX  := build/apk/classes.dex
# kept OUTSIDE build/ so `make clean` can't wipe it — a regenerated key changes
# the APK signature and blocks in-place updates (forcing an uninstall/data loss)
KEY  := tmp/debug.keystore

# stub .so's satisfy the linker; the phone's dynamic linker binds the real
# bionic libs at runtime (stub_*.c hold only the symbol names we reference)
STUBS := build/stub/libc.so build/stub/libandroid.so build/stub/liblog.so

# launcher icon + any future resources (compiled into the APK via aapt -S)
RES := $(wildcard res/*/*.xml)

# The manifest ships release-safe (no android:debuggable). Dev builds flip it on
# with aapt --debug-mode so crash.log is retrievable on-device; `make release`
# overrides this to empty for a debuggable=false artifact.
AAPT_DEBUG := --debug-mode

all: $(APK)

build/stub/lib%.so: src/stub_%.c
	@mkdir -p $(@D)
	$(CLANG) --target=$(TARGET) -Isrc -shared -nostdlib -fuse-ld=lld \
	    -Wl,-soname,lib$*.so -o $@ $<

# native sources: UI/JNI core, BLE transport, protocol driver, self-contained crypto
SRC := src/main.c src/font.c src/plot.c src/util.c src/stats.c src/store.c src/settings.c src/ui.c \
       src/alarmlogic.c src/scanlogic.c src/insulin.c \
       src/sensors.c src/otble.c \
       src/dexble.c src/dexdriver.c \
       src/dexauth.c src/dexdata.c src/p256.c src/sha256.c src/aes.c

# Headers are prerequisites too: the whole app is one clang invocation, so
# there is no incremental build to lose, and without this a header-only edit
# (this tree changed three struct layouts) silently relinks nothing and ships
# the previous .so.
HDR := $(wildcard src/*.h)

$(LIB): $(SRC) $(HDR) $(STUBS)
	@mkdir -p $(@D)
	$(CLANG) --target=$(TARGET) $(CFLAGS) -shared -nostdlib -fuse-ld=lld \
	    -Wl,--no-undefined -Lbuild/stub -lc -landroid -llog -o $@ $(SRC)
	$(STRIP) $@

build/classes/com/jk/pancra/Ble.class: src/Ble.java src/PancraService.java src/Alarm.java src/PancraFiles.java
	javac $(JAVACFLAGS) -d build/classes src/Ble.java src/PancraService.java src/Alarm.java src/PancraFiles.java

$(DEX): build/classes/com/jk/pancra/Ble.class
	@mkdir -p $(@D)
	$(D8) --release --min-api 29 --lib tmp/tools/android.jar --output $(@D) \
	    build/classes/com/jk/pancra/*.class

$(KEY):
	keytool -genkeypair -keystore $@ -alias debug -keyalg RSA -keysize 2048 \
	    -validity 10000 -storepass android -keypass android -dname CN=debug

# Mode stamp: AAPT_DEBUG isn't a file, so without this a `make` after `make
# release` (or vice-versa) would NOT rebuild the APK -- leaving the wrong
# debuggable flag installed. The stamp's contents change with the mode, forcing
# a rebuild exactly when it flips.
FORCE:
build/.aapt-mode: FORCE
	@mkdir -p $(@D); printf '%s' '$(AAPT_DEBUG)' | cmp -s - $@ 2>/dev/null \
	    || printf '%s' '$(AAPT_DEBUG)' > $@

$(APK): AndroidManifest.xml $(LIB) $(DEX) $(KEY) $(RES) build/.aapt-mode
	aapt package -f -M AndroidManifest.xml -S res -I $(FRAMEWORK) $(AAPT_DEBUG) \
	    -F build/pancra.unaligned.apk
	cd build/apk && aapt add ../pancra.unaligned.apk lib/arm64-v8a/libpancra.so classes.dex
	zipalign -f -p 4 build/pancra.unaligned.apk $@
	apksigner sign --ks $(KEY) --ks-pass pass:android $@

# Release artifact: identical build with debuggable off. Still signed with the
# local debug key -- swap in your Play upload key (apksigner --ks) before upload.
release:
	rm -f $(APK) build/pancra.unaligned.apk
	$(MAKE) AAPT_DEBUG= $(APK)
	@printf '\033[1;32mrelease\033[0m: %s built with debuggable=false.\n' "$(APK)"
	@printf '  Sign with your Play upload key before uploading.\n'

# Play App Bundle (.aab), built without Gradle: aapt2 links resources in
# protobuf format, we assemble bundletool's module layout, then build-bundle.
# debuggable is off (an .aab is a release artifact). The result is UNSIGNED --
# sign it with your upload key (jarsigner -keystore upload.jks build/pancra.aab)
# or let Play App Signing handle it. Requires tmp/tools/bundletool.jar.
AAB        := build/pancra.aab
BUNDLETOOL := java -jar tmp/tools/bundletool.jar

aab: $(AAB)
$(AAB): AndroidManifest.xml $(LIB) $(DEX) $(RES)
	@test -f tmp/tools/bundletool.jar || { echo "tmp/tools/bundletool.jar missing -- get it from https://github.com/google/bundletool/releases"; exit 1; }
	rm -rf build/aab && mkdir -p build/aab/module/manifest build/aab/module/dex build/aab/module/lib/arm64-v8a
	aapt2 compile --dir res -o build/aab/res.zip
	aapt2 link --proto-format -o build/aab/base-proto.apk -I $(FRAMEWORK) \
	    --manifest AndroidManifest.xml --min-sdk-version 29 --target-sdk-version 34 \
	    -R build/aab/res.zip --auto-add-overlay
	cd build/aab/module && unzip -qo ../base-proto.apk
	mv build/aab/module/AndroidManifest.xml build/aab/module/manifest/
	cp $(DEX) build/aab/module/dex/classes.dex
	cp $(LIB) build/aab/module/lib/arm64-v8a/
	cd build/aab/module && rm -f ../base.zip && zip -qr ../base.zip manifest dex res lib resources.pb
	$(BUNDLETOOL) build-bundle --modules=build/aab/base.zip --output=$@
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
# compilation database is needed — the whole app is one clang invocation, so we
# hand clang-tidy the exact compile flags after `--`.
# test/ IS the behavioural gate, so it gets the same formatting and encoding
# checks as src/. It was excluded, which meant the ~1400 lines that decide
# whether every other check means anything could rot unnoticed.
FMT_SRC   := $(wildcard src/*.c src/*.h test/*.c)
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
check: format tidy crosscheck javacheck $(LIB) $(DEX) uitest drivertest alarmtest storetest statstest metertest registrytest settingstest scantest insulintest done

format:
	grep -rlP '\r' --exclude='.*' src test res Makefile AndroidManifest.xml \
		&& { echo "CRLF line endings found (see above)"; exit 1; } || true
	grep -rlP '[^\x00-\x7F]' $(FMT_SRC) \
		&& { echo "Non-ASCII characters found (see above)"; exit 1; } || true
	clang-format --dry-run -Werror $(FMT_SRC)

format-fix:
	clang-format -i $(FMT_SRC)

# Constants that must agree ACROSS LANGUAGES. A _Static_assert cannot see Java,
# so nothing else can catch this: raise LINK_MAX without raising MAX_LINKS and
# Ble.link(id) returns null for the new link, silently dropping every GATT
# operation on it -- no exception, no log, no diagnostic on either side.
crosscheck:
	@c=$$(grep -oP '#define LINK_MAX\s+\K[0-9]+' src/dexdriver.h); \
	 j=$$(grep -oP 'MAX_LINKS\s*=\s*\K[0-9]+' src/Ble.java); \
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
	 n=$$(grep -oP '#define NHIST\s+\K[0-9]+' src/store.h); \
	 u=$$(grep -oP '#define UI_PLOT_MAX\s+\K[0-9]+' src/ui.c); \
	 if [ -z "$$n" ] || [ -z "$$u" ]; then \
	   echo "crosscheck: could not read NHIST ('$$n') or UI_PLOT_MAX ('$$u')"; \
	   exit 1; \
	 fi; \
	 if [ "$$n" != "$$u" ]; then \
	   echo "NHIST ($$n in store.h) != UI_PLOT_MAX ($$u in ui.c):"; \
	   echo "  the shell sends up to NHIST plot points but ui.c draws at most"; \
	   echo "  UI_PLOT_MAX of them -- a smaller UI cap silently truncates the"; \
	   echo "  oldest in-window points, shrinking the 7D plot below a week."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mcrosscheck\033[0m: NHIST == UI_PLOT_MAX (%s)\n' "$$n"

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
javacheck:
	@f=src/Alarm.java; \
	 grep -q 'static synchronized void silence' $$f || \
	   { echo "$$f: silence() must stay synchronized"; exit 1; }; \
	 awk '/static synchronized void silence/,/^    \}/' $$f | grep -q 'stopSound()' || \
	   { echo "$$f: silence() must call stopSound() -- without it the C side"; \
	     echo "  records the alarm dismissed while the player keeps looping"; \
	     exit 1; }; \
	 awk '/static void stopSound/,/^    \}/' $$f | grep -q 'release()' || \
	   { echo "$$f: stopSound() must release() the player"; exit 1; }; \
	 g=src/Ble.java; \
	 awk '/static boolean stop\(/,/^    \}/' $$g | grep -q 'stopScan' || \
	   { echo "$$g: stop() must actually call stopScan -- reporting success"; \
	     echo "  without it lets the self-heal stack a second scan client"; \
	     exit 1; }; \
	 printf '\033[1;32mjavacheck\033[0m: load-bearing Java lines present\n'

# src/ is tidied with the real freestanding cross-compile flags. test/ is tidied
# too -- it is the behavioural gate and must not rot -- but as HOST binaries it
# needs its own flag set (glibc FILE/fopen/exit, host target, the drivertest
# macro), not TIDY_ARGS. test/.clang-tidy inherits the root config and turns off
# only the checks that are wrong for a hosted harness (see that file); everything
# else is enforced identically to src/.
# Project headers come in via -iquote (so "" includes resolve to src/) and NOT
# -Isrc: src/ ships freestanding stubs named stdio.h/stdlib.h/string.h, and -Isrc
# would put those on the <> path, hiding glibc's real FILE/fopen from the host
# harness (implicit-decl errors). -iquote never affects <> includes, so <stdio.h>
# stays glibc's -- exactly as the test build (JVM_INC, not JNI_INC) does it.
TEST_SRC   := $(wildcard test/*.c)
TIDY_TEST  := -iquote src -iquote test -I/usr/lib/jvm/default-java/include \
              -I/usr/lib/jvm/default-java/include/linux -DDEXDRIVER_TEST \
              -Wall -Wextra
tidy:
	clang-tidy $(SRC) -- $(TIDY_ARGS)
	clang-tidy $(TEST_SRC) -- $(TIDY_TEST)

done:
	@printf '\033[1;32mSUCCESS\033[0m: all checks passed.\n'

# offline UI render harness: builds ui.c/font.c on the host and renders the
# screens to test/*.ppm, so the UI can be verified with no phone attached.
# -iquote so "" project headers come from src/ while <> headers stay glibc's
# (the freestanding shims lack FILE/fopen etc. the harness needs on the host).
JVM_INC := -I/usr/lib/jvm/default-java/include -I/usr/lib/jvm/default-java/include/linux
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
uitest:
	@mkdir -p tmp/uitest
	cc -iquote src $(JVM_INC) $(TESTWARN) test/uitest.c src/ui.c src/font.c \
	    src/plot.c src/sensors.c src/util.c -o tmp/uitest/uitest
	./tmp/uitest/uitest

# Behavioural gate for the alarm decision logic. Until this existed, NOTHING in
# main.c was covered by any test binary -- an adversarial review deleted the
# glucose alarm outright and `make check` stayed green. The LOW alarm was in
# fact dead at the time (see alarmlogic.h).
alarmtest:
	@mkdir -p tmp/uitest
	cc -iquote src $(TESTWARN) test/alarmtest.c src/alarmlogic.c -o tmp/uitest/alarmtest
	@./tmp/uitest/alarmtest > tmp/uitest/alarmtest.log 2>&1 \
	    && grep -q "ALL ALARM TESTS PASSED" tmp/uitest/alarmtest.log \
	    && printf '\033[1;32malarmtest\033[0m: alarm decision logic OK\n' \
	    || { cat tmp/uitest/alarmtest.log; exit 1; }

# Behavioural gate for the reading history / dedup model. store.c was in no
# test binary, and every caller persists only on a non-zero hist_insert result
# -- so a wrong return there drops a reading permanently and silently.
storetest:
	@mkdir -p tmp/uitest
	cc -iquote src $(TESTWARN) test/storetest.c src/store.c src/util.c \
	    src/sensors.c -o tmp/uitest/storetest
	@./tmp/uitest/storetest > tmp/uitest/storetest.log 2>&1 \
	    && grep -q "ALL STORE TESTS PASSED" tmp/uitest/storetest.log \
	    && printf '\033[1;32mstoretest\033[0m: history + dedup model OK\n' \
	    || { cat tmp/uitest/storetest.log; exit 1; }

# Behavioural gate for the rolling TIR/average buckets. The ring aliases an
# over-old reading onto a live bucket and ZEROES it, so the boundary guards are
# the whole safety of the statistics -- and they are exactly what a hand-check
# reads past.
statstest:
	@mkdir -p tmp/uitest
	cc -iquote src $(TESTWARN) test/statstest.c src/stats.c src/util.c \
	    -o tmp/uitest/statstest
	@./tmp/uitest/statstest > tmp/uitest/statstest.log 2>&1 \
	    && grep -q "ALL STATS TESTS PASSED" tmp/uitest/statstest.log \
	    && printf '\033[1;32mstatstest\033[0m: rolling TIR/average OK\n' \
	    || { cat tmp/uitest/statstest.log; exit 1; }

# Behavioural gate for the OneTouch meter driver, which had none. It decides
# which fingersticks reach the append-only log, and its timestamp gate, its
# walk-advance rule and its counter handling have all been wrong at some point.
metertest:
	@mkdir -p tmp/uitest
	cc -iquote src $(TESTWARN) test/metertest.c src/otble.c src/util.c \
	    -o tmp/uitest/metertest
	@./tmp/uitest/metertest > tmp/uitest/metertest.log 2>&1 \
	    && grep -q "ALL METER TESTS PASSED" tmp/uitest/metertest.log \
	    && printf '\033[1;32mmetertest\033[0m: OneTouch meter driver OK\n' \
	    || { cat tmp/uitest/metertest.log; exit 1; }

# Behavioural gate for the provenance registry. An id names one physical device
# forever and readings.csv cites those ids in rows that are never rewritten, so
# an id reused is one sensor's history permanently merged into another's.
# sensors.c was linked into other test binaries but nothing called mint, claim,
# forget or rebind.
registrytest:
	@mkdir -p tmp/uitest
	cc -iquote src $(TESTWARN) test/registrytest.c src/sensors.c src/util.c \
	    -o tmp/uitest/registrytest
	@./tmp/uitest/registrytest > tmp/uitest/registrytest.log 2>&1 \
	    && grep -q "ALL REGISTRY TESTS PASSED" tmp/uitest/registrytest.log \
	    && printf '\033[1;32mregistrytest\033[0m: provenance registry OK\n' \
	    || { cat tmp/uitest/registrytest.log; exit 1; }

# Behavioural gate for settings persistence. alarm_load's validation is the
# last thing standing between a corrupt file and a hypo alarm that cannot fire
# (an out-of-range low disables it; low > high latches both) -- while still
# accepting low == high, which alarm_step legitimately produces.
settingstest:
	@mkdir -p tmp/uitest
	cc -iquote src $(TESTWARN) test/settingstest.c src/settings.c src/util.c \
	    -o tmp/uitest/settingstest
	@./tmp/uitest/settingstest > tmp/uitest/settingstest.log 2>&1 \
	    && grep -q "ALL SETTINGS TESTS PASSED" tmp/uitest/settingstest.log \
	    && printf '\033[1;32msettingstest\033[0m: settings persistence OK\n' \
	    || { cat tmp/uitest/settingstest.log; exit 1; }

# Behavioural gate for the insulin dose log. Doses are user-entered facts in an
# append-only file the app reloads at every launch: the load-time validation is
# what keeps a corrupt row from resurrecting forever, and last-units-per-type is
# what the LOG INSULIN form pre-populates with.
insulintest:
	@mkdir -p tmp/uitest
	cc -iquote src $(TESTWARN) test/insulintest.c src/insulin.c src/util.c \
	    -o tmp/uitest/insulintest
	@./tmp/uitest/insulintest > tmp/uitest/insulintest.log 2>&1 \
	    && grep -q "ALL INSULIN TESTS PASSED" tmp/uitest/insulintest.log \
	    && printf '\033[1;32minsulintest\033[0m: insulin dose log OK\n' \
	    || { cat tmp/uitest/insulintest.log; exit 1; }

# Behavioural gate for the scan-lifecycle decision, which governs whether an
# already-paired CGM can reconnect at all. It has been wrong in both directions.
scantest:
	@mkdir -p tmp/uitest
	cc -iquote src $(TESTWARN) test/scantest.c src/scanlogic.c -o tmp/uitest/scantest
	@./tmp/uitest/scantest > tmp/uitest/scantest.log 2>&1 \
	    && grep -q "ALL SCAN TESTS PASSED" tmp/uitest/scantest.log \
	    && printf '\033[1;32mscantest\033[0m: scan lifecycle OK\n' \
	    || { cat tmp/uitest/scantest.log; exit 1; }

# Offline end-to-end protocol test: a simulated Stelo runs the real J-PAKE
# server side and answers the driver's writes, and the final glucose is decoded
# from REAL captured bytes. Covers subscribe sequencing, round
# request/reassembly/chunking, the 02/03/04/05 auth exchange, shared-key
# agreement + persistence, and EGV decode.
#
# This existed but was built by NO target, so nothing exercised dexdriver.c,
# dexauth.c, dexdata.c, p256.c, aes.c or sha256.c -- the crypto and protocol
# core had zero automated coverage, and the test had rotted (stale include, a
# typedef the tree no longer uses, four transport hooks added since). Wiring it
# in means a new hook breaks the build instead of silently going untested.
DRVTEST_SRC := test/test_driver.c src/dexdriver.c src/dexauth.c src/dexdata.c \
               src/p256.c src/sha256.c src/aes.c src/util.c

drivertest:
	@mkdir -p tmp/uitest
	cc -DDEXDRIVER_TEST -iquote src -iquote test $(JVM_INC) $(TESTWARN) \
	    $(DRVTEST_SRC) -o tmp/uitest/drivertest
	./tmp/uitest/drivertest > tmp/uitest/drivertest.log 2>&1 \
	    && grep -q "ALL DRIVER TESTS PASSED" tmp/uitest/drivertest.log \
	    && printf '\033[1;32mdrivertest\033[0m: pairing + auth + EGV decode OK\n' \
	    || { tail -20 tmp/uitest/drivertest.log; exit 1; }

.PHONY: FORCE all release aab install run uninstall clean check crosscheck javacheck format format-fix tidy done uitest drivertest alarmtest storetest statstest metertest registrytest settingstest scantest insulintest
