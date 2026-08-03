#!/bin/sh
# Pull the app's data files off the phone into data/, names unchanged.
# One line per file: add one when the app learns to write a new one.
# exec-out, not shell -- the PTY rewrites LF and would corrupt stelo.key.
mkdir -p data
a() { adb exec-out run-as com.jk.pancra cat "files/$1" >"data/$1"; }

a readings.csv
a sensors.csv
a slots.csv
a insulin.csv
a weight.csv
a pancra.csv
a alarm.cfg
a settings.cfg
a remote.cfg
a remote.pos
a remote.pull
a rescale.cfg
a meter.idx
a meter.sync
a cal.q
a paircode.txt
a stelo.info
a stelo.key
a stelo.mac
