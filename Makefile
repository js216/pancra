APP_CC = ccache clang-19 --target=aarch64-linux-android29
SRV_CC = ccache tools/riscv64-linux-musl-cross/bin/riscv64-linux-musl-cc

CFLAGS := -Werror -Wall -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes \
  -Wmissing-prototypes -Wwrite-strings -Wvla -Wformat=2 -Wundef -Wcast-qual \
  -Wdouble-promotion -Wswitch-enum -Wredundant-decls -Os -MMD -MP -std=c11 \
  -pedantic-errors -ffunction-sections -fdata-sections

CFLAGS_APP := $(CFLAGS) -D_XOPEN_SOURCE=700 -ffreestanding \
  -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -fvisibility=hidden -Iapp -Ilib \
  -I/usr/lib/jvm/java-21-openjdk-amd64/include \
  -I/usr/lib/jvm/java-21-openjdk-amd64/include/linux

CFLAGS_SRV := $(CFLAGS) -D_XOPEN_SOURCE=700 -iquote srv -iquote lib \
  -Itools/sqlite-amalgamation-3460100 -DSQLITE_THREADSAFE=1

APP_OBJ := $(patsubst %.c,build/app/obj/%.o,$(wildcard app/*.c lib/*.c)) \
           $(patsubst stub/%.c,build/stub/lib%.so,$(wildcard stub/*.c))

SRV_OBJ = $(patsubst %.c,build/srv/%.o,$(wildcard lib/*.c srv/*.c)) \
          build/srv/sqlite3.o

.PHONY: all clean install deploy

all: build/app/pancra.apk build/srv/pancra_srv

clean:
	rm -rf build

build:
	mkdir -p build/app/obj/app build/app/obj/lib build/app/obj/stub \
	    build/app/apk/lib/arm64-v8a \
	    build/stub build/srv/srv build/srv/lib

# app

build/app/obj/%.o: %.c | build
	$(APP_CC) $(CFLAGS_APP) -c -o $@ $<

build/app/apk/classes.dex: $(wildcard app/*.java) | build
	javac -Xlint:-options -source 8 -target 8 \
	    -bootclasspath tools/android.jar -d build/app/classes $^
	java -cp tools/r8.jar com.android.tools.r8.D8 --release \
	    --min-api 29 --lib tools/android.jar --output $(@D) \
	    $$(find build/app/classes -name '*.class')

build/stub/lib%.so: build/app/obj/stub/%.o | build
	$(APP_CC) -shared -nostdlib -fuse-ld=lld -Wl,-soname,lib$*.so -o $@ $<

build/app/apk/lib/arm64-v8a/libpancra.so: $(APP_OBJ) | build
	$(APP_CC) -shared -nostdlib -fuse-ld=lld -Wl,--no-undefined \
	    -Wl,--gc-sections -o $@ $^
	llvm-strip-19 $@

build/app/pancra.apk: app/AndroidManifest.xml Makefile \
        $(wildcard app/res/*/*) \
        build/app/apk/lib/arm64-v8a/libpancra.so build/app/apk/classes.dex \
        | build
	aapt package -f -M app/AndroidManifest.xml -S app/res \
	    -I /usr/share/android-framework-res/framework-res.apk --debug-mode \
	    --version-code 1 --min-sdk-version 29 --target-sdk-version 35 \
	    --version-name 0.1 -F build/app/pancra.unaligned.apk
	cd build/app/apk && aapt add ../pancra.unaligned.apk \
	    lib/arm64-v8a/libpancra.so classes.dex
	zipalign -f -p 4 build/app/pancra.unaligned.apk $@
	apksigner sign --ks tools/debug.keystore --ks-pass pass:android $@

install: build/app/pancra.apk
	adb install -r $<
	adb shell am start -n com.jk.pancra/android.app.NativeActivity

# server

build/srv/sqlite3.o: tools/sqlite-amalgamation-3460100/sqlite3.c | build
	$(SRV_CC) $(CFLAGS_SRV) -w -c -o $@ $<

build/srv/%.o: %.c | build
	$(SRV_CC) $(CFLAGS_SRV) -c -o $@ $<

build/srv/pancra_srv: $(SRV_OBJ) | build
	$(SRV_CC) -o $@ $^ -static -no-pie -s -pthread -Wl,--gc-sections

deploy: build/srv/pancra_srv
	ssh duo killall sync pancra_srv || echo "Server not yet running."
	scp -O build/srv/pancra_srv duo:projects/glucoserve/
	ssh duo projects/glucoserve/sync-tls.sh

-include $(patsubst %.o,%.d,$(filter %.o,$(APP_OBJ))) \
         $(SRV_OBJ:.o=.d)
