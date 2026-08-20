#!/bin/sh
# clitest.sh --- the CLI's help is the CLI
#
# WHAT THIS PINS.
#
# The verbs and the usage text used to be two lists in one file: an if-chain
# of strcmp, and a block of printf lines inside the "that is not a subcommand"
# branch. They disagreed. `logout` was implemented and unlisted; several
# verbs' optional [datadir] was undocumented; and `sync --help` -- the first
# thing anybody types -- fell through to the error branch, printed to stderr
# and exited 2.
#
# There is one table now (struct cli_cmd in srv/sync.c) and the help is
# generated from it. This checks that the table, the DISPATCH and the README
# still describe the same program:
#
#   1. help is an ANSWER: `help`, `--help` and `-h` all print to stdout and
#      exit 0.
#   2. EVERY VERB THE DISPATCH KNOWS is in the help. The dispatch's verbs are
#      read out of the source -- every strcmp(argv[1], "...") -- so a verb
#      added without a table row fails here.
#   3. EVERY VERB THE HELP LISTS is really dispatched: running it with no
#      arguments answers with ITS OWN usage line, not "not a subcommand".
#   4. THE README lists them too, so the page somebody reads first is not a
#      third, older list.
#   5. An unknown verb is still an ERROR (exit 2, stderr) and does NOT start a
#      server -- the defect that had `sync status` binding an ephemeral port
#      and sitting there as a daemon nobody asked for.
#
# Run by `make clitest`.
set -eu

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/test/testlib.sh"

BIN=${1:-$HERE/build/srv/sync}
[ -x "$BIN" ] || { echo "clitest: $BIN is not built"; exit 1; }

DIR=$(mktemp -d)
T_TMP=$DIR
trap 'rm -rf "$DIR"' EXIT INT TERM

# ---- 1: help is an answer ------------------------------------------------
for flag in help --help -h; do
  if "$BIN" "$flag" >"$DIR/help.txt" 2>"$DIR/err.txt"; then
    t_ok "'sync $flag' succeeds"
  else
    t_bad "'sync $flag' exited $? -- help is not an error"
  fi
  if [ -s "$DIR/help.txt" ]; then
    t_ok "...and prints to STDOUT, where a pipe can read it"
  else
    t_bad "'sync $flag' printed nothing to stdout"
  fi
done
HELP=$(cat "$DIR/help.txt")

# ---- 2: every dispatched verb is documented ------------------------------
# The dispatch, read from the source: this is the list the program acts on,
# whatever the help says.
VERBS=$(grep -o 'strcmp(argv\[1\], "[a-z-]*")' "$HERE/srv/sync.c" |
        sed 's/.*"\(.*\)".*/\1/' | grep -v '^-' | sort -u)
n=0
for v in $VERBS; do
  n=$((n + 1))
  case $v in
    help) continue ;; # listed as itself
  esac
  if printf '%s' "$HELP" | grep -q "sync $v "; then
    t_ok "the help documents '$v'"
  else
    t_bad "'$v' is dispatched but absent from the help"
  fi
done
if [ "$n" -ge 8 ]; then
  t_ok "...and the dispatch really has verbs in it ($n found)"
else
  t_bad "only $n verbs found in the source -- this test read the wrong thing"
fi

# ---- 3: every documented verb is dispatched ------------------------------
# A verb with no arguments either does its job or answers with its own usage.
# What it must NOT say is that it is not a subcommand.
LISTED=$(printf '%s' "$HELP" | sed -n 's/^ *sync \([a-z][a-z]*\) .*/\1/p' |
         sort -u)
for v in $LISTED; do
  out=$("$BIN" "$v" 2>&1 </dev/null || true)
  if printf '%s' "$out" | grep -q "is not a subcommand"; then
    t_bad "the help lists '$v' but the program does not know it"
  else
    t_ok "'$v' is a verb the program dispatches"
  fi
done

# ---- 4: the README says the same ----------------------------------------
for v in $LISTED; do
  if grep -q "sync $v" "$HERE/README.md"; then
    t_ok "the README documents '$v'"
  else
    t_bad "'$v' is in the help but not in the README"
  fi
done

# ---- 5: an unknown verb is an error, and starts nothing -------------------
if "$BIN" definitelynotaverb >"$DIR/o.txt" 2>"$DIR/e.txt"; then
  t_bad "an unknown verb SUCCEEDED -- it may have started a server"
else
  rc=$?
  if [ "$rc" = 2 ]; then
    t_ok "an unknown verb exits 2"
  else
    t_bad "an unknown verb exited $rc, not 2"
  fi
fi
if [ -s "$DIR/e.txt" ]; then
  t_ok "...and says so on STDERR, where a correction belongs"
else
  t_bad "an unknown verb printed nothing to stderr"
fi
if grep -q "not a subcommand" "$DIR/e.txt"; then
  t_ok "...naming what it did not understand"
else
  t_bad "the error does not say what was wrong"
fi
# The port form still works: a number is not a verb.
if "$BIN" 99999 >/dev/null 2>&1; then
  t_bad "a port outside 1..65535 was accepted"
else
  t_ok "a port outside 1..65535 is refused, not bound"
fi

# ---- SURPLUS ARGUMENTS ARE REFUSED, not ignored -----------------------
#
# A minimum count alone lets extras through silently, and an extra argument is
# never harmless: it is a mistyped one. `sync verify a.db b.db` verified only
# a.db and said nothing about b.db, which reads as "both verified".
for form in "verify $DIR/x.db extra" "backup $DIR/o.db $DIR extra" \
            "logout a@b.c $DIR extra" "invites $DIR extra" "help extra" \
            "bench $DIR extra" "invite a@b.c $DIR extra" \
            "adduser a@b.c stdin $DIR extra" "passwd a@b.c stdin $DIR extra" \
            "revoke all $DIR extra"; do
  # shellcheck disable=SC2086
  if "$BIN" $form >/dev/null 2>"$DIR/x.txt"; then
    t_bad "surplus arguments accepted: sync $form"
  elif grep -q "too many" "$DIR/x.txt"; then
    t_ok "surplus arguments refused: sync ${form%% *}"
  else
    t_bad "sync $form failed, but not for having extra arguments"
  fi
done

# ---- A CERTIFICATE NEEDS ITS KEY --------------------------------------
#
# port + datadir + ONE of the pair used to fall through to the PLAINTEXT
# branch, so a mistyped or missing key file started a server serving browser
# logins in the clear. It printed "NO TLS", on a board that boots unattended.
printf 'pw\n' | "$BIN" adduser tls@example.com stdin "$DIR" >/dev/null 2>&1
# TIMED OUT, not merely run: without the check this form does not fail -- it
# SERVES, in the clear, and blocks here forever. A bounded wait turns that into
# a reported failure instead of a hung gate.
if timeout 5 "$BIN" 8443 "$DIR" "$DIR/cert.pem" >/dev/null 2>"$DIR/tls.txt"; then
  t_bad "a certificate without its key started a server anyway"
elif ! [ -s "$DIR/tls.txt" ]; then
  t_bad "a certificate without its key started a server (it had to be killed)"
elif grep -q "needs its key" "$DIR/tls.txt"; then
  t_ok "a certificate without its key is refused, not served in the clear"
else
  t_bad "the lone-certificate form failed, but not for the stated reason"
fi

# ---- THE PASSWORD IS NEVER AN ARGUMENT ---------------------------------
#
# ITEM 148. `sync adduser <email> <password>` put the secret in argv, and argv
# is public: /proc/<pid>/cmdline is world-readable, so `ps` showed it to every
# other login on the box for as long as the command ran, and the shell that
# typed it wrote it into .bash_history for ever. This server has no password
# reset by email, so that one line IS the account.
#
# What is asserted here is WHERE THE SECRET WAS, not whether the account got
# made. The first case reads the running process's OWN argv out of /proc and
# requires the password not to be in it -- which is the item, exactly, and is
# a thing a shell can check.
PW=supersecretpassword
mkfifo "$DIR/pw.fifo"
# THE FIFO PARKS IT. A reader of a fifo blocks until a writer opens, so the
# command below is stopped at a known point with its argv already in place;
# this is not a race the test is hoping to win.
"$BIN" adduser proc@example.com stdin "$DIR" <"$DIR/pw.fifo" \
       >"$DIR/proc.out" 2>"$DIR/proc.err" &
pwpid=$!
exec 9>"$DIR/pw.fifo"
i=0
while [ "$i" -lt 200 ] && ! grep -q adduser "/proc/$pwpid/cmdline" 2>/dev/null
do
  i=$((i + 1))
  sleep 0.05
done
tr '\0' ' ' <"/proc/$pwpid/cmdline" >"$DIR/cmdline.txt" 2>/dev/null || true
printf '%s\n' "$PW" >&9
exec 9>&-
wait "$pwpid" || true

if [ -s "$DIR/cmdline.txt" ]; then
  t_ok "the running command's own argv was read from /proc (so a leak WOULD show)"
else
  t_bad "/proc/<pid>/cmdline could not be read -- the case below proves nothing"
fi
if grep -q "$PW" "$DIR/cmdline.txt"; then
  t_bad "THE PASSWORD IS IN THE PROCESS'S OWN ARGV: $(cat "$DIR/cmdline.txt")"
else
  t_ok "the password is NOT in the process's own argv, where any login could read it"
fi
if grep -q "created user" "$DIR/proc.out"; then
  t_ok "...and the account was created all the same (control)"
else
  t_bad "adduser taking its password from stdin did not create the account"
fi

# THE OLD FORM IS REFUSED, not deprecated in a comment. A form that still
# works is still the exposure, whatever the help says about it.
if "$BIN" adduser argv@example.com "$PW" "$DIR" \
     >"$DIR/argv.out" 2>"$DIR/argv.err" </dev/null; then
  t_bad "sync adduser <email> <password> was ACCEPTED -- the argv form still works"
else
  t_ok "sync adduser <email> <password> is REFUSED"
fi
if grep -q "/proc/" "$DIR/argv.err"; then
  t_ok "...and the refusal says where an argument would have been readable"
else
  t_bad "the refusal does not say why an argv password is refused"
fi
# REFUSED MEANS NOTHING HAPPENED: if the account already existed, this second
# call could not create it.
if printf '%s\n' "$PW" | "$BIN" adduser argv@example.com stdin "$DIR" 2>&1 |
   grep -q "created user"; then
  t_ok "...and the refused command had created NO account"
else
  t_bad "the refused argv form created the account anyway"
fi
if "$BIN" passwd tls@example.com someotherpassword "$DIR" \
     >/dev/null 2>"$DIR/pargv.err" </dev/null; then
  t_bad "sync passwd <email> <password> was ACCEPTED -- the argv form still works"
else
  t_ok "sync passwd <email> <password> is REFUSED too"
fi

# NO TERMINAL AND NO SOURCE NAMED: REFUSE.
#
# Reading the pipe unasked is the friendly-looking choice and the wrong one.
# `sync adduser a@b.c </dev/null` would set an empty password with nothing
# looking unusual, and the same line inside a shell script whose stdin is the
# script file would take the NEXT LINE OF THE SCRIPT as the password. Neither
# announces itself and neither can be undone on a server with no reset.
# THE PIPE HAS A PASSWORD ON IT, which is the whole of the case: a refusal
# that only happens when there is nothing to read is not this rule, it is the
# empty-password rule wearing its coat.
if printf 'sneakypassword\n' |
   "$BIN" adduser notty@example.com "$DIR" >/dev/null 2>"$DIR/notty.err"; then
  t_bad "with no terminal and no source named, adduser read the pipe anyway"
else
  t_ok "with no terminal and no source named, adduser refuses"
fi
if grep -q "not a terminal" "$DIR/notty.err"; then
  t_ok "...saying so, and naming stdin and fd:N as the way to ask"
else
  t_bad "the refusal does not explain how to supply the password"
fi
if printf 'realpassword\n' |
   "$BIN" adduser notty@example.com stdin "$DIR" 2>&1 | grep -q "created user"
then
  t_ok "...and it took NOTHING off that pipe: the account was never made"
else
  t_bad "the refused no-source form created the account off the pipe anyway"
fi

# AN INHERITED DESCRIPTOR, for a caller whose stdin is already busy.
printf 'fdpassword\n' >"$DIR/fdpw.txt"
if "$BIN" adduser fd@example.com fd:7 "$DIR" 7<"$DIR/fdpw.txt" 2>&1 </dev/null |
   grep -q "created user"; then
  t_ok "fd:N reads the password from an inherited descriptor"
else
  t_bad "fd:N did not work, so scripted use has only the pipe"
fi

# THE HELP MUST NOT TEACH IT BACK. A usage line that still shows
# `<email> <password>` is the exposure documented as the way to do it.
if printf '%s' "$HELP" | grep -q '<password>'; then
  t_bad "the help still shows an argv password form"
else
  t_ok "the help shows no argv password form for any verb"
fi
if grep -q 'sync \(adduser\|passwd\) *<email> *<password>' "$HERE/README.md"
then
  t_bad "the README still shows the argv password form"
else
  t_ok "the README shows no argv password form either"
fi
if printf '%s' "$HELP" | grep -q 'stdin'; then
  t_ok "...and the help does say where the password comes from instead"
else
  t_bad "the help removed the argv form without naming a replacement"
fi

# ---- ONE PASSWORD-CHANGE OPERATION, SHARED BY BOTH SURFACES -------------
#
# ITEM 149. The hash and the session revocation are one transaction in
# user_set_password (srv/auth.c). What made them drift apart the first time
# was a SECOND COPY: the CLI did the revocation itself, afterwards, as its own
# statement, and the browser path did not do it at all. So this reads the two
# dispatch branches and requires that neither has grown a copy back.
#
# Source-level, deliberately. faulttest.sh proves the BEHAVIOUR (a failed
# revocation leaves the old password working, on both surfaces); this proves
# there is one place where that behaviour lives, which is the half a
# behavioural test cannot see.
awk '
  /!strcmp\(argv\[1\], "passwd"\)/ { on = 1 }
  on && seen && /!strcmp\(argv\[1\], "/ { exit }
  on { print; seen = 1 }
' "$HERE/srv/sync.c" >"$DIR/cli_passwd.txt"
awk '
  /!strcmp\(what, "password"\)/ { on = 1 }
  on && seen && /!strcmp\(what, "/ { exit }
  on { print; seen = 1 }
' "$HERE/srv/settings.c" >"$DIR/web_passwd.txt"

if [ -s "$DIR/cli_passwd.txt" ] && [ -s "$DIR/web_passwd.txt" ]; then
  t_ok "both password-change branches were found in the source"
else
  t_bad "a password-change branch could not be located -- this section proves nothing"
fi
if grep -q 'user_set_password(' "$DIR/cli_passwd.txt"; then
  t_ok "the CLI's passwd goes through the shared user_set_password()"
else
  t_bad "the CLI's passwd does not call the shared operation"
fi
if grep -q 'user_set_password(' "$DIR/web_passwd.txt"; then
  t_ok "the browser's password form goes through the SAME shared operation"
else
  t_bad "the settings page does not call the shared operation"
fi
for own in 'session_drop_all(' 'db_durable_begin(' 'UPDATE user SET pw_'; do
  if grep -qF "$own" "$DIR/cli_passwd.txt"; then
    t_bad "the CLI's passwd has its own '$own' -- that second copy is the defect"
  else
    t_ok "the CLI's passwd has no '$own' of its own"
  fi
  if grep -qF "$own" "$DIR/web_passwd.txt"; then
    t_bad "the settings page has its own '$own' -- that second copy is the defect"
  else
    t_ok "the settings page has no '$own' of its own"
  fi
done
DEFS=$(grep -l '^int user_set_password' "$HERE"/srv/*.c | wc -l)
if [ "$DEFS" = 1 ]; then
  t_ok "user_set_password is defined in exactly one file"
else
  t_bad "user_set_password is defined in $DEFS files -- there is more than one"
fi

# Nothing here makes HTTP requests, but t_end is how every suite that
# sources testlib.sh ends: it is where a failure raised inside a subshell --
# where `fail=1` cannot escape -- becomes the verdict.
t_end
if [ "$fail" = 0 ]; then
  printf '\033[1;32mclitest\033[0m: the help, the dispatch and the README agree\n'
else
  printf 'clitest: FAILED\n'
fi
exit $fail
