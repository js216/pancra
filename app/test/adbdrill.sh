#!/bin/sh
# adbdrill.sh --- run the real phone-facing scripts against a FAKE adb.
#
# THIS TESTS THE SCRIPTS. IT DOES NOT TEST THE PHONE, AND IT CANNOT.
#
# Nothing here connects to a device, installs an APK, or observes Android. What
# it observes is app/fetch.sh and app/test/devicesmoke.sh -- the two programs
# that tell you whether the phone build is healthy -- run unmodified, with a
# fake `adb` first on PATH that can be told to fail at a chosen point, to hand
# back half a file and then die, to report a permission as requested-and-denied,
# or to vanish in the middle of a pipeline.
#
# WHY THAT IS WORTH A GATE OF ITS OWN.
#
# Both scripts had the same defect, and it is the worst one a diagnostic tool
# can have: a tool they depend on failed, and they reported success.
#
#   * fetch.sh redirected adb's output straight over data/readings.csv. The
#     shell truncates the destination before adb starts, so an adb that failed
#     left an EMPTY file -- and because the script's status was that of its last
#     line, a failure in the middle of the list exited zero. A truncated copy of
#     a year of glucose history, and a shell prompt with no error on it.
#   * devicesmoke.sh grepped `dumpsys package` for permission NAMES. Names are
#     in that dump because the app asks for them, granted or denied, so the one
#     check whose job is to catch a build that cannot scan for the sensor was
#     equivalent to grepping the manifest.
#   * devicesmoke.sh ended with `adb logcat -d | grep -E FATAL...`. A pipeline's
#     status is the last command's, so "the phone is not there" and "no fatal
#     message" were the same status, and the same status printed the all-clear.
#
# None of that produces a wrong app. It produces a FALSE ALL-CLEAR about one,
# which is worse, because the run is green and nobody looks again.
#
# HOW TO READ A FAILURE HERE. Every case below arranges one fault in the fake
# adb and asserts what the real script does about it. The last phase MUTATES the
# scripts -- one rule at a time, by the `#R:` markers they carry -- and requires
# a named assertion to fail for each. A mutant that SURVIVES means the assertion
# above it is decorative, which is the state both scripts were already in.
#
# Run by `make adbdrill`. No device, no network, no build.
set -eu

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/test/testlib.sh"

DIR=$(mktemp -d)
T_TMP=$DIR
trap 'rm -rf "$DIR"' EXIT INT TERM

# The scripts under test, overridable so the mutation phase can point this same
# suite at a copy with one rule removed.
FETCH=${ADBDRILL_FETCH:-$HERE/app/fetch.sh}
SMOKE=${ADBDRILL_SMOKE:-$HERE/app/test/devicesmoke.sh}
# One case only, for the mutation phase: a mutant is killed by a NAMED
# assertion, so the child run must be the one case that names the rule.
ONLY=${ADBDRILL_ONLY:-}
want() { [ -z "$ONLY" ] || [ "$ONLY" = "$1" ]; }

FAKE=$DIR/fake
BIN=$DIR/bin
DEST=$DIR/data
SMOKEOUT=$DIR/smokeout
APKFILE=$DIR/pancra.apk
mkdir -p "$BIN"
printf 'not really an APK; devicesmoke only needs it to exist\n' >"$APKFILE"
export FAKEADB=$FAKE
export PATH=$BIN:$PATH

# ---- THE FAKE adb ---------------------------------------------------------
#
# Levers are FILES in $FAKEADB, created and removed by the cases below, so a
# case reads as a list of the faults it arranges. The same shape as the fake
# board in srv/test/deploydrill.sh, where `ssh` is a shell function and `scp`
# is `cp`: the scripts are real, everything under them is arranged.
#
# It deliberately does NOT use `set -e`. A fake whose own bugs abort it mid-
# answer would look to the script above exactly like the device faults it is
# meant to be simulating, and the case would pass for the wrong reason.
cat >"$BIN/adb" <<'FAKEADB_EOF'
#!/bin/sh
D=${FAKEADB:?fake adb: FAKEADB is not set}
printf '%s\n' "$*" >>"$D/calls"
have() { [ -f "$D/$1" ]; }
val() { if [ -f "$D/$1" ]; then cat "$D/$1"; else printf '%s\n' "$2"; fi; }

sub=$1
shift

case $sub in
get-state)
   if have gone; then echo "error: no devices/emulators found" >&2; exit 1; fi
   echo device
   ;;
install)
   if have install-fail; then echo "adb: failed to install" >&2; exit 1; fi
   echo Success
   ;;
logcat)
   case ${1:-} in
   -c)
      if have logcat-clear-fail; then echo "failed to clear" >&2; exit 1; fi
      exit 0
      ;;
   *)
      # THE PARTIAL DUMP IS THE INTERESTING ONE: lines arrive, none of them
      # fatal, and then the transport dies. A checker that looks only at what
      # arrived sees a perfectly clean log.
      if have logcat-fail-partial; then
         if [ -f "$D/logcat" ]; then cat "$D/logcat"; fi
         echo "adb: device offline" >&2
         exit 1
      fi
      if have logcat-fail; then echo "adb: device offline" >&2; exit 1; fi
      if [ -f "$D/logcat" ]; then cat "$D/logcat"; fi
      exit 0
      ;;
   esac
   ;;
exec-out)
   # exec-out run-as <pkg> ls files
   # exec-out run-as <pkg> cat files/<name>
   if have runas-fail; then
      echo "run-as: package not debuggable" >&2
      exit 1
   fi
   case ${3:-} in
   ls)
      if have list-fail; then echo "adb: device offline" >&2; exit 1; fi
      if [ -f "$D/listing" ]; then cat "$D/listing"; fi
      exit 0
      ;;
   cat)
      name=${4#files/}
      if [ -f "$D/fail-at" ] && [ "$(cat "$D/fail-at")" = "$name" ]; then
         # Bytes first when asked: the case where the shell's redirect has
         # already truncated the destination AND something landed in it.
         if have partial; then printf 'PARTIAL-BYTES-THAT-MUST-NOT-LAND\n'; fi
         echo "adb: device offline" >&2
         exit 1
      fi
      if [ -f "$D/files/$name" ]; then cat "$D/files/$name"; exit 0; fi
      echo "cat: files/$name: No such file or directory" >&2
      exit 1
      ;;
   esac
   echo "fake adb: unhandled exec-out: $*" >&2
   exit 99
   ;;
shell)
   rest="$*"
   case $rest in
   'getprop ro.product.cpu.abi') val abi arm64-v8a ;;
   'getprop ro.build.version.sdk') val sdk 34 ;;
   pidof*)
      if have no-process; then rc=1; else val pid 4242; rc=0; fi
      # The status marker, when the caller asked for one. `sh -c 'pidof x;
      # echo rc=$?'` exits with echo's status, so the transport succeeding and
      # the process being absent are visibly different things.
      case $rest in *'echo rc='*) echo "rc=$rc" ;; esac
      ;;
   'am force-stop'*) : ;;
   'am start'*)
      if have start-fail; then echo "Error: Activity not started" >&2; exit 1; fi
      printf 'Starting: Intent { cmp=com.jk.pancra/android.app.NativeActivity }\n'
      printf 'Status: ok\nActivity: com.jk.pancra/android.app.NativeActivity\n'
      printf 'TotalTime: 210\n'
      ;;
   'dumpsys activity services'*)
      printf 'ACTIVITY MANAGER SERVICES\n'
      printf '  ServiceRecord{a1 u0 com.jk.pancra/.PancraService}\n'
      printf '    isForeground=true\n'
      ;;
   'dumpsys notification'*)
      printf 'NOTIFICATION MANAGER (dumpsys notification)\n'
      printf '  NotificationRecord(pkg=com.jk.pancra id=1)\n'
      ;;
   'dumpsys package'*)
      # THE REQUESTED LIST IS ALWAYS COMPLETE. That is the whole point of the
      # permission cases: a name is in this block whether the user granted it,
      # denied it, or was never asked, so a check that greps for the name is
      # satisfied by a build nobody has granted anything to.
      out() {
         printf 'Packages:\n'
         printf '  Package [com.jk.pancra] (a1b2c3):\n'
         printf '    userId=10234\n'
         printf '    requested permissions:\n'
         printf '      android.permission.BLUETOOTH_SCAN\n'
         printf '      android.permission.BLUETOOTH_CONNECT\n'
         printf '      android.permission.ACCESS_FINE_LOCATION\n'
         printf '      android.permission.POST_NOTIFICATIONS\n'
         printf '      android.permission.FOREGROUND_SERVICE\n'
         printf '    install permissions:\n'
         printf '      android.permission.FOREGROUND_SERVICE: granted=true\n'
         printf '    User 0: ceDataInode=54321 installed=true hidden=false\n'
         printf '      gids=[3003]\n'
         printf '      runtime permissions:\n'
         if [ -f "$D/perms" ]; then
            while read -r p g; do
               [ -n "$p" ] || continue
               printf '        %s: granted=%s, flags=[ USER_SET]\n' "$p" "$g"
            done <"$D/perms"
         fi
         # A SECOND USER, when the case asks for one. A device with a work
         # profile or a second account prints one runtime block per user, and
         # `granted=true` somewhere in the dump is not the same claim as
         # "granted for the user this is about".
         if [ -f "$D/perms-user10" ]; then
            printf '    User 10: ceDataInode=99999 installed=true hidden=false\n'
            printf '      runtime permissions:\n'
            while read -r p g; do
               [ -n "$p" ] || continue
               printf '        %s: granted=%s, flags=[ USER_SET]\n' "$p" "$g"
            done <"$D/perms-user10"
         fi
      }
      if have dumpsys-partial; then
         # Half a dump, cut where a USB reset would cut it: after the requested
         # list and before any grant flag. Every permission NAME is in it.
         out | head -9
         echo "adb: device offline" >&2
         exit 1
      fi
      out
      ;;
   'pm grant'*)
      # pm grant <pkg> <perm>
      if have grant-fail; then
         echo "Operation not allowed: java.lang.SecurityException" >&2
         exit 1
      fi
      perm=$4
      if ! grep -q "^$perm " "$D/perms" 2>/dev/null; then
         echo "Unknown permission: $perm" >&2
         exit 1
      fi
      # THE GRANT THAT IS ACCEPTED AND DOES NOT TAKE. A hard-restricted
      # permission, or a policy that re-denies: `pm grant` exits zero and the
      # state does not move. Nothing but the grant FLAG can see it.
      if have grant-noop; then exit 0; fi
      awk -v p="$perm" '{ if ($1 == p) print p, "true"; else print }' \
         "$D/perms" >"$D/perms.new" && mv "$D/perms.new" "$D/perms"
      ;;
   'cmd appops get'*)
      # cmd appops get <pkg> <op>
      if have appops-fail; then echo "adb: device offline" >&2; exit 1; fi
      if [ -f "$D/appops-raw" ]; then cat "$D/appops-raw"; exit 0; fi
      op=$5
      if [ -f "$D/appop.$op" ]; then
         printf '  %s: %s; time=+1h2m3s ago\n' "$op" "$(cat "$D/appop.$op")"
      fi
      # Nothing printed otherwise: an app whose ops have never been touched.
      exit 0
      ;;
   'input keyevent'*)
      if have keyevent-fail; then echo "adb: device offline" >&2; exit 1; fi
      ;;
   *)
      echo "fake adb: unhandled shell command: $rest" >&2
      exit 99
      ;;
   esac
   ;;
*)
   echo "fake adb: unhandled subcommand: $sub $*" >&2
   exit 99
   ;;
esac
FAKEADB_EOF
chmod +x "$BIN/adb"

# ---- THE OLD RULES, KEPT AS AN EXECUTABLE FIXTURE -------------------------
#
# Not the old files -- the old RULES, spelled exactly as they were, so that
# every case below can be asked the question that matters: is this fault one
# the old rule would have MISSED? A case the old rule already failed proves
# nothing about the fix, and three of the cases here are of that kind. The
# ones the old rule passes are marked ISOLATING in their output.
#
# Reading them from git was the obvious alternative and is a trap: the moment
# this work is committed, HEAD is the fixed version and the fixture silently
# becomes a copy of the thing it is supposed to contrast with.
cat >"$DIR/oldfetch.sh" <<'OLDFETCH_EOF'
#!/bin/sh
# app/fetch.sh as it was: a redirect the shell performs before adb runs, no
# `set -e`, no status check, and a script status that is the last line's.
D=$1
mkdir -p "$D"
a() { adb exec-out run-as com.jk.pancra cat "files/$1" >"$D/$1"; }
a readings.csv
a sensors.csv
a slots.csv
a settings.cfg
OLDFETCH_EOF
cat >"$DIR/oldsmoke.sh" <<'OLDSMOKE_EOF'
#!/bin/sh
# devicesmoke.sh's two old rules: grep the package dump for permission NAMES,
# and pipe logcat into grep.
set -eu
PKG=com.jk.pancra
O=$1
adb shell dumpsys package "$PKG" >"$O/old-package.txt"
for permission in android.permission.BLUETOOTH_SCAN \
                  android.permission.BLUETOOTH_CONNECT \
                  android.permission.POST_NOTIFICATIONS; do
  grep -q "$permission" "$O/old-package.txt" || {
    echo "old rule: $permission is absent from package state" >&2; exit 1; }
done
echo "old rule ok: BLE and notification permissions reached Android package state"
if adb logcat -d -v brief |
   grep -E "FATAL EXCEPTION|UnsatisfiedLinkError|dlopen failed|JNI DETECTED ERROR"; then
  echo "old rule: fatal Java/JNI error found in logcat" >&2; exit 1
fi
echo "old rule ok: no fatal Java/JNI error was logged"
OLDSMOKE_EOF
chmod +x "$DIR/oldfetch.sh" "$DIR/oldsmoke.sh"

# ---- the fixture ----------------------------------------------------------
#
# The phone's own files. Distinctive contents, because "the destination was not
# touched" has to be checkable byte for byte: a check for existence passes over
# the empty file the old redirect left behind.
PHONEFILES='readings.csv sensors.csv slots.csv settings.cfg alarm.cfg stelo.key'

reset() {
   rm -rf "$FAKE" "$DEST" "$SMOKEOUT"
   mkdir -p "$FAKE/files" "$DEST" "$SMOKEOUT"
   for f in $PHONEFILES; do
      printf 'phone bytes of %s\n' "$f" >"$FAKE/files/$f"
   done
   (cd "$FAKE/files" && ls -1) >"$FAKE/listing"
   # Denied, as a fresh install is: nothing has been asked and nothing granted.
   # `pm grant` in the script under test is what moves these.
   {
      printf 'android.permission.BLUETOOTH_SCAN false\n'
      printf 'android.permission.BLUETOOTH_CONNECT false\n'
      printf 'android.permission.POST_NOTIFICATIONS false\n'
   } >"$FAKE/perms"
   printf 'I/ActivityManager( 900): Displayed com.jk.pancra/.NativeActivity\n' \
      >"$FAKE/logcat"
}
lever() { : >"$FAKE/$1"; }              # arrange a fault
levers() { printf '%s\n' "$2" >"$FAKE/$1"; }   # ...one that carries a value

# The destination, seeded with a previous good pull, so that "left truncated"
# and "left alone" are different observable states.
seed_dest() {
   for f in $PHONEFILES; do
      printf 'PREVIOUS PULL of %s\n' "$f" >"$DEST/$f"
   done
}
unchanged() { # unchanged <name> -- still the previous pull, byte for byte
   printf 'PREVIOUS PULL of %s\n' "$1" | cmp -s - "$DEST/$1"
}
fetched() { # fetched <name> -- now the phone's bytes, byte for byte
   printf 'phone bytes of %s\n' "$1" | cmp -s - "$DEST/$1"
}
# `ls` does not show dotfiles and the staging names begin with one, so the
# glob is what counts them. A number, so the failure message can say how many.
leftovers() {
   lo_n=0
   for lo_p in "$DEST"/.fetch.*; do
      if [ -e "$lo_p" ]; then lo_n=$((lo_n + 1)); fi
   done
   printf '%s' "$lo_n"
}

run_fetch() { "$FETCH" "$DEST" >"$DIR/out.txt" 2>&1; }
run_smoke() {
   PANCRA_SMOKE_OUT=$SMOKEOUT PANCRA_SMOKE_SETTLE=0 \
      "$SMOKE" "$APKFILE" >"$DIR/out.txt" 2>&1
}
says() { grep -q "$1" "$DIR/out.txt"; }
out1() { t_show "$(cat "$DIR/out.txt")"; }

# THE OLD RULE, ASKED THE SAME QUESTION. Prints whether the fault now arranged
# in $FAKE is one the old code let through -- which is what makes a case
# ISOLATING rather than merely red.
old_fetch_passes() {
   rm -rf "$DIR/olddata"
   mkdir -p "$DIR/olddata"
   sh "$DIR/oldfetch.sh" "$DIR/olddata" >"$DIR/oldout.txt" 2>&1
}
old_smoke_passes() { sh "$DIR/oldsmoke.sh" "$SMOKEOUT" >"$DIR/oldout.txt" 2>&1; }
isolating() { # isolating <what the old rule did with this input>
   if $1; then
      t_ok "  ...and this case is ISOLATING: the old rule PASSED on this input"
   else
      t_bad "  ...but the old rule already failed on this input, so this case\
 pins nothing: $(t_show "$(cat "$DIR/oldout.txt")")"
   fi
}

# ==========================================================================
# ITEM 87 -- app/fetch.sh
# ==========================================================================

if want fetch_happy; then
echo "== a pull with everything present succeeds and lands the phone's bytes =="
reset
seed_dest
if run_fetch; then
   t_ok "a pull of a healthy phone succeeds"
else
   t_bad "a healthy pull failed: $(out1)"
fi
bad=
for f in $PHONEFILES; do fetched "$f" || bad="$bad $f"; done
if [ -z "$bad" ]; then
   t_ok "...and every file on the phone is now in the destination, byte for byte"
else
   t_bad "these did not arrive intact:$bad"
fi
if says 'is not on the phone yet'; then
   t_ok "...and the files this phone has never written are named, not fetched"
else
   t_bad "an optional file that is absent was not reported: $(out1)"
fi
if [ "$(leftovers)" = 0 ]; then
   t_ok "...and no staging file is left in the destination directory"
else
   t_bad "$(leftovers) staging file(s) left behind after a SUCCESSFUL pull"
fi
fi

if want fetch_midfail; then
echo "== THE CASE ITEM 87 EXISTS FOR: adb fails in the MIDDLE of the list =="
# sensors.csv is the second of four required files, so there is a successful
# fetch before it and two more after. The old script's exit status was the last
# line's, which is why the failure had to be in the middle to be invisible.
reset
seed_dest
levers fail-at sensors.csv
if run_fetch; then
   t_bad "a pull whose adb FAILED reported success -- the whole item"
else
   t_ok "a pull whose adb failed is reported as a failure"
fi
if says 'sensors.csv'; then
   t_ok "...naming the file it failed on"
else
   t_bad "...without naming the file: $(out1)"
fi
if unchanged sensors.csv; then
   t_ok "...AND THE DESTINATION FILE IS UNTOUCHED, not truncated to nothing"
else
   t_bad "sensors.csv was left as '$(t_show "$(cat "$DEST/sensors.csv")")'"
fi
if fetched readings.csv; then
   t_ok "...while the file fetched BEFORE the failure did land (the case is live)"
else
   t_bad "readings.csv never arrived, so this case never reached the failure"
fi
if unchanged slots.csv && unchanged settings.cfg; then
   t_ok "...and the run stopped there: nothing after sensors.csv was fetched"
else
   t_bad "the pull carried on past its first failure"
fi
if [ "$(leftovers)" = 0 ]; then
   t_ok "...and the trap removed the staging file on the way out"
else
   t_bad "$(leftovers) staging file(s) left in $DEST after the failure"
fi
isolating old_fetch_passes
fi

if want fetch_partial; then
echo "== adb writes half a file and then dies =="
# The nastier half of the same defect: the destination is not merely emptied,
# it is left holding a PREFIX of the new file -- which parses, loads, and is
# missing the end.
reset
seed_dest
levers fail-at sensors.csv
lever partial
if run_fetch; then
   t_bad "a partial transfer reported success"
else
   t_ok "a transfer that died part way is reported as a failure"
fi
if unchanged sensors.csv; then
   t_ok "...AND THE PARTIAL BYTES DID NOT LAND: the previous copy is intact"
else
   t_bad "sensors.csv now holds '$(t_show "$(cat "$DEST/sensors.csv")")'"
fi
if [ "$(leftovers)" = 0 ]; then
   t_ok "...and the half-written staging file was removed"
else
   t_bad "$(leftovers) staging file(s) left holding the partial transfer"
fi
isolating old_fetch_passes
fi

if want fetch_missing; then
echo "== a file the app maintains is NOT on the phone =="
reset
seed_dest
rm -f "$FAKE/files/readings.csv"
(cd "$FAKE/files" && ls -1) >"$FAKE/listing"
if run_fetch; then
   t_bad "a pull missing a required file reported success"
else
   t_ok "a required file that is not on the phone is a failure"
fi
if says 'is not on the phone'; then
   t_ok "...said in those words, and not as a transport error"
else
   t_bad "...but reported as something else: $(out1)"
fi
if unchanged sensors.csv && unchanged slots.csv && unchanged settings.cfg; then
   t_ok "...and NOTHING was written: the check runs before the first fetch"
else
   t_bad "it had already replaced part of the destination before refusing"
fi
isolating old_fetch_passes
fi

if want fetch_nolist; then
echo "== the phone cannot be asked what it has =="
reset
seed_dest
lever list-fail
if run_fetch; then
   t_bad "a pull that could not reach the device reported success"
else
   t_ok "a device that cannot be listed is a failure"
fi
if says 'could not list'; then
   t_ok "...reported as a transport failure, not as a missing file"
else
   t_bad "...but blamed the wrong thing: $(out1)"
fi
if [ "$(leftovers)" = 0 ]; then
   t_ok "...and the listing temporary was cleaned up"
else
   t_bad "$(leftovers) temporary file(s) left behind"
fi
fi

if want fetch_emptylist; then
echo "== run-as succeeds and the data directory is empty =="
reset
seed_dest
: >"$FAKE/listing"
if run_fetch; then
   t_bad "a pull from an empty data directory reported success"
else
   t_ok "an empty files/ directory on the phone is a failure"
fi
if says 'EMPTY'; then
   t_ok "...and says the phone had nothing, rather than copying nothing over"
else
   t_bad "...without saying so: $(out1)"
fi
if unchanged readings.csv; then
   t_ok "...and the previous pull is still there"
else
   t_bad "it overwrote a good destination with nothing"
fi
fi

# ==========================================================================
# ITEM 88 -- the permissions devicesmoke.sh reports
# ==========================================================================

if want smoke_happy; then
echo "== a healthy phone passes the smoke test =="
reset
if run_smoke; then
   t_ok "the smoke test passes against a device with nothing wrong with it"
else
   t_bad "the healthy case FAILED, so nothing below it means anything: $(out1)"
fi
if says 'Android boundary smoke passed'; then
   t_ok "...and says so"
else
   t_bad "...without its verdict line: $(out1)"
fi
fi

if want smoke_perm_denied; then
echo "== THE CASE ITEM 88 EXISTS FOR: requested, and NOT granted =="
# `pm grant` is accepted and the state does not move -- a hard-restricted
# permission, or a policy that re-denies. The permission NAME is in the
# requested block of the dump either way, which is all the old rule ever asked.
reset
lever grant-noop
if run_smoke; then
   t_bad "a build whose BLE permissions are DENIED reported a clean smoke run"
else
   t_ok "a permission that is requested and not granted is a failure"
fi
if says 'BLUETOOTH_SCAN' && says 'NOT granted'; then
   t_ok "...naming the permission, and saying it is not granted"
else
   t_bad "...without naming what is denied: $(out1)"
fi
if says 'Android boundary smoke passed'; then
   t_bad "...and it printed the all-clear anyway"
else
   t_ok "...and no all-clear was printed"
fi
isolating old_smoke_passes
fi

if want smoke_perm_multiuser; then
echo "== two users, and they disagree about one permission =="
# A work profile, or a second account. The dump holds one runtime block per
# user, so `granted=true` IS present -- for somebody else. A check that stops
# at the first encouraging line reports a permission this user does not have.
reset
printf 'android.permission.BLUETOOTH_SCAN false\n' >"$FAKE/perms-user10"
if run_smoke; then
   t_bad "a permission denied for one user was accepted because another has it"
else
   t_ok "a permission granted for one user and denied for another FAILS CLOSED"
fi
if says 'BLUETOOTH_SCAN' && says 'NOT granted'; then
   t_ok "...naming the permission that is denied"
else
   t_bad "...without naming it: $(out1)"
fi
fi

if want smoke_grant_fail; then
echo "== the grant itself is refused =="
reset
lever grant-fail
if run_smoke; then
   t_bad "a refused pm grant reported a clean smoke run"
else
   t_ok "a permission the platform will not grant is a failure"
fi
if says 'pm grant'; then
   t_ok "...and says the grant was refused, not that the flag was missing"
else
   t_bad "...but reported it as something else: $(out1)"
fi
isolating old_smoke_passes
fi

if want smoke_appop_ignored; then
echo "== granted, and the app-op says no =="
# The second switch, in series with the grant: granted=true and the op ignored
# is what a restricted permission or a lapsed one-time grant leaves behind, and
# the app then gets an empty scan result rather than an exception -- silence,
# which looks exactly like a sensor out of range.
reset
levers appop.android:bluetooth_scan ignore
if run_smoke; then
   t_bad "a permission whose app-op is ignored reported a clean smoke run"
else
   t_ok "a granted permission with its app-op ignored is a failure"
fi
if says 'app-op'; then
   t_ok "...and names the app-op rather than the grant"
else
   t_bad "...without naming the app-op: $(out1)"
fi
fi

if want smoke_appop_unknown; then
echo "== the app-op answer is in a shape the check does not recognise =="
# The honest half of the app-op check. The op NAMES in devicesmoke.sh have
# never been confirmed against a real Android build; if one of them is wrong,
# `cmd appops` says so, and the rule must FAIL rather than read an unparsable
# answer as "nothing is set, so the default applies".
reset
printf 'Unknown operation string: android:bluetooth_scan\n' >"$FAKE/appops-raw"
if run_smoke; then
   t_bad "an app-op answer the check could not read was treated as allowed"
else
   t_ok "an unrecognised app-op answer FAILS CLOSED"
fi
fi

if want smoke_sdk30; then
echo "== an older device is asked for the permissions it actually has =="
# minSdkVersion is 29. BLUETOOTH_SCAN and BLUETOOTH_CONNECT exist from API 31
# and POST_NOTIFICATIONS from 33; on anything older BLE scanning is gated on
# ACCESS_FINE_LOCATION. Requiring the modern three everywhere would fail every
# Android 10, 11 and 12 device -- and requiring only their PRESENCE, which is
# what the old rule did, passed everywhere and meant nothing.
reset
levers sdk 30
{
   printf 'android.permission.ACCESS_FINE_LOCATION false\n'
   printf 'android.permission.POST_NOTIFICATIONS false\n'
} >"$FAKE/perms"
if run_smoke; then
   t_ok "an API 30 device passes on ACCESS_FINE_LOCATION"
else
   t_bad "an API 30 device was failed: $(out1)"
fi
if says 'API level (30)'; then
   t_ok "...and the report says which API level's set it checked"
else
   t_bad "...without saying which set it used: $(out1)"
fi
fi

# ==========================================================================
# ITEM 89 -- the logcat devicesmoke.sh reports on
# ==========================================================================

if want smoke_logcat_unreachable; then
echo "== THE CASE ITEM 89 EXISTS FOR: logcat cannot be collected =="
# The dump starts, ordinary lines arrive, and the transport dies. What arrived
# contains no fatal message -- because it contains almost nothing -- so a grep
# on the far end of a pipeline reports a perfectly clean log.
reset
lever logcat-fail-partial
if run_smoke; then
   t_bad "a logcat that could NOT be collected was reported as clean"
else
   t_ok "a logcat that could not be collected is a failure"
fi
if says 'could not be collected'; then
   t_ok "...and says the collection failed"
else
   t_bad "...but blamed something else: $(out1)"
fi
if says 'no fatal Java/JNI error was logged'; then
   t_bad "...and printed the all-clear about a log it never got"
else
   t_ok "...and does NOT print the all-clear about a log it never got"
fi
isolating old_smoke_passes
fi

if want smoke_logcat_fatal; then
echo "== a logcat that WAS collected and holds a fatal error =="
# The other verdict, kept apart from the one above: this is the message that
# should send a reader to the app, and the one above is the message that should
# send them to the cable.
reset
printf 'E/AndroidRuntime( 900): FATAL EXCEPTION: main\n' >>"$FAKE/logcat"
if run_smoke; then
   t_bad "a fatal Java exception in the log was reported as clean"
else
   t_ok "a fatal Java/JNI error in a collected log is a failure"
fi
if says 'fatal Java/JNI error found'; then
   t_ok "...reported as a fatal error in the app"
else
   t_bad "...but not as a fatal error: $(out1)"
fi
if says 'could not be collected'; then
   t_bad "...and confused it with a collection failure"
else
   t_ok "...and NOT confused with a collection failure"
fi
fi

if want smoke_logcat_empty; then
echo "== logcat succeeds and returns nothing at all =="
# The buffer was cleared and an app was started, so an empty snapshot is a
# statement about the collection, not about the app. An empty file satisfies
# every "no fatal message" check ever written.
reset
rm -f "$FAKE/logcat"
if run_smoke; then
   t_bad "an EMPTY logcat snapshot was reported as a clean log"
else
   t_ok "an empty logcat snapshot is a failure"
fi
if says 'came back EMPTY'; then
   t_ok "...and says the snapshot was empty, not that the log was clean"
else
   t_bad "...but said something else: $(out1)"
fi
fi

if want smoke_dumpsys_partial; then
echo "== a package dump that arrives half way =="
# The same shape one step earlier: half of `dumpsys package` still contains
# every permission name, so a truncated dump answers the permission questions
# just as confidently as a whole one.
reset
lever dumpsys-partial
if run_smoke; then
   t_bad "a truncated package dump was accepted"
else
   t_ok "a package dump that did not finish is a failure"
fi
if says 'could not be collected'; then
   t_ok "...reported as a collection failure"
else
   t_bad "...but reported as something else: $(out1)"
fi
if [ -f "$SMOKEOUT/device-package.txt" ]; then
   t_bad "...and the half-written dump was left behind for a human to read"
else
   t_ok "...and the half-written dump was NOT left where a human would read it"
fi
if ls "$SMOKEOUT"/*.part >/dev/null 2>&1; then
   t_bad "a .part file was left in $SMOKEOUT"
else
   t_ok "...and the trap removed the staging file"
fi
fi

# ==========================================================================
# THE MUTATION PHASE
# ==========================================================================
#
# Each rule above is removed from the script, one at a time, and the case that
# names it must FAIL. A rule nobody has watched fail is a rule that may not be
# doing anything -- which is the state both of these scripts were in before
# this work, with green runs to prove it.
#
# TWICE, and both runs must fail. Two agents on this tree have accepted a kill
# that turned out to be timing; nothing in this drill is timing-dependent, and
# running each mutant twice is what makes that a measured claim rather than an
# assumption.
if [ -z "$ONLY" ]; then
echo "== every rule, removed, must break the case that names it =="
MUT=$DIR/mut
mkdir -p "$MUT"
mutant() { # mutant <label> <fetch|smoke> <case> <sed program...>
   mu_label=$1
   mu_which=$2
   mu_case=$3
   shift 3
   case $mu_which in
   fetch) mu_src=$FETCH; mu_dst=$MUT/fetch.sh ;;
   smoke) mu_src=$SMOKE; mu_dst=$MUT/devicesmoke.sh ;;
   esac
   sed "$@" "$mu_src" >"$mu_dst"
   chmod +x "$mu_dst"
   # A MUTATION THAT DID NOT APPLY is a mutant that was never made, and the
   # case would "kill" it by passing. This has happened here: a marker was
   # renamed and four mutants quietly became copies of the original.
   if cmp -s "$mu_src" "$mu_dst"; then
      t_bad "MUTANT NOT APPLIED: $mu_label -- the sed matched nothing"
      return
   fi
   if ! sh -n "$mu_dst" 2>"$MUT/syntax"; then
      t_bad "MUTANT DOES NOT PARSE: $mu_label -- $(t_show "$(cat "$MUT/syntax")")"
      return
   fi
   mu_run=1
   mu_survived=0
   while [ "$mu_run" -le 2 ]; do
      if [ "$mu_which" = fetch ]; then
         mu_f=$mu_dst; mu_s=$SMOKE
      else
         mu_f=$FETCH; mu_s=$mu_dst
      fi
      if ADBDRILL_FETCH=$mu_f ADBDRILL_SMOKE=$mu_s ADBDRILL_ONLY=$mu_case \
            sh "$HERE/app/test/adbdrill.sh" >"$MUT/out.$mu_run" 2>&1; then
         mu_survived=1
      fi
      mu_run=$((mu_run + 1))
   done
   if [ "$mu_survived" = 1 ]; then
      t_bad "MUTANT SURVIVED: $mu_label -- $mu_case still passed without it"
      return
   fi
   # KILLED BY A NAMED ASSERTION, not by the script falling over. A mutant that
   # crashes the drill is a mutant nobody has learned anything from: the case
   # did not notice the missing rule, the harness merely died.
   if grep -q '^  FAIL ' "$MUT/out.1"; then
      t_ok "killed: $mu_label -- \
$(grep '^  FAIL ' "$MUT/out.1" | head -1 | cut -c8-64)"
   else
      t_bad "killed by a CRASH, not a named assertion: $mu_label --\
 $(t_show "$(cat "$MUT/out.1")")"
   fi
}

# --- item 87
mutant "fetch: the staging file and the atomic rename" fetch fetch_partial \
   -e 's@>"\$STAGE"@>"$DEST/$1"@' \
   -e 's@^   mv "\$STAGE".*#R:rename@   :@'
mutant "fetch: adb's exit status on each file" fetch fetch_midfail \
   -e 's@ || fetch_died "\$1"@@'
mutant "fetch: the trap that removes temporaries" fetch fetch_midfail \
   -e "s@^trap 'cleanup' EXIT.*#R:trap@trap ':' EXIT@"
mutant "fetch: the missing-file pre-flight" fetch fetch_missing \
   -e 's@^   has "\$f" .*#R:missing@   :@'
mutant "fetch: the status of the listing itself" fetch fetch_nolist \
   -e 's@ || cannot_list.*#R:list@@'
mutant "fetch: the empty-listing rule" fetch fetch_emptylist \
   -e 's@-s "\$LIST"@-n x@'

# --- item 88
mutant "smoke: the runtime grant FLAG (the name is not the grant)" \
   smoke smoke_perm_denied \
   -e 's@^      return 1.*#R:grantedno@      :@' \
   -e 's@^   grep -qE .*#R:granted$@   grep -q "$1" "$OUT/device-package.txt"@'
mutant "smoke: the explicit pm grant" smoke smoke_grant_fail \
   -e 's@^   adb shell pm grant.*#R:grant@   :@'
mutant "smoke: the app-op check" smoke smoke_appop_ignored \
   -e 's@^   appop_is_allowed .*#R:appop@   true ||@'
mutant "smoke: the permission set chosen by API level" smoke smoke_sdk30 \
   -e 's@"\$sdk" -ge 31@true@' \
   -e 's@"\$sdk" -ge 33@true@'
mutant "smoke: granted=false winning over granted=true" \
   smoke smoke_perm_multiuser \
   -e 's@^      return 1.*#R:grantedno@      :@'

# --- item 89
mutant "smoke: adb's exit status on a snapshot" \
   smoke smoke_logcat_unreachable \
   -e 's@ || snap_died "\$sn_what" "\$?"@@'
mutant "smoke: the empty-snapshot rule" smoke smoke_logcat_empty \
   -e 's@^      \[ -s .*#R:empty@      :@'
mutant "smoke: the .part-then-rename for snapshots" smoke smoke_dumpsys_partial \
   -e 's@\$sn_dst\.part@$sn_dst@g' \
   -e 's@^   mv .*#R:atomic@   :@'
mutant "smoke: the fatal-error scan itself" smoke smoke_logcat_fatal \
   -e 's@"FATAL EXCEPTION|@"ZZZ_NEVER_MATCHES|@'
fi

t_end
if [ "$fail" = 0 ]; then
   printf '\033[1;32madbdrill\033[0m: fetch and device smoke FAIL when adb does\n'
else
   printf 'adbdrill: FAILED\n'
fi
exit $fail
