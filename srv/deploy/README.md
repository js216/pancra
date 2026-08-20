# Deploying Pancra's server

Everything here reads one file: [`pancra.conf`](pancra.conf). It names the
host, the executable, the data directory, the pid file, the log, the port, the
certificate and what "healthy" looks like. Nothing else in the repository may
name a path on the board — if you find one, that is the defect.

Every value is overridable from the environment, so a second board or a
staging instance needs no edit:

    PANCRA_HOST=duo2 PANCRA_ROOT=/srv/pancra make duodeploy

## The front door

The server listens on `PANCRA_PORT` (8443). The world asks for `PANCRA_URL`
on `PANCRA_PUBLIC_PORT` (443). **Something maps one to the other**, and for a
long time this repository never said what: the top-level README said "no
proxy", the health check probed the public name, and step 4 of *Moving the
root* below told you to point a reverse proxy at the new port. Three files,
three different pictures, and the component holding the deployment together
appeared in none of the procedures.

It is now declared in `pancra.conf`:

    PANCRA_FRONT=direct | nat | proxy:<name>
    PANCRA_FRONT_OWNER=<who maintains it>
    PANCRA_PUBLIC_PORT=443

The distinction that matters is **who owns the public certificate**. With
`direct` or `nat` it is this server's `PANCRA_CERT`/`PANCRA_KEY`, and
*Rotating the certificate* below is the whole story. With `proxy:<name>` the
proxy terminates TLS for the public name, and `PANCRA_CERT`/`PANCRA_KEY` are a
*backend* pair the proxy trusts: rotating the public certificate is that
component's procedure, not this one, and doing only what is written below
would leave the public name serving an expired certificate.

**Every verb has a step for it:**

| Verb | What the front door needs |
|---|---|
| deploy | Nothing, *if the port is unchanged*. `PANCRA_PORT` is what the front door forwards to — change it and the mapping must change in the same maintenance window, or the health check fails on a server that is running perfectly. |
| rollback | Nothing, unless the release you are rolling back to listened on a different port. Then the mapping must go back too; a rollback that is healthy on the board and unreachable from outside is this. |
| backup | Nothing to copy — but the *configuration* of a `nat` or `proxy` front door lives outside this repository and outside these backups. A board restored from `backups/` onto new hardware is not reachable until that mapping is recreated. Whoever owns it is named in `PANCRA_FRONT_OWNER` for that reason. |
| failure | `wait_healthy_since` reports the process and the public URL separately. Process up + URL down = this server's TLS, **or** the front door. The failure message prints which front door is configured so the next place to look is named, not guessed. |

Undeclared is not a working state: `wait_healthy_since` refuses to report
anything verified while `PANCRA_FRONT` is `unset`. The checks all traverse the
front door, so a pass with nothing declared is resting on a component nobody
has written down — which is exactly the situation this section exists to end.

**Where the refusal happens matters, and it is not the same in every verb.**
`deploy.sh` asks *before it copies anything* (`require_front`), so an
undeclared front door costs you a re-run and nothing else. It used to ask only
through `wait_healthy_since`, which runs after the swap and the restart — so a
perfectly good build was installed, refused, **automatically rolled back**, and
then reported as "the board is DOWN" while the board was up and serving. A
topology nobody has written down is a reason not to start; it is not evidence
about the build, and it is never an outage.

`rotate.sh` asks before it copies too, and for a sharper reason: its answer to
"the service is not healthy" is to **put the old certificate back**. Reached
through `wait_healthy_since`, a missing config line would present as "the new
certificate does not serve", so the script would revert a rotation that had
worked perfectly and then report the board down twice over.

`rollback.sh` and `restore.sh` deliberately do **not** have that precondition.
They run when something is already wrong, and a recovery that refuses to
recover because a variable was never filled in is worse than the missing
variable. They install, start, and then say exactly what they could not do:
the service is listening, it was not verified from outside, and the board is
*not* known to be down. `restore.sh` asks it **twice** for that reason — before
deciding to roll a restore back, and again after rolling one back — because
either answer taken as "unreachable" turns a missing config line into a
destroyed restore or an invented outage. `make deploydrill` pins all of this —
deploycheck can only grep for the words.

## What "healthy" is allowed to mean

A deploy used to collect four facts about the board — the sha256 of the file it
copied, a pid that answers `kill -0`, a readiness line somewhere in the log,
and a public page containing the marker — and report success when all four held.
**Nothing required them to be about the same program.**

- Replacing a running executable by rename leaves the old inode open. A start
  that fails while the previous process survives gives a live pid, a readiness
  line, and a new binary on disk that nothing is running. `exe_is` hashes
  `/proc/<pid>/exe`, which follows the inode rather than the path, so the pid
  and the artifact are one fact.
- The log is append-only, so a byte offset taken before the restart stops being
  a reliable divider the moment anything truncates or rotates it. Every start
  now writes `=== pancra start-<pid>-<random> ===` before launching, and the
  readiness line has to fall between *that* banner and the next one. The banner
  replaces the offset rather than joining it: a rotation between the mark and
  the check makes the offset skip past this start's own banner, which would
  fail a service that came up perfectly. Ending the range at the next banner is
  the other half — `,$p` would let a *later* start's readiness line answer for
  this one, which is the same wrong-start bug from the other side.
- The public name serving a page proves *some* backend is up — a stale NAT rule
  or a proxy still pointing at a retired instance satisfies it just as well.
  `PANCRA_BACKEND_PROBE` asks this listener directly -- run ON the board,
  against 127.0.0.1, because `PANCRA_HOST` is an ssh destination and curl does
  not read ~/.ssh/config. Both must answer.

`make deploydrill` arranges each of these failures on a fake board — including a
live, banner-tagged, backend-answering process running a *different* executable
from the one on disk — and requires the deploy to refuse.

## The five verbs

| Command | What it does | Touches the database? |
|---|---|---|
| `make duodeploy` | build, copy, swap, restart, health-check, roll back on failure | takes a backup first; otherwise no |
| `make duosmoke` | check that what is running is what we built, and answers HTTPS | no |
| `make duobackup` | WAL-safe copy of the live database, verified, brought home | reads it |
| `make duorollback HASH=<sha>` | reinstall a previous executable and restart | no |
| `make duorotate CERT=<c> KEY=<k>` | validate a certificate/key pair, swap it in, restore the old one if it does not serve | no |

Restoring a backup is deliberate and separate: `./srv/deploy/restore.sh
<backup.db>`. It asks for confirmation, because it destroys everything synced
since that backup was taken — and it puts the displaced database back, complete
with its `-wal` and `-shm`, if the restored one does not serve. See *Putting a
backup back, and putting it back back*.

## One operation at a time

Every one of those verbs, and `restore.sh`, takes a **board-wide lock** before
it changes anything and holds it until its final health verdict or its
rollback. Two at once is not exotic — it is what an incident looks like, with
one person restoring because the data is wrong and another re-deploying the
last good build because the service is down. Unlocked, those two shared the
staged artifact name, the pid file, the running process and the live database:
they published each other's binary, killed each other's server, and interleaved
a database move, each reporting success.

    PANCRA_LOCK=$PANCRA_ROOT/deploy.lock     # a directory; mkdir is the atom
    PANCRA_LOCK_STALE=3600                   # seconds before it is assumed dead

A refusal names **who holds it and since when**, because "busy" is what makes a
tired operator force it:

    deploy: FAILED: another operation is running on this board.
      holder: restore (jk@laptop, pid 31337)
      since:  42s ago (board clock)

**A stuck lock is its own outage.** The holder runs on *your* machine, not the
board, so the board cannot ask whether it is still alive — an ssh that dies
mid-deploy would otherwise jam every future operation for ever. So a lock older
than `PANCRA_LOCK_STALE` is broken automatically and loudly, and
`PANCRA_LOCK_BREAK=1` is the override for when you already know the holder is
gone. Both announce what they broke and whose it was.

`deploy.sh` runs `rollback.sh` itself when a build does not come up healthy;
that child re-enters the same lock by token rather than deadlocking on its
parent.

## Supervision, and coming back after a reboot

**Nothing used to restart this server.** The deploy started a process over ssh
and wrote its pid to a file, and that was the whole arrangement:

* the server exits — a fatal signal, an OOM kill on a board with 56 MB of RAM —
  and it stays exited. The pid file goes on naming the dead process, the front
  door goes on forwarding to a port with nothing behind it, and the recovery
  (`make duodeploy`, which restarts a dead service) waits for a person who has
  noticed.
* the board reboots and the server is not there at all. No startup script had
  ever heard of it, because the only thing that had ever started it runs on
  your laptop.

Both are now covered by one small program on the board and one guarded block in
the board's own startup script.

    PANCRA_SUPERVISOR=watchdog              # or: none
    PANCRA_BOOT_HOOK=unset                  # a script the board runs at boot
    PANCRA_SUPERVISE=$PANCRA_ROOT/supervise.sh
    PANCRA_SUPERVISE_ENV=$PANCRA_ROOT/supervise.env
    PANCRA_SUPERVISE_LOG=$PANCRA_ROOT/supervise.log
    PANCRA_SUPERVISE_INTERVAL=5             # how often it looks
    PANCRA_SUPERVISE_READY=30               # how long a start has to prove itself
    PANCRA_SUPERVISE_BACKOFF=300            # the ceiling on the retry delay
    PANCRA_SUPERVISE_GRACE=600              # how long a deliberate stop is obeyed
    PANCRA_DOWN=$PANCRA_ROOT/service.down   # "this stop was asked for"

### Why not a unit file

Because the board is not a systemd machine, and this repository cannot know
what any other board is either. The Milk-V Duo this deployment targets runs a
buildroot image with BusyBox init: no `systemctl`, no unit directory, and a
boot chain (`inittab` → `rcS` → `/etc/init.d/S99user` → the board's own
`auto.sh`) that ends in a script belonging to the login user. A deployment that
wrote a unit file would have installed supervision that never runs — and
reported success.

So the mechanism is **declared**, exactly as the front door is. The default,
`watchdog`, needs nothing from the board at all: no init system, no root, no
cron. A board that *does* have a service manager can use it — set
`PANCRA_SUPERVISOR=none` and point that manager at **`supervise.sh`**, never at
the server directly. `srv/deploy/start.sh` holds the only copy of the start
command; a unit file that spelled it out would be the fifth hand-written copy,
and `make deploycheck` fails the build over it.

### What the watchdog does, and what it refuses to do

It polls. When the service is **gone** it starts it, using a *generated* copy of
the start command — `start_template()` writes it into `supervise.env` with the
per-start tag left as a placeholder, and the watchdog mints a tag of its own for
each attempt so its starts are readable in `sync.log` like any other.

*Gone* is **identity-bound**, in the sense `health.sh` established: the pid file
must name a live process **whose `/proc/<pid>/exe` hashes to the installed
binary**. A `kill -0` alone is not enough, and the case that proves it is the
one this exists for — after a reboot the pid file still holds a number from
before it, and on a freshly booted board that number has been handed straight
back out to something else. A watchdog that asked only whether *a* process was
alive would find one, conclude the server was running, and do nothing.

**It never signals anything.** It starts a service that is gone; it does not
stop one that is there. The only handle it has is a pid file, a pid file
survives a reboot, and killing what a stale one names is shooting bystanders.
For the same reason it does **not** restart a server that is up and answering
nothing: telling that apart from a healthy one means probing the service, and
acting on it means killing a process. That is a different fault with a
different remedy, and it is still yours.

### A deliberate stop is not fought

`deploy`, `rollback`, `rotate` and `restore` all stop the server on purpose for
a second or two. A watchdog racing them would start the *old* binary in the
middle of the swap, the procedure would then start the new one over the top of
it, and the board would be left with two servers sharing one data directory and
a pid file naming whichever start wrote it last. So there are two gates, and
the watchdog stands aside for either:

* **the board lock** (`PANCRA_LOCK`), held by the operation from before its
  first change until its final verdict. This is the one that matters — it
  covers the copy, the swap and the thirty-second health wait. The watchdog
  *reads* the lock and never takes it: it is not an operation, and a lock it
  held would be a lock the next deploy is refused by.
* **the stop marker** (`PANCRA_DOWN`), raised by `stop_block` and cleared by
  `start_block`, so every verb gets it without having to remember. It covers
  what the lock does not: an operation that has already dropped its lock and
  left the service down — a rotation whose swap failed halfway, which stops
  exactly there and says so.

Both **expire**, because both are files and a file outlives the thing it was
about. An ssh that dies between a stop and a start leaves a marker;
`PANCRA_SUPERVISE_GRACE` (ten minutes) is how long that is obeyed before the
watchdog says so in its log and brings the board back. A lock older than
`PANCRA_LOCK_STALE` is treated the same way. Obeying either for ever would make
the supervisor the reason the board stayed down, which is the outage it was
installed to end.

### Backoff

A start that does not come up healthy within `PANCRA_SUPERVISE_READY` counts as
failed, and the delay doubles from `PANCRA_SUPERVISE_INTERVAL` up to
`PANCRA_SUPERVISE_BACKOFF` and stays there. It never gives up. The commonest
cause on this hardware fixes itself: the board has no RTC, so it comes up with
a clock from 1970, the certificate is not yet valid, and every start fails until
`ntpd` catches up — a watchdog that had given up would leave the board down
after the cause had gone. The other wrong answer is a restart every five
seconds, which fills `sync.log` until the filesystem the database lives on is
full.

### Boot enablement, and the `exec` that eats appended lines

`PANCRA_BOOT_HOOK` names a script the board already runs at boot; the deploy
installs one sentinel-delimited block into it, replacing any block it installed
before, so ten deploys leave one launch line rather than ten. On the Duo that
script is the user script the init chain ends in — it needs no root and it
survives an image update.

**The block goes at the top, after any `#!` line.** That is not a style choice:
the board's startup script ends by handing its process over to the sensor logger
with `exec`, and a block appended to the end of such a file is present, visible
in review, and never reached. The hook file is rewritten *in place* (so its
owner and mode survive — it is not ours), with the previous contents kept beside
it as `<hook>.pancra-prev`.

An undeclared `PANCRA_BOOT_HOOK`, or one naming a file that is not on the board,
is reported and **not invented**: a startup script this deployment created is
one the board's init never runs, and a file that looks like boot enablement and
is not is worse than an honest gap. The deploy says so, in those words, and
carries on — the service is live and the watchdog is running; what is missing is
the half that survives a power cut.

### Operating it

    ./supervise.sh status $PANCRA_SUPERVISE_ENV   # on the board
    ./supervise.sh stop   $PANCRA_SUPERVISE_ENV   # stops the WATCHDOG, not the server
    tail $PANCRA_SUPERVISE_LOG                    # what it did, and when

A deploy installs, refreshes and restarts the watchdog every time — including on
the "already running this exact binary, and it is healthy" path, because a board
that is up is exactly a board nobody restarts, and it would otherwise be the one
board without supervision. A deploy whose supervision could not be established
still reports the deployment as done (the service is live and was verified) and
prints `THE SERVICE IS LIVE AND UNSUPERVISED` with the reason.

`make deploydrill` kills the server and requires it to come **back**; simulates a
reboot — both processes gone, both pid files surviving and naming a live
unrelated process — and requires the board's own boot script to bring the
service up; holds the service down under a lock and under a marker and requires
the watchdog **not** to fight it; and runs a real deploy through a live watchdog,
asserting that exactly one server is running afterwards.

## Installing (atomic staging)

`deploy.sh` never writes over the running executable. It copies to
`sync.new-<operation>`, compares hashes on the board, keeps the current binary under
`releases/sync-<sha256>`, stops the service, renames the new one into place
and starts it. Then it checks three things — the pid is alive, the log says it
is listening, and the public URL serves the login page — and if any of them
fails it puts the previous release back automatically. A deploy either leaves
a healthy server or leaves the one that was already there.

Three details that cost real outages and are therefore not negotiable:

* **The machine type is checked before anything is copied.** A native binary
  on the riscv64 board is an "Exec format error" at startup: the service is
  simply gone, and it reads as a crash rather than a bad copy. `make clean`
  before a `CROSS=` build, and `file` says what you actually built.
* **`setsid`, never a bare `&`.** A process backgrounded from the ssh session
  dies when that session closes, so the deploy "succeeds" and the service
  disappears the moment the terminal does.
* **A backup is taken before the restart.** If the new build turns out to
  write something wrong, the state from before it is already saved.
* **Staged names carry the operation's id.** `sync.new` was one name for both
  `deploy.sh` and `rollback.sh`, and both renamed it over the live executable.
  The lock makes the overlap impossible; the unique name makes the damage
  impossible on the day somebody breaks the lock about a holder that was not
  in fact gone.

## Rolling back

`releases/` holds previous executables, each named by its own SHA-256, so a
rollback needs no build, no cross-compiler and no network:

    make duorollback                 # the most recent release
    make duorollback HASH=<sha256>   # a specific one

If the release you are rolling back to listened on a **different port**, the
front door must move back with it — see *The front door* above. Nothing here
can do that for you, and the symptom if you forget is a rollback that reports
the process healthy while the public name stays down.

A rollback is "run the previous code". It does **not** restore the database:
the data written while the bad build was up is still real data, and rolling it
back would lose exactly the syncs that happened during the incident.

## Backups, and the drill that proves them

`cp sync.db` **is not a backup.** The database runs in WAL mode, so the most
recent commits live in `sync.db-wal` until a checkpoint folds them in. A plain
copy is a perfectly valid database missing exactly the rows that arrived most
recently — it opens, it queries, and the loss is discovered months later as
"some of my history is gone". The host drill measures this every run; on this
machine a `cp` taken during a sync captured **0 of 320** rows.

So the copy is made by the server itself, through sqlite's online backup API:

    ./build/srv/sync backup <out.db> [datadir]     # WAL-safe, verified
    ./build/srv/sync verify <file.db>              # integrity + is it ours

`backup.sh` runs that on the board, brings the copy home, verifies it again
here (a truncated transfer is a file that exists and is wrong), and keeps the
last fourteen on the board. The board's copies are named
`sync-<stamp>-<operation>.db`: the stamp is only good to the second, and
`deploy.sh` builds its pre-deploy backup's name the same way, so two in one
second used to write one file — and, underneath it, one `<dest>.part` for two
sqlite backups to stage into.

**Three outcomes, not two.** The board renames the verified `.part` into place
and then syncs the directory it renamed into. A rename is not durable until
that happens: the file's contents are on the disk and the *name* is not, so a
power cut in the seconds after "backed up ..." was printed leaves a backups
directory with no such entry — and you find out on the one morning you ever
look. So:

| exit | meaning |
|---|---|
| 0 | published, and the directory entry is on the disk |
| 2 | **published and readable now**, but the directory could not be synced; a power loss in the next moments can erase the entry |
| other | nothing was published; the previous backup at that name is untouched |

`2` is deliberately neither of the other two. Read as success, you go to bed on
an artifact that may evaporate; read as failure, `deploy.sh` stops with
"refusing to deploy over unsaved data" and `backup.sh` files the arrived copy
under `.unverified` — both about a backup that is complete and verified and
sitting right there. `backup.sh` passes the same `2` on, having still brought
the copy home: it is the *board's* copy whose durability is in doubt.

**What a backup does not contain** is the front door. A `nat` forward or a
reverse proxy's configuration lives outside this repository, so restoring
`backups/` onto fresh hardware gets you a correct database behind a public
name that still points somewhere else. `PANCRA_FRONT_OWNER` names who can fix
that; it belongs in the same runbook as this file.

**The drill.** `make restoredrill` — part of `make check` — takes a backup
from a live server that is being written to, restores it into a fresh
directory, starts a second server on it, and makes the *same paired phone*
fetch its history back through the signed API. It also feeds the procedure
random bytes, a valid-but-foreign database and a truncated file, and requires
each to be refused. A backup nobody has ever restored is a hope.

## Putting a backup back, and putting it back back

    ./srv/deploy/restore.sh <backup.db>

The only verb here that **destroys data** — everything synced since that backup
was taken is gone — so it is not in the table above, nothing calls it
automatically, and it asks for `YES` on the terminal. It verifies the file here
first (`PANCRA_VERIFY_BIN`, the *native* build: `PANCRA_LOCAL_BIN` is the
riscv64 artifact and this machine cannot run it), copies it beside the live
database, verifies the copy on the board, stops the service, moves the working
set aside to `backups/displaced-<stamp>-<operation>.db` **with its `-wal` and
`-shm`**,
installs the backup, restarts, and asks health.sh the same two questions a
deploy asks.

**And it puts the displaced database back when the restored one does not
serve.** That was the hole this script had longest. A backup can verify
perfectly — integrity check clean, schema present — and still be one the server
will not start on, or one it starts on and then answers no request out of. When
that happened, the script stopped: the service **down**, the file that had just
proved it would not serve installed as the live database, and the complete
working set one directory away needing three renames nobody had written down,
at an hour when nobody wants to be inventing them. Every ingredient of the
recovery was present and the recovery was manual.

It now, in one remote shell with the service stopped:

* **stops the attempted process.** "Not healthy" does not mean "not running": a
  database that loads and answers nothing gives a live pid and a readiness
  line. Moving files under a live server and starting a second one leaves two
  processes on one data directory.
* **quarantines the restored set** as `backups/refused-<stamp>-<operation>.db*` — not
  deleted. It is the evidence: that backup was chosen for a reason and now has
  to be explained. And its `-wal`/`-shm`, written by the refused start, *must*
  leave the data directory (below).
* **reinstalls the complete displaced set**, `.db` with its `-wal` and `-shm`.
  A partial reinstall is a database beside a **different** database's
  write-ahead log, and sqlite replays it rather than refusing: that is not a
  failed restore any more, it is a corrupted one. The set moves as a set, both
  ways, and a `mv` that fails aborts the whole block rather than leaving a
  mixture.
* **restarts and asks the same health question**, not a weaker one.

**Then it says which of three things happened**, because they need different
responses and only one of them needs anybody tonight:

| Outcome | What the report says |
|---|---|
| the restore worked | `restored and answering <URL>`, and where the displaced set is |
| the restore failed, **the previous database is back and serving** | `THE ROLLBACK SUCCEEDED` — nothing was lost, the board is **not** down, and the backup that would not serve is kept at `refused-<stamp>` |
| the restore failed **and so did the rollback** | `THE BOARD IS DOWN`, and both complete sets named by path |

Two special cases it must not blur into those:

* **Nothing was displaced** — a rebuilt board, new hardware, the first thing a
  restore is ever used for. There is no working set to reinstall, so a rollback
  that ran anyway would quarantine the only copy the board has and leave the
  data directory empty. It says so and leaves the restored file installed.
* **An undeclared front door** is not an outage. `restore.sh` has no
  `require_front` precondition on purpose (see *The front door* above), so an
  undeclared one arrives at the health wait — where it is indistinguishable
  from an unreachable service unless the script asks. If it does not ask, a
  restore that is installed and **listening** gets rolled back and the operator
  is told the backup could not be restored when it had been. Both waits ask,
  going and coming back.

`make deploydrill` arranges all of it on the fake board: a backup that verifies
and will not start, one that starts and answers nothing (a live pid, a useless
service), a rollback that fails too, a board with no database at all, and each
of those with the front door undeclared. It checks the **files** as well as the
words — all three back byte for byte, the refused set present, no stale copy
left — and that the process serving the refused database was stopped. Every
part of the rollback was then broken on purpose, one at a time, and each break
fails a named assertion.

## Rotating the certificate

The server reads `cert.pem` and `key.pem` once, at startup, from the paths in
`pancra.conf`. Rotation is therefore a file swap and a restart — and it is one
command, because doing it by hand went wrong in three ways at once:

    make duorotate CERT=new-cert.pem KEY=new-key.pem

It validates the pair **before anything on the board moves** (a certificate
with the wrong key gives a server that starts, logs that it is listening, and
then fails every handshake — the pid check and the readiness line both pass,
and only the public probe notices), refuses one that has already expired,
warns about one expiring within the week, keeps the old pair as `.prev`,
restarts through the same operation `duodeploy` uses, and **puts the old pair
back automatically** if the new one does not serve.

This used to be five paragraphs to paste. The paste hard-coded the port and
both pem paths as literals — in a guide that opens by saying `pancra.conf`
owns every path on the board — so a board on another port came back up on
8443, listening where nothing forwards. And its start line ended at `&` with
nothing writing the new pid, so `sync.pid` kept naming the process that had
just been killed: every later health check asked after a dead pid, and the
next deploy's stop step killed whatever the kernel had since given that number
to. `srv/deploy/start.sh` is now the only place the start command exists, and
`make deploycheck` fails if a second copy appears — including in this file.

**If `PANCRA_FRONT` is a proxy, this is not the certificate the world sees.**
The proxy terminates TLS for the public name and `PANCRA_CERT`/`PANCRA_KEY`
are the backend pair; rotating the public one is that component's procedure.
See *The front door* above.

Check the dates from outside afterwards:

    echo | openssl s_client -connect pancra.org:443 2>/dev/null |
      openssl x509 -noout -dates -subject

The phone
notices a refused certificate specifically: the sync status reads `TLS
REFUSED`, and it keeps retrying **every five minutes** — a fixed schedule, not
the doubling backoff an unreachable server earns, because the repair is at
this end and the phone has to notice it. So a botched rotation is visible on
the phone rather than silent, and a fixed one is picked up within five minutes
without touching the phone at all.

(That number is `REMOTE_FIXED_RETRY` in `app/remote.c`, and
`app/test/remotetest.c` holds it to exactly this: five minutes, and it does
not climb with repeated failures. This paragraph used to say the app "does not
back off", while the code went straight to its slowest schedule — thirty
minutes — which is the opposite.)

## Moving the root

`PANCRA_ROOT` is `/home/jk/pancra`. A board still running from the tree the
server was installed into under its previous name needs the override until it
is moved:

    PANCRA_ROOT=/home/jk/projects/glucoserve make duosmoke

Moving it is a deliberate procedure, not a variable change — renaming a
running service by surprise is how a smoke test starts failing against a
perfectly healthy board:

1. `make duobackup` — and check that the copy verifies here.
2. Deploy to the new root with the service still running on the old one:
   `PANCRA_ROOT=/srv/pancra PANCRA_PORT=8444 make duodeploy` (a different port,
   so the two do not fight for the listener).
3. Copy the database across with the old server **stopped**, so nothing is
   written between the copy and the switch.
4. Move the front door to the new port — whatever `PANCRA_FRONT` says it is,
   and it is not always a proxy — then run `PANCRA_ROOT=/srv/pancra make
   duosmoke`, and only then remove the old tree. The smoke test goes through
   the public URL, so it is what proves this step happened.
5. Drop the override, so the repository and the board agree about where the
   service lives without anyone having to remember a variable.
