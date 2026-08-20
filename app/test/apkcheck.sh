#!/bin/sh
set -eu
APK=${1:-build/app/pancra.apk}
fail() { printf 'apkcheck: FAIL: %s\n' "$*" >&2; exit 1; }
[ -f "$APK" ] || fail "missing $APK"
list=$(unzip -l "$APK")
printf '%s' "$list" | grep -q 'lib/arm64-v8a/libpancra.so' || fail "JNI library missing"
printf '%s' "$list" | grep -q 'classes.dex' || fail "classes.dex missing"
badging=$(aapt dump badging "$APK") || fail "aapt could not parse APK"
for p in android.permission.BLUETOOTH_SCAN android.permission.BLUETOOTH_CONNECT \
         android.permission.FOREGROUND_SERVICE_CONNECTED_DEVICE \
         android.permission.POST_NOTIFICATIONS android.permission.VIBRATE; do
  printf '%s' "$badging" | grep -q "uses-permission: name='$p'" || fail "missing $p"
done
# THE ABI BOUNDARY, ENFORCED AT PACKAGING TIME.
#
# arm64-v8a ALONE. Not "arm64 is present": a package that also carries
# armeabi-v7a installs the 32-bit library on a 32-bit device, where `long` is
# four bytes -- and every id, timestamp and bucket in this program is a long
# (see srv/proto.h). That build does not fail; it stops representing
# timestamps in 2038 and truncates ids before then. The static assertions
# refuse to COMPILE such a target; this refuses to SHIP one.
abis=$(printf '%s' "$badging" | sed -n "s/^native-code: //p" | tr -d "'")
[ "$abis" = "arm64-v8a" ] || fail "native ABIs are '$abis', not arm64-v8a alone"
printf '%s' "$list" | grep 'lib/' | grep -v 'lib/arm64-v8a/' \
  && fail "the package carries a library outside lib/arm64-v8a"
# ...and the library really is what it says: a 64-bit AArch64 object. A file
# under lib/arm64-v8a/ is only named arm64.
#
# EXTRACTED FROM THE PACKAGE, not read from the staging tree.
#
# These checks used to open build/app/apk/lib/arm64-v8a/libpancra.so -- the
# file the packaging step copies FROM. So they described the library the build
# produced, while the APK is what ships, and the two are only the same file
# while nothing has gone wrong in between. A truncated or half-written zip
# member, an aapt that packaged an older library, or a hand-edited APK all
# pass a check aimed at the staging copy, because the staging copy is
# healthy. The one artifact nobody inspected was the one that goes on the
# phone.
#
# The APK argument is the subject of every other assertion here (unzip -l,
# aapt dump badging, the DEX strings); this makes the ELF and symbol checks
# agree with them.
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
unzip -p "$APK" lib/arm64-v8a/libpancra.so > "$TMP/libpancra.so" \
  || fail "could not extract lib/arm64-v8a/libpancra.so from the package"
[ -s "$TMP/libpancra.so" ] || fail "the packaged JNI library is EMPTY"
so=$TMP/libpancra.so
# od -N20 wraps, so the newline is stripped too: the 20 bytes become one hex
# string. Bytes 0..3 are ELF's magic, 4 is the class (02 = 64-bit), 5 the
# endianness (01 = little), and 18..19 e_machine, which is 0x00B7 (AArch64)
# little-endian.
elfhdr=$(od -An -tx1 -N20 "$so" | tr -d " \n")
case $elfhdr in
  7f454c460201*b700) : ;;
  *) fail "$so is not a 64-bit AArch64 ELF object (header $elfhdr)" ;;
esac
# THE DEX, EXTRACTED BEFORE IT IS SEARCHED -- the same rule the ELF checks
# above already follow, and the last place in this file that broke it.
#
# It used to be `unzip -p "$APK" classes.dex | strings | grep -q ...`. A
# pipeline's status is the LAST command's, so an unzip that could not find or
# could not read classes.dex fed grep nothing, grep matched nothing, and the
# script reported "PancraService absent from DEX" -- which sends the reader to
# the Java build to look for a class that is sitting there perfectly. The
# package had no readable DEX at all. Same family as the fetch and logcat
# defects app/test/adbdrill.sh covers: the tool failed, and the failure was
# reported as a finding about its input.
unzip -p "$APK" classes.dex > "$TMP/classes.dex" \
  || fail "could not extract classes.dex from the package"
[ -s "$TMP/classes.dex" ] || fail "the packaged classes.dex is EMPTY"
strings "$TMP/classes.dex" > "$TMP/dex.strings" \
  || fail "the packaged classes.dex could not be read as a file"
grep -q 'com/jk/pancra/PancraService' "$TMP/dex.strings" \
  || fail "PancraService absent from DEX"
# READABLE AS AN OBJECT AT ALL, said separately from what is in it.
#
# A truncated member keeps its ELF header, so the magic check above still
# passes; nm is then what notices, and folding the two together reported a
# half-written library as "JNI entry point missing" -- which sends the reader
# looking for a linker problem that does not exist.
syms=$(nm -D "$so" 2>"$TMP/nm.err") || {
  fail "the packaged JNI library cannot be read as an object: $(head -1 "$TMP/nm.err")"
}
printf '%s' "$syms" | grep -q ' ANativeActivity_onCreate' \
  || fail "NativeActivity JNI entry point missing"
# THE PACKAGED LIBRARY IS THE ONE THAT WAS BUILT. Everything above proves the
# payload is a healthy AArch64 object with the right entry point; this proves
# it is the same healthy object the rest of the gate just tested, so a stale
# member cannot pass by being valid.
staged=build/app/apk/lib/arm64-v8a/libpancra.so
if [ -f "$staged" ]; then
  cmp -s "$staged" "$so" \
    || fail "the packaged library differs from the one this build produced"
fi

# =====================================================================
# WHAT THE PACKAGE SAYS IT IS -- READ OUT OF THE PACKAGE.
# ---------------------------------------------------------------------
# Everything from here down asks aapt what the FINISHED artifact declares.
# Not app/AndroidManifest.xml, and not the Makefile variables that were fed
# to aapt, because neither of those is what installs on a phone and the two
# can disagree without any diagnostic at all.
#
# THEY DEMONSTRABLY CAN. Measured on this toolchain (aapt v0.2-debian,
# aapt2 2.19-debian): --version-code, --version-name, --min-sdk-version and
# --target-sdk-version are INJECT-IF-ABSENT. Hand aapt a manifest that
# already carries the attribute and the flag is parsed, accepted and
# discarded in silence. The AAB rule passed `--target-sdk-version 34` for
# months against a manifest that said 34; had anyone edited the flag to
# retarget the app, the build would have succeeded and shipped the old
# value, and the ONLY witness anywhere would have been a badging dump like
# this one. So the flags are checked by reading the result of using them.
#
# EACH EXPECTATION IS REQUIRED TO BE NON-EMPTY FIRST. A comparison against
# "" passes on everything, and an unset variable is exactly how a gate comes
# to report success over an artifact it never looked at.
want() {
  eval "v=\${$1:-}"
  [ -n "$v" ] || fail "$1 was not supplied, so there is nothing to compare
  the package against and this check would pass on any APK at all."
  printf '%s' "$v"
}
got() { printf '%s' "$badging" | sed -n "s/^$1:'\(.*\)'\$/\1/p" | head -1; }

# THE VERSION CODE. The field that cannot be repaired after distribution:
# Android will not install a package whose version code does not exceed the
# installed one, so a second artifact carrying the first one's code cannot
# update it. The user is shown an install failure and the only route past it
# is uninstall -- which deletes this app's private storage, i.e. every
# glucose reading, insulin dose and meal they have ever logged. There is no
# support answer for that, which is why it is asserted on the bytes that
# ship rather than on the number the build meant to use.
# Bound FIRST, all of them, before a single comparison. Written inline as
# `[ "$got" = "$(want EXPECT_MIN_SDK)" ] || fail "... $EXPECT_MIN_SDK"`, an
# unsupplied expectation made the comparison merely false and then the
# failure message itself referenced the unset variable -- so `set -u` killed
# the script with "EXPECT_MIN_SDK: unbound variable" and the reader learned
# nothing about the APK. A missing expectation has to be its own named
# failure, said before anything depends on it.
wvc=$(want EXPECT_VERSION_CODE)
wvn=$(want EXPECT_VERSION_NAME)
wmin=$(want EXPECT_MIN_SDK)
wtgt=$(want EXPECT_TARGET_SDK)
wcmp=$(want EXPECT_COMPILE_SDK)
pkgline=$(printf '%s' "$badging" | sed -n 's/^package: //p' | head -1)
[ -n "$pkgline" ] || fail "the package has no 'package:' badging line"
gvc=$(printf '%s' "$pkgline" | sed -n "s/.*versionCode='\([^']*\)'.*/\1/p")
gvn=$(printf '%s' "$pkgline" | sed -n "s/.*versionName='\([^']*\)'.*/\1/p")
[ -n "$gvc" ] || fail "the packaged manifest declares NO versionCode.
  It defaults to 0 on the device, and every later artifact then has to
  exceed a number this one never admitted to having."
[ "$gvc" = "$wvc" ] || fail "packaged versionCode is '$gvc', the build asked
  for '$wvc'. The flag was accepted and did not reach the package -- check
  whether android:versionCode has come back into app/AndroidManifest.xml,
  where it silently wins."
[ "$gvn" = "$wvn" ] || fail "packaged versionName is '$gvn', expected '$wvn'"

# ...AND THAT IT EXCEEDS WHAT WAS ALREADY PUBLISHED, on the release paths.
# The Makefile checks this too, against its own variable; this checks the
# artifact. They are not the same assertion: the Makefile's says the build
# INTENDED a newer code, this one says the file in your hand HAS one. Only
# the second is true of the thing you are about to upload.
if [ "${REQUIRE_NEWER:-0}" = "1" ]; then
  pub=$(want PUBLISHED_VERSION_CODE)
  case $pub in *[!0-9]*) fail "PUBLISHED_VERSION_CODE='$pub' is not an integer;
  an unreadable ledger must not compare true";; esac
  [ "$gvc" -gt "$pub" ] || fail "packaged versionCode $gvc does NOT exceed the
  last published code $pub. This artifact cannot update the one already
  installed on people's phones; they get an install failure, and the only
  way past it is an uninstall that deletes their history."
fi

# THE SDK LEVELS, all three, from the package.
#
# targetSdkVersion is a condition of publishing -- Play refuses updates from
# an app targeting too low an API -- and it is also how the app opts in to a
# platform generation's behaviour changes, so a stale one is both an upload
# rejection and a phone that behaves like an older release.
#
# compileSdkVersion is not passed by the build at all: aapt stamps it from
# the framework-res.apk it was given with -I. Asserting it is how replacing
# that input becomes a failure naming COMPILE_SDK, instead of a quietly
# different package.
gmin=$(got sdkVersion); gtgt=$(got targetSdkVersion)
gcmp=$(printf '%s' "$pkgline" | sed -n "s/.*compileSdkVersion='\([^']*\)'.*/\1/p")
[ -n "$gmin" ] || fail "the packaged manifest declares no minSdkVersion"
[ -n "$gtgt" ] || fail "the packaged manifest declares no targetSdkVersion.
  Android then treats the app as targeting minSdkVersion, turning every
  compatibility shim back on, and Play refuses the upload."
[ -n "$gcmp" ] || fail "the package declares no compileSdkVersion"
[ "$gmin" = "$wmin" ] || \
  fail "packaged minSdkVersion is '$gmin', the build asked for '$wmin'"
[ "$gtgt" = "$wtgt" ] || \
  fail "packaged targetSdkVersion is '$gtgt', the build asked for
  '$wtgt'. A --target-sdk-version that does not reach the package is the
  exact defect this check exists for: aapt accepts the flag and discards it
  when <uses-sdk> is present in app/AndroidManifest.xml."
[ "$gcmp" = "$wcmp" ] || \
  fail "packaged compileSdkVersion is '$gcmp', COMPILE_SDK says '$wcmp'.
  aapt takes this from framework-res.apk, so either FRAMEWORK now points
  somewhere else or COMPILE_SDK is stale."

# =====================================================================
# BACKUP AND DEVICE-TO-DEVICE TRANSFER, ASSERTED IN THE PACKAGED MANIFEST
# ---------------------------------------------------------------------
# The files this app keeps privately are the user's medical history and the
# credential that authenticates their phone to the copy of it on the server.
# allowBackup="false" alone settles cloud backup and says nothing about the
# device-to-device channel Android 12 split out, whose behaviour for an app
# supplying no rules is a platform default that has not been constant.
#
# THE WHOLE CHAIN IS READ OUT OF THE PACKAGE, link by link: the manifest
# attribute, the resource id it points at, the name that id resolves to, the
# file that name maps to, and the RULES INSIDE THAT FILE. Checking that
# app/res/xml/data_extraction_rules.xml exists on disk would prove none of
# it -- a resource can be absent from the package, an attribute can point at
# the wrong id, and a file can be present and empty.
xt=$(aapt2 dump xmltree --file AndroidManifest.xml "$APK") \
  || fail "aapt2 could not read the packaged manifest"
printf '%s' "$xt" | grep -q 'E: manifest' \
  || fail "the packaged manifest did not decode as an XML tree"
printf '%s' "$xt" | grep -q 'allowBackup([^)]*)=false' \
  || fail "the packaged manifest does not declare android:allowBackup=false"
res=$(aapt2 dump resources "$APK") || fail "aapt2 could not read the resource table"
# id -> name -> packaged file, for one manifest attribute.
rulefile() {
  id=$(printf '%s' "$xt" | sed -n "s/.*$1([^)]*)=@\(0x[0-9a-f]*\).*/\1/p" | head -1)
  [ -n "$id" ] || fail "the packaged manifest has no android:$1 pointing at a
  resource. Without it Android decides for itself what leaves this device."
  name=$(printf '%s' "$res" | sed -n "s/^ *resource $id \(.*\)\$/\1/p" | head -1)
  [ -n "$name" ] || fail "android:$1 points at $id, which is not in the
  packaged resource table -- the attribute survived and the rules did not."
  path=$(printf '%s' "$res" | grep -A2 "resource $id " \
         | sed -n 's/.*(file) \(res\/[^ ]*\).*/\1/p' | head -1)
  [ -n "$path" ] || fail "resource $name ($id) has no file in the package"
  printf '%s' "$list" | grep -q " $path\$" \
    || fail "$path is named by the resource table and is NOT in the zip"
  aapt2 dump xmltree --file "$path" "$APK" \
    || fail "$path could not be decoded out of the package"
}
# API 31+: both channels, every domain. Whatever is not excluded is included,
# so this counts the domains rather than trusting that a file called
# data_extraction_rules contains rules.
DOMAINS='root file database sharedpref external device_root device_file device_database device_sharedpref'
der=$(rulefile dataExtractionRules)
for sect in cloud-backup device-transfer; do
  body=$(printf '%s\n' "$der" | awk -v s="E: $sect" '
    index($0,s){d=1;next} d && /E: (cloud-backup|device-transfer)/{d=0} d')
  [ -n "$body" ] || fail "the packaged data-extraction rules have no <$sect>
  section, so Android's default decides that channel"
  printf '%s' "$body" | grep -q 'E: include' \
    && fail "<$sect> in the packaged rules carries an <include>; this app
  excludes everything and an include re-admits it"
  for d in $DOMAINS; do
    printf '%s' "$body" | grep -q "domain=\"$d\"" \
      || fail "<$sect> in the PACKAGED data-extraction rules does not exclude
  domain \"$d\". Everything not excluded is included: that domain's files --
  glucose history, or the sync credential -- would be copied off the device."
  done
done
# API 30 and below read a different attribute entirely and ignore this one;
# minSdkVersion is 29, so both generations are live and both need rules.
fbc=$(rulefile fullBackupContent)
printf '%s' "$fbc" | grep -q 'E: full-backup-content' \
  || fail "the packaged legacy backup rules are not a <full-backup-content>"
printf '%s' "$fbc" | grep -q 'E: include' \
  && fail "the packaged legacy backup rules carry an <include>"
for d in $DOMAINS; do
  printf '%s' "$fbc" | grep -q "domain=\"$d\"" \
    || fail "the PACKAGED legacy backup rules do not exclude domain \"$d\";
  Android 29 and 30 -- the phones most likely to be the OLD one in a
  device-to-device transfer -- would copy it."
done

printf 'apkcheck: package, permissions, DEX, JNI entry point, version %s (%s),\n' \
  "$gvc" "$gvn"
printf '  sdk %s..%s (compile %s) and backup/transfer exclusions passed\n' \
  "$gmin" "$gtgt" "$gcmp"
