#!/bin/sh
# THE SAME QUESTIONS AS app/test/apkcheck.sh, ASKED OF A BUNDLE.
#
# WHY A SEPARATE SCRIPT AND NOT A MODE OF apkcheck.sh: an .aab is not a zip
# full of binary XML, it is bundletool's module layout wrapping a PROTOBUF
# manifest. aapt cannot read it -- `aapt dump badging` on a bundle fails --
# so every tool in the other script is the wrong tool here. The assertions
# are deliberately the same ones; only the instrument differs.
#
# AND IT MATTERS MORE HERE, not less. The .aab is the artifact Play
# distributes. The APK path at least has releasecheck looking at a signature;
# a bundle is built, uploaded, and turned into APKs by somebody else's
# server, so this is the last point at which anyone we know reads what it
# says.
set -eu
AAB=${1:-build/app/pancra.aab}
BUNDLETOOL=${BUNDLETOOL:-java -jar tools/bundletool.jar}
fail() { printf 'aabcheck: FAIL: %s\n' "$*" >&2; exit 1; }
[ -f "$AAB" ] || fail "missing $AAB"

want() {
  eval "v=\${$1:-}"
  [ -n "$v" ] || fail "$1 was not supplied, so there is nothing to compare
  the bundle against and this check would pass on any bundle at all."
  printf '%s' "$v"
}
wvc=$(want EXPECT_VERSION_CODE)
wvn=$(want EXPECT_VERSION_NAME)
wmin=$(want EXPECT_MIN_SDK)
wtgt=$(want EXPECT_TARGET_SDK)
wcmp=$(want EXPECT_COMPILE_SDK)

# THE MANIFEST OUT OF THE BUNDLE, not the one that was handed to aapt2.
# aapt2 treats --version-code and --target-sdk-version as inject-if-absent:
# with the attribute present in app/AndroidManifest.xml the flag is accepted
# and silently discarded. Only this dump can tell the two apart.
man=$($BUNDLETOOL dump manifest --bundle "$AAB") \
  || fail "bundletool could not read the manifest out of $AAB"
printf '%s' "$man" | grep -q '<manifest' \
  || fail "bundletool printed no <manifest> element for $AAB"
attr() { printf '%s' "$man" | sed -n "s/.*android:$1=\"\([^\"]*\)\".*/\1/p" | head -1; }

gvc=$(attr versionCode); gvn=$(attr versionName)
[ -n "$gvc" ] || fail "the bundle manifest declares NO versionCode; it
  defaults to 0 on the device and no later artifact can be ordered after it"
[ "$gvc" = "$wvc" ] || fail "bundle versionCode is '$gvc', the build asked
  for '$wvc' -- the flag was accepted and did not reach the bundle. Check
  whether android:versionCode has come back into app/AndroidManifest.xml."
[ "$gvn" = "$wvn" ] || fail "bundle versionName is '$gvn', expected '$wvn'"

# EXCEEDING WHAT WAS PUBLISHED, asserted on the bundle rather than on the
# build's intention. Android refuses to install a package whose version code
# does not exceed the installed one; the way past that refusal is an
# uninstall, and uninstalling this app deletes the user's glucose history.
if [ "${REQUIRE_NEWER:-0}" = "1" ]; then
  pub=$(want PUBLISHED_VERSION_CODE)
  case $pub in *[!0-9]*) fail "PUBLISHED_VERSION_CODE='$pub' is not an
  integer; an unreadable ledger must not compare true";; esac
  [ "$gvc" -gt "$pub" ] || fail "bundle versionCode $gvc does NOT exceed the
  last published code $pub. Uploaded, this cannot update the installed app."
fi

gmin=$(printf '%s' "$man" | sed -n 's/.*android:minSdkVersion="\([^"]*\)".*/\1/p' | head -1)
gtgt=$(printf '%s' "$man" | sed -n 's/.*android:targetSdkVersion="\([^"]*\)".*/\1/p' | head -1)
gcmp=$(attr compileSdkVersion)
[ -n "$gtgt" ] || fail "the bundle manifest declares no targetSdkVersion.
  Android then treats the app as targeting minSdkVersion and Play refuses
  the upload."
[ "$gmin" = "$wmin" ] || fail "bundle minSdkVersion is '$gmin', expected '$wmin'"
[ "$gtgt" = "$wtgt" ] || fail "bundle targetSdkVersion is '$gtgt', the build
  asked for '$wtgt'. This is the flag that was dead for months: aapt2 took
  --target-sdk-version and discarded it because <uses-sdk> was in the
  manifest source, and no build ever said so."
[ "$gcmp" = "$wcmp" ] || fail "bundle compileSdkVersion is '$gcmp', COMPILE_SDK
  says '$wcmp' -- framework-res.apk changed, or COMPILE_SDK is stale."

# BACKUP AND DEVICE-TRANSFER, from the bundle's own manifest and the bundle's
# own resource table. bundletool prints the attribute as a resource
# reference; the rules themselves live in a protobuf XML file inside the
# bundle, which is extracted and decoded here. The point of doing all of it
# is that "app/res/xml/data_extraction_rules.xml is in the source tree" says
# nothing about whether the bundle carries it.
printf '%s' "$man" | grep -q 'android:allowBackup="false"' \
  || fail "the bundle manifest does not declare android:allowBackup=false"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT INT TERM
unzip -qo "$AAB" -d "$tmp/x" || fail "$AAB does not unzip"
DOMAINS='root file database sharedpref external device_root device_file device_database device_sharedpref'
for a in dataExtractionRules fullBackupContent; do
  printf '%s' "$man" | grep -q "android:$a=" \
    || fail "the bundle manifest has no android:$a. Without it the platform
  decides for itself what leaves the device, and the files in question are
  the user's medical history and the sync credential for it."
done
# The resource files themselves. Names are matched rather than resolved
# through ids because a bundle's table is protobuf; what has to be true is
# that the RULES ARE IN THE BUNDLE and say no to every domain.
for f in data_extraction_rules backup_rules; do
  src=$(find "$tmp/x" -path '*res/xml/'"$f"'*' | head -1)
  [ -n "$src" ] || fail "res/xml/$f.xml is not in $AAB. The manifest points
  at rules the bundle does not contain."
  txt=$(aapt2 dump xmltree --file "${src#$tmp/x/}" "$AAB" 2>/dev/null || strings "$src")
  [ -n "$txt" ] || fail "res/xml/$f.xml in the bundle is EMPTY"
  for d in $DOMAINS; do
    printf '%s' "$txt" | grep -q "$d" \
      || fail "the BUNDLED $f does not exclude domain \"$d\". Whatever is not
  excluded is included: that domain's files would be copied to a new phone."
  done
done
# device_* are substrings of nothing, but `root` IS a substring of
# device_root -- so the loop above cannot distinguish "excludes root" from
# "excludes device_root only". The two channels are checked by name here,
# where the structure is legible.
for want_el in cloud-backup device-transfer; do
  src=$(find "$tmp/x" -path '*res/xml/data_extraction_rules*' | head -1)
  strings "$src" | grep -q "$want_el" \
    || fail "the bundled data-extraction rules have no <$want_el> section,
  so Android's default decides that channel"
done
printf 'aabcheck: %s -- version %s (%s), sdk %s..%s (compile %s), backup and\n' \
  "$AAB" "$gvc" "$gvn" "$gmin" "$gtgt" "$gcmp"
printf '  device-transfer excluded on every domain\n'
