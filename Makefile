# SPDX-License-Identifier: GPL-3.0
# pancra: plain-C Android app, no Gradle. `make run` builds, installs, launches.
# DISCOVERED, not hardcoded. These were absolute Debian paths with no ?= and
# no guard, so on any other distribution make died with a bare "No such file or
# directory" naming a compiler the developer never asked for. `make duo` and
# `make aab` already check their tool and point at the README; these do too.
# Override any of them on the command line: `make CLANG=/path/to/clang`.
CLANG     ?= $(shell command -v clang-19 2>/dev/null || command -v clang)
STRIP     ?= $(shell command -v llvm-strip-19 2>/dev/null || \
                     command -v llvm-strip 2>/dev/null || echo strip)
JAVA_HOME ?= $(shell dirname $$(dirname $$(readlink -f $$(command -v javac \
                     2>/dev/null) 2>/dev/null) 2>/dev/null) 2>/dev/null)
TARGET    := aarch64-linux-android29
# -I, not -iquote: the app builds freestanding and its own <stdio.h>,
# <string.h> and friends in app/ are what angle-bracket includes must resolve
# to. lib/ joins them because the shared sources live there now.
JNI_INC   := -Iapp -Ilib -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux
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
D8        := java -cp tools/r8.jar com.android.tools.r8.D8
# javac: only -target 8 still allows -bootclasspath; d8 desugars 8 fine
JAVACFLAGS := -Xlint:-options -source 8 -target 8 -bootclasspath tools/android.jar

# One sentence per missing prerequisite, rather than a failure from inside a
# recipe that names a path nobody chose.
ifeq ($(strip $(CLANG)),)
$(error no clang found: install clang, or run `make CLANG=/path/to/clang` -- see README)
endif
ifeq ($(strip $(JAVA_HOME)),)
$(error no JDK found: install a JDK (javac must be on PATH), or run `make JAVA_HOME=/path/to/jdk` -- see README)
endif

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
RES := $(wildcard app/res/*/*.xml)

# The manifest ships release-safe (no android:debuggable). Dev builds flip it on
# with aapt --debug-mode so crash.log is retrievable on-device; `make release`
# overrides this to empty for a debuggable=false artifact.
AAPT_DEBUG := --debug-mode

# Both halves. `make app` or `make srv` for one of them.
all: app srv
app: $(APK)

build/app/stub/lib%.so: app/stub_%.c
	@mkdir -p $(@D)
	$(CLANG) --target=$(TARGET) -Iapp -shared -nostdlib -fuse-ld=lld \
	    -Wl,-soname,lib$*.so -o $@ $<

# native sources: UI/JNI core, BLE transport, protocol driver, self-contained crypto
SRC := app/main.c app/font.c lib/plot.c app/util.c app/stats.c app/store.c app/settings.c app/ui.c \
       app/alarmlogic.c app/scanlogic.c app/ingest.c app/insulin.c app/weight.c app/plotdata.c \
       app/calib.c app/crashlog.c app/tzoff.c \
       app/sensors.c app/otble.c \
       app/dexble.c app/dexdriver.c \
       lib/jpake.c lib/rand.c app/dexcom.c app/dexdata.c lib/p256.c lib/sha256.c lib/aes.c lib/ecdsa.c lib/hmac.c lib/ct.c \
       app/sync.c app/syncjni.c

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

build/app/obj/%.o: %.c
	@mkdir -p $(@D)
	$(CLANG) --target=$(TARGET) $(CFLAGS) -MMD -MP -c -o $@ $<

# READ what -MMD writes. Without this the .d files are generated and ignored,
# so editing a header rebuilt NOTHING in the app: a change to a struct in
# ui.h would leave every object that was not itself edited holding the old
# layout, and the linker would join them without complaint. The server half
# has always had its `-include $(SRVDEP)`; the app half never did, while the
# comment above claimed "each object knows exactly which headers it read".
-include $(DEP)

$(LIB): $(OBJ) $(STUBS)
	@mkdir -p $(@D)
	$(CLANG) --target=$(TARGET) -shared -nostdlib -fuse-ld=lld \
	    -Wl,--no-undefined -Lbuild/app/stub -lc -landroid -llog -o $@ $(OBJ)
	$(STRIP) $@

build/app/classes/com/jk/pancra/Ble.class: app/Ble.java app/PancraService.java app/Alarm.java app/PancraFiles.java
	javac $(JAVACFLAGS) -d build/app/classes app/Ble.java app/PancraService.java app/Alarm.java app/PancraFiles.java

$(DEX): build/app/classes/com/jk/pancra/Ble.class
	@mkdir -p $(@D)
	$(D8) --release --min-api 29 --lib tools/android.jar --output $(@D) \
	    build/app/classes/com/jk/pancra/*.class

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

$(APK): app/AndroidManifest.xml $(LIB) $(DEX) $(KEY) $(RES) build/app/.aapt-mode
	aapt package -f -M app/AndroidManifest.xml -S app/res -I $(FRAMEWORK) $(AAPT_DEBUG) \
	    -F build/app/pancra.unaligned.apk
	cd build/app/apk && aapt add ../pancra.unaligned.apk lib/arm64-v8a/libpancra.so classes.dex
	zipalign -f -p 4 build/app/pancra.unaligned.apk $@
	apksigner sign --ks $(KEY) --ks-pass pass:android $@

# Release artifact: identical build with debuggable off. Still signed with the
# local debug key -- swap in your Play upload key (apksigner --ks) before upload.
release:
	rm -f $(APK) build/app/pancra.unaligned.apk
	$(MAKE) AAPT_DEBUG= $(APK)
	@printf '\033[1;32mrelease\033[0m: %s built with debuggable=false.\n' "$(APK)"
	@printf '  Sign with your Play upload key before uploading.\n'

# Play App Bundle (.aab), built without Gradle: aapt2 links resources in
# protobuf format, we assemble bundletool's module layout, then build-bundle.
# debuggable is off (an .aab is a release artifact). The result is UNSIGNED --
# sign it with your upload key (jarsigner -keystore upload.jks build/app/pancra.aab)
# or let Play App Signing handle it. Requires tools/bundletool.jar.
AAB        := build/app/pancra.aab
BUNDLETOOL := java -jar tools/bundletool.jar

aab: $(AAB)
$(AAB): app/AndroidManifest.xml $(LIB) $(DEX) $(RES)
	@test -f tools/bundletool.jar || { echo "tools/bundletool.jar missing -- get it from https://github.com/google/bundletool/releases"; exit 1; }
	rm -rf build/app/aab && mkdir -p build/app/aab/module/manifest build/app/aab/module/dex build/app/aab/module/lib/arm64-v8a
	aapt2 compile --dir app/res -o build/app/aab/res.zip
	aapt2 link --proto-format -o build/app/aab/base-proto.apk -I $(FRAMEWORK) \
	    --manifest app/AndroidManifest.xml --min-sdk-version 29 --target-sdk-version 34 \
	    -R build/app/aab/res.zip --auto-add-overlay
	cd build/app/aab/module && unzip -qo ../base-proto.apk
	mv build/app/aab/module/app/AndroidManifest.xml build/app/aab/module/manifest/
	cp $(DEX) build/app/aab/module/dex/classes.dex
	cp $(LIB) build/app/aab/module/lib/arm64-v8a/
	cd build/app/aab/module && rm -f ../base.zip && zip -qr ../base.zip manifest dex res lib resources.pb
	$(BUNDLETOOL) build-bundle --modules=build/app/aab/base.zip --output=$@
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
FMT_SRC   := $(wildcard app/*.c app/*.h app/test/*.c lib/*.c lib/*.h \
                        srv/*.c srv/*.h)
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
# ONE gate for both halves. srvcheck is last because it is the slowest and
# the only one that builds a second toolchain's worth of code; put it earlier
# and a formatting slip costs a minute to discover.
check: format tidy crosscheck javacheck $(LIB) $(DEX) uitest plottest drivertest alarmtest storetest statstest metertest registrytest settingstest scantest insulintest weighttest calibtest ingesttest crashtest cryptotest modeltest interoptest srvcheck srvasan tlstest duocheck done

# The CRLF scan named `src test res` -- the pre-merge directory layout. Those
# have not existed since the trees became app/ lib/ srv/, so grep failed with
# "No such file or directory", the `|| true` swallowed it, and the check
# scanned NOTHING for months while reporting success. A gate that passes
# because its target is gone is worse than no gate: it is a gate you trust.
#
# It now scans exactly what the formatter scans, and a grep that cannot read
# its input is a failure rather than a shrug.
# ...and it uses PORTABLE grep. `-P` is GNU-only, and on a host whose grep
# lacks it grep exits 2 -- which `if grep ...` reads as "no matches", so both
# scans would have passed silently on exactly the machine least likely to be
# producing clean files. The exit status is now inspected: 0 means matches
# (fail), 1 means clean, anything else means the scan itself did not run.
format:
	@cr=$$(printf '\r'); out=$$(LC_ALL=C grep -rln "$$cr" \
	     $(FMT_SRC) Makefile app/AndroidManifest.xml); st=$$?; \
	 if [ $$st -gt 1 ]; then \
	   echo "format: the CRLF scan could not run (grep exit $$st)"; exit 1; \
	 fi; \
	 if [ -n "$$out" ]; then \
	   echo "$$out"; echo "CRLF line endings found (see above)"; exit 1; \
	 fi
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
	 n=$$(sed -n 's/^#define NHIST[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/store.h); \
	 u=$$(sed -n 's/^#define UI_PLOT_GLU[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/ui.c); \
	 if [ -z "$$n" ] || [ -z "$$u" ]; then \
	   echo "crosscheck: could not read NHIST ('$$n') or UI_PLOT_GLU ('$$u')"; \
	   exit 1; \
	 fi; \
	 if [ "$$n" != "$$u" ]; then \
	   echo "NHIST ($$n in store.h) != UI_PLOT_GLU ($$u in ui.c):"; \
	   echo "  the shell sends up to NHIST plot points but ui.c draws at most"; \
	   echo "  UI_PLOT_GLU of them -- a smaller UI cap silently truncates the"; \
	   echo "  oldest in-window points, shrinking the 7D plot below a week."; \
	   echo "  (ui.c's own cap is UI_PLOT_GLU + NINS: the shell appends the"; \
	   echo "  insulin doses after the glucose points in the same array.)"; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mcrosscheck\033[0m: NHIST == UI_PLOT_GLU (%s)\n' "$$n"; \
	 p=$$(sed -n 's/^#define PCELL_GLU[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/plotdata.c); \
	 s=$$(sed -n 's/^#define STORE_GLU_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/store.h); \
	 if [ -z "$$p" ] || [ -z "$$s" ]; then \
	   echo "crosscheck: could not read PCELL_GLU ('$$p') or STORE_GLU_MAX ('$$s')"; \
	   echo "  the check compares two greps; if BOTH miss it would pass on \"\" == \"\"."; \
	   exit 1; \
	 fi; \
	 if [ "$$p" -le "$$s" ]; then \
	   echo "PCELL_GLU ($$p in plotdata.c) <= STORE_GLU_MAX ($$s in store.h):"; \
	   echo "  plotdata drops any reading at or above PCELL_GLU, and a dropped"; \
	   echo "  reading is not clamped to the top of the plot -- it is ABSENT."; \
	   echo "  A severe high would be stored, alarmed on and counted in TIR, and"; \
	   echo "  then silently missing from the 30- and 90-day plots."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mcrosscheck\033[0m: PCELL_GLU (%s) > STORE_GLU_MAX (%s)\n' "$$p" "$$s"; \
	 ar=$$(sed -n 's/^#define SYNC_ROW_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' app/sync.h); \
	 sr=$$(sed -n 's/^#define ROW_MAX[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1/p' srv/sync.h); \
	 if [ -z "$$ar" ] || [ -z "$$sr" ]; then \
	   echo "crosscheck: could not read SYNC_ROW_MAX ('$$ar') or ROW_MAX ('$$sr')"; \
	   echo "  the check compares two greps; if BOTH miss it would pass on \"\" == \"\"."; \
	   exit 1; \
	 fi; \
	 if [ "$$ar" != "$$sr" ]; then \
	   echo "SYNC_ROW_MAX ($$ar in app/sync.h) != ROW_MAX ($$sr in srv/sync.h):"; \
	   echo "  the two halves disagree about how long a row may be. The longer"; \
	   echo "  side accepts rows the shorter one refuses, so a bucket hashes"; \
	   echo "  differently on each end and the sync can never converge."; \
	   exit 1; \
	 fi; \
	 printf '\033[1;32mcrosscheck\033[0m: SYNC_ROW_MAX == ROW_MAX (%s)\n' "$$ar"

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
	@f=app/Alarm.java; \
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
# modeltest is EXCLUDED from clang-tidy, and only from clang-tidy: it is the
# one test that reaches inside a translation unit rather than calling across a
# header, because build_model and the state it reads are static in main.c.
# That makes three checks fire on the technique itself rather than on any
# defect -- the .c include, "no header provides g_cur_glu" (there is none, by
# design), and dexlibc.h's freestanding decls colliding with glibc's when the
# file is tidied with host headers. It is still compiled with -Werror
# (TESTWARN, the same set every other host test gets), still format-checked,
# and still run by `make check`; only the linter skips it.
TEST_SRC   := $(filter-out app/test/modeltest.c,$(wildcard app/test/*.c))
TIDY_TEST  := -iquote app -iquote lib -iquote app/test -I/usr/lib/jvm/default-java/include \
              -I/usr/lib/jvm/default-java/include/linux -DDEXDRIVER_TEST \
              -Wall -Wextra
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
tidy:
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

# offline UI render harness: builds ui.c/font.c on the host and renders the
# screens to app/test/*.ppm, so the UI can be verified with no phone attached.
# -iquote so "" project headers come from app/ while <> headers stay glibc's
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
JNI_TESTINC := -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux

define APPTEST
$(1):
	@mkdir -p build/app/test
	cc -iquote app -iquote lib $$(JNI_TESTINC) $$(TESTWARN) \
	    app/test/$(1).c $(2) -o build/app/test/$(1)
	@./build/app/test/$(1)
endef

$(eval $(call APPTEST,alarmtest,app/alarmlogic.c))
$(eval $(call APPTEST,storetest,app/store.c app/util.c app/sensors.c))
$(eval $(call APPTEST,crashtest,app/crashlog.c))
$(eval $(call APPTEST,calibtest,app/calib.c app/util.c app/stub_log.c))
$(eval $(call APPTEST,statstest,app/stats.c app/util.c app/sensors.c))
$(eval $(call APPTEST,metertest,app/otble.c app/util.c))
$(eval $(call APPTEST,registrytest,app/sensors.c app/util.c))
$(eval $(call APPTEST,settingstest,app/settings.c app/util.c))
$(eval $(call APPTEST,weighttest,app/weight.c app/util.c))
$(eval $(call APPTEST,insulintest,app/insulin.c app/util.c))
$(eval $(call APPTEST,ingesttest,app/ingest.c))
$(eval $(call APPTEST,scantest,app/scanlogic.c))
$(eval $(call APPTEST,plottest,app/plotdata.c app/sensors.c app/util.c))

uitest:
	@mkdir -p build/app/test
	cc -iquote app -iquote lib $(JVM_INC) $(TESTWARN) app/test/uitest.c app/ui.c app/font.c \
	    lib/plot.c app/sensors.c app/util.c app/weight.c -o build/app/test/uitest
	./build/app/test/uitest

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
modeltest:
	@mkdir -p build/app/test
	cc -iquote app -iquote lib $(JVM_INC) $(TESTWARN) app/test/modeltest.c app/ui.c app/font.c app/crashlog.c app/tzoff.c \
	    lib/plot.c app/util.c app/stats.c app/store.c app/settings.c \
	    app/alarmlogic.c app/scanlogic.c app/ingest.c app/insulin.c app/weight.c \
	    app/plotdata.c app/sensors.c app/otble.c app/calib.c app/dexdriver.c \
	    lib/jpake.c lib/rand.c app/dexcom.c app/dexdata.c lib/p256.c lib/sha256.c lib/aes.c lib/ecdsa.c \
	    lib/hmac.c \
	    app/sync.c app/syncjni.c app/stub_android.c app/stub_log.c \
	    -o build/app/test/modeltest
	@./build/app/test/modeltest > build/app/test/modeltest.log 2>&1 \
	    && grep -q "modeltest: the model the app builds" build/app/test/modeltest.log \
	    && printf '\033[1;32mmodeltest\033[0m: build_model + screen mapping OK\n' \
	    || { cat build/app/test/modeltest.log; exit 1; }

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

cryptotest:
	@mkdir -p build/lib/test
	cc -iquote lib -iquote srv $(TESTWARN) srv/test/cryptotest.c srv/tls.c lib/ct.c \
	    $(LIBCRYPTO) lib/aes.c lib/sha256.c lib/p256.c lib/rand.c \
	    -o build/lib/test/cryptotest
	@./build/lib/test/cryptotest > build/lib/test/cryptotest.log 2>&1 \
	    && grep -q "agree with the specs" build/lib/test/cryptotest.log \
	    && printf '\033[1;32mcryptotest\033[0m: GCM, HKDF, ECDSA vs the specs OK\n' \
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
# The two implementations against each other. SKIPS when glucoserve is not
# built, so pancra still builds on a machine that has never seen it.
interoptest: $(SRVBIN)
	@mkdir -p build/app/test
	cc -iquote app -iquote lib $(TESTWARN) app/test/interoptest.c app/sync.c app/util.c \
	    lib/jpake.c lib/rand.c app/dexcom.c lib/p256.c lib/sha256.c lib/aes.c lib/ecdsa.c \
	    lib/hmac.c \
	    -o build/app/test/interoptest
	@./app/test/interop.sh > build/app/test/interoptest.log 2>&1 \
	    || { cat build/app/test/interoptest.log; exit 1; }
	@if grep -q "ALL INTEROP TESTS PASSED" build/app/test/interoptest.log; then \
	    printf '\033[1;32minteroptest\033[0m: pancra <-> glucoserve agree\n'; \
	 elif grep -q "SKIP" build/app/test/interoptest.log; then \
	    printf '\033[1;33minteroptest\033[0m: skipped (glucoserve not built)\n'; \
	 else cat build/app/test/interoptest.log; exit 1; fi


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
DRVTEST_SRC := app/test/test_driver.c app/dexdriver.c lib/jpake.c lib/rand.c \
               app/dexcom.c app/dexdata.c lib/p256.c lib/sha256.c lib/aes.c \
               lib/ecdsa.c app/util.c

drivertest:
	@mkdir -p build/app/test
	cc -DDEXDRIVER_TEST -iquote app -iquote lib -iquote app/test $(JVM_INC) $(TESTWARN) \
	    $(DRVTEST_SRC) -o build/app/test/drivertest
	./build/app/test/drivertest > build/app/test/drivertest.log 2>&1 \
	    && grep -q "ALL DRIVER TESTS PASSED" build/app/test/drivertest.log \
	    && printf '\033[1;32mdrivertest\033[0m: pairing + auth + EGV decode OK\n' \
	    || { tail -20 build/app/test/drivertest.log; exit 1; }

.PHONY: app srv duo srvcheck tlstest crashtest cryptotest FORCE all release aab install run uninstall clean check crosscheck javacheck format format-fix tidy done uitest plottest drivertest alarmtest storetest statstest metertest registrytest settingstest scantest insulintest

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
# The object tree (and the binaries in it) are keyed by build mode, so a
# native build and a cross build cannot overwrite each other's files -- see
# the .build-mode stamp below for the failure that motivated it. Native keeps
# the plain build/srv/ path so nothing else has to change; a cross build goes
# to build/srv-cross/.
SRVMODE = $(if $(CROSS),-cross,)
SRVCC  = $(CROSS)gcc
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
ifneq ($(CROSS),)
SRVLDFLAGS += -static -no-pie -s
ARCH := riscv64
else
ARCH := native
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
SHARED = lib/jpake.c lib/rand.c lib/ct.c lib/p256.c lib/sha256.c lib/aes.c lib/plot.c

# The rest of lib/. Same rule as above -- one algorithm per file, nothing in
# them that knows what a glucose reading or an HTTP request is -- these are
# just the ones the server links and the app mostly does not. `ecdsa` is the
# exception and appears in SRC too: app/dexcom.c signs the sensor's key
# challenge with it rather than carrying a second copy of the arithmetic.
LIBCRYPTO = lib/gcm.c lib/hmac.c lib/hkdf.c lib/pbkdf2.c lib/ecdsa.c

SRVSRC = srv/sync.c srv/db.c srv/util.c srv/auth.c srv/logs.c srv/pair.c \
         srv/web.c srv/page.c srv/home.c srv/settings.c srv/invite.c \
         srv/plotpages.c srv/plots.c srv/gif.c srv/http.c srv/https.c \
         srv/tls.c $(LIBCRYPTO) $(SHARED)

# Per-file objects, under build/ like everything else, and keeping the source
# directory so srv/util.c and app/util.c can never collide. -MMD writes the
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
	@printf '\033[1;32mduo\033[0m: %s\n' "build/srv-cross/sync"

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
$(SRVCLI): srv/synccli.c srv/util.c $(filter-out lib/plot.c,$(SHARED)) srv/sync.h build/srv$(SRVMODE)/.build-mode
	@mkdir -p $(@D)
	$(SRVCC) $(SRVCFLAGS) $(SRVINC) $(TLSINC) -o $@ srv/synccli.c srv/util.c \
	    $(filter-out lib/plot.c,$(SHARED)) lib/hmac.c $(SRVLDFLAGS)

-include $(SRVDEP)

# The wire contract is the only thing standing between a stranger and the
# data, and "it stored the row" is not the same as "the two sides now agree".
srvcheck: $(SRVBIN) $(SRVCLI)
	./srv/test/synctest.sh

# THE SAME SUITE, UNDER AddressSanitizer AND UBSan.
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
srvasan:
	@$(MAKE) --no-print-directory build/srv-asan/sync build/srv-asan/synccli \
	    SRVMODE=-asan \
	    SRVCFLAGS="$(STD) -O1 -g -fsanitize=address,undefined \
	               -fno-omit-frame-pointer $(SRVWARN)" >/dev/null
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
	 ./srv/test/synctest.sh build/srv-asan/sync 18093 \
	   && printf '\033[1;32msrvasan\033[0m: the suite is clean under ASan/UBSan\n'

# synctest.sh drives the server over PLAIN HTTP -- it passes two arguments, so
# no certificate is loaded -- which meant srv/tls.c, a thousand lines of
# hand-written TLS 1.3 on the open internet, was reachable by no test at all.
# This one hands it a throwaway P-256 certificate and points real clients at
# it: openssl for the handshake, curl for a page, and a tampering proxy for
# the case that matters, a record whose ciphertext is valid and whose
# authentication tag is not. It also pins the reset-connection regression that
# used to spin a worker at 100% CPU forever.
tlstest: $(SRVBIN)
	@./srv/test/tlstest.sh $(SRVBIN) 18443
