#!/bin/sh
set -eu

PKG=com.jk.pancra
DEST=${1:-data}
mkdir -p "$DEST"

fetch() {
   tmp=$(mktemp "$DEST/.fetch.XXXXXX")
   if adb exec-out run-as "$PKG" cat "files/$1" >"$tmp" 2>/dev/null; then
      mv "$tmp" "$DEST/$1"
      printf '  %s\n' "$1"
   else
      rm -f "$tmp"
      printf '  -- %s\n' "$1"
   fi
}

for f in \
   readings.csv sensors.csv slots.csv settings.cfg state.gen \
   alarm.cfg remote.cfg rescale.cfg paircode.txt crash.log \
   insulin.csv weight.csv food.csv foodtypes.csv exercise.csv \
   meter.idx meter.sync cal.q stelo.info session.cache \
   stelo.key stelo.key.1 stelo.key.2 stelo.key.3 \
   stelo.key.4 stelo.key.5 stelo.key.6 stelo.key.7 \
   stelo.mac stelo.mac.1 stelo.mac.2 stelo.mac.3 \
   stelo.mac.4 stelo.mac.5 stelo.mac.6 stelo.mac.7; do
   fetch "$f"
done
