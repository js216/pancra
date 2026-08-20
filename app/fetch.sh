#!/bin/sh
# Pull the app's data files off the phone into data/, names unchanged.
#
# WHAT THIS USED TO DO, AND WHAT A GREEN RUN LOOKED LIKE.
#
# Every file was fetched by one line:
#
#     a() { adb exec-out run-as com.jk.pancra cat "files/$1" >"data/$1"; }
#
# The redirect is performed by the SHELL, before adb is even started, so
# data/readings.csv was truncated to nothing the instant that line ran. If adb
# then failed -- the cable moved, the screen locked and run-as was refused, the
# file was not there -- the destination was left at zero bytes, or at however
# many bytes had arrived before the transport died. Nothing checked adb's exit
# status, there was no `set -e`, and the script's own status was that of the
# LAST line. So a run that lost the middle of the list printed nothing, exited
# zero, and left the operator with a data/ directory that looked complete and
# was not: months of glucose history replaced by an empty file, and a shell
# prompt with no error on it.
#
# That is the worst shape a backup tool can have, because the moment you need
# the backup is the moment you find out. So:
#
#   * the phone is asked ONCE what it actually has, and that listing is the
#     evidence for "the file is not there" -- which is a different failure from
#     "adb could not be asked", and is now reported in those words;
#   * every fetch lands in a temporary IN THE DESTINATION DIRECTORY and is
#     renamed over the real name only after adb has exited zero, so a
#     destination file is either the previous copy or the new one, never a
#     prefix of the new one;
#   * the temporaries are removed by a trap on every exit path, including the
#     Ctrl-C in the middle of a slow pull that this is most likely to meet;
#   * the FIRST failure stops the run, so the file you are told about is the
#     file that went wrong rather than the last one in the list.
#
# app/test/adbdrill.sh runs this script against a fake adb and asserts each of
# those four, and mutates the marked lines below to check that the assertions
# can fail. The `#R:` markers are what it matches on; they are load-bearing.
#
# exec-out, not shell -- the PTY rewrites LF and would corrupt stelo.key.
set -eu

PKG=com.jk.pancra
DEST=${1:-data}

# One name per line: add one when the app learns to write a new one.
#
# THE SPLIT IS A CLAIM ABOUT WHEN THE APP FIRST WRITES EACH FILE, and it is the
# only reason "fail on the first missing file" can be a useful rule rather than
# a rule that fires on every phone. REQUIRED is the record the app maintains
# from its first run; a phone that has been running the app and does not have
# these has something wrong with it, and saying so is the point. OPTIONAL
# exists only after a particular thing has happened -- a Dexcom bond (stelo.*),
# a dose logged (insulin.csv), a weight logged (weight.csv), a meter paired
# (meter.*), a calibration (cal.q, rescale.cfg), a server configured
# (remote.cfg), a pairing round (paircode.txt), an alarm edited (alarm.cfg), a
# crash (crash.log) -- and its absence is a fact about the phone, not a fault.
#
# pancra.csv, remote.pos and remote.pull used to be in this list and are gone:
# the first was the single fixed export name the app stopped writing when
# exports became per-snapshot files, and the other two never existed at all.
# They cost nothing while a failed fetch was silent. Under the rule above they
# would have failed every pull on every phone, which is how a list nobody
# checks gets found out.
REQUIRED='
readings.csv
sensors.csv
slots.csv
settings.cfg
'
OPTIONAL='
alarm.cfg
remote.cfg
rescale.cfg
insulin.csv
weight.csv
food.csv
foodtypes.csv
exercise.csv
meter.idx
meter.sync
cal.q
paircode.txt
stelo.info
stelo.key
stelo.mac
session.cache
crash.log
'

die() {
   printf 'fetch: FAILED: %s\n' "$*" >&2
   exit 1
}

# The three ways this can end badly, each said in its own words. They are
# separate functions so that the rule that invokes them is ONE line -- which is
# what lets the drill neuter exactly one rule at a time without rewriting the
# shape of the script around it.
cannot_list() {
   die "adb could not list $PKG's files/ directory. The device is not
     reachable, or run-as refused it (the app must be a debuggable build,
     installed for this user). NOTHING was fetched."
}
not_on_phone() { # not_on_phone <name>
   die "files/$1 is not on the phone.
     It is one of the files the app maintains from its first run, so this is a
     phone that has not run the app, a different package, or a build that no
     longer writes it. Nothing was fetched."
}
fetch_died() { # fetch_died <name>
   die "adb failed while reading files/$1 off the phone.
     $DEST/$1 has NOT been touched -- it still holds whatever it held before,
     rather than the prefix that arrived. Files listed after $1 were not
     fetched."
}

# THE TEMPORARIES ARE CLEANED BY THE TRAP AND BY NOTHING ELSE.
#
# Deliberately: an error path that removes its own temporary and then exits
# leaves the trap with no work to do, and a trap with no work to do is a trap
# whose absence nobody would notice. Every way out of this script -- die, a
# `set -e` abort on a failed mv, Ctrl-C during a slow pull, a SIGTERM from a
# terminal that went away -- arrives here, and the drill asserts that the
# destination directory has no leftovers after a failed fetch.
STAGE=
LIST=
cleanup() {
   [ -z "$STAGE" ] || rm -f "$STAGE"
   [ -z "$LIST" ] || rm -f "$LIST"
   :
}
trap 'cleanup' EXIT                                                     #R:trap
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM

command -v adb >/dev/null 2>&1 || die "adb is required"
mkdir -p "$DEST"

# WHAT THE PHONE HAS, ASKED ONCE AND ASKED SEPARATELY.
#
# `cat` of a file that is not there and an adb that could not reach the device
# both exit 1, and there is no honest way to tell them apart from the status of
# a fetch. A listing can: if this command succeeds, everything after it that
# fails is a transport failure, and everything absent from the listing is
# absent from the phone. That is the difference between "plug the phone back
# in" and "the app has never written that file", and the operator is entitled
# to be told which.
LIST=$(mktemp "$DEST/.fetch.XXXXXX")
# `ls -1`, NOT `ls`. One name per line is not the default and must not be
# assumed: Android's toybox ls lays its output out in COLUMNS, and it did so
# here -- four names to a line -- so the whole-line match below could never
# match anything and every required file was reported missing on a phone that
# had all of them. The failure read as "the app has never run", which sent the
# operator to reinstall an app that was working.
#
# Whole-line matching is still right and is why the flag matters: a name that
# is a PREFIX of another ("remote.pos" against "remote.pull") must not match,
# and a substring test would let it. Fix the input, keep the exact test.
adb exec-out run-as "$PKG" ls -1 files >"$LIST" || cannot_list          #R:list
# Not the same as "ls printed an error": an empty listing from a run-as that
# SUCCEEDED means the app's data directory is empty, and copying nothing over a
# good data/ is exactly the silent loss this script is being fixed for.
[ -s "$LIST" ] || die "$PKG's files/ directory is EMPTY on the phone.
     Nothing was fetched, and nothing in $DEST was touched."

# The listing, one name per line, with the carriage return exec-out does not
# add but a future adb might. Read through a function rather than rewritten in
# place, because rewriting it in place means a pipeline whose adb status is
# lost -- the very defect this file exists to remove.
has() { tr -d '\r' <"$LIST" | grep -qx -- "$1"; }

# THE FIRST MISSING FILE STOPS THE RUN, BEFORE ANYTHING IS WRITTEN.
#
# Checked for all of REQUIRED up front rather than as each fetch comes round: a
# pull that copies four files and then discovers the fifth is not there has
# already replaced part of the destination, and the operator has to work out
# which part. Nothing here writes until every required name is known to exist.
for f in $REQUIRED; do
   has "$f" || not_on_phone "$f"                                     #R:missing
done

fetch_one() { # fetch_one <name>
   # A NEW TEMPORARY EACH TIME, in the destination directory so that the rename
   # is within one filesystem and therefore atomic. mktemp also means two pulls
   # running at once cannot write each other's staging file; they will still
   # race on the final rename, and the loser's copy is then a complete file
   # rather than a mixture, which is the property that matters.
   #
   # mktemp creates the file mode 600 and the rename carries that through, so
   # data/stelo.key is no longer world-readable the way a plain redirect under
   # the default umask left it.
   STAGE=$(mktemp "$DEST/.fetch.XXXXXX")
   adb exec-out run-as "$PKG" cat "files/$1" >"$STAGE" || fetch_died "$1" #R:status
   mv "$STAGE" "$DEST/$1"                                             #R:rename
   STAGE=
   printf '  %s\n' "$1"
}

for f in $REQUIRED; do
   fetch_one "$f"
done

for f in $OPTIONAL; do
   if has "$f"; then
      fetch_one "$f"
   else
      printf '  -- %s is not on the phone yet\n' "$f"
   fi
done

# A FILE THE PHONE HAS AND THIS SCRIPT DOES NOT KNOW ABOUT is a note rather
# than a failure: the day the app learns to write something new, whoever added
# it should be told to add a line here, and whoever is pulling their data off a
# phone should still get their data off the phone.
known=" $(echo $REQUIRED $OPTIONAL) "
for f in $(tr -d '\r' <"$LIST"); do
   case $known in
   *" $f "*) ;;
   *) printf 'fetch: note: the phone has files/%s, which this script does not\n' "$f"
      printf '  know about. Add a line for it if it should be pulled.\n' ;;
   esac
done

printf 'fetch: %s is up to date with the phone\n' "$DEST"
