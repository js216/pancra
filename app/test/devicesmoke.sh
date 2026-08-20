#!/bin/sh
# Device boundary smoke test. This intentionally uses only adb/system surfaces:
# it checks the packaged JNI library, real lifecycle, foreground service,
# notification, and the permissions Android actually granted.
#
# THE TWO WAYS THIS SCRIPT USED TO REPORT A FALSE ALL-CLEAR.
#
# 1. THE PERMISSIONS. It ran `dumpsys package` and grepped the dump for each
#    permission NAME. A permission name appears in that dump because the app
#    ASKS for it -- the "requested permissions:" block lists every uses-
#    permission in the manifest whether the user granted it, denied it, or was
#    never asked. So the check was equivalent to grepping AndroidManifest.xml,
#    and it printed
#
#      ok   BLE and notification permissions reached Android package state
#
#    on a phone where the user had tapped Deny on the Bluetooth dialog -- a
#    build that cannot see the sensor at all, reported as healthy by the one
#    check whose whole job is to catch that. What is asked for now is the
#    runtime GRANT flag, per permission, for the permissions this API level
#    actually has, after an explicit `pm grant`.
#
# 2. THE LOGCAT. It ended with
#
#      if adb logcat -d -v brief | grep -E "FATAL EXCEPTION|..."; then
#
#    A pipeline's status is the LAST command's, so "adb could not talk to the
#    device" and "grep found no fatal message" are the same status, and the
#    same status printed "no fatal Java/JNI error was logged". A phone that had
#    gone away mid-run -- the commonest thing that happens to a phone on a
#    desk -- produced a clean log with no lines in it, and the run went green.
#    The snapshot is now collected to a file, adb's own status is checked, the
#    snapshot is required to be non-empty (the log was cleared and an app was
#    started; a log with nothing in it means the collection, not the app), and
#    only then is it searched.
#
# Both are the same defect: a tool that failed and a tool that found nothing
# were indistinguishable. Every adb collection in this file now goes through
# snap() for that reason -- see the comment there.
#
# app/test/adbdrill.sh runs this script against a fake adb, with no phone
# attached, and mutates the lines marked `#R:` to check that each rule can
# still be seen to fail. Those markers are load-bearing.
set -eu

APK=${1:-build/app/pancra.apk}
PKG=com.jk.pancra
ACT=android.app.NativeActivity
# WHERE THE SNAPSHOTS LAND, overridable so the drill can run this script
# unmodified without scribbling over the artifacts of a real device run sitting
# in build/app/. `make devicecheck` passes nothing and writes where it always
# did.
OUT=${PANCRA_SMOKE_OUT:-build/app}
# HOW LONG TO LET THE PHONE SETTLE after a launch or a keyevent. A real device
# needs it; the drill's fake adb answers instantly and would otherwise pay four
# seconds on every one of its two dozen runs. It is a duration, not a switch:
# nothing is skipped when it is zero.
SETTLE=${PANCRA_SMOKE_SETTLE:-2}

fail() { printf 'devicecheck: FAIL: %s\n' "$*" >&2; exit 1; }
ok() { printf '  ok   %s\n' "$*"; }

command -v adb >/dev/null 2>&1 || fail "adb is required"
[ -f "$APK" ] || fail "APK not found: $APK"
[ "$(adb get-state 2>/dev/null)" = device ] || fail "exactly one ready device is required"
mkdir -p "$OUT"

# THE TEMPORARIES, CLEANED BY THE TRAP AND BY NOTHING ELSE -- the same rule as
# app/fetch.sh, and for the same reason: fail() exits from wherever it is
# called, so an error path that tidied up after itself would have to exist in
# nine places and would be forgotten in one of them.
trap 'rm -f "$OUT"/device-*.txt.part' EXIT INT TERM

# EVERY adb COLLECTION GOES THROUGH HERE.
#
# Three separate rules, each of which was missing somewhere in this file:
#
#   * adb's own exit status is checked, so "the device went away" is a failure
#     and not an empty file that later reads as "nothing wrong";
#   * the output lands in a .part and is renamed only when adb has finished, so
#     a collection that died half way leaves the destination absent rather than
#     a truncated dump that a human then reads and believes. `dumpsys package`
#     is 300 lines; the half of it that arrives before a USB reset still
#     contains the word BLUETOOTH_SCAN;
#   * an empty snapshot is not evidence of anything, and is refused. This is a
#     separate verdict from the two above, in separate words, because "the log
#     could not be collected" and "the log came back empty" send the reader to
#     different places.
#
# The destination is removed BEFORE the collection, so a failed collection can
# never leave the PREVIOUS run's snapshot in place for the next check to read
# as this run's answer.
snap_died() { # snap_died <what> <status>
   fail "$1 could not be collected: adb exited $2. The device is not reachable,
     so there is no snapshot here and nothing below it could be checked."
}
snap_empty() { # snap_empty <what>
   fail "$1 came back EMPTY. That is not the same as finding nothing in it: an
     empty snapshot means the collection did not happen, and every check that
     would have read it is meaningless."
}
snap_raw() { # snap_raw <destination> <what> <adb args...>
   sn_dst=$1
   sn_what=$2
   shift 2
   rm -f "$sn_dst" "$sn_dst.part"
   adb "$@" >"$sn_dst.part" || snap_died "$sn_what" "$?"           #R:adbstatus
   if [ "$SNAP_EMPTY_OK" = 0 ]; then
      [ -s "$sn_dst.part" ] || snap_empty "$sn_what"                   #R:empty
   fi
   mv "$sn_dst.part" "$sn_dst"                                        #R:atomic
}
snap() { SNAP_EMPTY_OK=0; snap_raw "$@"; }
# For the one collection whose emptiness is a legitimate answer: `cmd appops
# get` prints nothing at all for an app whose ops have never been touched.
snap_may_be_empty() { SNAP_EMPTY_OK=1; snap_raw "$@"; }

snap "$OUT/device-abi.txt" "the device ABI" shell getprop ro.product.cpu.abi
abi=$(tr -d '\r' <"$OUT/device-abi.txt")
case "$abi" in arm64-v8a) ;; *) fail "APK is arm64-v8a but device ABI is $abi";; esac

snap "$OUT/device-sdk.txt" "the device API level" shell getprop ro.build.version.sdk
sdk=$(tr -d '\r' <"$OUT/device-sdk.txt")
case "$sdk" in
[0-9]|[0-9][0-9]|[0-9][0-9][0-9]) ;;
*) fail "the device reported API level '$sdk', which is not a number" ;;
esac

adb install -r "$APK" >/dev/null || fail "APK install failed"
adb logcat -c || fail "the logcat buffer could not be cleared, so anything found
     in it afterwards could be from a previous run"
adb shell am force-stop "$PKG" || fail "the app could not be force-stopped"
snap "$OUT/device-start.txt" "the activity launch report" \
   shell am start -W -n "$PKG/$ACT"
grep -q 'Status: ok' "$OUT/device-start.txt" || fail "activity did not report Status: ok"
ok "NativeActivity launched"

sleep "$SETTLE"
# THE PROCESS, ASKED WITH A MARKER ON THE END.
#
# `adb shell pidof <pkg> | tr -d '\r'` cannot tell "no such process" from "adb
# never ran anything", because pidof exits 1 for the first and adb exits 1 for
# the second, and the pipeline swallows both into an empty string. So the
# device shell is asked to append its own status: if `rc=` is not in the
# snapshot then the remote shell did not run, which is a different report from
# "the app died".
alive_snapshot() { # alive_snapshot <what>
   snap "$OUT/device-pid.txt" "$1" shell "pidof $PKG; echo rc=\$?"
   grep -q '^rc=' "$OUT/device-pid.txt" ||
      fail "$1: the device shell produced no status marker, so whether the app
     is running was never established"
   pid=$(grep -v '^rc=' "$OUT/device-pid.txt" | tr -d '\r' | head -1)
}
alive_snapshot "the app's process id"
[ -n "$pid" ] || fail "app process died after launch"
ok "JNI library loaded and process remains alive (pid $pid)"

snap "$OUT/device-services.txt" "the service dump" \
   shell dumpsys activity services "$PKG"
grep -q 'PancraService' "$OUT/device-services.txt" || fail "foreground service is absent"
ok "foreground service is running"

snap "$OUT/device-notifications.txt" "the notification dump" \
   shell dumpsys notification --noredact
grep -q "$PKG" "$OUT/device-notifications.txt" || fail "foreground notification is absent"
ok "foreground notification is posted"

# ---- THE PERMISSIONS ANDROID ACTUALLY GRANTED ----------------------------
#
# WHICH PERMISSIONS ARE REQUIRED IS A QUESTION ABOUT THE DEVICE, not about the
# manifest. minSdkVersion is 29 and the manifest declares the whole span:
# BLUETOOTH_SCAN and BLUETOOTH_CONNECT exist from API 31, POST_NOTIFICATIONS
# from API 33, and on anything older BLE scanning is gated on
# ACCESS_FINE_LOCATION instead. A permission the platform has never heard of is
# listed as requested and is granted to nobody, so requiring the modern three
# everywhere would fail every Android 10, 11 and 12 device -- and requiring
# only their PRESENCE, which is what this used to do, passed everywhere and
# meant nothing. The set is chosen by API level and then each member of it must
# be genuinely granted.
if [ "$sdk" -ge 31 ]; then                                            #R:sdkgate
   perms="android.permission.BLUETOOTH_SCAN android.permission.BLUETOOTH_CONNECT"
else
   perms="android.permission.ACCESS_FINE_LOCATION"
fi
if [ "$sdk" -ge 33 ]; then
   perms="$perms android.permission.POST_NOTIFICATIONS"
fi

# EXERCISE THE GRANT, do not merely observe one. A smoke run on a fresh install
# has been asked nothing, so every runtime permission is denied and this whole
# section would be a report about a dialog nobody tapped. `pm grant` is the
# non-interactive form of that dialog; if it FAILS, the manifest does not
# declare the permission, or the platform does not have it, or policy forbids
# it -- all of which are things this gate exists to find, and none of which
# were visible before because nothing was granted and nothing was checked.
for p in $perms; do
   adb shell pm grant "$PKG" "$p" || fail "pm grant of $p was refused"      #R:grant
done

snap "$OUT/device-package.txt" "the package state" shell dumpsys package "$PKG"

# `granted=true` FOR THIS PERMISSION, not the permission's name somewhere in a
# 300-line dump. The false test is asked FIRST and wins: a multi-user device
# prints one runtime block per user, and a permission granted for one user and
# denied for another is not granted. Fail closed on the disagreement.
#
# The dots in a permission name are regex wildcards; escaped so that the check
# is about the permission asked for and not about anything that resembles it.
perm_is_granted() { # perm_is_granted <permission>
   pg_re=$(printf '%s' "$1" | sed 's/\./\\./g')
   if grep -qE "(^|[[:space:]])$pg_re: granted=false" "$OUT/device-package.txt"; then
      return 1                                                     #R:grantedno
   fi
   grep -qE "(^|[[:space:]])$pg_re: granted=true" "$OUT/device-package.txt" #R:granted
}

# THE APP-OP, which is a second switch in series with the grant and is how a
# permission comes to be `granted=true` and still refused at the call. A
# restricted permission, an enterprise policy, or a user who chose "only this
# time" and let it lapse all leave the grant flag set and the op ignored, and
# the app then gets an empty scan result rather than a SecurityException --
# silence, which looks exactly like a sensor that is out of range.
#
# THE OP NAMES BELOW ARE NOT VERIFIED AGAINST A REAL DEVICE. They are what the
# platform's own OPSTR_ constants say, but nothing here has run on Android to
# confirm that `cmd appops get` spells them the same way. That is why the check
# FAILS CLOSED on an answer it does not recognise: a wrong name produces a red
# run naming this comment, not a green run that silently checked nothing.
appop_for() { # appop_for <permission>
   case $1 in
   android.permission.BLUETOOTH_SCAN)       printf 'android:bluetooth_scan' ;;
   android.permission.BLUETOOTH_CONNECT)    printf 'android:bluetooth_connect' ;;
   android.permission.POST_NOTIFICATIONS)   printf 'android:post_notification' ;;
   android.permission.ACCESS_FINE_LOCATION) printf 'android:fine_location' ;;
   esac
}
appop_is_allowed() { # appop_is_allowed <permission> <op>
   snap_may_be_empty "$OUT/device-appop.txt" "the app-op state of $1" \
      shell cmd appops get "$PKG" "$2"
   # An answer this check cannot read is a failure, not a pass. `Unknown
   # operation string` is what a wrong name in appop_for produces, and it must
   # not be mistaken for "nothing is set, so the default applies".
   if grep -qiE 'unknown|no such|not a valid|exception' "$OUT/device-appop.txt"; then
      return 1
   fi
   ao_re=$(printf '%s' "$2" | sed 's/\./\\./g')
   ao_mode=$(sed -n "s/^[[:space:]]*$ao_re:[[:space:]]*\([A-Za-z][A-Za-z]*\).*/\1/p" \
      "$OUT/device-appop.txt" | head -1)
   case $ao_mode in
   # allow, and foreground (allowed while the app is visible; this app holds a
   # foreground service, so that is a working state), and default (the op has
   # never been set, which for a granted runtime permission means allowed).
   allow | foreground | default) return 0 ;;
   # Nothing parsed at all. Legitimate only when the device said so in the one
   # way that means "this app has no recorded ops"; anything else is an answer
   # in a shape this check does not understand, and is refused.
   '') if [ ! -s "$OUT/device-appop.txt" ]; then return 0; fi
       if grep -qi 'no operations' "$OUT/device-appop.txt"; then return 0; fi
       return 1 ;;
   *) return 1 ;;
   esac
}

for p in $perms; do
   perm_is_granted "$p" ||
      fail "$p is requested by the manifest and is NOT granted on this device.
     A denied permission appears in the package dump exactly like a granted one
     -- the name is in the requested list either way -- which is why this gate
     used to pass here. This build cannot use the permission."
   op=$(appop_for "$p")
   [ -n "$op" ] ||
      fail "no app-op is mapped for $p in appop_for. Add one: an unmapped
     permission would otherwise have its second switch checked by nobody."
   appop_is_allowed "$p" "$op" ||                                     #R:appop
      fail "$p is granted but its app-op ($op) is not allowed, or 'cmd appops'
     answered in a shape this check does not recognise. See the app-op comment
     above: the op NAMES here have never been confirmed on a real device, so
     this may be the mapping and not the phone. The snapshot is in
     $OUT/device-appop.txt."
done
ok "every runtime permission this API level ($sdk) needs is granted and its app-op allows it"

# ---- LIFECYCLE ------------------------------------------------------------
adb shell input keyevent KEYCODE_HOME || fail "the HOME keyevent was not delivered"
sleep "$SETTLE"
alive_snapshot "the app's process id after backgrounding"
[ -n "$pid" ] || fail "process died when backgrounded"
snap "$OUT/device-start.txt" "the activity resume report" \
   shell am start -W -n "$PKG/$ACT"
grep -q 'Status: ok' "$OUT/device-start.txt" || fail "activity resume failed"
adb shell input keyevent KEYCODE_BACK || fail "the BACK keyevent was not delivered"
sleep "$SETTLE"
snap "$OUT/device-start.txt" "the activity recreation report" \
   shell am start -W -n "$PKG/$ACT"
grep -q 'Status: ok' "$OUT/device-start.txt" || fail "activity recreation failed"
ok "background, resume, destroy, and recreate lifecycle completed"

# ---- THE LOG ---------------------------------------------------------------
#
# Collected first, checked second, and the two verdicts kept apart. See the
# header: this was one pipeline, and "the phone is gone" printed the same
# reassuring line as "the app is fine".
snap "$OUT/device-logcat.txt" "the logcat snapshot" logcat -d -v brief
# grep's exit 2 -- an unreadable file, a broken regex -- is NOT "no match", and
# `if grep ...; then` reads it as one. The same trap the CRLF scan in the
# Makefile fell into, in the one place where reading it as "no match" prints
# the all-clear.
if hits=$(grep -E "FATAL EXCEPTION|UnsatisfiedLinkError|dlopen failed|JNI DETECTED ERROR" \
   "$OUT/device-logcat.txt"); then
   gst=0
else
   gst=$?
fi
if [ "$gst" -gt 1 ]; then
   fail "the fatal-error scan could not run over the logcat snapshot (grep exit $gst)"
fi
if [ "$gst" -eq 0 ]; then
   printf '%s\n' "$hits" >&2
   fail "fatal Java/JNI error found in logcat"
fi
ok "no fatal Java/JNI error was logged"
printf 'devicecheck: Android boundary smoke passed\n'
