#!/bin/sh
# deploydrill.sh --- run the real deploy script against a FAKE board
#
# WHY THIS EXISTS.
#
# deploy.sh is the only thing that installs the server, and until now nothing
# ran it. `make deploycheck` reads the scripts -- it can say they parse and
# that they get their paths from one contract -- but it cannot say what they
# DO, and the defect this drill was written for is entirely behavioural:
#
#   the identical-hash path exited 0 the moment the local and live binaries
#   hashed the same, BEFORE looking at the pid, the log or the URL. So a
#   re-deploy of an unchanged build could not restart a service that had died
#   -- and re-deploying the same build is the first thing anyone does when
#   something is wrong.
#
# Every path on the board comes from pancra.conf and every value in it is
# overridable, which is what makes this possible: the "board" is a temp
# directory, `ssh` is a shell, `scp` is cp, the public URL is a file:// URL,
# and the "server" is a tiny static binary that writes the ready line and
# waits. Nothing here touches a network or the real board.
#
# Run by `make deploydrill`.
set -eu

HERE=$(cd "$(dirname "$0")/../.." && pwd)
. "$HERE/srv/test/testlib.sh"

need curl "the deploy drill" || exit 1
DIR=$(mktemp -d)
T_TMP=$DIR
# BEFORE the trap is installed, because cleanup reads it: set afterwards, a
# failure in between would run cleanup with BOARD unset, and under `set -u`
# that kills the handler before the `rm -rf`.
BOARD=$DIR/board
cleanup() {
  # THE WATCHDOG FIRST, and everything else after it. It exists to start the
  # server again when it finds it gone, so killing the servers first and the
  # watchdog second is a race this drill would lose about one run in three --
  # and what it would leave behind is a supervisor looping over a temp
  # directory that has been deleted, for ever, once per run.
  #
  # EVERY watchdog, by the pid each one writes into its own log, not merely the
  # one the pid file names: the reboot case deliberately leaves a pid file
  # naming something else.
  if [ -f "$BOARD/supervise.log" ]; then
    for p in $(sed -n 's/.*supervisor\[\([0-9]*\)\].*/\1/p' \
                 "$BOARD/supervise.log" | sort -u); do
      kill -9 "$p" 2>/dev/null || true
    done
  fi
  if [ -f "$BOARD/supervise.pid" ]; then
    kill -9 "$(cat "$BOARD/supervise.pid")" 2>/dev/null || true
  fi
  # Anything the drill started, whatever state it left the pid file in.
  #
  # EVERY START, not only the one the pid file names. The pid file holds the
  # LAST start, and this drill deliberately arranges a procedure that has to
  # stop a process it is superseding -- so a mutant that skips that stop leaves
  # a `sleep 60` loop behind, and with the old cleanup it was leaked for ever,
  # once per run. Each start prints its own pid into the log; kill them all.
  if [ -f "$BOARD/sync.log" ]; then
    for p in $(grep -o 'pancra pid [0-9]*' "$BOARD/sync.log" |
                 awk '{print $3}'); do
      kill "$p" 2>/dev/null || true
    done
  fi
  # `|| true` on the test as well: this runs from a trap under `set -e`, and a
  # missing pid file used to abort cleanup right here -- leaving the temp
  # directory behind on every run that ended with nothing started.
  if [ -f "$BOARD/sync.pid" ]; then
    kill "$(cat "$BOARD/sync.pid")" 2>/dev/null || true
  fi
  rm -rf "$DIR"
}
trap cleanup EXIT INT TERM

mkdir -p "$BOARD"

# ---- the fake board's ssh and scp ---------------------------------------
#
# deploy.sh calls `$SSH <host> <command>` and `$SSH <host> "sh -s" <<EOF`.
# Both are the same thing locally: ignore the host, run the command with
# stdin passed through.
cat > "$DIR/fakessh" <<'EOF'
#!/bin/sh
host=$1; shift
exec sh -c "$*"
EOF
cat > "$DIR/fakescp" <<'EOF'
#!/bin/sh
# scp <local> <host>:<path>   -- and, for backup.sh, <host>:<path> <local>.
#
# BOTH DIRECTIONS. This stripped `host:` from the destination only, so a copy
# HOME -- which is how backup.sh brings the artifact off the board, and the
# whole reason a backup exists -- ran `cp fake:/path/x.db out/x.db` and failed
# on a file called "fake:/path/x.db". backup.sh was therefore the one procedure
# in srv/deploy that this drill could not run at all.
src=$1; dst=$2
exec cp "${src#*:}" "${dst#*:}"
EOF
chmod +x "$DIR/fakessh" "$DIR/fakescp"

# ---- the fake server ------------------------------------------------------
#
# STATIC, because deploy.sh checks that what it installs is statically linked
# -- a real check worth keeping, so the drill satisfies it rather than
# disabling it. It writes the ready line its own log is grepped for, writes
# the page the URL check reads, and then waits to be killed.
cat > "$DIR/fake.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The directory a board path lives in, which is where the drill leaves the
 * levers below: the fake server is handed absolute paths and nothing else. */
static void dir_of(const char *path, char *out, size_t cap)
{
   snprintf(out, cap, "%s", path);
   char *slash = strrchr(out, '/');
   if (slash)
      *slash = '\0';
   else
      snprintf(out, cap, ".");
}

static int there(const char *path)
{
   return access(path, F_OK) == 0;
}

/* ---- HOLDING ONE OPERATION OPEN WHILE ANOTHER TRIES TO START ------------
 *
 * The lock is only worth testing if two operations genuinely OVERLAP. A test
 * that runs one after the other, or that starts both and hopes, proves nothing
 * -- it passes just as happily against no lock at all, and a concurrency test
 * that can pass without ever having raced is worse than no test, because it is
 * counted.
 *
 * So the first operation is made to STOP, inside its critical section, at a
 * point the drill can observe: it announces itself in `verify-started` and then
 * waits for `verify-go`. The drill waits for the announcement -- which is proof
 * the first operation is holding the lock right now -- runs the second one, and
 * only then lets the first continue.
 *
 * BOUNDED, because a drill that hangs is a `make check` that never finishes and
 * a CI job somebody kills without reading. If the release never comes, this
 * gives up and carries on, and the assertion that was waiting on it fails
 * loudly instead of silently costing an hour.
 */
static void hang_until_released(const char *dir, const char *what)
{
   char lever[600], started[600], go[600];
   snprintf(lever, sizeof lever, "%s/hang-%s", dir, what);
   if (!there(lever))
      return;
   snprintf(started, sizeof started, "%s/%s-started", dir, what);
   FILE *m = fopen(started, "a");
   if (m) {
      fprintf(m, "%d\n", (int)getpid());
      fclose(m);
   }
   snprintf(go, sizeof go, "%s/%s-go", dir, what);
   for (int i = 0; i < 60 && !there(go); i++)
      sleep(1);
}

int main(int argc, char **argv)
{
   /* THE BACKUP VERB, which the deploy runs ON THE BOARD before it installs
    * anything ("refusing to deploy over unsaved data"). A fake server that
    * only ever listens sits in the sleep loop below instead, and the deploy
    * waits on it for ever -- which is what happened the first time this drill
    * ran, and is a fair reminder that the deploy really does wait for it. */
   if (argc > 2 && argv[1][0] == 'b') {
      FILE *b = fopen(argv[2], "w");
      if (!b)
         return 1;
      fputs("fake backup\n", b);
      fclose(b);
      /* THE THIRD OUTCOME (srv/sync.c): the backup is published and readable,
       * and the directory entry naming it was never synced. Exit 2, which the
       * real server returns when the fsync of the backups directory fails --
       * and which deploy.sh and backup.sh each have to treat as neither
       * success nor failure. The lever is a file in the destination directory,
       * because the drill's board has no way to break an fsync. */
      char bdir[512], lever[600];
      dir_of(argv[2], bdir, sizeof bdir);
      snprintf(lever, sizeof lever, "%s/unsynced", bdir);
      if (there(lever))
         return 2;
      return 0;
   }
   /* THE VERIFY VERB, which restore.sh runs twice: once HERE (on the file the
    * operator named, with PANCRA_VERIFY_BIN) and once ON THE BOARD (on the
    * copy that landed, with PANCRA_BIN). Without it `sync verify <file>` fell
    * through to the listener below, took "verify" for a port and the backup
    * for a data directory, and sat in the sleep loop for ever -- the restore
    * hung instead of failing.
    *
    * It accepts any file that opens and is not empty, and says NOTHING about
    * whether this server will serve it. That gap is the whole subject of the
    * restore rollback: a backup can verify perfectly, structurally, and still
    * be one the server refuses at startup or cannot answer a request out of. */
   if (argc > 2 && argv[1][0] == 'v') {
      /* ...AND IT IS WHERE A RESTORE CAN BE HELD OPEN. This is the first thing
       * restore.sh runs ON THE BOARD, after the staged copy has landed and
       * while the operation lock is held -- so a restore stopped here is a
       * restore demonstrably inside its critical section, which is what the
       * contention cases need. Only the board-side call is affected: the
       * verifier that runs on the operator's machine is handed the backup's
       * own path, not a `.restoring-` one. */
      if (strstr(argv[2], ".restoring")) {
         char vdir[512];
         dir_of(argv[2], vdir, sizeof vdir);
         hang_until_released(vdir, "verify");
      }
      FILE *v = fopen(argv[2], "r");
      if (!v)
         return 1;
      int ch = fgetc(v);
      fclose(v);
      return ch == EOF ? 1 : 0;
   }
   const char *port = argc > 1 ? argv[1] : "8443";
   const char *dir  = argc > 2 ? argv[2] : ".";
   /* A CERTIFICATE THIS SERVER WILL NOT SERVE. The real one loads its pem
    * pair at startup and a bad one is fatal there; the drill needs the same
    * lever, because "the new pair does not serve" is the case rotate.sh
    * exists for and the only way to reach its restore path. A cert whose
    * first byte is 'X' is refused: no ready line, no page, no process. */
   if (argc > 3) {
      FILE *c = fopen(argv[3], "r");
      if (c) {
         int ch = fgetc(c);
         fclose(c);
         if (ch == 'X') {
            fprintf(stderr, "refusing this certificate\n");
            return 1;
         }
      }
   }
   /* THE DATABASE THIS SERVER IS ASKED TO SERVE, with the same lever as the
    * certificate above -- the first byte of the file decides -- because the
    * restore procedure's recovery path is reached only through a backup that
    * VERIFIES AND THEN DOES NOT SERVE, and there are two ways for that to
    * happen which the recovery has to treat differently:
    *
    *   'X'  the server will not start on it at all. A schema from a newer
    *        build, a file this version refuses: no ready line, no process.
    *   'B'  it starts and listens, and no request against it is answered.
    *        THE PROCESS IS ALIVE -- which is exactly the case a rollback has
    *        to STOP before it moves the files under it, and the case that used
    *        to satisfy a health check that only ever looked for a live pid.
    *
    * Anything else serves normally. A board with no database at all (every
    * phase of this drill before the restore ones) is not a refusal. */
   char dbp[512];
   snprintf(dbp, sizeof dbp, "%s/sync.db", dir);
   int dbfirst = 0;
   FILE *db     = fopen(dbp, "r");
   if (db) {
      dbfirst = fgetc(db);
      fclose(db);
   }
   if (dbfirst == 'X') {
      fprintf(stderr, "refusing this database\n");
      return 1;
   }
   /* THE -wal AND -shm, CREATED ONLY IF THEY ARE ABSENT, which is what sqlite
    * does and is load-bearing twice over in the restore drill:
    *
    *   - a start on a data directory whose log was moved away leaves behind a
    *     -wal describing the database IT opened. Left in place, the next
    *     database moved in beside it inherits another database's write-ahead
    *     log, and sqlite MERGES rather than refuses. That is what the rollback
    *     must quarantine, and it cannot be tested unless something creates it.
    *   - a start over a set that was just put back must leave that set alone,
    *     or "the previous database is back byte for byte" could never be
    *     asserted about anything but the .db itself. An existing log belongs
    *     to the database it is beside.
    */
   char side[512];
   const char *sfx[2] = {"-wal", "-shm"};
   for (int i = 0; i < 2; i++) {
      snprintf(side, sizeof side, "%s/sync.db%s", dir, sfx[i]);
      FILE *s = fopen(side, "r");
      if (s) {
         fclose(s);
         continue;
      }
      s = fopen(side, "w");
      if (s) {
         fprintf(s, "%s of pid %d\n", sfx[i], (int)getpid());
         fclose(s);
      }
   }
   /* THE PID, IN THE LOG, so the drill can ask afterwards which processes a
    * procedure left running. The pid file only ever names the LAST start, so
    * a rollback that forgot to stop the process it was rolling back looks
    * identical to one that stopped it -- until every start it made can be
    * named and asked. */
   printf("pancra pid %d\n", (int)getpid());
   fflush(stdout);
   /* A SERVER THAT STARTS, LIVES, AND NEVER SAYS IT IS LISTENING. The lever
    * for the one thing a supervisor cannot learn from `kill -0`: a process
    * that came up and got stuck before it could serve -- waiting on a lock, on
    * a device, on a name lookup. It matters because the log is append-only, so
    * a readiness check that greps the whole file finds the line the PREVIOUS
    * start wrote and calls this one healthy; the tagged banner range is what
    * tells them apart, and without a case like this that range is never the
    * thing under test. */
   char stuck[512];
   snprintf(stuck, sizeof stuck, "%s/never-ready", dir);
   if (there(stuck)) {
      for (;;)
         sleep(60);
   }
   char page[512];
   snprintf(page, sizeof page, "%s/page.html", dir);
   FILE *f = fopen(page, "w");
   if (f) {
      fputs("<h1>Pancra</h1><p>sign in</p>\n", f);
      fclose(f);
   }
   /* ...unless the drill asked it not to, which is how "the public name
    * answers but this backend does not" is arranged -- a stale NAT rule or a
    * proxy still pointing at a retired instance, the exact case that used to
    * pass because only the public page was ever fetched. */
   char nob[512];
   snprintf(nob, sizeof nob, "%s/no-backend", dir);
   int nobackend = 0;
   FILE *nb      = fopen(nob, "r");
   if (nb) {
      fclose(nb);
      nobackend = 1;
   }
   /* ...or because the DATABASE is one this server can open and cannot answer
    * out of ('B' above): the restore case where the process is alive, the log
    * says it is listening, and the service is useless. The stale page goes
    * with it -- backend.html was written by the PREVIOUS start, and a probe
    * reading it would call this process healthy on the evidence of the one
    * before. The drill removes it by hand for the no-backend file above; a
    * database this process refuses to serve is this process's own answer. */
   if (dbfirst == 'B') {
      char stale[512];
      snprintf(stale, sizeof stale, "%s/backend.html", dir);
      remove(stale);
      nobackend = 1;
   }
   /* A BUILD THAT STARTS AND NEVER SERVES, baked into the BINARY rather than
    * left in the data directory. That distinction is the whole point: the
    * `no-backend` file above makes every start unhealthy, including the one the
    * automatic rollback performs, so a deploy arranged that way ends in "the
    * rollback ALSO failed" and cannot show whether the rollback ran at all.
    * Compiled into a second fake, only THIS build is unhealthy -- so the deploy
    * fails, rolls back to the previous release, and the board comes up serving,
    * which is the path deploy.sh's last four lines exist for and which nothing
    * had ever executed. */
#ifdef DRILL_NEVER_SERVES
   {
      char stale[512];
      snprintf(stale, sizeof stale, "%s/backend.html", dir);
      remove(stale);
      nobackend = 1;
   }
#endif
   if (nobackend) {
      printf("listening on port %s\n", port);
      fflush(stdout);
      for (;;)
         sleep(60);
   }
   /* The same page as the listener itself serves. The real server answers
    * both the public route and a direct request; here that is two files, and
    * writing them from the one process is what makes them one server. */
   snprintf(page, sizeof page, "%s/backend.html", dir);
   f = fopen(page, "w");
   if (f) {
      fputs("<h1>Pancra</h1><p>sign in</p>\n", f);
      fclose(f);
   }
   printf("listening on port %s\n", port);
   fflush(stdout);
   for (;;)
      sleep(60);
}
EOF
# A SKIP IS A FAILURE UNLESS SOMEBODY ASKED FOR ONE -- the same rule as
# testlib.sh, tlstest.sh, interop.sh and duocheck, and this was the one place
# that did not follow it.
#
# It printed SKIP and exited ZERO whenever the static link failed, so on any
# machine without a static libc `make check` reported a green deployment
# recovery drill that had never run. That is the worst kind of gate: the one
# you trust. The drill covers deploy, health-check, rollback and the refusal
# to deploy over unsaved data, and none of it was being exercised.
#
# STATIC is not incidental here: deploy.sh refuses to install a binary that is
# not statically linked, and the drill exists to satisfy that check rather
# than to disable it. So a missing static libc really does mean the drill
# cannot run -- which must be said out loud, not swallowed.
cc -static -O0 -o "$DIR/sync-fake" "$DIR/fake.c" 2>"$DIR/cc.log" || {
  if [ "${ALLOW_SKIP:-0}" = "1" ]; then
    echo "deploydrill: no static libc, SKIPPED (ALLOW_SKIP=1)"
    echo "  $(head -1 "$DIR/cc.log")"
    exit 0
  fi
  echo "deploydrill: the fixture will not link statically, so the deployment"
  echo "  recovery drill did NOT run -- deploy, health check, rollback and"
  echo "  the refusal to deploy over unsaved data are all UNTESTED."
  echo "  $(head -1 "$DIR/cc.log")"
  echo "  Install a static libc (glibc-static / libc6-dev on most systems),"
  echo "  or re-run with ALLOW_SKIP=1 to accept that."
  exit 1
}

# THE SAME FAKE, BUILT NOT TO SERVE. See DRILL_NEVER_SERVES in fake.c: it is
# how the drill reaches deploy.sh's automatic rollback with a PREVIOUS release
# that still works. Failing this compile is not a skip -- the first one already
# proved a static libc is present, so a failure here is a broken fixture.
cc -static -O0 -DDRILL_NEVER_SERVES -o "$DIR/sync-lame" "$DIR/fake.c" \
   2>>"$DIR/cc.log" || {
  echo "deploydrill: the never-serves fixture will not build:"
  echo "  $(tail -1 "$DIR/cc.log")"
  exit 1
}

ARCH=$(file -b "$DIR/sync-fake" | sed -n 's/.*ELF 64-bit LSB [a-z]*, \([^,]*\),.*/\1/p')
[ -n "$ARCH" ] || ARCH=x86-64

# ---- the contract, pointed at the fake board ------------------------------
export PANCRA_HOST=fake
export PANCRA_SSH=$DIR/fakessh
export PANCRA_SCP=$DIR/fakescp
export PANCRA_ROOT=$BOARD
export PANCRA_ARCH=$ARCH
export PANCRA_URL=file://$BOARD/page.html
# THE BACKEND, ADDRESSED DIRECTLY. On the real board this is https on
# PANCRA_PORT; here the "listener" is a file the fake server writes, so the
# distinction the drill can still pin is the one that matters -- the health
# verdict asks BOTH, and a deploy that satisfies only the public probe is not
# reported healthy.
# The board's own answer, read the way this board can answer it. `cat` of a
# missing file fails, which is precisely the "the backend does not answer"
# arrangement the case below needs.
export PANCRA_BACKEND_PROBE="cat $BOARD/backend.html"
export PANCRA_URL_MARKER="sign in"
export PANCRA_LOCAL_BIN=$DIR/sync-fake
# THE BACKUP VERIFIER, which runs HERE rather than on the board -- so on a real
# deployment it is the native build and on this fake board it is the same fake
# server, which grows a `verify` verb for exactly this. Pointing it at the fake
# is what makes the restore drill hermetic: it needs no sqlite database and no
# native server build, and the shape of the file is then free to be the lever
# the fake refuses (see fake.c above).
export PANCRA_VERIFY_BIN=$DIR/sync-fake
# THE FRONT DOOR, declared -- because the drill is a deployment and every
# deployment has to say what maps the public port to the listener. `direct` is
# the truthful answer for a file:// URL served straight out of the board
# directory, and PANCRA_PUBLIC_PORT then has to equal PANCRA_PORT: `direct`
# means the board answers on the public port itself.
export PANCRA_FRONT=direct
export PANCRA_FRONT_OWNER="the drill"
export PANCRA_PUBLIC_PORT=8443
# NOBODY SUPERVISES THE BOARD FOR THE FIRST HALF OF THIS DRILL, and it is
# declared rather than left to a default. Almost every case above arranges a
# dead service ON PURPOSE -- a crash to recover from, a build that will not
# serve, a restore that has to be rolled back -- and a watchdog polling the
# same pid file would be a second actor in every one of them, restarting the
# thing the case is about at an unrepeatable moment. So supervision is switched
# off here and switched on, from the contract's own default, in the section
# that is about it.
export PANCRA_SUPERVISOR=none

deploy() { "$HERE/srv/deploy/deploy.sh" "$@" >"$DIR/out.txt" 2>&1; }
alive() { [ -f "$BOARD/sync.pid" ] && kill -0 "$(cat "$BOARD/sync.pid")" 2>/dev/null; }
# What the last procedure SAID. Defined here rather than beside the restore
# cases that first needed it: every section below asserts on the words a
# procedure used, and a helper defined halfway down a file is one the sections
# above it silently do not have.

echo "== a first deploy installs and starts the service =="
if deploy "$DIR/sync-fake"; then
  t_ok "the first deploy succeeds"
else
  t_bad "the first deploy failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
if alive; then t_ok "...and the service is running"; else t_bad "nothing is running"; fi
PID1=$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)

echo "== the SAME binary, with the service HEALTHY, is a no-op =="
if deploy "$DIR/sync-fake"; then
  t_ok "a same-hash deploy over a healthy service succeeds"
else
  t_bad "a same-hash deploy failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
PID2=$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)
if [ "$PID1" = "$PID2" ]; then
  t_ok "...and does NOT restart a service that is already fine"
else
  t_bad "it restarted a healthy service (pid $PID1 -> $PID2)"
fi

echo "== THE CASE THIS DRILL EXISTS FOR: same binary, DEAD service =="
# What a crash leaves behind: the binary on the board is the one we would
# install, and nothing is running.
kill "$PID2" 2>/dev/null || true
i=0
while alive && [ $i -lt 25 ]; do sleep 0.2; i=$((i + 1)); done
if alive; then
  t_bad "could not stop the service to arrange the case"
else
  t_ok "the service is stopped, as a crash would leave it"
fi
if deploy "$DIR/sync-fake"; then
  t_ok "re-deploying the same build succeeds"
else
  t_bad "re-deploying the same build failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
if alive; then
  t_ok "...AND THE SERVICE IS RUNNING AGAIN"
else
  t_bad "the deploy reported success with nothing running -- the bug"
fi
PID3=$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)
if [ "$PID3" != "$PID2" ]; then
  t_ok "...as a new process, not the old pid re-reported"
else
  t_bad "the pid file still names the dead process"
fi

echo "== --restart restarts a HEALTHY service, for a cert or config change =="
if deploy --restart "$DIR/sync-fake"; then
  t_ok "--restart succeeds"
else
  t_bad "--restart failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
PID4=$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)
if [ "$PID4" != "$PID3" ]; then
  t_ok "...and the service really was restarted"
else
  t_bad "--restart did not restart anything (pid $PID4)"
fi
if alive; then t_ok "...and is running afterwards"; else t_bad "nothing is running"; fi

echo "== a NEW binary is installed, and the old one kept for a rollback =="
cp "$DIR/sync-fake" "$DIR/sync-new"
printf '\n' >> "$DIR/sync-new" 2>/dev/null || true
chmod +x "$DIR/sync-new"
OLDHASH=$(sha256sum "$DIR/sync-fake" | awk '{print $1}')
if deploy "$DIR/sync-new"; then
  t_ok "deploying a different build succeeds"
else
  t_bad "deploying a different build failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
if [ -f "$BOARD/releases/sync-$OLDHASH" ]; then
  t_ok "...and the previous binary is kept under its hash, for a rollback"
else
  t_bad "the previous release was not kept"
fi
if alive; then t_ok "...and the service is running"; else t_bad "nothing is running"; fi

echo "== an UNDECLARED front door is refused BEFORE anything is installed =="
#
# deploycheck can only grep for the words `front_declared` inside
# wait_healthy_since. Two mutations survive that grep -- replacing the call
# with a comment that mentions it, and gutting front_declared to `return 0` --
# so the refusal has to be exercised, not merely named.
#
# AND IT HAS TO BE REFUSED EARLY. When the only check lived in
# wait_healthy_since, deploy.sh got there after the swap and the restart: an
# undeclared front door read as "the new build is NOT healthy", so a perfectly
# good build was installed, rolled back, and reported as "the board is DOWN"
# while the board was up and serving. That is what the two assertions below
# pin -- the refusal, and the fact that nothing moved.
BEFORE=$(sha256sum "$BOARD/sync" | awk '{print $1}')
PID5=$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)
# A SUBSHELL, not a `VAR=x deploy ...` prefix: assignments prefixed to a
# FUNCTION persist after it in most shells, and this must not leak.
if (PANCRA_FRONT=unset; PANCRA_FRONT_OWNER=unset; deploy "$DIR/sync-fake"); then
  t_bad "a deploy with no front door declared REPORTED SUCCESS"
else
  t_ok "a deploy with no front door declared is refused"
fi
if grep -q 'UNDECLARED' "$DIR/out.txt"; then
  t_ok "...and says so, in words naming the front door"
else
  t_bad "...but the message never names it: $(t_show "$(cat "$DIR/out.txt")")"
fi
if grep -q 'rolling back\|board is DOWN' "$DIR/out.txt"; then
  t_bad "...and it wrongly reported an outage: $(t_show "$(cat "$DIR/out.txt")")"
else
  t_ok "...and does NOT claim the board is down, because it is not"
fi
AFTER=$(sha256sum "$BOARD/sync" | awk '{print $1}')
if [ "$BEFORE" = "$AFTER" ]; then
  t_ok "...and nothing was installed: the binary on the board is untouched"
else
  t_bad "it installed $AFTER over $BEFORE before refusing"
fi
PID6=$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)
if [ "$PID5" = "$PID6" ] && alive; then
  t_ok "...and the service that was running is still the one running"
else
  t_bad "the running service was disturbed (pid $PID5 -> $PID6)"
fi

# ---- THE EVIDENCE IS ABOUT ONE PROCESS AND ONE BACKEND --------------------
#
# A hash of a file on disk, a live pid, a line in a log and a public page are
# four facts about a board, and nothing used to require them to be about the
# same program. These pin each binding on its own, by calling health.sh
# directly -- the deploy paths above already exercise them together.
echo "== health evidence is bound to the artifact, the start and the backend =="
# The contract too: everything above exported the values that OVERRIDE it, and
# ${VAR:-default} means those win -- but PANCRA_PID and PANCRA_LOG are DERIVED
# from PANCRA_ROOT and exist nowhere until the file is read. The deploy paths
# above never needed it because each script sources it itself.
. "$HERE/srv/deploy/pancra.conf"
. "$HERE/srv/deploy/health.sh"
. "$HERE/srv/deploy/start.sh"
SSH=$PANCRA_SSH

LIVEHASH=$(sha256sum "$BOARD/sync" | awk '{print $1}')
if exe_is "$LIVEHASH"; then
  t_ok "the running process IS the binary that was installed"
else
  t_bad "exe_is cannot recognise the binary it is running"
fi
if exe_is "0000000000000000000000000000000000000000000000000000000000000000"; then
  t_bad "a live pid satisfied a hash it is not running -- the whole hole"
else
  t_ok "...and a live pid running something ELSE is not accepted"
fi

# THE TAG. A readiness line is only evidence about the start that wrote it.
LASTTAG=$(grep -o 'start-[0-9a-z-]*' "$BOARD/sync.log" | tail -1)
if [ -n "$LASTTAG" ] && health_since 0 "$LASTTAG"; then
  t_ok "the readiness line after THIS start's banner counts"
else
  t_bad "the tagged health check cannot see the start that just happened"
fi
if health_since 0 "start-never-happened"; then
  t_bad "a start that never happened was reported healthy"
else
  t_ok "...and a start whose banner is not in the log is NOT healthy"
fi

# THE TAG REPLACES THE BYTE OFFSET rather than joining it, and the range the
# banner opens has to CLOSE. Neither half is visible to any assertion above:
# the drill's log is never rotated and two deploys never race here, which is
# exactly why both had to be arranged by hand. They are the same wrong-start
# bug approached from either side -- one refuses a good start, one accepts a
# bad one -- so the log is borrowed and put back.
SAVEDLOG=$DIR/sync.log.saved
cp "$BOARD/sync.log" "$SAVEDLOG"
ROTMARK=$(log_mark)
ROTTAG=$(start_tag)
# A ROTATION BETWEEN THE MARK AND THE CHECK: logrotate's copytruncate, or an
# operator emptying a log that had grown. The offset now points far past
# everything this start wrote; the banner is still the first line in the file.
# Applying both would report a service that came up perfectly as unhealthy --
# and in deploy.sh that means a good build is automatically rolled back.
: > "$BOARD/sync.log"
printf '=== pancra %s ===\nlistening on port 8443\n' "$ROTTAG" >>"$BOARD/sync.log"
if health_since "$ROTMARK" "$ROTTAG"; then
  t_ok "a start whose log was rotated under it is still healthy"
else
  t_bad "the stale byte offset skipped past this start's own banner"
fi
# TWO DEPLOYS RACING: ours never came up, theirs did, and theirs is written
# AFTER our banner. A range that runs to end of file reads their readiness
# line as ours -- the very substitution the tag was added to prevent.
MINETAG=$(start_tag)
THEIRTAG=$(start_tag)
: > "$BOARD/sync.log"
printf '=== pancra %s ===\nfatal: address already in use\n' \
   "$MINETAG" >>"$BOARD/sync.log"
printf '=== pancra %s ===\nlistening on port 8443\n' \
   "$THEIRTAG" >>"$BOARD/sync.log"
if health_since 0 "$MINETAG"; then
  t_bad "a LATER start's readiness line was accepted as this start's"
else
  t_ok "...and a later start's readiness line does not answer for this one"
fi
cp "$SAVEDLOG" "$BOARD/sync.log"

# ...AND THE BINDING HAS TO BE INSIDE THE VERDICT, not merely available next
# to it. Calling exe_is directly proves the function works; it does not prove
# wait_healthy_since consults it, and a health check that holds the evidence
# without using it is the hole this item is about.
#
# The arrangement is the real failure: a live pid, a banner, a readiness line
# and a backend that answers -- every signal healthy -- while the process is
# running a DIFFERENT executable from the one on disk. That is what replacing
# a running binary by rename leaves behind when the new process dies and the
# old one does not.
cp "$DIR/sync-fake" "$DIR/impostor"
printf '\n\n\n' >> "$DIR/impostor" 2>/dev/null || true
chmod +x "$DIR/impostor"
IMPHASH=$(sha256sum "$DIR/impostor" | awk '{print $1}')
REALHASH=$(sha256sum "$BOARD/sync" | awk '{print $1}')
IMPTAG=$(start_tag)
IMPMARK=$(log_mark)
OLDPID=$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)
kill "$OLDPID" 2>/dev/null || true
printf '=== pancra %s ===\n' "$IMPTAG" >> "$BOARD/sync.log"
# A PLAIN `&`, not setsid: this is the drill arranging a rogue process, not
# the deployment starting the service -- and deploycheck rightly refuses a
# second copy of the start command anywhere outside start.sh. The drill's
# shell outlives it, so nothing here needs a new session.
"$DIR/impostor" 8443 "$BOARD" </dev/null >>"$BOARD/sync.log" 2>&1 &
IMPPID=$!
echo "$IMPPID" > "$BOARD/sync.pid"
sleep 1
if wait_healthy_since "$IMPMARK" "$IMPTAG" "$IMPHASH"; then
  t_ok "a healthy start of a known binary is verified"
else
  t_bad "the arrangement itself is not healthy; the case cannot be tested"
fi
if wait_healthy_since "$IMPMARK" "$IMPTAG" "$REALHASH"; then
  t_bad "EVERY signal was healthy while the process ran another binary"
else
  t_ok "...and the same signals do NOT verify a different installed artifact"
fi

# ...AND THE SAME-BINARY FAST PATH HAS TO ASK THE SAME QUESTION. The impostor
# is still up, so the board is in the exact state that path is written for:
# the file on disk IS the binary being deployed, a pid is alive, the log holds
# a readiness line and the public page answers. Every one of those is true of
# a process running something else, which is why "the hashes match" was never
# a reason to exit 0 -- and this is the most-taken path of all, a re-deploy of
# an unchanged build.
if deploy "$DIR/sync-new"; then
  t_ok "a same-hash deploy over an impostor process still succeeds"
else
  t_bad "the same-hash path failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
if exe_is "$REALHASH"; then
  t_ok "...by RESTARTING it: the live process runs the installed artifact"
else
  t_bad "the no-op path left a process running a DIFFERENT binary"
fi
# ...AND THE IMPOSTOR IS NOT WHAT IT STOPPED. The pid file named a live process
# that is not this deployment's server, which is the arrangement a recycled pid
# produces -- the restart above had to leave it alone and start its own.
if kill -0 "$IMPPID" 2>/dev/null; then
  t_ok "...without signalling the process the stale pid file named"
else
  t_bad "the restart killed a live process that was NOT this deployment's"
fi
kill "$IMPPID" 2>/dev/null || true
kill "$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)" 2>/dev/null || true
if deploy --restart "$DIR/sync-fake"; then
  t_ok "...and a restart puts the real service back"
else
  t_bad "could not restore the service after the impostor case"
fi

# THE TAG, ALSO INSIDE THE VERDICT. By now the log holds the readiness lines
# of several earlier starts, which is the ordinary state of an append-only log
# on a board that has been deployed to more than once. Asked with no tag and
# no offset, the check is satisfied by any of them -- that is the documented
# behaviour for a service that was already running. Asked about a start whose
# banner was never written, it must not be.
GHOSTTAG=$(start_tag)
if wait_healthy_since 0 "" ""; then
  t_ok "with no tag, a running service satisfies the check"
else
  t_bad "the untagged check cannot see a service that is running"
fi
if wait_healthy_since 0 "$GHOSTTAG" ""; then
  t_bad "a start that never happened was verified from an OLDER start's line"
else
  t_ok "...but a start whose banner was never written is NOT verified"
fi

# ---- A PID FILE IS A NUMBER, AND THE KERNEL REUSES NUMBERS ---------------
#
# Every stop in this deployment used to send TERM and then KILL to whatever
# number was in sync.pid. That is a signal aimed by a file, and the file goes
# stale in exactly the two situations the deployment exists for: the server
# crashes and the number is handed out again, or the board reboots and the file
# survives while the process does not -- on a freshly booted board the low
# numbers go straight back out to ntpd, to dropbear, to the board's own sensor
# logger. The kill then succeeds, the stop reports nothing unusual, and the
# casualty is a program nobody was thinking about.
#
# THE ONLY TEST OF THIS WORTH HAVING IS ONE WHERE THE SIGNAL WOULD LAND. A case
# that merely checks the right process still gets stopped passes identically
# against no identity check at all -- it is the wrong half of the claim. So each
# case below puts a LIVE process that is not the server behind the pid file, and
# asserts that it is still alive afterwards.
#
# AND IDENTITY IS TWO FACTS. A pid is not one, and neither half alone is:
#
#   the EXECUTABLE, because the number may now belong to something else
#   entirely -- which is what the stranger cases are;
#   the START TIME, because it may belong to a SECOND COPY OF THIS SERVER,
#   which has the same executable to the byte and is the likeliest thing on
#   this board to be holding a recycled number. That is the twin case, and it
#   is the one an executable check alone cannot see.
#
# Both are recorded by start_block beside the pid and both are checked by
# stop_block; the last two cases tamper with one half of the record each, so
# that a check which consults only the other half is a failing drill rather
# than a silent regression.
echo "== a pid file naming a live STRANGER is quarantined, not signalled =="

# The drill's own way to end a process and wait for it to go. Emphatically NOT
# stop_block: that is the thing under test, and a case arranged with it would
# be asking the mechanism to set up its own examination.
drill_kill() {
  dk_p=${1:-}
  [ -n "$dk_p" ] || return 0
  kill "$dk_p" 2>/dev/null || true
  dk_i=0
  while kill -0 "$dk_p" 2>/dev/null && [ "$dk_i" -lt 25 ]; do
    sleep 0.2
    dk_i=$((dk_i + 1))
  done
}
# The single quarantined pid file, if there is one. A glob rather than a name:
# the quarantine carries the board's clock and the remote shell's pid so that
# two stops in one second cannot overwrite each other's evidence.
one_stale() {
  os_hit=
  for os_p in "$BOARD"/sync.pid.stale-*; do
    [ -e "$os_p" ] || continue
    # The identity record is quarantined beside it under the same stem, and it
    # is not the pid file: a glob that counted both would report two.
    case "$os_p" in *.id) continue ;; esac
    [ -z "$os_hit" ] || return 0
    os_hit=$os_p
  done
  printf '%s' "$os_hit"
}

# CASE ONE: THE RECORD IS THERE AND DISAGREES. The server is stopped by this
# drill (so nothing of ours is running), an unrelated process is put behind the
# pid file, and a deploy is asked to restart the service.
drill_kill "$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)"
rm -f "$BOARD"/sync.pid.stale-*
sleep 300 &
STRANGER=$!
printf '%s\n' "$STRANGER" > "$BOARD/sync.pid"
if deploy --restart "$DIR/sync-fake"; then
  t_ok "a deploy whose pid file names a stranger still starts the service"
else
  t_bad "a stale pid file stopped the deploy: $(t_show "$(cat "$DIR/out.txt")")"
fi
if kill -0 "$STRANGER" 2>/dev/null; then
  t_ok "...AND THE STRANGER WAS NOT SIGNALLED -- the whole item"
else
  t_bad "THE DEPLOY KILLED AN UNRELATED PROCESS that held the recycled pid"
fi
IDSTALE=$(one_stale)
if [ -n "$IDSTALE" ]; then
  t_ok "...and the stale pid file was quarantined, not left to be re-read"
else
  t_bad "the pid file that named a stranger is still in place for the next \
operation, the health check and the watchdog to reason about"
fi
if [ "$(tr -dc '0-9' < "$IDSTALE" 2>/dev/null)" = "$STRANGER" ]; then
  t_ok "...holding the number that was refused, so it can be diagnosed"
else
  t_bad "the quarantined file does not hold the refused number"
fi
if out_has 'NOT SIGNALLING'; then
  t_ok "...and the refusal is said out loud, not swallowed"
else
  t_bad "a signal was withheld silently: $(t_show "$(cat "$DIR/out.txt")")"
fi
if alive && [ "$(cat "$BOARD/sync.pid")" != "$STRANGER" ]; then
  t_ok "...and the service that came up is this deployment's own"
else
  t_bad "the pid file still names the stranger after the restart"
fi
drill_kill "$STRANGER"

# CASE TWO: NO RECORD AT ALL -- a pid file from a deployment older than the
# record, or one an operator wrote by hand. The claim that can still be made
# from the board alone is that /proc/<pid>/exe resolves to the installed
# binary, and a sleeping stranger's does not.
drill_kill "$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)"
rm -f "$BOARD/sync.pid.id"
sleep 300 &
STRANGER2=$!
printf '%s\n' "$STRANGER2" > "$BOARD/sync.pid"
if deploy --restart "$DIR/sync-fake" && kill -0 "$STRANGER2" 2>/dev/null; then
  t_ok "...and with NO identity record either, a stranger is still not signalled"
else
  t_bad "with no identity record the stop fell back to signalling the number"
fi
drill_kill "$STRANGER2"

# CASE THREE: THE TWIN. A live process running THE SAME EXECUTABLE, byte for
# byte, that this deployment did not start -- which on a board whose most
# frequently started program is this server is the likeliest holder of a
# recycled number there is. The executable half of the identity cannot tell it
# apart; the start time can, and must.
drill_kill "$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)"
# A plain `&`, not setsid: the drill is arranging a rogue process, not starting
# the service, and deploycheck rightly refuses a second copy of the start.
"$BOARD/sync" 8443 "$BOARD" </dev/null >>"$BOARD/sync.log" 2>&1 &
TWIN=$!
sleep 1
printf '%s\n' "$TWIN" > "$BOARD/sync.pid"
if deploy --restart "$DIR/sync-fake"; then
  t_ok "a deploy over a pid file naming a TWIN of the server still deploys"
else
  t_bad "the twin case stopped the deploy: $(t_show "$(cat "$DIR/out.txt")")"
fi
if kill -0 "$TWIN" 2>/dev/null; then
  t_ok "...and a process running THE SAME BINARY, that this deployment did \
not start, is not signalled either"
else
  t_bad "the executable matched, so the number was signalled -- which is what \
a recycled pid on this board looks like"
fi
drill_kill "$TWIN"

# CASE FOUR: THE RECORDED EXECUTABLE IS CONSULTED. Here the pid and the start
# time are the live server's own and only the recorded hash disagrees, so a
# stop that checked the start time alone would signal. It is the mirror of the
# twin above: one case fails a check that skips the start time, this one fails
# a check that skips the executable, and neither can stand in for the other.
IDLIVE=$(cat "$BOARD/sync.pid")
sed 's/^exe .*/exe 0000000000000000000000000000000000000000000000000000000000000000/' \
   "$BOARD/sync.pid.id" > "$DIR/pid.id.tampered"
cat "$DIR/pid.id.tampered" > "$BOARD/sync.pid.id"
if deploy --restart "$DIR/sync-fake" && kill -0 "$IDLIVE" 2>/dev/null; then
  t_ok "...and a process whose EXECUTABLE does not match the record is not \
signalled even when its number and start time do"
else
  t_bad "the recorded executable is not part of the identity that is checked"
fi
drill_kill "$IDLIVE"

# ...AND THE POSITIVE HALF, which is the one that keeps all of the above from
# being satisfied by a stop that never signals anything. The service running
# now was started by this deployment and its record describes it, so the next
# stop must end it.
IDGOOD=$(cat "$BOARD/sync.pid")
if deploy --restart "$DIR/sync-fake"; then
  t_ok "a deploy over its OWN server restarts it"
else
  t_bad "the restart failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
if kill -0 "$IDGOOD" 2>/dev/null; then
  t_bad "IDENTITY CHECKING TURNED THE STOP INTO A NO-OP: the process this \
deployment started is still running beside the one that replaced it"
else
  t_ok "...and the process it had started IS stopped, so the check refuses \
strangers rather than refusing everything"
fi

# THE BACKEND. The public page answering is not this server answering.
echo "== the public page answering is not THIS backend answering =="
# A DIFFERENT HASH, so this is a real install and restart rather than the
# same-binary path -- the appended byte is ignored by the loader.
cp "$DIR/sync-fake" "$DIR/sync-nb"
printf '\n\n' >> "$DIR/sync-nb" 2>/dev/null || true
chmod +x "$DIR/sync-nb"
: > "$BOARD/no-backend"
# AND THE STALE ONE GOES. A file left by the previous start is exactly the
# retired instance this case is about: the probe would read it and call the
# backend healthy while the process that wrote it is long gone.
rm -f "$BOARD/backend.html"
if deploy "$DIR/sync-nb"; then
  t_bad "a deploy passed with the backend not answering"
else
  t_ok "a deploy whose backend does not answer is refused"
fi
if grep -q "did not answer" "$DIR/out.txt"; then
  t_ok "...and the message names the direct backend, not the public URL"
else
  t_bad "...but never says which probe failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
rm -f "$BOARD/no-backend"

# ---- ROTATING THE CERTIFICATE --------------------------------------------
#
# Nothing ran rotate.sh. Its whole promise is that a certificate the server
# will not serve is PUT BACK automatically -- the one path an operator can
# never rehearse on the real board, and the one most likely to be needed at an
# hour when nobody wants to be reading a runbook.
echo "== rotating the certificate =="
rotate() { "$HERE/srv/deploy/rotate.sh" "$@" >"$DIR/out.txt" 2>&1; }

if command -v openssl >/dev/null 2>&1; then
  openssl req -x509 -newkey rsa:2048 -keyout "$DIR/a.key" -out "$DIR/a.crt" \
     -days 2 -nodes -subj /CN=drill-a >/dev/null 2>&1
  openssl req -x509 -newkey rsa:2048 -keyout "$DIR/b.key" -out "$DIR/b.crt" \
     -days 2 -nodes -subj /CN=drill-b >/dev/null 2>&1

  # The pair the board is running now, so the restore can be checked by hash.
  cp "$DIR/a.crt" "$BOARD/cert.pem"
  cp "$DIR/a.key" "$BOARD/key.pem"
  ORIG=$(sha256sum "$BOARD/cert.pem" | awk '{print $1}')

  # A MISMATCHED PAIR IS REFUSED BEFORE ANYTHING MOVES. A certificate with
  # the wrong key starts perfectly and fails every handshake, so the pid check
  # and the readiness line both pass and only the public probe notices.
  if rotate "$DIR/a.crt" "$DIR/b.key"; then
    t_bad "a mismatched cert/key pair was accepted"
  else
    t_ok "a mismatched cert/key pair is refused"
  fi
  if [ "$(sha256sum "$BOARD/cert.pem" | awk '{print $1}')" = "$ORIG" ]; then
    t_ok "...before anything on the board moved"
  else
    t_bad "it had already installed the certificate"
  fi

  # A GOOD ROTATION, and the pid file must name the process that is running:
  # the hand-written procedure this replaced ended at `&` and never wrote it.
  PIDR=$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)
  if rotate "$DIR/b.crt" "$DIR/b.key"; then
    t_ok "a matching pair rotates in"
  else
    t_bad "a good rotation failed: $(t_show "$(cat "$DIR/out.txt")")"
  fi
  if [ "$(sha256sum "$BOARD/cert.pem" | awk '{print $1}')" \
       = "$(sha256sum "$DIR/b.crt" | awk '{print $1}')" ]; then
    t_ok "...and the board is serving the new certificate"
  else
    t_bad "the certificate on the board is not the one that was rotated in"
  fi
  PIDS=$(cat "$BOARD/sync.pid" 2>/dev/null || echo 0)
  if alive && [ "$PIDS" != "$PIDR" ]; then
    t_ok "...as a restarted process whose pid was RECORDED"
  else
    t_bad "the pid file does not name the running process (was $PIDR, now $PIDS)"
  fi
  if [ -f "$BOARD/key.pem" ] &&
     [ "$(stat -c %a "$BOARD/key.pem" 2>/dev/null)" = 600 ]; then
    t_ok "...and the private key is not readable by anyone else"
  else
    t_bad "the private key landed as mode \
$(stat -c %a "$BOARD/key.pem" 2>/dev/null)"
  fi

  # THE POINT OF THE SCRIPT: a pair the server refuses is PUT BACK.
  NOW=$(sha256sum "$BOARD/cert.pem" | awk '{print $1}')
  # A REAL CERTIFICATE, and one the fake server refuses. PEM parsers ignore
  # text BEFORE the BEGIN line, so this is valid to openssl -- which matters:
  # rotate.sh validates the pair before it swaps, and a file openssl rejects
  # would stop there, never reach the swap, and leave the restore path as
  # untested as it was. The marker needs its own line for the same reason:
  # glued to "-----BEGIN" it breaks the header and openssl refuses it.
  printf 'X\n' > "$DIR/bad.crt"
  openssl x509 -in "$DIR/b.crt" >> "$DIR/bad.crt" 2>/dev/null
  if rotate "$DIR/bad.crt" "$DIR/b.key"; then
    t_bad "a certificate the server refuses was reported as rotated in"
  else
    t_ok "a certificate the server will not serve is reported as a failure"
  fi
  if [ "$(sha256sum "$BOARD/cert.pem" | awk '{print $1}')" = "$NOW" ]; then
    t_ok "...AND THE PREVIOUS PAIR IS BACK"
  else
    t_bad "the board was left holding the certificate that does not serve"
  fi
  if alive; then
    t_ok "...and the service is running again on it"
  else
    t_bad "the rotation left nothing running"
  fi
else
  t_bad "openssl is missing: the rotation drill cannot run"
fi

# ---- RESTORING A BACKUP, AND PUTTING THE DATABASE BACK -------------------
#
# NOTHING RAN restore.sh EITHER, and it is the only procedure here that
# destroys data. Its recovery path -- the backup that verifies, installs, and
# then will not serve -- left the board in the worst state this deployment can
# be in: the service DOWN, the file that had just proved it would not serve
# installed as the live database, and the complete working set it had displaced
# sitting one directory away needing three renames nobody had written down.
#
# The cases below arrange each of the three outcomes restore.sh must be able to
# tell apart, and check the FILES as well as the words: a rollback that reports
# success while sync.db-shm is missing has left a database beside another
# database's index, which sqlite merges rather than refuses.
echo "== restoring a backup =="
restore() {
  printf 'YES\n' | "$HERE/srv/deploy/restore.sh" "$@" >"$DIR/out.txt" 2>&1
}
out_has() { grep -q "$1" "$DIR/out.txt"; }

# A DATABASE SET: the file, its write-ahead log and its index, all three with
# distinctive contents. "The previous database is back" has to be checkable
# byte for byte -- a check for existence passes over a file that the refused
# start wrote, which is the corruption this is all about.
#
# THE FIRST BYTE IS THE LEVER the fake server reads (see fake.c above):
#   L  serves normally
#   B  starts and listens and answers nothing -- a live pid, a useless service
#   X  refuses to start at all
mkdb() { # mkdb <name> <first byte> <path of the .db>
  printf '%s database %s\n' "$2" "$1" >"$3"
  printf '%s log %s\n' "$2" "$1" >"$3-wal"
  printf '%s index %s\n' "$2" "$1" >"$3-shm"
}
# The live set on the board, remembered here so it can be compared afterwards.
# It also clears the previous case's displaced/refused files, so each case can
# glob for exactly one of each rather than guessing a timestamp.
livedb() { # livedb <name> <first byte>
  mkdb "$1" "$2" "$BOARD/sync.db"
  for s in '' '-wal' '-shm'; do
    cp "$BOARD/sync.db$s" "$DIR/keep-$1.db$s"
  done
  rm -f "$BOARD"/backups/displaced-* "$BOARD"/backups/refused-*
}
# A backup FILE, which is one file: what an operator hands the procedure.
mkbak() { # mkbak <name> <first byte>
  printf '%s backup %s\n' "$2" "$1" >"$DIR/$1.db"
}
setis() { # setis <.db path> <remembered name> <what this proves>
  bad=
  for s in '' '-wal' '-shm'; do
    cmp -s "$1$s" "$DIR/keep-$2.db$s" || bad="$bad .db$s"
  done
  if [ -z "$bad" ]; then
    t_ok "$3"
  else
    t_bad "$3 -- these differ or are missing:$bad"
  fi
}
one() { # one <glob>: the single existing match, or nothing
  for p in $1; do
    if [ -e "$p" ]; then
      printf '%s' "$p"
      return 0
    fi
  done
  return 0
}
# Every start's own pid, from the log. The pid FILE names only the last one, so
# a rollback that never stopped the process it superseded is indistinguishable
# from one that did -- until each start can be named and asked.
pidlines() { grep -c 'pancra pid' "$BOARD/sync.log" 2>/dev/null || true; }
pidsafter() { # pidsafter <how many pid lines there were before>
  grep -o 'pancra pid [0-9]*' "$BOARD/sync.log" | awk '{print $3}' |
    tail -n +"$(($1 + 1))"
}

livedb r1 L
mkbak g1 G
# THE CONFIRMATION IS THE ONLY THING BETWEEN A TYPO AND A DESTROYED DATABASE.
if printf 'no\n' | "$HERE/srv/deploy/restore.sh" "$DIR/g1.db" \
     >"$DIR/out.txt" 2>&1; then
  t_bad "a restore ran without the YES confirmation"
else
  t_ok "a restore that is not confirmed with YES is cancelled"
fi
setis "$BOARD/sync.db" r1 "...and the live database is untouched"

PIDN=$(pidlines)
if restore "$DIR/g1.db"; then
  t_ok "a backup this server can serve is restored"
else
  t_bad "a good restore failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
if cmp -s "$BOARD/sync.db" "$DIR/g1.db"; then
  t_ok "...and the board is serving the restored file"
else
  t_bad "the live database is not the backup that was restored"
fi
DISP=$(one "$BOARD/backups/displaced-*.db")
if [ -n "$DISP" ]; then
  setis "$DISP" r1 "...and the COMPLETE working set was displaced, not just the .db"
else
  t_bad "the working set was not kept anywhere"
fi
if alive; then t_ok "...and the service is running"; else t_bad "nothing is running"; fi

# AN UNDECLARED FRONT DOOR MUST NOT UNDO A RESTORE THAT WORKED. restore.sh has
# no require_front precondition on purpose -- a recovery that refuses to
# recover over a missing config line is worse than the missing line -- so the
# undeclared case arrives at the health wait, where it is indistinguishable
# from "the service is unreachable" unless the script asks. If it does not ask,
# a restore that is installed and listening gets ROLLED BACK, and the operator
# is told the backup could not be restored when it had been.
echo "== a restore that worked is not undone by a missing config line =="
livedb r2 L
mkbak g2 G
if (PANCRA_FRONT=unset; PANCRA_FRONT_OWNER=unset; restore "$DIR/g2.db"); then
  t_bad "a restore over an undeclared front door REPORTED VERIFIED"
else
  t_ok "a restore that cannot be checked from outside is not reported verified"
fi
if out_has 'UNDECLARED'; then
  t_ok "...and says so, naming the front door"
else
  t_bad "...but never names it: $(t_show "$(cat "$DIR/out.txt")")"
fi
if out_has 'NOTHING WAS ROLLED BACK'; then
  t_ok "...and says the restore is still in place"
else
  t_bad "...and does not say what state it left: $(t_show "$(cat "$DIR/out.txt")")"
fi
if cmp -s "$BOARD/sync.db" "$DIR/g2.db"; then
  t_ok "...WHICH IT IS: the restored database was not rolled back"
else
  t_bad "a restore that was installed and listening was thrown away"
fi
if alive; then t_ok "...and the service is running on it"; else t_bad "nothing is running"; fi

# ---- THE CASE THE ROLLBACK EXISTS FOR ------------------------------------
#
# A backup that VERIFIES and then does not serve. Here it is the second, nastier
# shape: the server starts on it, logs that it is listening, and answers no
# request at all -- so there is a live process holding the data directory that
# the rollback has to stop before it moves anything, and a pid that satisfies
# every check this deployment used to make.
echo "== a backup that verifies and will NOT serve is rolled back =="
livedb r3 L
mkbak b1 B
PIDN=$(pidlines)
if restore "$DIR/b1.db"; then
  t_bad "a backup the service cannot serve was reported as restored"
else
  t_ok "a backup that verifies and does not serve is reported as a failure"
fi
setis "$BOARD/sync.db" r3 \
  "...AND THE PREVIOUS DATABASE IS BACK: .db, -wal and -shm, byte for byte"
if alive; then
  t_ok "...and the service is running again"
else
  t_bad "the rollback left nothing running"
fi
if out_has 'ROLLBACK SUCCEEDED'; then
  t_ok "...and the report says the RECOVERY itself succeeded"
else
  t_bad "...but never says whether the recovery worked: \
$(t_show "$(cat "$DIR/out.txt")")"
fi
if out_has 'BOARD IS DOWN'; then
  t_bad "...and wrongly called the board down: $(t_show "$(cat "$DIR/out.txt")")"
else
  t_ok "...and does NOT call the board down, because it is not"
fi
REF=$(one "$BOARD/backups/refused-*.db")
if [ -n "$REF" ] && cmp -s "$REF" "$DIR/b1.db"; then
  t_ok "...the backup that would not serve is QUARANTINED, not deleted"
else
  t_bad "the backup that would not serve was not kept for inspection"
fi
# The refused start's own write-ahead log. It has to LEAVE the data directory:
# beside the database put back below it is a database plus a different
# database's log, which sqlite replays rather than refuses.
if [ -n "$REF" ] && [ -f "$REF-wal" ]; then
  t_ok "...with the -wal the refused start left behind"
else
  t_bad "the refused start's write-ahead log was not moved out of the way"
fi
if [ -z "$(one "$BOARD/backups/displaced-*.db")" ]; then
  t_ok "...and the displaced set was MOVED back, leaving no stale copy"
else
  t_bad "a stale copy of the live database is still in backups/"
fi
# THE PROCESS THAT WAS SERVING THE REFUSED DATABASE. Skipping the stop leaves
# it running on a data directory whose files have been renamed under it, with
# the pid file naming a different process -- and every check still passes.
NEWPIDS=$(pidsafter "$PIDN")
LASTPID=$(printf '%s\n' "$NEWPIDS" | tail -1)
LEFT=
for p in $NEWPIDS; do
  if [ "$p" != "$LASTPID" ] && kill -0 "$p" 2>/dev/null; then LEFT="$LEFT $p"; fi
done
if [ -z "$LEFT" ]; then
  t_ok "...and the process serving the refused database was STOPPED"
else
  t_bad "the rollback started a second server and left$LEFT running"
fi

# ---- WHEN THE ROLLBACK ITSELF FAILS -------------------------------------
#
# The third outcome, and the only one that needs a human right now, so it has
# to be unmistakable. Arranged the way it really happens: the operator was
# restoring BECAUSE the live database had stopped serving, and the backup does
# not serve either -- so putting it back cannot bring the service up.
echo "== when the rollback ALSO fails, it says the board is down =="
livedb r4 B
mkbak b2 B
if restore "$DIR/b2.db"; then
  t_bad "a restore whose rollback also failed REPORTED SUCCESS"
else
  t_ok "a restore whose rollback also fails is reported as a failure"
fi
if out_has 'BOARD IS DOWN'; then
  t_ok "...and says the BOARD IS DOWN, in those words"
else
  t_bad "...without saying the board is down: $(t_show "$(cat "$DIR/out.txt")")"
fi
setis "$BOARD/sync.db" r4 \
  "...and the previous database is still put back, all three files"
REF=$(one "$BOARD/backups/refused-*.db")
if [ -n "$REF" ] && cmp -s "$REF" "$DIR/b2.db"; then
  t_ok "...and the backup that would not serve is still quarantined"
else
  t_bad "the backup that would not serve was lost on the way"
fi

# ---- AND WHEN THERE IS NOTHING TO GO BACK TO ----------------------------
#
# The first thing a restore is ever used for: a rebuilt board, or new hardware,
# with no database on it at all. Nothing is displaced, so there is no working
# set to reinstall -- and a rollback that ran anyway would quarantine the only
# copy the board has and leave the data directory EMPTY. It has to say that
# instead of doing it.
echo "== a restore onto a board with no database has nothing to roll back =="
rm -f "$BOARD"/sync.db "$BOARD"/sync.db-wal "$BOARD"/sync.db-shm
rm -f "$BOARD"/backups/displaced-* "$BOARD"/backups/refused-*
mkbak b3 B
if restore "$DIR/b3.db"; then
  t_bad "a restore that never came up healthy reported success"
else
  t_ok "a restore onto an empty board that does not serve is a failure"
fi
if out_has 'NO previous database'; then
  t_ok "...and says there was no previous database to put back"
else
  t_bad "...but blames the rollback: $(t_show "$(cat "$DIR/out.txt")")"
fi
if [ -z "$(one "$BOARD/backups/refused-*.db")" ] &&
   cmp -s "$BOARD/sync.db" "$DIR/b3.db"; then
  t_ok "...and leaves the restored file installed rather than emptying the dir"
else
  t_bad "it quarantined the only database the board had"
fi

# ---- WHEN ONE OF THE RENAMES FAILS ---------------------------------------
#
# The reinstall is all-or-nothing, and the file that makes that
# non-negotiable is the -wal: carry on past a failed `mv` of it and the service
# comes up on a database whose most recent transactions were left behind in
# backups/ -- valid, queryable, and missing exactly the rows that arrived last.
# That is the loss restoredrill.sh measures a plain `cp` producing, arrived at
# from the other direction, and NOTHING here could see it: the drill's board
# has no way to make one rename fail.
#
# So it gets one. The board's tools are the drill's to arrange, exactly like its
# ssh (a shell) and its scp (a cp): a `mv` that fails for one named source and
# is /usr/bin/mv for everything else. A permission trick cannot do this job --
# an unwritable backups/ breaks the DISPLACEMENT too, which happens before the
# start and would destroy the live database rather than reach this path.
mkdir -p "$DIR/bin"
REALMV=$(command -v mv)
# SOURCE AND TARGET ARE SEPARATE LEVERS, because every rename in this procedure
# has a partner going the other way and a glob that names one file names both.
# `sync.db-wal` is the SOURCE of the displacement and the TARGET of the
# reinstall; `displaced-<stamp>.db-wal` is the target of the one and the source
# of the other. Matching "any argument" would arrange three renames when the
# case is about one.
cat > "$DIR/bin/mv" <<EOF
#!/bin/sh
if [ -f "\${DRILL_MV_FAIL_SRC:-}" ]; then
   case "\${1:-}" in
      \$(cat "\$DRILL_MV_FAIL_SRC"))
         echo "mv: cannot rename '\$1' (arranged by the drill)" >&2
         exit 1
         ;;
   esac
fi
if [ -f "\${DRILL_MV_FAIL_DST:-}" ]; then
   case "\${2:-}" in
      \$(cat "\$DRILL_MV_FAIL_DST"))
         echo "mv: cannot rename to '\$2' (arranged by the drill)" >&2
         exit 1
         ;;
   esac
fi
exec $REALMV "\$@"
EOF
chmod +x "$DIR/bin/mv"
export PATH="$DIR/bin:$PATH"
export DRILL_MV_FAIL_SRC=$BOARD/mv-fails-src
export DRILL_MV_FAIL_DST=$BOARD/mv-fails-dst

echo "== a rename that fails aborts the rollback rather than losing the -wal =="
livedb r6 L
mkbak b5 B
printf '*displaced-*.db-wal\n' >"$BOARD/mv-fails-src"
if restore "$DIR/b5.db"; then
  t_bad "a rollback whose rename failed REPORTED SUCCESS"
else
  t_ok "a rollback whose rename failed is reported as a failure"
fi
rm -f "$BOARD/mv-fails-src"
if out_has 'ROLLBACK ITSELF FAILED' && out_has 'BOARD IS DOWN'; then
  t_ok "...saying the rollback itself failed and the board is DOWN"
else
  t_bad "...but not in those terms: $(t_show "$(cat "$DIR/out.txt")")"
fi
if alive; then
  t_bad "IT STARTED THE SERVICE on a database whose -wal was left behind"
else
  t_ok "...and did NOT start the service on a half-reinstalled database"
fi
if [ -f "$(one "$BOARD/backups/displaced-*.db-wal")" ]; then
  t_ok "...with the log that could not be moved still in backups, by name"
else
  t_bad "the write-ahead log it could not reinstall is gone"
fi

# ...AND A QUARANTINE THAT CANNOT BE COMPLETED MUST NOT LEAVE THE FOREIGN LOG
# BEHIND. The one state this procedure must never produce, and the only way to
# reach it: the live database had NO write-ahead log (a clean shutdown, or a
# checkpoint just before), so nothing was displaced under that name -- and the
# refused start then wrote one of its own. If moving it aside fails and the code
# shrugs, the database put back lands beside a DIFFERENT database's log, which
# sqlite replays into it. It is unrecoverable and it is silent.
echo "== a -wal that cannot be quarantined is removed, never left behind =="
rm -f "$BOARD"/backups/displaced-* "$BOARD"/backups/refused-*
printf 'L database r7\n' >"$BOARD/sync.db"
rm -f "$BOARD/sync.db-wal" "$BOARD/sync.db-shm"
cp "$BOARD/sync.db" "$DIR/keep-r7.db"
mkbak b6 B
printf '*refused-*.db-wal\n' >"$BOARD/mv-fails-dst"
PIDN=$(pidlines)
if restore "$DIR/b6.db"; then
  t_bad "a backup that will not serve was reported as restored"
else
  t_ok "a backup that will not serve is still reported as a failure"
fi
rm -f "$BOARD/mv-fails-dst"
if cmp -s "$BOARD/sync.db" "$DIR/keep-r7.db"; then
  t_ok "...and the previous database is back"
else
  t_bad "the previous database did not come back"
fi
REFPID=$(pidsafter "$PIDN" | head -1)
if [ -n "$REFPID" ] && grep -q "pid $REFPID\$" "$BOARD/sync.db-wal" 2>/dev/null; then
  t_bad "IT IS BESIDE THE REFUSED START'S -wal: sqlite would merge that in"
else
  t_ok "...and NOT beside the write-ahead log of the start that was refused"
fi

# ...AND THE SAME ASYMMETRY ON THE WAY BACK. The rollback's health wait goes
# through the front door too, so an undeclared one reads as "the previous
# database did not come back" -- and the script would report an outage about a
# board that is up and listening. This is also the case that requires the
# rollback's log mark to be taken BEFORE its restart: taken after, the offset
# sits past the readiness line the branch below asks about, and a board that
# came back up perfectly is reported DOWN. That bug was found once in
# rotate.sh; nothing outside this case can see it here, because every other
# check is tag-based and the tag deliberately replaces the offset.
echo "== a rollback over an undeclared front door is not an outage =="
livedb r5 L
mkbak x1 X
if (PANCRA_FRONT=unset; PANCRA_FRONT_OWNER=unset; restore "$DIR/x1.db"); then
  t_bad "a failed restore over an undeclared front door reported success"
else
  t_ok "a backup the server will not start on at all is a failure"
fi
setis "$BOARD/sync.db" r5 "...and the previous database is back, all three files"
if alive; then
  t_ok "...and the service is running on it again"
else
  t_bad "the rollback left nothing running"
fi
if out_has 'UNDECLARED' && out_has 'listening on'; then
  t_ok "...and the report says it is listening but was not verified from outside"
else
  t_bad "...but does not say what it could not check: \
$(t_show "$(cat "$DIR/out.txt")")"
fi
if out_has 'BOARD IS DOWN'; then
  t_bad "...and called the board DOWN over a missing config line"
else
  t_ok "...and does NOT call the board down over a missing config line"
fi

# ---- A BACKUP THAT IS PUBLISHED AND MAY NOT SURVIVE A POWER CUT ----------
#
# The board renames the verified copy into place and then syncs the directory
# it renamed into. If that sync fails the backup IS there -- readable, complete,
# verified -- and only the survival of its directory entry across a power cut is
# unknown. `sync backup` says so with exit status 2, and the two callers here
# have to treat it as neither of the two answers they already had.
#
# WHY IT MATTERS TO GET THE THIRD ANSWER RIGHT rather than folding it into one
# of the two: read as SUCCESS, an operator goes to bed on an artifact that may
# not be in the directory after the next power cut, and finds out on the one
# morning they ever look. Read as FAILURE, deploy.sh stops with "refusing to
# deploy over unsaved data" and backup.sh files the arrived copy under
# `.unverified` -- both about a backup sitting right there, complete.
echo "== a backup published without a durable directory entry is its own answer =="
mkdir -p "$BOARD/backups"
: > "$BOARD/backups/unsynced"   # the lever fake.c reads; see the backup verb
cp "$DIR/sync-fake" "$DIR/sync-d1"
printf '\n\n' >> "$DIR/sync-d1"   # a different hash, so the backup step runs
chmod +x "$DIR/sync-d1"
if deploy "$DIR/sync-d1"; then
  t_ok "a deploy over a durability-uncertain backup still deploys"
else
  t_bad "a published backup was treated as no backup: $(t_show "$(cat "$DIR/out.txt")")"
fi
if out_has 'DURABILITY UNCERTAIN'; then
  t_ok "...and says the backup's durability is uncertain, in those words"
else
  t_bad "...and never mentions it: $(t_show "$(cat "$DIR/out.txt")")"
fi
if out_has 'refusing to deploy over unsaved data'; then
  t_bad "...and wrongly called a published, verified backup 'unsaved data'"
else
  t_ok "...and does NOT call the data unsaved, because it was saved"
fi

# backup.sh, which until now this drill could not run at all (its copy home
# went through an scp fixture that only understood one direction).
BKOUT=$DIR/backups-home
bkup() { "$HERE/srv/deploy/backup.sh" "$BKOUT" >"$DIR/out.txt" 2>&1; }
BKRC=0
bkup || BKRC=$?
if [ "$BKRC" = 2 ]; then
  t_ok "backup.sh reports a durability-uncertain board copy as its own status"
elif [ "$BKRC" = 0 ]; then
  t_bad "backup.sh reported plain success for a backup that may not survive"
else
  t_bad "backup.sh reported failure ($BKRC) for a backup it had in its hand"
fi
if out_has 'DURABILITY UNCERTAIN ON THE BOARD'; then
  t_ok "...saying which copy is in doubt: the board's, not the one it fetched"
else
  t_bad "...but does not say which: $(t_show "$(cat "$DIR/out.txt")")"
fi
if [ -n "$(one "$BKOUT/sync-*.db")" ]; then
  t_ok "...and the copy it brought home is HERE, which is the whole point"
else
  t_bad "no local copy was kept, so the uncertain one is the only one"
fi
if [ -n "$(one "$BKOUT/sync-*.unverified")" ]; then
  t_bad "...and it filed a good backup as unverified"
else
  t_ok "...and did not file that good backup under .unverified"
fi
# ...and the ordinary case still reports ordinary success, or the status above
# would be pinning nothing: a script that always exits 2 passes every check so
# far. THIS is the assertion that makes the one above mean something.
rm -f "$BOARD/backups/unsynced"
BKRC=0
bkup || BKRC=$?
if [ "$BKRC" = 0 ]; then
  t_ok "...while a backup whose directory WAS synced is plain success"
else
  t_bad "an ordinary backup exited $BKRC: $(t_show "$(cat "$DIR/out.txt")")"
fi
# AND ITS DESTINATION IS NOT A TIMESTAMP ALONE. `sync-<stamp>.db` was built the
# same way here and in deploy.sh, from a stamp good only to the second -- so a
# scheduled backup and a deploy landing in one second wrote one file, and
# underneath it one `<dest>.part` for two sqlite backups to stage into. A torn
# .part that one of them then verifies and publishes is a backup that opens and
# is wrong, which is the only kind that matters.
BKNAME=$(basename "$(one "$BKOUT/sync-*.db")")
case "$BKNAME" in
  sync-*Z-?*.db)
    t_ok "...named so that two in one second cannot collide ($BKNAME)" ;;
  *)
    t_bad "the backup is named by a second-granular stamp alone: $BKNAME" ;;
esac

# ---- ONE OPERATION AT A TIME ---------------------------------------------
#
# THE ONLY VERSION OF THIS TEST THAT IS WORTH ANYTHING IS ONE WHERE THE TWO
# OPERATIONS REALLY OVERLAP. Two scripts started back to back may serialise by
# accident -- on a fast board they usually will -- and a test that passes that
# way passes identically with no lock at all, which is the worst outcome
# available here: a green check on the mechanism that is supposed to stop two
# operators destroying each other's work at 3am.
#
# So the first operation is STOPPED inside its critical section, at a point it
# announces (see hang_until_released in fake.c): restore.sh has taken the lock,
# copied its staged database to the board, and is sitting in the board-side
# verify. Only when the drill has SEEN that announcement does it run the second
# operation -- so the overlap is a fact, not a hope -- and only after the second
# has been refused is the first released.
LOCK=$BOARD/deploy.lock
echo "== two operations at once: the second is refused, not interleaved =="
rm -f "$BOARD/verify-started" "$BOARD/verify-go"
livedb r6 L
mkbak h1 G
: > "$BOARD/hang-verify"
( printf 'YES\n' | "$HERE/srv/deploy/restore.sh" "$DIR/h1.db" \
    >"$DIR/hold.txt" 2>&1 ) &
HOLDPID=$!
i=0
while [ ! -f "$BOARD/verify-started" ] && [ $i -lt 150 ]; do
  sleep 0.2
  i=$((i + 1))
done
if [ -f "$BOARD/verify-started" ]; then
  t_ok "a restore is stopped INSIDE its own operation, so the overlap is real"
else
  t_bad "the drill never got a restore to overlap -- nothing below is a test"
fi
if [ -d "$LOCK" ]; then
  t_ok "...and the board carries a lock while it runs"
else
  t_bad "no lock was taken, so nothing below can be refused"
fi
DBEFORE=$(sha256sum "$BOARD/sync" | awk '{print $1}')
if deploy "$DIR/sync-new"; then
  t_bad "A DEPLOY RAN STRAIGHT THROUGH A RUNNING RESTORE -- the whole item"
else
  t_ok "a deploy started during a restore is refused"
fi
if out_has 'another operation is running on this board'; then
  t_ok "...in words, not as a stray non-zero exit"
else
  t_bad "...but does not say why: $(t_show "$(cat "$DIR/out.txt")")"
fi
# WHO AND SINCE WHEN. "Busy" is what makes a tired operator force it; naming
# the holder is what lets them decide whether to wait or to go and stop it.
if grep -q 'holder:.*restore' "$DIR/out.txt"; then
  t_ok "...naming the operation that holds it"
else
  t_bad "...but not which operation: $(t_show "$(cat "$DIR/out.txt")")"
fi
if grep -q 'since:' "$DIR/out.txt"; then
  t_ok "...and how long it has been held"
else
  t_bad "...and not since when: $(t_show "$(cat "$DIR/out.txt")")"
fi
if out_has 'PANCRA_LOCK_BREAK'; then
  t_ok "...and how to get out of it if the holder is really gone"
else
  t_bad "...leaving no way out but invention: $(t_show "$(cat "$DIR/out.txt")")"
fi
if [ "$DBEFORE" = "$(sha256sum "$BOARD/sync" | awk '{print $1}')" ]; then
  t_ok "...and the refused deploy changed NOTHING on the board"
else
  t_bad "the refused deploy installed something anyway"
fi
# The lock is ONE lock for every verb, not one per script.
if "$HERE/srv/deploy/backup.sh" "$BKOUT" >"$DIR/out.txt" 2>&1; then
  t_bad "a backup ran straight through a running restore"
else
  t_ok "...and so is a backup: one lock, not one lock per script"
fi
if "$HERE/srv/deploy/rollback.sh" >"$DIR/out.txt" 2>&1; then
  t_bad "a rollback ran straight through a running restore"
else
  t_ok "...and so is a rollback"
fi
if out_has 'another operation is running on this board'; then
  t_ok "...refused for the same stated reason, before it chose a release"
else
  t_bad "the rollback failed for some other reason: \
$(t_show "$(cat "$DIR/out.txt")")"
fi

echo "== ...and the lock is released, so the board is not left jammed =="
: > "$BOARD/verify-go"
HOLDRC=0
wait "$HOLDPID" || HOLDRC=$?
rm -f "$BOARD/hang-verify" "$BOARD/verify-started" "$BOARD/verify-go"
if [ "$HOLDRC" = 0 ]; then
  t_ok "the held restore finishes normally once it is let go"
else
  t_bad "the held restore failed ($HOLDRC): $(t_show "$(cat "$DIR/hold.txt")")"
fi
if [ ! -d "$LOCK" ]; then
  t_ok "...and drops the lock on its way out"
else
  t_bad "the lock outlived the operation that took it -- the board is jammed"
fi
if deploy --restart "$DIR/sync-fake"; then
  t_ok "...so the next operation is no longer refused"
else
  t_bad "the board is still locked: $(t_show "$(cat "$DIR/out.txt")")"
fi

# ---- A STUCK LOCK IS ITS OWN OUTAGE -------------------------------------
#
# The holder runs on the OPERATOR'S machine, so the board cannot ask whether it
# is still alive -- the pid in the lock is a pid somewhere else. An ssh that
# dies mid-deploy therefore leaves a directory that nothing living will ever
# remove, and every deploy, rollback, backup and restore after it is refused for
# ever. That is an outage this mechanism would have inflicted on itself, and it
# would be discovered at the moment somebody needed to restore.
echo "== a lock nobody can still be holding is broken, not obeyed for ever =="
mkdir -p "$LOCK"
printf 'restore (ghost@laptop, pid 1)\n' > "$LOCK/owner"
printf 'a-token-from-a-dead-shell\n' > "$LOCK/token"
# Two hours, measured by the BOARD's clock -- the one lock.sh ages it against.
printf '%s\n' "$(( $(date -u +%s) - 7200 ))" > "$LOCK/since"
if deploy --restart "$DIR/sync-fake"; then
  t_ok "a two-hour-old lock does not jam the board for ever"
else
  t_bad "a stale lock jammed every future operation: \
$(t_show "$(cat "$DIR/out.txt")")"
fi
if out_has 'BROKE A STALE LOCK'; then
  t_ok "...and the breaking is announced, never silent"
else
  t_bad "...and broke it silently: $(t_show "$(cat "$DIR/out.txt")")"
fi
if grep -q 'ghost@laptop' "$DIR/out.txt"; then
  t_ok "...naming whose lock it broke, in case they are still out there"
else
  t_bad "...without saying whose: $(t_show "$(cat "$DIR/out.txt")")"
fi
# ...AND A LOCK THAT IS SECONDS OLD IS NOT STALE. Without this the rule above
# is satisfied by "break every lock", which is no lock at all -- and the case
# further up would be the only thing standing between this drill and a
# mechanism that does nothing.
rm -rf "$LOCK"
mkdir -p "$LOCK"
printf 'deploy (someone@else, pid 2)\n' > "$LOCK/owner"
printf 'a-live-token\n' > "$LOCK/token"
date -u +%s > "$LOCK/since"
if deploy --restart "$DIR/sync-fake"; then
  t_bad "a lock taken seconds ago was broken as though it were stale"
else
  t_ok "a lock that is seconds old is refused, not broken"
fi
# ...and the operator who KNOWS the holder is gone does not wait an hour.
#
# `export`, not a bare assignment. deploy() runs a SEPARATE PROCESS, so a plain
# `VAR=1; deploy` sets a variable this shell can see and the script cannot --
# and the case passed for the wrong reason, reporting that the documented way
# out did not work when what did not work was the test. The neighbouring
# front-door cases get away with it only because those names were exported at
# the top of this file, which is not a property to rely on by accident.
if (export PANCRA_LOCK_BREAK=1; deploy --restart "$DIR/sync-fake"); then
  t_ok "...unless the operator says PANCRA_LOCK_BREAK=1, which is the way out"
else
  t_bad "the documented override did not work: \
$(t_show "$(cat "$DIR/out.txt")")"
fi
rm -rf "$LOCK"

# ---- WHAT UNIQUE STAGING NAMES ARE FOR ----------------------------------
#
# The lock makes an overlap impossible. Unique staging names make the DAMAGE
# impossible when the lock has been broken anyway -- which is not hypothetical,
# it is the escape hatch two cases above, used by an operator who was wrong
# about the holder being gone. Belt and braces, deliberately: the lock is the
# rule, this is what happens when somebody overrides the rule at 3am.
#
# `sync.db.restoring` was one name for every restore. Two of them and the second
# scp overwrites the first's staged database, so the first renames the OTHER
# operator's backup into place -- and tells its own operator that the file they
# named is now live. There is no error anywhere; the wrong data is simply
# serving.
echo "== two overlapping operations do not stage at the same name =="
rm -f "$BOARD/verify-started" "$BOARD/verify-go"
livedb r7 L
mkbak o1 G
mkbak o2 G
: > "$BOARD/hang-verify"
( printf 'YES\n' | "$HERE/srv/deploy/restore.sh" "$DIR/o1.db" \
    >"$DIR/hold1.txt" 2>&1 ) &
H1=$!
i=0
while [ ! -f "$BOARD/verify-started" ] && [ $i -lt 150 ]; do
  sleep 0.2
  i=$((i + 1))
done
( printf 'YES\n' | PANCRA_LOCK_BREAK=1 "$HERE/srv/deploy/restore.sh" \
    "$DIR/o2.db" >"$DIR/hold2.txt" 2>&1 ) &
H2=$!
i=0
while [ "$(grep -c . "$BOARD/verify-started" 2>/dev/null || echo 0)" -lt 2 ] &&
      [ $i -lt 150 ]; do
  sleep 0.2
  i=$((i + 1))
done
if [ "$(grep -c . "$BOARD/verify-started" 2>/dev/null || echo 0)" -ge 2 ]; then
  t_ok "two restores are inside their staging step at the same instant"
else
  t_bad "the second restore never overlapped -- nothing below is a test"
fi
if [ -e "$BOARD/sync.db.restoring" ]; then
  t_bad "both restores staged at the ONE shared name; one has lost its backup"
else
  t_ok "...and neither used the shared name that used to be the only one"
fi
STAGEN=0
for p in "$BOARD"/sync.db.restoring-*; do
  [ -e "$p" ] && STAGEN=$((STAGEN + 1))
done
if [ "$STAGEN" = 2 ]; then
  t_ok "...they staged at TWO different paths, one each"
else
  t_bad "$STAGEN staged copies exist where there should be two"
fi
# ...AND EACH HOLDS THE BACKUP ITS OWN OPERATOR NAMED. Counting two files is
# not enough: the damage is not "a file is missing", it is "the file has the
# other operator's data in it", which a count cannot see.
GOT1=0
GOT2=0
for p in "$BOARD"/sync.db.restoring-*; do
  [ -e "$p" ] || continue
  cmp -s "$p" "$DIR/o1.db" && GOT1=1
  cmp -s "$p" "$DIR/o2.db" && GOT2=1
done
if [ "$GOT1$GOT2" = 11 ]; then
  t_ok "...each holding the backup ITS operator named, not the other's"
else
  t_bad "a staged copy holds the wrong backup (o1=$GOT1 o2=$GOT2)"
fi
: > "$BOARD/verify-go"
wait "$H1" 2>/dev/null || true
wait "$H2" 2>/dev/null || true
rm -f "$BOARD/hang-verify" "$BOARD/verify-started" "$BOARD/verify-go"
rm -f "$BOARD"/sync.db.restoring-*
rm -rf "$LOCK"

# ...AND THE EXECUTABLE IS STAGED THE SAME WAY. deploy.sh and rollback.sh both
# staged at `sync.new` and both then renamed that name over the live binary, so
# together they published each other's build -- with the deploy's hash check
# passing, because it hashed the copy before the other script replaced it.
#
# Observed on a deploy that ABANDONS its staged copy: a truncating scp makes the
# hash check refuse, which is the one path that leaves the staged file on the
# board to be looked at. It also exercises a failure BETWEEN taking the lock and
# any restart, which must still release it.
cat > "$DIR/fakescp-short" <<'EOF'
#!/bin/sh
src=$1; dst=$2
head -c 32 "${src#*:}" > "${dst#*:}"
EOF
chmod +x "$DIR/fakescp-short"
rm -f "$BOARD"/sync.new*
if (export PANCRA_SCP=$DIR/fakescp-short; deploy "$DIR/sync-new"); then
  t_bad "a deploy whose copy arrived truncated reported success"
else
  t_ok "a deploy whose copy hashes wrong is refused"
fi
if [ -e "$BOARD/sync.new" ]; then
  t_bad "...but staged at sync.new, the name rollback.sh also renames over"
else
  t_ok "...and did not stage the executable at the shared name sync.new"
fi
STAGEB=0
for p in "$BOARD"/sync.new-*; do
  [ -e "$p" ] && STAGEB=$((STAGEB + 1))
done
if [ "$STAGEB" = 1 ]; then
  t_ok "...it staged under a name of this operation's own"
else
  t_bad "$STAGEB staged executables where there should be one"
fi
if [ ! -d "$LOCK" ]; then
  t_ok "...and released the lock on a failure that never reached a restart"
else
  t_bad "a deploy that failed before restarting left the board locked"
fi
rm -f "$BOARD"/sync.new*

# ---- THE RECOVERY MUST NOT BE REFUSED BY ITS OWN CALLER'S LOCK ----------
#
# deploy.sh runs rollback.sh ITSELF when a new build does not come up healthy,
# and it is holding the board lock the whole time. A child that took the lock
# the ordinary way would be refused by its own parent -- so the single most
# important path in this directory, the automatic recovery from a bad deploy,
# would become "the rollback ALSO failed; the board is DOWN" on every bad
# deploy, caused by the very mechanism added to make deploys safe.
#
# NOTHING EXECUTED THIS PATH BEFORE. The drill had cases for a refused deploy
# and for rollback.sh run by hand, but never for a deploy that installs, starts,
# fails its health check and recovers -- which is the sequence the script's last
# four lines are entirely about.
echo "== a deploy that must roll back is not refused by its own lock =="
PREV=$(sha256sum "$BOARD/sync" | awk '{print $1}')
if deploy "$DIR/sync-lame"; then
  t_bad "a build that starts and never serves was reported healthy"
else
  t_ok "a build that starts and serves nothing is not reported healthy"
fi
if out_has 'another operation is running on this board'; then
  t_bad "THE DEPLOY'S OWN ROLLBACK WAS REFUSED BY THE DEPLOY'S OWN LOCK"
else
  t_ok "...and its automatic rollback was not refused by its own lock"
fi
if out_has 'rollback: restoring'; then
  t_ok "...the rollback really ran, rather than being skipped"
else
  t_bad "the rollback never ran: $(t_show "$(cat "$DIR/out.txt")")"
fi
if [ "$(sha256sum "$BOARD/sync" | awk '{print $1}')" = "$PREV" ]; then
  t_ok "...and the previous build is back on the board"
else
  t_bad "the board is left running the build that would not serve"
fi
if alive; then
  t_ok "...and the service is running on it"
else
  t_bad "the rollback left nothing running"
fi
if [ ! -d "$LOCK" ]; then
  t_ok "...and the lock is dropped even though the deploy FAILED"
else
  t_bad "a failed deploy left the board locked -- the next one cannot run"
fi

# ---- WHO STARTS IT AGAIN ------------------------------------------------
#
# Everything above this point is a deploy, a rollback, a rotation or a restore
# -- an operator, at a keyboard, doing something on purpose. None of it can
# answer the two questions a service is actually judged by:
#
#   THE SERVER EXITED AND NOBODY WAS AWAKE. Until now nothing on the board
#   restarted it. The pid file went on naming the dead process, the front door
#   went on forwarding to a port with nothing behind it, and the recovery --
#   which exists, and works, and is `make duodeploy` -- waited for somebody to
#   notice.
#   THE BOARD REBOOTED. Nothing started the server at all, because nothing in
#   any startup script had ever heard of it: the deploy starts a process over
#   ssh, and a process is not a service.
#
# A TEST THAT ASSERTS A UNIT FILE WAS WRITTEN PROVES NEITHER OF THOSE. So
# every case below kills something and then asks the board a question: is it
# back, is it the binary we installed, and did it come back because something
# on the board brought it back rather than because this drill did.
#
# THE BOARD, FOR THESE CASES, GROWS A BOOT SCRIPT -- with the shape that
# matters. The real one ends by handing its process over to the board's other
# service with `exec`, so a launch line APPENDED to it is in the file, visible
# in review, and never reached. The reboot case below runs this script the way
# init does, and would pass just as happily against an appended block if the
# script did not end the way the real one does.
HOOK=$BOARD/boot.sh
cat > "$HOOK" <<EOF
#!/bin/sh
# The board's own startup script: what the init chain ends in. It starts the
# board's other service and then becomes it.
printf 'the board booted\n' >> '$BOARD/boot.log'
exec sleep 30
EOF
chmod +x "$HOOK"

sup_alive() {
  [ -f "$BOARD/supervise.pid" ] &&
    kill -0 "$(cat "$BOARD/supervise.pid" 2>/dev/null)" 2>/dev/null
}
# The server's pid, and whether the process holding it is running the binary
# that is installed -- /proc/<pid>/exe, the same question health.sh's exe_is
# asks over ssh. "Something is alive" is not "the service is back".
srv_pid() { cat "$BOARD/sync.pid" 2>/dev/null; }
exe_matches() {
  em_p=$(srv_pid)
  [ -n "$em_p" ] || return 1
  [ "$(sha256sum "/proc/$em_p/exe" 2>/dev/null | awk '{print $1}')" \
    = "$(sha256sum "$BOARD/sync" | awk '{print $1}')" ]
}
# Up to $1 seconds for a live server whose pid is not $2.
wait_alive() {
  wa_i=0
  while [ "$wa_i" -lt "$1" ]; do
    if alive && [ "$(srv_pid)" != "${2:-}" ]; then return 0; fi
    sleep 1
    wa_i=$((wa_i + 1))
  done
  return 1
}
# How many of the processes this board has ever started are still SERVERS. Every
# start prints its own pid into the log, so this counts processes rather than
# pid files -- which is the only way to see the failure a fighting supervisor
# causes: two servers on one data directory, with the pid file naming one.
#
# AND EACH PID IS CHECKED FOR IDENTITY, not merely for life, for the reason
# this whole section is about. By this point the drill has started thirty-odd
# processes on a busy machine, most of them long dead, and the kernel hands
# those numbers back out -- to a shell, to a sleep, to the next start. `kill
# -0` on a recycled pid succeeds exactly as happily as on the right one, so
# counting that way reports two servers on a board running one, at random, in
# maybe one run in three. /proc/<pid>/exe is what tells them apart; it reads
# `<root>/sync` for a live server and `<root>/sync (deleted)` for one whose
# executable a deploy has since renamed over, and both of those are servers.
servers_alive() {
  sa_n=0
  for sa_p in $(grep -o 'pancra pid [0-9]*' "$BOARD/sync.log" |
                  awk '{print $3}' | sort -u); do
    kill -0 "$sa_p" 2>/dev/null || continue
    case "$(readlink "/proc/$sa_p/exe" 2>/dev/null)" in
      "$BOARD/sync"*) sa_n=$((sa_n + 1)) ;;
    esac
  done
  echo "$sa_n"
}
suplog() { grep -q "$1" "$BOARD/supervise.log" 2>/dev/null; }

echo "== nothing supervises the board until the deployment says so =="
# Every deploy above ran with PANCRA_SUPERVISOR=none, which is a declaration
# and not an accident -- and a deploy that SUCCEEDS under it has to have SAID
# so, because a board that recovers from nothing is a thing an operator must be
# told about rather than left to infer from the absence of a message.
if deploy --restart "$DIR/sync-fake"; then
  t_ok "a deploy with supervision declined succeeds"
else
  t_bad "the deploy failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
if [ ! -e "$BOARD/supervise.sh" ]; then
  t_ok "...and PANCRA_SUPERVISOR=none installs no watchdog"
else
  t_bad "a watchdog was installed on a board that asked for none"
fi
if out_has 'NOTHING SUPERVISES THIS SERVER'; then
  t_ok "...and says so: the board recovers from nothing"
else
  t_bad "an unsupervised deployment reported nothing: $(t_show "$(cat "$DIR/out.txt")")"
fi

# From here on the board is supervised. The mechanism is the CONTRACT'S
# DEFAULT, reached by unsetting the override rather than by asking for it --
# so what is exercised below is what a board gets when nobody said anything,
# which is the configuration every real deployment starts from.
unset PANCRA_SUPERVISOR
export PANCRA_BOOT_HOOK=$HOOK
# The watchdog's own timings, from the contract like everything else. Seconds
# rather than the real board's minutes: this is a drill, and a five-second poll
# with a thirty-second readiness wait would make the cases below a four-minute
# test that people would then take out of `make check`.
export PANCRA_SUPERVISE_INTERVAL=1
export PANCRA_SUPERVISE_READY=2
export PANCRA_SUPERVISE_BACKOFF=8
export PANCRA_SUPERVISE_GRACE=60
# THE REAL STOP AND THE REAL START, used below to arrange what a deploy does to
# the service. Sourced rather than imitated: the whole question in the
# "deliberate stop" cases is whether the marker the real stop_block raises is
# the one the real watchdog obeys, and a drill that wrote that marker itself
# would be testing its own idea of the protocol.
. "$HERE/srv/deploy/pancra.conf"
. "$HERE/srv/deploy/start.sh"
board_stop() {
  $PANCRA_SSH board "sh -s" <<EOF
$(stop_block)
EOF
}
board_start() {
  bs_tag=$(start_tag)
  $PANCRA_SSH board "sh -s" <<EOF
$(start_block "$bs_tag")
EOF
}

echo "== a deploy installs supervision, and it is a PROCESS, not a file =="
if deploy --restart "$DIR/sync-fake"; then
  t_ok "a deploy with supervision declared succeeds"
else
  t_bad "the deploy failed: $(t_show "$(cat "$DIR/out.txt")")"
fi
if [ -x "$BOARD/supervise.sh" ] && [ -f "$BOARD/supervise.env" ]; then
  t_ok "...the watchdog and its generated configuration are on the board"
else
  t_bad "the watchdog was not installed"
fi
if sup_alive; then
  t_ok "...AND IT IS RUNNING -- installed is not supervised"
else
  t_bad "the watchdog was installed and is not running: \
$(t_show "$(cat "$BOARD/supervise.log" 2>/dev/null || echo none)")"
fi
# THE START COMMAND ON THE BOARD IS A GENERATED COPY, not a second definition.
# It has to carry this deployment's own port and paths, or the watchdog would
# restart the server on the compiled-in defaults -- which is precisely what the
# fourth hand-written copy of the start command did before start.sh existed.
if grep -q "$BOARD/sync" "$BOARD/supervise.env" &&
   grep -q '@PANCRA_TAG@' "$BOARD/supervise.env"; then
  t_ok "...and its start command is generated from start.sh, tag and all"
else
  t_bad "the watchdog's start command is not the one start.sh emits"
fi

echo "== the boot hook goes where a script that ends in exec will reach it =="
if [ "$(grep -c '^# >>> pancra supervisor >>>' "$HOOK")" = 1 ]; then
  t_ok "the boot hook carries exactly one pancra block"
else
  t_bad "$(grep -c '^# >>> pancra supervisor >>>' "$HOOK") blocks in the boot hook"
fi
BLKLINE=$(grep -n '^# >>> pancra supervisor >>>' "$HOOK" | head -1 | cut -d: -f1)
EXECLINE=$(grep -n '^exec ' "$HOOK" | head -1 | cut -d: -f1)
if [ -n "$BLKLINE" ] && [ -n "$EXECLINE" ] && [ "$BLKLINE" -lt "$EXECLINE" ]; then
  t_ok "...ABOVE the exec that ends the script, so the boot reaches it"
else
  t_bad "the block is at line ${BLKLINE:-none}, the exec at ${EXECLINE:-none}"
fi
if [ "$(head -1 "$HOOK")" = '#!/bin/sh' ]; then
  t_ok "...and below the #! line, which still has to be the first line"
else
  t_bad "the shebang is no longer first: $(head -1 "$HOOK")"
fi
if deploy --restart "$DIR/sync-fake" &&
   [ "$(grep -c '^# >>> pancra supervisor >>>' "$HOOK")" = 1 ]; then
  t_ok "...and deploying again REPLACES that block rather than stacking one"
else
  t_bad "a second deploy left $(grep -c '^# >>> pancra supervisor >>>' "$HOOK") \
blocks in somebody else's boot script"
fi

echo "== THE CASE THIS ITEM EXISTS FOR: the server exits, and comes BACK =="
GONE=$(srv_pid)
kill -9 "$GONE" 2>/dev/null || true
if wait_alive 25 "$GONE"; then
  t_ok "AN UNEXPECTED EXIT IS RECOVERED WITHOUT A DEPLOY"
else
  t_bad "the server was killed and nothing brought it back -- the item: \
$(t_show "$(cat "$BOARD/supervise.log" 2>/dev/null || echo 'no watchdog log')")"
fi
if exe_matches; then
  t_ok "...as the binary that is installed, checked through /proc/<pid>/exe"
else
  t_bad "something came back and it is not the installed binary"
fi
# IDENTITY-BOUND HEALTH, in the sense health.sh established: the readiness line
# has to belong to THIS start. The log is append-only, so a watchdog that
# grepped it whole would count the readiness line of the start that died as
# evidence that its own start had worked -- and would then stop backing off.
BTAG=$(grep -o '=== pancra watchdog-[^ ]*' "$BOARD/sync.log" | tail -1 |
       awk '{print $3}')
if [ -n "$BTAG" ]; then
  t_ok "...under a banner of its own ($BTAG), not inside the dead start's"
else
  t_bad "the watchdog's start left no banner, so its readiness line is unattributable"
fi
if sed -n "/=== pancra $BTAG ===/,/=== pancra /p" "$BOARD/sync.log" |
     grep -q 'listening on port'; then
  t_ok "...with THIS start's own readiness line under it"
else
  t_bad "no readiness line belongs to the watchdog's own start"
fi
if suplog 'the service is back'; then
  t_ok "...and the watchdog log says what it did and when"
else
  t_bad "the recovery is not in $BOARD/supervise.log"
fi

echo "== a deliberate stop is NOT fought: the marker the stop itself raises =="
# What a deploy, a rotation and a restore each do for a second or two. The
# marker comes from the real stop_block, and nothing else here is arranged --
# no lock -- so this case is about the marker alone.
board_stop
if [ -f "$BOARD/service.down" ]; then
  t_ok "the shared stop announces itself, so a watchdog can tell it from a crash"
else
  t_bad "nothing marks a deliberate stop; the watchdog cannot tell it from a crash"
fi
sleep 4
if ! alive; then
  t_ok "...AND THE WATCHDOG LEAVES IT DOWN while the operation holds it there"
else
  t_bad "THE WATCHDOG RESTARTED A SERVICE AN OPERATION HAD STOPPED -- two \
servers on one data directory is what that becomes"
fi
if suplog 'standing by'; then
  t_ok "...saying so once, not once per poll"
else
  t_bad "it stood by silently, so an operator reading the log cannot tell why"
fi
board_start
if wait_alive 15; then
  t_ok "...and the operation's own start is the one that brings it back"
else
  t_bad "the service did not come back when the operation started it"
fi
if [ ! -f "$BOARD/service.down" ]; then
  t_ok "...which clears the marker, so the next exit is a crash again"
else
  t_bad "the marker outlived the stop: the next real crash is ignored"
fi

echo "== ...and a held board lock is obeyed even with no marker at all =="
# The second gate, tested alone: an operation that is holding the lock while
# the service happens to be down -- a deploy between its stop and its start,
# whose marker this drill deliberately does not raise here.
mkdir -p "$LOCK"
printf 'deploy (drill@here, pid %s)\n' "$$" > "$LOCK/owner"
printf 'a-drill-token\n' > "$LOCK/token"
printf '%s\n' "$(date -u +%s)" > "$LOCK/since"
LOCKED=$(srv_pid)
kill -9 "$LOCKED" 2>/dev/null || true
sleep 4
if ! alive; then
  t_ok "an operation holding the lock is not raced by the watchdog"
else
  t_bad "the watchdog started a server in the middle of a locked operation"
fi
rm -rf "$LOCK"
if wait_alive 25 "$LOCKED"; then
  t_ok "...and the moment the lock is dropped, the service comes back"
else
  t_bad "the service stayed down after the operation finished"
fi

echo "== a stop that was never finished is not obeyed for ever =="
# THE OUTAGE A SUPERVISOR CAN INFLICT ON ITSELF. An operation whose ssh dies
# between its stop and its start leaves the marker raised and the service down.
# A watchdog that obeyed that for ever would be the reason the board stayed
# down -- the exact failure it was installed to end. The marker carries the
# board's clock so it can be aged; here it is backdated, exactly as the stale
# lock case above backdates the lock.
board_stop
printf '%s\n' "$(( $(date -u +%s) - 4000 ))" > "$BOARD/service.down"
if wait_alive 25; then
  t_ok "an abandoned stop marker expires and the service comes back"
else
  t_bad "a marker left by a dead operation kept the board down for ever"
fi
if suplog 'no longer a deliberate stop'; then
  t_ok "...and the watchdog says it overrode a marker, rather than doing it quietly"
else
  t_bad "it started the service without saying it had ignored the marker"
fi

echo "== ...and neither is a lock nobody can still be holding =="
mkdir -p "$LOCK"
printf 'restore (ghost@laptop, pid 1)\n' > "$LOCK/owner"
printf '%s\n' "$(( $(date -u +%s) - 7200 ))" > "$LOCK/since"
STUCK=$(srv_pid)
kill -9 "$STUCK" 2>/dev/null || true
if wait_alive 25 "$STUCK"; then
  t_ok "a two-hour-old lock does not keep the service down for ever either"
else
  t_bad "a stale lock left the board down: $(t_show "$(cat "$BOARD/supervise.log")")"
fi
rm -rf "$LOCK"

echo "== A REBOOT: the pid file survives it and the processes do not =="
# WHAT A POWER CUT LEAVES. Both processes are gone; both pid files are still on
# disk, and both name numbers the kernel has since handed to something else --
# which on a freshly booted board it does, immediately, because the low numbers
# go out first. A watchdog that tested those pids with `kill -0` alone would
# find them alive, conclude that the server and a previous watchdog were both
# running, and do nothing at all -- failing at the one job it was installed
# for, in the one case it was installed for.
REBOOTPID=$(cat "$BOARD/supervise.pid" 2>/dev/null)
kill -9 "$REBOOTPID" 2>/dev/null || true
kill -9 "$(srv_pid)" 2>/dev/null || true
sleep 1
# The heir to both numbers: a live process that is not ours.
sleep 300 &
STRAY=$!
printf '%s\n' "$STRAY" > "$BOARD/sync.pid"
printf '%s\n' "$STRAY" > "$BOARD/supervise.pid"
mkdir -p "$BOARD/supervise.pid.claim"
printf '%s\n' "$STRAY" > "$BOARD/supervise.pid.claim/pid"
rm -f "$BOARD/boot.log"
# ...and init runs the board's startup script, which is the only thing that
# happens here. Nothing in this drill starts a server or a watchdog.
sh "$HOOK" >/dev/null 2>&1 &
HOOKPID=$!
if wait_alive 30 "$STRAY"; then
  t_ok "THE SERVICE COMES BACK AFTER A REBOOT, from the board's own boot script"
else
  t_bad "the board rebooted and the service did not come back: \
$(t_show "$(cat "$BOARD/supervise.log" 2>/dev/null || echo 'no watchdog log')")"
fi
if exe_matches; then
  t_ok "...running the installed binary, not whatever the stale pid named"
else
  t_bad "what came back is not the installed binary"
fi
if kill -0 "$STRAY" 2>/dev/null; then
  t_ok "...and the process that inherited the stale pid was NOT signalled"
else
  t_bad "THE WATCHDOG KILLED AN UNRELATED PROCESS that held a recycled pid"
fi
if [ -f "$BOARD/boot.log" ]; then
  t_ok "...and the boot script still did its own work as well as ours"
else
  t_bad "the installed block broke the board's startup script"
fi
kill "$STRAY" 2>/dev/null || true
kill "$HOOKPID" 2>/dev/null || true
pkill -P "$HOOKPID" 2>/dev/null || true

echo "== a server that will not start is retried, and NOT in a tight loop =="
# A certificate the server refuses -- the fake's lever, and on the real board
# the commonest version of this: a board with no RTC comes up with a clock from
# 1970, the certificate is not yet valid, and every start fails until ntpd
# catches up. The two wrong answers are a restart every second (which fills the
# log and the filesystem the database lives on) and giving up (which leaves the
# board down after the cause has gone away).
cp "$PANCRA_CERT" "$DIR/cert.good"
printf 'X-not-a-certificate\n' > "$PANCRA_CERT"
BOFF_START=$(grep -c '=== pancra ' "$BOARD/sync.log")
kill -9 "$(srv_pid)" 2>/dev/null || true
sleep 16
BOFF_TRIES=$(( $(grep -c '=== pancra ' "$BOARD/sync.log") - BOFF_START ))
if [ "$BOFF_TRIES" -ge 2 ]; then
  t_ok "it keeps trying after a start that fails ($BOFF_TRIES attempts)"
else
  t_bad "it gave up after $BOFF_TRIES attempt(s); the cause is usually fixed \
from outside, and then nothing would bring the service back"
fi
if [ "$BOFF_TRIES" -le 5 ]; then
  t_ok "...but backs off rather than restarting every ${PANCRA_SUPERVISE_INTERVAL}s"
else
  t_bad "$BOFF_TRIES attempts in 16s is a tight restart loop"
fi
BOFF_SEQ=$(grep -o 'backing off [0-9]*s' "$BOARD/supervise.log" | tail -3 |
           awk '{print $3}' | tr '\n' ' ')
BOFF_1=$(printf '%s' "$BOFF_SEQ" | awk '{print $1}' | tr -dc '0-9')
BOFF_N=$(printf '%s' "$BOFF_SEQ" | awk '{print $NF}' | tr -dc '0-9')
if [ -n "$BOFF_1" ] && [ -n "$BOFF_N" ] && [ "$BOFF_N" -gt "$BOFF_1" ]; then
  t_ok "...and the delay GROWS between attempts ($BOFF_SEQ)"
else
  t_bad "the delay does not grow: '$BOFF_SEQ'"
fi
cp "$DIR/cert.good" "$PANCRA_CERT"
if wait_alive 30; then
  t_ok "...and when the cause is fixed it recovers by itself, with no deploy"
else
  t_bad "the watchdog had given up: the board stayed down after the fix"
fi

echo "== a start that lives and never says it is listening is a FAILED start =="
# THE ONE THING `kill -0` CANNOT SEE. A process that comes up and gets stuck
# before it serves is alive, and its readiness line is missing -- but the log is
# append-only, so a check that greps the whole file finds the line the PREVIOUS
# start wrote and calls this start healthy. The watchdog would then stop backing
# off, and a board stuck in this state would be reported as recovered.
: > "$BOARD/never-ready"
NR_BEFORE=$(grep -c 'did not come up healthy' "$BOARD/supervise.log")
NR_KILLED=$(srv_pid)
kill -9 "$NR_KILLED" 2>/dev/null || true
sleep 8
if [ "$(grep -c 'did not come up healthy' "$BOARD/supervise.log")" -gt "$NR_BEFORE" ]
then
  t_ok "a start with no readiness line of its OWN is counted as failed"
else
  t_bad "the watchdog read an earlier start's readiness line as its own: $(t_show "$(tail -3 "$BOARD/supervise.log")")"
fi
rm -f "$BOARD/never-ready"
# ...and the stuck process is not left behind to be counted as a second server
# below. It is killed HERE, by this drill, because the watchdog will not: it
# starts what is gone and never signals what is there.
for nr_p in $(grep -o 'pancra pid [0-9]*' "$BOARD/sync.log" | awk '{print $3}' |
                sort -u); do
  case "$(readlink "/proc/$nr_p/exe" 2>/dev/null)" in
    "$BOARD/sync"*) kill -9 "$nr_p" 2>/dev/null || true ;;
  esac
done
if wait_alive 30; then
  t_ok "...and once it can serve again, the watchdog brings it back"
else
  t_bad "the service did not come back after the stuck start was cleared"
fi

echo "== a deploy runs THROUGH a live watchdog without a second server =="
# The end-to-end shape of the whole rule: a real deploy, with a real watchdog
# polling every second, stops the service on purpose in the middle. If the two
# fight, the board ends up with two servers sharing one data directory and a
# pid file naming whichever start wrote it last -- and every later stop kills
# one of the two.
if [ "$(servers_alive)" = 1 ]; then
  t_ok "exactly one server is running before the deploy"
else
  t_bad "$(servers_alive) servers were already running"
fi
if deploy "$DIR/sync-new"; then
  t_ok "a deploy succeeds with a watchdog watching the service it restarts"
else
  t_bad "the deploy failed under supervision: $(t_show "$(cat "$DIR/out.txt")")"
fi
if [ "$(servers_alive)" = 1 ]; then
  t_ok "...and there is STILL exactly one server afterwards"
else
  t_bad "$(servers_alive) servers are running: the watchdog raced the deploy"
fi
if alive && exe_matches; then
  t_ok "...the one the pid file names, running the binary just installed"
else
  t_bad "the pid file does not name a live process running the new binary"
fi

echo "== supervision can be turned off deliberately, and says so =="
"$BOARD/supervise.sh" stop "$BOARD/supervise.env" >"$DIR/out.txt" 2>&1 || true
if ! sup_alive; then
  t_ok "the watchdog stops when it is told to"
else
  t_bad "the watchdog could not be stopped: $(t_show "$(cat "$DIR/out.txt")")"
fi
if alive; then
  t_ok "...WITHOUT stopping the server, which is the operator's own verb"
else
  t_bad "stopping the watchdog took the service down with it"
fi
if suplog 'NOTHING is supervising'; then
  t_ok "...and the log says the board is on its own again"
else
  t_bad "supervision was turned off silently"
fi
# The board goes back to the way the rest of this drill expects to find it: no
# watchdog, so the epilogue's deploy is the only thing acting on the service.
export PANCRA_SUPERVISOR=none
unset PANCRA_BOOT_HOOK

# The board is left as an operator would want to find it: whatever the drill
# did to it, an ordinary deploy still works afterwards.
if deploy --restart "$DIR/sync-fake"; then
  t_ok "and the board can still be deployed to after all of that"
else
  t_bad "the board is not deployable afterwards: $(t_show "$(cat "$DIR/out.txt")")"
fi
if alive; then t_ok "...and the service is running"; else t_bad "nothing is running"; fi

# Nothing here makes HTTP requests, but t_end is how every suite that
# sources testlib.sh ends: it is where a failure raised inside a subshell --
# where `fail=1` cannot escape -- becomes the verdict.
t_end
if [ "$fail" = 0 ]; then
  printf '\033[1;32mdeploydrill\033[0m: deploy, rotate and restore each leave the board SERVING\n'
else
  printf 'deploydrill: FAILED\n'
fi
exit $fail
