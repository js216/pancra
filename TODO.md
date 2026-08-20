# TODO

1. [DONE] Make Dexcom link selection explicit and reject invalid links. The driver owns
   per-link contexts, but `app/dexdriver.c` still selects one through mutable
   file-wide `ctx` and `g_cur_link`; `driver_enter()`/`driver_leave()` must
   temporarily switch and restore them, and `driver_select()` maps an invalid
   link to `LINK_CGM`. Pass a validated `struct dex_ctx *` and link through the
   internal state-machine helpers, retain locking at the public boundary, and
   return without touching state when the link is invalid. This removes hidden
   target selection and the possibility of corrupting the primary CGM context.

   Summary: Pass Dexcom contexts explicitly and reject invalid links.

2. [DONE] Scope Android prerequisites structurally to Android targets. The Makefile
   currently decides at parse time whether Clang/JDK are needed by checking all
   requested goals against the hand-maintained `SRV_ONLY_GOALS` allowlist. A
   newly added server test or deployment target therefore requires a distant
   second edit or spuriously demands Android tools. Move app/server/check/deploy
   rules into focused included fragments or attach prerequisite validation to
   the targets that actually consume it, preserving the existing public target
   names.

   Summary: Scope build prerequisites to the targets that consume them.

3. [DONE] Make the reusable crypto library obtain entropy through a platform boundary.
   `lib/rand.c` directly declares Unix syscalls and opens `/dev/urandom`, despite
   `lib/` being described as self-contained reusable code. Define a small
   entropy-provider interface or platform backend, retain `/dev/urandom` as the
   Unix implementation, and make crypto code depend only on that interface.

   Summary: Inject entropy instead of hard-wiring `/dev/urandom` in lib.

4. [DONE] Do not claim a sync schedule until Java accepts the request. In
   `app/remote.c`, `remote_sync_locked()` records the current state stamp and
   advances its deadlines before calling the void `syncjni_sync_request()`;
   that function can drop the request when JNI is unavailable or a Java call
   throws. Since no `sync_report()` follows, the scheduler may then wait for the
   six-hour safety deadline. Return an enqueue result and commit the stamp and
   deadlines only on success, otherwise schedule the ordinary prompt retry.

   Summary: Retry promptly when Java rejects a sync enqueue.

5. [DONE] Turn every JNI exception into an explicit native failure. Several functions
   in `app/jbridge.c` and the permission paths in `app/scan.c` call Java without
   checking for a pending exception, then advance native state or consume a
   default result. Provide typed checked-call helpers which detect, describe or
   safely clear exceptions and return failure; update native state only after a
   confirmed Java operation. Cover a missing/mismatched method and throwing
   implementations at the host boundary.

   Summary: Turn every JNI exception into a checked native result.

6. [DONE] Preserve database error, absence, and value as distinct outcomes. Helpers
   such as `db_one_long()` in `srv/db.c`, `email_of()` and `tz_resolve()` in
   `srv/page.c`, and pairing-code lookup in `srv/settings.c` currently turn a
   prepare/step failure into the same result as no row. Callers can consequently
   render plausible blank/UTC/empty pages or choose the wrong owner during a DB
   fault. Return a typed status plus found/value, require `SQLITE_DONE` for true
   absence, and make page handlers return an error rather than authoritative
   missing data.

   Summary: Keep database failures distinct from missing rows.

7. [DONE] Make `deploydrill` fail closed when its fixture cannot be built. The target
   is part of `make check`, but `srv/test/deploydrill.sh` currently reports SKIP
   and exits zero when its static fixture cannot link, without consulting
   `ALLOW_SKIP`. Use the repository's normal prerequisite/explicit-skip policy
   or a hermetic dynamic fixture so a default green gate proves the deployment
   recovery drill actually ran.

   Summary: Fail the gate when the deploy drill cannot build its fixture.

8. [DONE] Add a ThreadSanitizer gate for compatible host concurrency tests. The app and
   server suites deliberately exercise real overlap, but sanitizer builds use
   only AddressSanitizer and UndefinedBehaviorSanitizer, which do not diagnose
   C data races. Run the compatible thread, meter, pairing, remote, GIF, and
   representative server concurrency paths under TSAN, documenting exclusions
   only for demonstrated tool incompatibilities.

   Summary: Run concurrent host and server tests under ThreadSanitizer.

9. [DONE] Behavior-test Android orchestration instead of treating source tokens as
   proof. `javacheck` greps for calls such as `stopSound`, `release`,
   `stopScan`, and `serviceAction`; dead or incorrectly ordered calls still
   satisfy it. Put Alarm, BLE scanning, and service-recreation effects behind
   narrow injected adapters or host Android stubs, and assert call count, order,
   returned failure, and retry state. Keep grep checks only as cheap secondary
   diagnostics; no phone is required for this boundary test.

   Summary: Test Java boundary behavior instead of grepping source tokens.

10. [DONE] Validate the native library actually embedded in the APK. `apkcheck.sh`
    verifies that the APK contains the expected member name, but performs ELF
    and symbol checks on the separate staging copy under `build/app/apk`.
    Extract `lib/arm64-v8a/libpancra.so` from the APK argument into a temporary
    directory and inspect that exact payload, so a stale or corrupt packaged
    library cannot pass because the staging file is healthy.

    Summary: Inspect the APK's embedded JNI library, not its staging copy.

11. [DONE] Give the native sync engine an explicit, serialized context. `app/sync.c`
    keeps transport, account key, log registry, response storage, and several
    reallocating work buffers in independent process-wide globals while public
    operations take no context. Configuration changes and pair/run/restore
    operations can race or consume stale shared scratch. Introduce an opaque
    `sync_ctx` owning immutable-per-run configuration and operation-private
    workspace, and serialize configuration swaps and operations at that
    boundary.

    Summary: Give each native sync operation an explicit serialized context.

12. [DONE] Keep settings filesystem I/O outside the spinning state lock. Persistent
    setters in `app/settings.c` hold the yield-spin `set_lk` while performing
    `atomic_replace()`, including write, fsync, rename, and directory fsync, so
    unrelated readers can spin for storage latency. Snapshot bytes and a state
    generation under `set_lk`, release it for serialized file I/O, then publish
    or reconcile the result without allowing an older failed save to undo a
    newer edit.

    Summary: Keep settings flash I/O outside the spinning state lock.

13. [DONE] Snapshot remote endpoint and credentials as one immutable unit. Remote
    scheduling and Java request construction currently call `settings_get()`
    and `sync_creds_get()` separately, each under a separate acquisition of
    `set_lk`. A concurrent server or identity change can combine an old endpoint
    with a new account, or vice versa. Add one `remote_config_get()` snapshot
    containing enablement, server, port, email, uid, and key, and pass that
    immutable value through scheduling and request construction.

    Summary: Snapshot endpoint and credentials as one remote configuration.

14. [DONE] Serialize every pairing-advert field shared with the binder callback.
    `jni_on_advert()` updates plain `g_total` and per-link `g_link_try` and reads
    plain `g_smart_pairing`, while main-thread paths read or write the same
    objects; the candidate-list lock does not cover them. Put the control,
    counter, and throttle claim behind one lock or appropriate atomics, take one
    coherent decision snapshot, and prove at most one reconnect claim per link
    and window while smart-pair mode changes.

    Summary: Serialize advert counters, smart mode, and reconnect throttles.

15. [DONE] Make `synccli` HTTP exchanges exact, bounded, and framing-aware. The client
    uses single writes for headers/bodies, grows response storage without a
    ceiling or overflow guard, treats read errors like EOF, and can accept a
    partial status line without a complete framed response. Add deadline-aware
    full-write/read loops, strict status/header parsing, response limits,
    Content-Length validation, and premature-EOF rejection. For body files,
    check every seek/size/read/close result and refuse non-regular, oversized,
    or incompletely read input before signing it.

    Summary: Bound and fully frame every synccli HTTP exchange.

16. [DONE] Require local verification before reporting a downloaded backup successful.
    `srv/deploy/backup.sh` currently exits zero and prints the destination when
    `build/srv/sync` is absent, even though its contract says the transferred
    copy is verified locally. Require the verifier before transfer, or fail and
    quarantine/remove the unverified artifact; cover direct script invocation
    as well as the Make target.

    Summary: Fail backups unless the downloaded copy verifies locally.

17. [DONE] State the real cross-build ABI contract. README and Makefile comments say
    `CROSS=<prefix>` supports another static Linux target and even cite armv7,
    while `srv/proto.h` requires 64-bit `long`/`time_t` and P-256 requires a
    GCC-compatible `unsigned __int128`. Document support as LP64 static Linux
    with the required 64-by-64-to-128 extension and remove the armv7 example,
    unless a substantially larger fixed-width/32-bit port is intentionally
    undertaken.

    Summary: Document CROSS as LP64 Linux with compatible `__int128`.

18. [DONE] Hide split alarm-threshold mutation behind the atomic domain operation.
    `settings.h` still publicly exposes individual threshold setters and the
    temporal requirement that callers hold the alarm lock, validate the other
    field, mutate, and separately persist. `alarm_set_threshold()` already owns
    that safe operation. Make the primitive setters private to alarm/settings
    internals so future callers cannot install an unordered live pair, omit
    persistence, or mishandle a save failure.

    Summary: Hide split threshold setters behind one atomic alarm operation.

19. [DONE] Return typed outcomes from persistent-state loaders. Settings, alarm,
    credentials, session cache, and meter-store load APIs commonly return void
    and collapse missing/defaulted, malformed/truncated, and I/O failure into
    retained or default state plus incidental logging. Give startup orchestration
    explicit absent/loaded/corrupt/error results so it can distinguish a clean
    first run from degraded storage and record or surface the correct recovery
    state.

    Summary: Return explicit outcomes from persistent-state loaders.

20. [DONE] Revoke all sessions atomically when the web password changes. The browser
    settings path updates the password hash and reports success without calling
    `session_drop_all()`, unlike the CLI path and the stated revocation
    rationale. Put password update and session deletion in one transaction and
    claim completion only after both commit, so an old or stolen cookie cannot
    remain valid after a password change.

    Summary: Revoke every session atomically on web password changes.

21. [DONE] Make restore and rollback prove public reachability before success. Their
    current health checks accept a live PID plus a generic readiness line that
    may predate the current start, whereas deploy also probes the public URL.
    Reuse a common health operation with a per-start marker and public response
    check, and do not report restored/running until the actual service path is
    reachable.

    Summary: Prove restore and rollback reach the public service.

22. [DONE] Align HTTP success with the server's promised power-loss durability.
    `srv/db.c` claims committed data survives power loss, but configures SQLite
    WAL with `synchronous=NORMAL`; a bucket PUT receives HTTP 200 immediately
    after a commit whose recent WAL pages may be lost on power failure. Either
    use `synchronous=FULL` for acknowledged mutations or explicitly define and
    implement a weaker acknowledgement/reconciliation guarantee, then correct
    the documentation and exercise the chosen restart/power-loss boundary.

    Summary: Match server acknowledgements to power-loss durability.

23. [DONE] Distinguish pre-rename failure from post-rename durability uncertainty.
    `replace_finish()` returns failure when directory fsync fails after rename,
    even though the new pathname is already visible; callers then roll memory
    back as if disk were unchanged. Return a distinct changed-but-not-durable
    outcome and reload/reconcile the visible file, or use a recoverable two-name
    protocol, so memory, feedback, and the state seen after restart do not
    contradict one another.

    Summary: Treat post-rename sync failure as changed but not durable.

24. [DONE] Version evolving app settings and credential formats explicitly. The main
    settings file is 22 positional integers whose schema is inferred from the
    number parsed, and remote credentials evolve by appending fields; legacy
    shortcut migration already requires a numeric heuristic. Add a version
    marker, retain a version-zero reader for deployed files, perform ordered
    migrations, and reject unknown future versions without partially applying
    defaults.

    Summary: Version settings files and migrate each historical schema.

25. [DONE] Reject overlong data paths instead of silently truncating them. `data_path()`
    stops copying the directory at `cap - 32`, appends a filename, and returns no
    status; calibration constructs paths through a second unchecked convention.
    Make the shared builder calculate exact required length and return failure,
    route all persistence owners through it, and refuse startup persistence
    initialization if any canonical path cannot be represented.

    Summary: Reject overlong data paths instead of truncating filenames.

26. [DONE] Model the OneTouch protocol as explicit request/response transitions.
    `app/otble.c` interprets any CRC-valid success according to the current
    mutable phase without validating command/transaction identity, and a
    non-ACK frame can call completion even in idle/done states. A delayed or
    duplicated record can therefore be attributed to the next requested index
    or repeat teardown. Track the expected response shape/index and terminal
    state, reject impossible/late/duplicate events, make completion idempotent,
    and add sequence tests for those cases.

    Summary: Reject late, duplicate, or wrong-phase OneTouch responses.

27. [DONE] Reject surplus and incomplete CLI forms rather than guessing intent. Command
    dispatch generally enforces only a minimum argument count and ignores
    extras; server startup with port, datadir, and only one TLS file silently
    falls back to plaintext. Give each command exact accepted forms/minimum and
    maximum counts, reject extras with command-specific usage, and require
    either both certificate and key or neither.

    Summary: Reject extra or incomplete CLI arguments, especially TLS.

28. [DONE] Treat BLE native registration failure as fatal app initialization failure.
    `init_java()` promises to fail every unsuccessful initialization step, but
    only logs a failed `dexble_register()` and returns success. The process can
    then appear initialized without the callbacks and transport needed for
    monitoring and sync. Set a specific native status and return failure before
    publishing initialized state.

    Summary: Fail startup when BLE native registration fails.

29. [DONE] Define and own the production proxy/routing topology. README claims the
    server needs no proxy, while deployment listens on 8443, public checks use
    `https://pancra.org`, and migration instructions explicitly require
    repointing a reverse proxy. Document whether exposure is direct, NAT, or a
    named proxy, who owns 443-to-8443 routing/TLS, and include that component in
    deployment, rollback, backup, and failure procedures.

    Summary: Document and own the production proxy and routing topology.

30. [DONE] Make certificate rotation use the single deployment configuration. The
    deployment guide says `pancra.conf` owns every board path, but its rotation
    commands hard-code binary, data, PID, log, port, certificate, and key paths
    and omit reliably recording the replacement PID. Provide one script/target
    that sources the config, validates the certificate/key pair, swaps them
    recoverably, uses the configured start/health operation, records the PID,
    and restores the previous pair on failure.

    Summary: Rotate certificates through the deployment configuration.

31. [DONE] Bind deployment health evidence to the exact artifact and public backend.
    Current checks independently hash the file on disk, observe a PID and a
    generic recent log marker, and fetch any public page containing the expected
    text; these facts need not identify the same process or backend. Expose a
    non-sensitive build/instance identity at health, or verify the running
    `/proc/<pid>/exe` and direct backend before checking the public route, and
    tag/truncate logs per start.

    Summary: Bind health checks to the exact process and public backend.

32. [DONE] Rewrite implementation comments as current contracts rather than incident
    archaeology. Current comments still contradict symbols and ownership—for
    example, Dexcom commentary names nonexistent `D`, and forms commentary says
    state is exported after it became snapshot-based—amid hundreds of “used to”
    narratives. Keep concise live invariants and reasons beside code; move
    retired implementation stories to version history or focused design notes.

    Summary: Keep current invariants in code and move incident history out.

33. [DONE] Group each forms workflow's state into a named object. `app/forms.c` keeps
    weight draft, insulin draft, keypad, calibration, picker, pagination,
    scrubbing, and return-routing fields as adjacent globals whose sentinel
    meanings live mainly in comments. Use cohesive state structs with typed
    modes and reset/open helpers so a workflow cannot accidentally retain or
    combine another editor's stale fields.

    Summary: Group each form workflow's draft, mode, and return state.

34. [DONE] Centralize the established bounded CSV cursor primitives. Insulin and weight
    maintain nearly identical signed-decimal and separator readers, with sensor
    provenance carrying related variants. Extract only the common bounded
    cursor operations with explicit success/overflow results, while keeping each
    record schema and semantic validation local, so malformed-field behavior
    cannot drift between persisted formats.

    Summary: Share bounded CSV primitives while keeping schemas local.

35. [DONE] Make auth-maintenance write failures observable and safely retryable.
    Session rolling-expiry updates, nonce pruning, and app `last_seen` updates
    currently ignore DB failures. Core authentication should remain fail-closed
    without rejecting an already valid request solely for telemetry, but these
    failures should be logged with operation context and cleanup retried so
    sessions do not expire unexpectedly, nonce storage does not grow silently,
    and activity data does not remain stale without diagnosis.

    Summary: Surface failed auth maintenance and retry cleanup safely.

36. [DONE] Synchronize native bond-state publication and lookup. `app/dexble.c` grows
    and writes `g_bond`/`g_bond_n` from a binder callback while model rendering
    reads the count, MAC text, and state without synchronization. Insertion even
    publishes the incremented count before initializing the new MAC and state,
    so a concurrent lookup can pass a partially initialized buffer to
    `strcmp()`. Protect the table with a dedicated lock and return a copied
    matched state, or publish fully initialized immutable entries through a
    correct atomic protocol.

    Summary: Synchronize native bond-state publication and lookup.

37. [DONE] Bind every queued GATT operation to one client generation. In `Ble.java`,
    subscribe/read/write first discover data through `L.gatt` and later
    dereference `L.gatt` again without holding the Link lock, while disconnect
    and connection callbacks can close, clear, or replace it between those
    steps. Capture `(BluetoothGatt, generation)` when dequeuing, operate only on
    that captured client, and report native failure if its generation is no
    longer current; make completion generation-aware so a stale callback cannot
    complete a replacement operation.

    Summary: Bind queued GATT operations to one client generation.

38. [DONE] Publish foreground-notification content as one immutable snapshot.
    `PancraService` stores title, text, value, lock state, pixels, width, and
    height in separate volatile fields, publishes them sequentially, and reads
    them separately while building a notification. A concurrent build can pair
    a new pixel array with old dimensions or mix readings, and an exception in
    `onStartCommand()` can prevent timely `startForeground()`. Publish one
    immutable state object through a single volatile reference, owning/copying
    the pixel array as necessary, and build entirely from one captured object.

    Summary: Publish notification content as one immutable snapshot.

39. [DONE] Restore buckets through durable atomic file replacement. `app/sync.c`
    currently appends a downloaded bucket directly to the live log with one
    write, ignores `close()`, performs no fsync, and admits that a short write
    leaves a torn row. Stage the complete restored result, full-write with EINTR
    handling, check close, fsync file and directory, and atomically rename before
    reporting restored rows. Preserve a distinct changed-but-uncertain result if
    a post-publication durability operation fails.

    Summary: Restore buckets through durable atomic file replacement.

40. [DONE] Validate the complete digest reply before restoring any data.
    `digest_line()` returns the same result for clean end and syntax failure, so
    `sync_restore()` accepts a valid prefix followed by malformed or truncated
    data as the complete server bucket list and can report successful partial
    restoration. Return explicit row/end/error states; require numeric fields,
    exact hash width, overflow-safe bucket syntax, and clean framing; parse and
    validate the entire digest before modifying a local log.

    Summary: Reject incomplete digest replies before restoring any data.

41. [DONE] Reject certificate/private-key mismatches before listening. TLS startup
    parses a certificate and EC scalar independently but never derives and
    compares the scalar's public point with the leaf certificate SPKI. A valid
    but mismatched pair therefore reaches ready/listening state while every
    client rejects CertificateVerify. Parse the accepted leaf public key,
    compare it with the point derived from the private scalar, and fail startup
    with a precise diagnostic before opening the service.

    Summary: Reject certificate and private-key mismatches before listening.

42. [DONE] Read complete bounded TLS credential files or fail startup. `srv/tls.c`
    performs one read into a fixed 16 KiB buffer for each certificate/key file,
    without looping to EOF or detecting oversize input. Legal short reads cause
    intermittent parse failures, while long files are silently prefix-truncated.
    Use a bounded full-read loop, check read and close errors, require EOF within
    the documented ceiling, and require the complete expected PEM structure.

    Summary: Read complete bounded TLS credential files or fail startup.

43. [DONE] Give glucose alarms and stopped-monitoring warnings distinct notification
    IDs. `Alarm.java` and `PancraService.java` both use notification ID 2, so
    either notice can overwrite the other and either cancellation path can
    cancel the wrong one. Own all application notification IDs in one shared
    constants definition and cover simultaneous publication plus independent
    cancellation.

    Summary: Give alarm and monitoring-stopped notices distinct IDs.

44. [DONE] Bind every gesture to one Android pointer ID. Native input masks the action
    and always reads pointer index zero while ignoring POINTER_DOWN and
    POINTER_UP. A second finger can therefore inherit the first finger's armed
    control or scrub and later fire it. Latch the pointer ID on ACTION_DOWN,
    resolve that ID's current index for MOVE/UP, and cancel when that pointer
    leaves or when the chosen multi-touch policy is violated.

    Summary: Bind armed touches and scrubs to one Android pointer ID.

45. [DONE] Isolate normal and sanitizer artifacts during parallel checks. `make check`
    exposes ordinary host tests and `appasan` as independent prerequisites, but
    recursive sanitizer recipes overwrite and execute the same
    `build/app/test/<name>` files. `srvasan` and `tlsasan` likewise launch
    separate makes against the same server ASan tree. Key outputs by build mode
    and flags, and build shared sanitizer server prerequisites once, so `make
    -j check` cannot run a half-written or wrong-configuration binary.

    BINARIES first. The app suites build into `$(TESTDIR)` (`build/app/test`,
    with `appasan` and `apptsan` passing `-asan` and `-tsan` trees), the server
    host suites into `$(SRVTESTDIR)` (so `srvtsan`'s recursive httptest no
    longer overwrites the plain one), and `srvasan` and `tlsasan` share ONE
    `srvasanbuild` prerequisite with one flag variable instead of two recursive
    makes against one directory. Verified by running them, singly and under
    `make -j8`, and by `nm`: the sanitizer trees really do carry
    libasan/libubsan/libtsan and the plain tree carries none.

    FIXTURES second, and they were the harder half, because the paths were
    string literals in the test sources rather than anything the Makefile could
    reach. `build/app/test` was written out in twelve suites -- calibtest's
    `calibdata`, meterstoretest's `mstore`, durabilitytest's `durable`, the
    directories handed to `settings_paths`, `sensors_paths`, `store_paths`,
    `weight_paths` and `insulin_paths`, and uitest's fourteen `.ppm` screens --
    so a sanitizer binary read and wrote the PLAIN tree. app/test/testdir.h now
    owns the answer for every suite: `test_dir()` reads `APP_TESTDIR` from the
    environment and falls back to `build/app/test`, so a binary run by hand
    behaves exactly as it did before, and `test_path()` builds a fixture path
    into the caller's buffer and ABORTS rather than truncating. The Makefile
    passes it at the point where each binary is launched (`$(TESTENV)`,
    alongside the existing `$(TESTDIR)`), and uitest.c now names screens rather
    than paths.

    PER SUITE, NOT ONLY PER MODE. Keying by mode alone was not enough and the
    parallel run said so: `make -j8` gives the jobserver to the sanitizer
    submake too, so a dozen suites of the SAME mode run at once, and they do
    not have distinct filenames -- registrytest, statstest, modeltest and
    pairingtest each point `sensors_paths` at their fixture directory and then
    unlink and re-mint `sensors.csv` and `slots.csv`; settingstest and modeltest
    share `settings.cfg` and `remote.cfg`; storetest, weighttest and insulintest
    each share a log with modeltest. `make -j8 uitest appasan apptsan` failed in
    statstest on "a sensor with a known activation is registered" -- a mint into
    a registry file another process had just truncated. The fixture directory is
    therefore `$(TESTDIR)/fixtures/$@`: one variable, derived from the target
    name, so a new suite gets its own directory by existing.

    Two collisions outside the app tree came with it. registrytest, weighttest
    and insulintest staged their unreadable-file case in `build/app` itself
    (`build/app/sensors.csv` and friends, created as directories and rmdir'd),
    which is one path for every mode; each now uses an `eisdir` subdirectory of
    its own fixture directory. modeltest wrote `/tmp/settings.cfg` -- shared
    with all three of its own build modes and with every other process on the
    machine.

    Verified by execution: all 27 app host suites pass individually; `make
    appasan` and `make apptsan` pass from wiped trees; `make -j2 appasan
    apptsan` and `make -j8 uitest appasan apptsan` pass; `make appasan` passes
    with `build/app/test` absent and LEAVES it absent; the fixtures are visibly
    in `build/app/test{,-asan,-tsan}/fixtures/<suite>/` and neither sanitizer
    trace mentions the plain tree.

    Summary: Isolate normal and sanitizer artifacts for parallel checks.

46. [DONE] Build invitation URLs only from a configured public origin. The settings
    page copies the request `Host` value into a share URL, escaping it for HTML
    but treating it as authoritative. An authenticated request through a
    permissive proxy or alternate host can therefore produce an attacker-domain
    link containing the live single-use token. Configure one canonical public
    origin shared with deployment/CLI, ignore or reject other Host values, and
    never disclose a token into a request-selected origin.

    Summary: Build invitation links only from a configured public origin.

47. [DONE] Accept only canonical listed time-zone offsets. The settings handler passes
    arbitrary nonempty `tz` text through `strtol()` without end, range, or
    membership validation, narrows it to int, stores it, and reports success.
    Require an exact canonical decimal whose value occurs in the supported
    offset table (or the explicit empty/default choice), reject everything else
    without updating the DB, and constrain stored values where practical.

    Summary: Accept only listed canonical time-zone offsets.

48. [DONE] Require exact HTTP methods for every web route. Routing commonly treats GET
    specially and sends every other method through POST logic, so HEAD, PUT,
    DELETE, or PATCH can invoke login, logout, settings mutation, or invitation
    redemption when their bodies/session/CSRF happen to qualify. Declare the
    allowed methods per route, return 405 with `Allow` for every other method,
    and make HEAD explicitly read-only or reject it.

    Summary: Require exact HTTP methods for every web route.

49. [DONE] Reject nonexistent calendar dates rather than normalizing them. Day/plot
    routing bounds the day only to 1..31 and lets `timegm()` normalize February
    31, April 31, or a non-leap February 29 into another month while retaining
    the requested title. Require eight digits and validate month-specific/leap
    bounds, or round-trip the constructed UTC date and reject any mismatch.

    Summary: Reject invalid calendar dates instead of normalizing them.

50. [DONE] Roll back the database automatically when restored service startup fails.
    `restore.sh` moves the live DB/WAL/SHM aside and installs the backup, but a
    failed startup/health wait leaves the restored set active and the service
    down even though the displaced working set remains available. Stop the
    attempted process, quarantine its files, reinstall the complete displaced
    set, restart and verify the former service, and report whether automatic
    recovery itself succeeded.

    Summary: Roll back the database when restore startup fails.

51. [DONE] Propagate asynchronous scan failure into native retry state. `Ble.scan()`
    installs a callback and immediately returns success, but `onScanFailed()`
    only logs; native code has already latched `g_scanning=1`, and its recovery
    path starts only when that flag is false. Add a generation-aware Java-to-
    native failure callback, clear only the matching scan registration, reset
    native scan state, and enter the existing throttled retry path without
    allowing a stale failure to cancel a newer scan.

    Summary: Propagate scan failure and retry the matching generation.

52. [DONE] Validate every GATT setup transition before declaring the link ready. The
    connection callback ignores status and the return from `requestMtu()`, MTU
    completion ignores status and the return from `discoverServices()`, and
    service discovery calls native connected regardless of status. Handle each
    failure/false return, use a safe discovery fallback where valid, otherwise
    close and report a generation-bound failure; call native connected only
    after successful discovery.

    Summary: Validate MTU and discovery before declaring GATT ready.

    WHAT CHANGED. All six ignored signals are now decisions, and each decision
    is one pure function in app/BoundaryLogic.java (`gattConnected`,
    `gattMtuRequested`, `gattMtuChanged`, `gattDiscoverRequested`,
    `gattDiscovered`), so the interesting cases are executable on the host
    instead of reachable only through a misbehaving Bluetooth stack. The two
    failure kinds are kept apart deliberately: an MTU that was refused (the API
    returned false, so no completion ever arrives) or that completed badly
    continues to discovery at the DEFAULT MTU, because every message this app
    sends is chunked to 20 bytes to fit an un-negotiated link -- the 185-byte
    MTU is an optimisation. A discovery that was refused, that completed with a
    bad status, or that succeeded with an EMPTY service table has no fallback
    at all -- `find()` iterates `getServices()`, so there is nothing to look a
    characteristic up in -- and closes the link through the new
    `Ble.setupFailed()`, which bumps the link generation under the monitor
    BEFORE disconnecting (so the DISCONNECTED callback that follows stays
    silent) and reports exactly one `onDisconnected`, putting the driver's own
    reconnect in charge. Native `onConnected(id)` is now reachable from exactly
    one place, after a discovery that actually succeeded; `make javacheck`
    holds that (gattDiscovered in the callback, the call inside it, and a count
    of one call site) plus the presence of the other four decisions.

    VERIFIED BY EXECUTION: every transition's accept/refuse answer, including
    a superseded generation on each of the three callbacks, in
    `make boundaryjavatest`; 12 mutants of those rules were each killed by a
    named assertion in an isolated /tmp copy, and each javacheck gate was shown
    to fire on a mutant (including one that restores the original bug). NOT
    CLOSED BY EXECUTION, and it needs a phone: that a real Dexcom pairing
    completes at the default 23-byte MTU (the fallback's validity is argued
    from the 20-byte chunking, not measured), and that a real refused
    `requestMtu`/`discoverServices` takes the new path -- no host harness can
    run Ble.java, which is Android API calls throughout.

53. [DONE] Share each export as a unique immutable file snapshot. `Ble.exportData()`
    always truncates and rewrites `files/pancra.csv`, and the content provider
    resolves that same live pathname when the receiving app later opens it. A
    second export can therefore replace or truncate the first recipient's data.
    Generate a constrained unique filename per export, grant that URI, validate
    names in the provider, and retain snapshots long enough for delayed readers
    before conservative cleanup.

    Summary: Share unique immutable exports instead of one live file.

    WHAT CHANGED. Each export is its own file under `files/exports/`, named
    `pancra-DDDDDDDDDD-HHHHHHHH.csv`: the prefix, EXACTLY ten ASCII digits (the
    export instant in epoch seconds, zero-padded -- the unit the CSV rows are
    already stamped in), a `-`, EXACTLY eight LOWERCASE hex digits of
    SecureRandom, then `.csv`. Always 30 characters, and no separator, dot-dot
    or percent-escape can be spelled in it. It is created with
    `createNewFile()` -- so uniqueness is the filesystem's answer, not a
    promise made by the random tag -- made read-only once written, and only
    that one URI goes into the share intent. The provider validates the segment
    it is handed with the same rule (`BoundaryLogic.exportNameValid`, one rule,
    both sides), requires a single path segment, and re-checks after
    canonicalisation that the resolved file is inside the snapshot directory,
    which also catches a symlink whose NAME is perfectly valid. That validation
    is the security half: the segment arrives URL-DECODED from another process,
    and `stelo.key` and `paircode.txt` live in the same tree.

    RETENTION, concretely: a snapshot is kept for 24 hours, and the four
    newest are kept whatever their age. A recipient reads the URI when it
    SENDS -- a Gmail draft, a queued message, a phone that regains network
    overnight -- so a day covers a delayed reader without the app having to
    guess which app got it, and the four-newest rule means the export the share
    sheet is pointing at right now can never be the file cleanup removes.
    Cleanup runs at export time, before the new snapshot exists, considers only
    files whose names are valid snapshot names, and keeps anything whose mtime
    cannot be read (0 reads as 1970, i.e. as infinitely old). Retention cannot
    extend a URI grant Android has revoked; it protects the reader that still
    holds one and opens late, which is the case that was corrupting exports.

    VERIFIED BY EXECUTION: name generation (padding, the low-32-bit tag,
    out-of-range instants, two exports in the same second) and refusal of
    `..`, `/`, absolute paths, the old live filename, empty, over-length,
    under-length and a one-character mutation at every position class, plus the
    retention rule at both boundaries -- `make boundaryjavatest`. 22 mutants
    were run in an isolated /tmp copy: 21 were killed by a named assertion, and
    the survivor is recorded below. NOT CLOSED BY EXECUTION, and it needs a
    phone: that a share target still receives the file through the new per-URI
    grant, that `setReadOnly()` does not disturb the provider's read, and that
    the snapshot directory is created where expected. `app/fetch.sh` still
    lists the retired `pancra.csv`; it is a device-pull helper and was left to
    the agent that owns it.

    THE ONE SURVIVING MUTANT, stated rather than hidden: the `age < 0` clause
    in `exportSnapshotExpendable` (a snapshot stamped in the FUTURE by a clock
    correction is kept) cannot be killed -- a negative age is not greater than
    a day either, including the `Long.MIN_VALUE` overflow, so the retention
    comparison already refuses it. It is kept as an executable statement of
    intent and both the code and the test say plainly that no assertion pins
    it. A second redundancy found the same way -- an `s %= 10^10` above a
    digit loop that already takes each digit `% 10` -- was DELETED rather than
    documented, because it had no intent the loop does not already carry.

54. [DONE] Fail logout unless server-side session deletion succeeds. The web logout
    route calls void `session_drop()`, clears the browser cookie, and redirects,
    while the DB helper silently ignores prepare/step failure. Return a typed
    deletion result, require statement completion (and expected row semantics),
    and do not claim server revocation on failure; otherwise a copied year-long
    cookie remains valid after the user sees a completed logout.

    Summary: Fail logout unless server-side session deletion succeeds.

    VERIFIED BY EXECUTION: `session_drop` now answers `enum session_drop`
    (GONE / ABSENT / FAILED), requiring `db_finished` (SQLITE_DONE) and reading
    `db_changes` before the finalize; `/logout` answers 500 with the cookie
    left in place on FAILED. faulttest's new `prep_case "DELETE FROM session
    WHERE selector=?"` proves the injected prepare failure fired and that the
    route answers 500, the row survives, and the browser stays signed in.
    synctest pins the success side by keeping the cookie VALUE, logging out,
    and presenting that value by hand -- the copied-cookie case, which the
    pre-existing "Sign in" assertion could not see because it re-wrote its own
    jar. Mutants: FAILED-treated-as-success and prepare-failure-reported-as-
    ABSENT each kill two faulttest assertions; a DELETE rewritten to match no
    row kills four synctest assertions.

    NOT COVERED BY EXECUTION (defence in depth, stated rather than claimed):
    the `db_finished` branch itself -- the fault harness can only fail a
    DELETE's PREPARE, since `fault_step_wrap` wraps a statement in `UNION ALL`
    and cannot wrap a DELETE -- and the GONE/ABSENT distinction, because
    /logout treats both as revoked (deliberately: two tabs, or a signout-all
    that got there first) and ABSENT is unreachable there anyway, the router
    having already resolved a user from the cookie. Both mutants survive.

55. [DONE] Do not let an email-only round-one request displace live pairing. Pairing
    currently lets a request naming the same account reset an unexpired exchange
    before proving knowledge of its code/session, then charges even an invalid
    packet. Anyone knowing the email can repeatedly abort the phone and burn the
    three-attempt code. Return conflict until expiry, or require an unguessable
    capability from the existing exchange to cancel/restart it, and explicitly
    define when malformed round one consumes a guess.

    Summary: Do not let email-only round one displace live pairing.

    VERIFIED BY EXECUTION: CONFLICT UNTIL EXPIRY was the design chosen, not the
    cancel capability -- a capability would have to travel in the round-1 body,
    whose shape is fixed by app/sync.c and by every installed copy of the app,
    so requiring one would refuse every retry after the first. TRADEOFF
    ACCEPTED: a phone that dies mid-exchange makes its owner wait out
    PAIR_SESS_TTL (120s), with the one exception that an exchange the server has
    already answered round 3 for no longer blocks -- otherwise a mistyped digit,
    which is the commonest pairing failure, would cost two minutes.

    THE CHARGING RULE, stated in the code: a try is charged exactly once per
    exchange that gets ESTABLISHED, meaning after the peer's round-1 packet
    passes its zero-knowledge proof. A malformed round one -- bad framing,
    unparsable hex, a ZKP the packet fails, an unknown address, no live code,
    or the conflict refusal -- consumes NO guess, because round 1 predates the
    password in EC-J-PAKE and therefore tests nothing about the code.

    Tested by srv/test/pairproxy.py, which forwards the honest client's rounds
    and replays its round-1 request mid-exchange (a replayed packet is
    well-formed, so only the rule under test can refuse it). ISOLATING CASES:
    the 320-zero round one with nothing in flight pins where the charge happens
    (nothing else can move `tries`); the injected 409 plus "the honest exchange
    still gets through rounds 2 and 3" pins the conflict rule. Mutants:
    restoring the old `cur.uid != uid` test kills six assertions (interloper
    gets 200, honest exchange dies at round 2, `tries` reaches 2); charging
    before validation kills two; removing the round-3 exception kills six,
    which is the tradeoff made visible.

56. Use constant-time elliptic-curve code at the public TLS boundary.
    `lib/p256.h` explicitly documents secret-bit branches, exceptional addition
    paths, and data-dependent reduction, yet the public server uses it for
    attacker-triggered ECDSA signatures and ephemeral ECDHE. Replace that TLS
    boundary with a reviewed constant-time implementation/platform TLS backend,
    or comprehensively make and validate the scalar, point, and reduction code
    constant-time; retain the current primitive only where its narrower threat
    model is actually acceptable.

    PARTLY DONE. The scalar's HIGH BITS no longer leak; its Hamming weight
    still does; the decision about a reviewed implementation is untouched and
    is the only thing that can close this.

    WHAT CHANGED, all of it inside lib/p256.c and all of it mechanical. Every
    secret-dependent branch that had a provably equivalent branchless form now
    has one, using two new header-only selects in lib/ct.h (`ct_cmov64`,
    `ct_nz64`): `u256_cmp` no longer returns at the first differing limb;
    `mod512`'s 512 per-bit reductions, `modadd`, `modsub`, `p256_sc_from_be`
    and `p256_sc_neg` compute the corrected value unconditionally and select;
    `p256_eq` accumulates its byte difference instead of exiting early; and
    `fast_reduce`'s two data-dependent `while` loops became fixed 5-then-8
    iteration passes, the analytic worst case (the argument is written out at
    the call site; measured maxima were 4 and 4).

    The one the item did not name and the one that mattered: `jdouble` returned
    early for the point at infinity, and `p256_mul` starts its accumulator at
    infinity, so a scalar with z leading zero bits bought z nearly-free ladder
    steps -- and the leading-zero count of an ECDSA nonce is exactly the
    quantity a lattice attack on partial nonce leakage consumes. Deleting that
    branch is exact, not approximate: the general doubling formula already
    yields Z' = 2YZ = 0 from Z == 0, and infinity is represented by Z == 0
    alone throughout the file.

    WHAT WAS MEASURED, on this x86 host, by counting the limb operations inside
    one k*G for scalars of FIXED Hamming weight 32 with the top set bit walked
    from bit 255 down to bit 40 (holding the two signals apart, which a naive
    "clear the top bits" sweep does not). Before: 49206 operations falling
    monotonically to 14122, a 3.5x readout of the leading-zero count, and
    2.67 ms of wall clock falling to 0.79 ms. After: 121402 operations for every
    one of them, identically. `p256_sc_inv` -- the ECDSA k^-1 -- went from a
    per-input operation mix (its conditional subtraction fired once for the
    input 1 and about 54000 times for a random scalar; 2.39 ms against 3.45 ms)
    to the same 653226 operations and 5.42 +/- 0.01 ms for every input, which
    makes that whole routine constant-time. Cost: about +10% on a k*G, +60% on
    a scalar inversion, and a full `ecdsa_p256_sign` from 7.5 ms to 10.2 ms
    here. The ~170 ms this repo quotes for the Milk-V Duo was NOT re-measured
    (no device access); expect the same proportions.

    Equivalence was proved rather than assumed: a 38000-line transcript of
    every internal and public routine -- u256_cmp, modadd, modsub, mod512,
    fast_reduce on both products and raw 512-bit inputs, jdouble, p256_padd
    including all five exceptional cases, p256_mul over small, boundary and
    random scalars, sc_inv, sc_neg, to_xy/from_xy/eq -- over random and
    boundary operands, diffed against the pristine code. It matches on 37891 of
    37893 lines; the two that differ are the meaningless X and Y of an infinity
    result from doubling an OFF-CURVE Y == 0 input, where both answers are
    Z == 0. srv/test/cryptotest.c gained three regression families for the path
    the CAVP vector cannot reach (d*G against the vector's published public key;
    m*G by ladder against m*G by repeated addition for m = 2..8, which is where
    the ladder spends 250-odd steps doubling infinity; and 0*G), and all three
    were confirmed load-bearing by mutation. cryptotest, tlstest and tlsasan
    pass.

    WHAT REMAINS, and what each option costs.

    (a) The Hamming weight of the scalar. `p256_mul` still skips the addition on
    a zero bit, so the cost is 256 doublings plus one addition per set bit --
    measured at 101376 operations for weight 1 and 183418 for weight 128, a
    straight line at 646 operations per set bit. This is one aggregate number
    per signature with NO positional information, which is a far weaker input to
    a lattice attack than the leading-zero count it replaces, but it is not
    zero. Closing it needs an always-add ladder with a masked accumulator, which
    is worthless unless (b) is done too, and together they roughly double the
    cost of every scalar multiplication.

    (b) `p256_padd` still branches on infinity and on point equality. Unlike
    jdouble's, these cannot be deleted -- the general formula answers infinity
    for "infinity + Q" where the answer is Q -- so it needs a select over five
    outcomes plus an unconditional doubling, about 25 field multiplies where
    there are now 16. Inside the ladder it contributes no positional leak
    (exactly one addition per scalar meets an infinite accumulator, whatever the
    scalar, and the equality cases are unreachable there); the exposure is
    lib/jpake.c and ecdsa_p256_verify, whose operands are the peer's public
    values.

    (c) THE ACTUAL DECISION, which is not ours to take. A reviewed constant-time
    P-256 -- vendored, or a platform TLS backend -- is what the item asks for
    first and what the TLS boundary deserves. This repo deliberately vendors
    nothing but sqlite, and the server cross-compiles for riscv64/musl while the
    app cross-compiles for aarch64/Android, so "use the platform's TLS" means
    two different platforms and a build that no longer stands alone. The
    alternative -- finishing (a) and (b) by hand -- would produce a file that
    CLAIMS constant time with no test in this repo able to detect a violation,
    which is why the work above stopped at transformations whose equivalence
    could be diffed. lib/p256.h now states all of this where the old warning
    was, including the residual, so nobody reads the file and assumes more than
    it does.

    RESIDUAL RISK if the current primitive is kept: an attacker who can force
    full handshakes learns, per signature, a noisy sample of a function whose
    only secret-dependent term is popcount(k). No published attack recovers a
    P-256 key from remote Hamming-weight timings of a ~170 ms operation, and the
    network noise is far larger than the 646-operation-per-bit signal. That is
    an argument for it being impractical, not for it being absent.

    Summary: Use constant-time elliptic-curve code for public TLS.

57. Replace secret-indexed AES lookup tables with constant-time AES. The shared
    AES implementation indexes its S-box with secret-derived key and state bytes
    for TLS GCM traffic, exposing cache access patterns to co-resident or fine-
    grained timing observation. Use a vetted hardware-backed implementation
    where available or a constant-time bitsliced/software backend with no
    secret-dependent table access.

    PARTLY DONE, and honestly barely: the leak is real, it is exactly where the
    item says, and NOTHING short of a different implementation removes it. What
    changed is that the file now says so, and one free non-fix was taken.

    WHAT CHANGED. `xtime` folded in the reduction polynomial with
    `(x >> 7) * 0x1b`, a multiply by a secret 0 or 1; it is now a mask,
    `(0 - (x >> 7)) & 0x1b`. Every core this repo targets has a fixed-latency
    multiplier so this was almost certainly not leaking, but "almost certainly"
    is a claim about a microarchitecture and the mask costs the same. Verified
    exhaustively over all 256 inputs, and the whole cipher against the pristine
    version over 200000 random (key, block) pairs, byte for byte. That is a
    rounding error next to the S-box and is recorded only so the diff is
    explicable.

    THE LEAK, precisely. lib/aes.c:85 indexes `sbox` with a state byte, 16 times
    a round, 10 rounds, and lib/aes.c:64-67 indexes it with key-schedule bytes,
    16 more times per block -- because `expand()` runs inside
    `aes128_encrypt()`, so the key schedule is recomputed for every 16 bytes of
    TLS record traffic rather than once per connection. That is 176 key- and
    state-derived cache-line selections per block, which is the whole of the
    Bernstein / Osvik-Shamir-Tromer attack surface. It is NOT a timing leak in
    the network sense: it needs an observer sharing a cache with this process.

    WHAT WAS NOT DONE, and why not. A bitsliced AES written here would be a real
    cryptographic implementation claiming a property no test in this repo can
    check -- worse than the present file, which at least does not claim it. A
    hardware backend exists for the phone (ARMv8 AES) and does NOT exist on the
    riscv64 C906 the server runs on, which is the side that needs it, so that
    route is two backends plus a feature check plus this fallback. Vendoring a
    vetted implementation is the right answer for the TLS side and is a policy
    question about a repo that vendors only sqlite. All three are decisions for
    the owner, not edits.

    THE JUSTIFICATION FOR RETAINING IT MEANWHILE, argued rather than asserted,
    and now written into lib/aes.h where a reader will find it. app/dexcom.c's
    use (the Dexcom per-connection auth hash) is fine on its own terms: the
    observer would have to be code on the same phone defeating the Android
    sandbox, at which point the sensor key is not the problem. The TLS use is
    acceptable ONLY because of where this server is deployed -- srv/deploy puts
    one binary on a dedicated Milk-V Duo behind a front door, with no second
    tenant to be co-resident with, and an attacker who can already run code on
    that board does not need the traffic key. That justification is a property
    of the deployment, not of the code, so aes.h states the condition out loud:
    do not run this server anywhere it shares a CPU with code you do not
    control. Change the deployment and this item reopens as a blocker.

    NOT MARKED DONE because the item offers no "accept it" branch -- it asks for
    a vetted or bitsliced backend, and neither exists here.

    Summary: Replace secret-indexed AES tables with constant-time AES.

58. [DONE] Enforce HKDF input and output bounds before deriving bytes. `hkdf_expand()`
    copies arbitrary `infon` into a 289-byte stack buffer and appends a counter,
    while its one-byte counter can wrap when output exceeds the RFC limit. Make
    the API return status and reject info beyond local capacity and output above
    `255 * HASH_LEN` before writing, or stream info through HMAC to remove the
    scratch bound; require callers to handle refusal.

    Summary: Enforce HKDF info and output bounds before deriving bytes.

59. [DONE] Bound every field in the TLS HKDF-label encoder. `hkdf_expand_label()` uses
    a fixed stack buffer but copies caller-provided label and context lengths and
    encodes output size without checking local capacity or TLS vector widths.
    Return failure for oversized label, context, or output before copying, or
    construct the label through a checked bounded writer and propagate errors.

    Summary: Bound every field while constructing TLS HKDF labels.

60. [DONE] Reject invalid PBKDF2 parameters instead of silently changing them. The
    implementation truncates salts beyond its scratch limit and treats zero
    iterations like one, while its void API cannot report either problem.
    Stream `salt || INT(block)` into HMAC or reject oversize salt explicitly,
    reject zero iterations with a typed status, and leave output untouched on
    failure.

    Summary: Reject invalid PBKDF2 costs and never truncate salts.

61. [DONE] Publish J-PAKE peer rounds only after their proofs validate. Peer packet
    decoding writes directly into persistent round state and sets `have_r*`
    before checking the zero-knowledge proof. A failed call therefore leaves an
    object apparently holding an attacker-controlled accepted round if reused.
    Decode and verify into temporaries, publish and set the flag only on success,
    or make failure terminal and explicitly clear the poisoned exchange.

    Summary: Publish J-PAKE peer rounds only after proof validation.

    Both designs were taken, because they fail independently. All three
    `jpake_peer_round*` now decode into a stack `PCert`, verify THAT, and copy
    into `p->r*` / raise `have_r*` only once the proof holds; and any failure
    from any entry point sets `poisoned`, clears every `have_r*`, zeroes
    r1/r2/r3, and makes every later call refuse. All three rounds had the
    pattern, not just one. `jpake_accepted()` and `jpake_poisoned()` were added
    so the state is assertable at all -- it was previously invisible from
    outside the object, which is why nothing caught this.

    NO CURRENT CALLER REUSED THE OBJECT AFTER A FAILURE, so this was hardening
    rather than a live exploit: srv/pair.c calls pair_reset(), app/sync.c
    breaks to jpake_free(), srv/synccli.c exits, app/dexdriver.c goes to
    P_FAIL. app/dexdriver.c is the one that keeps the object alive (dc->pairing
    is freed only on connect/disconnect/forget), so it was the near miss.

    TWO BEHAVIOURS ARE NOT PINNED BY MUTATION, deliberately recorded rather
    than left to be rediscovered. (a) Publish-before-verify on a single round
    is an EQUIVALENT MUTANT while the failure-clear exists: the clear scrubs
    exactly the state the early flag would have exposed, so no black-box
    assertion can tell them apart. The pair is only killed together (mutants
    M3/M4/M5). (b) The `have_r1` precondition in `jpake_peer_round3` cannot be
    isolated: `validate_round3` against the calloc'd zero base refuses every
    packet a test can build without reimplementing lib/jpake.c's Schnorr hash,
    so a LATER check does the refusing. A round-3 proof forged for the
    (pA + pB) base IS constructible in principle, so the guard is not vacuous
    -- it just has no failing assertion behind it.

62. [DONE] Wipe cryptographic secrets with a non-elidable primitive. J-PAKE uses an
    ordinary `memset()` immediately before `free()`, which optimizing compilers
    may remove, and TLS/crypto lifecycle buffers similarly retain passphrases,
    scalars, and traffic secrets. Provide `explicit_bzero`/`memset_s` or a
    validated volatile-backed abstraction and apply it before free and at TLS
    teardown/failure boundaries.

    Summary: Wipe cryptographic secrets with a non-elidable primitive.

63. [DONE] Reject numeric overflow before routing or narrowing parsed fields. Route
    bucket parsing calls `strtol()` without errno/range checking, while row
    decoding accepts long decimal fields and casts trend/source to int without
    verifying `INT_MIN..INT_MAX`. Use cutoff arithmetic or checked conversion
    with exact end pointers and semantic bounds, rejecting overflow rather than
    creating noncanonical bucket aliases or implementation-dependent row data.

    Summary: Reject numeric overflow before routing or narrowing fields.

64. [DONE] Enforce GCM counter and bit-length limits before sealing. The generic GCM
    API increments a 32-bit counter without checking block count and converts
    `size_t` lengths to 64-bit bit counts modulo the format domain. Oversized
    calls can wrap the counter and reuse keystream. Return typed failure and
    enforce the SP 800-38D per-invocation plaintext/AAD/counter limits before
    producing any output.

    NOW COMPLETE. The limits are enforced, the keystream can no longer be
    reused, and the two srv/tls.c callers that could not see the refusal now
    check it -- srv/tls.c:451 (a record) and :1119 (a session ticket), each
    answering 0, this file's failure convention. `warn_unused_result` is on
    `aes128_gcm_seal`, so the compiler now refuses any future caller that
    discards the status: that attribute, not a comment, is what keeps this
    closed. Verified: make srv, cryptotest, tlstest (real handshakes through
    the new sealer) and format all pass with the attribute in place.

    Neither branch is reachable today -- a record is at most REC_MAX + 1 bytes
    and a ticket is 40, both far under 2^36-32 -- and both are written anyway,
    because "unreachable" is a property of REC_MAX and of sizeof pt, not of
    these calls.

    The residual recorded below stands: the ACCEPTANCE side of both bounds is
    pinned by the pure predicate `aes128_gcm_limits`, never by driving the
    sealer at 64 GiB, and a sealer made one byte STRICTER than the predicate
    survives the suite. That threshold cannot be pinned from below.

    WHAT CHANGED, all of it in lib/gcm.{c,h}. `aes128_gcm_seal` returned void;
    it now returns `enum gcm_status` and both it and the new
    `aes128_gcm_unseal` judge every argument and both lengths BEFORE the first
    output byte -- including before the first TAG byte, which is the value a
    caller that ignored the status is likeliest to pass on regardless. The
    bounds are `GCM_PT_MAX` (2^36 - 32 bytes = SP 800-38D 5.2.1.1's
    len(P) <= 2^39 - 256 bits) and `GCM_AAD_MAX` (2^61 - 1 bytes = its
    len(A) <= 2^64 - 1 bits). The plaintext bound IS the 32-bit counter's block
    capacity rather than merely resembling it -- 2^36 - 32 bytes is exactly
    2^32 - 2 blocks, the largest count for which a counter starting at 2 never
    leaves {2, ..., 0xFFFFFFFF} -- and five `_Static_assert`s in gcm.c tie the
    constants to that derivation so the two cannot drift.

    The AAD bound is a separate defect from the counter and was the quieter of
    the two: `be64(len, (uint64_t)aadn * 8)` wraps, so an AAD of 2^61 bytes
    encoded the same all-zero GHASH length block as an EMPTY one. A length
    encoding that is not injective stops the tag committing to how the
    authenticated bytes were split between AAD and ciphertext, which is the only
    thing that final GHASH block is for.

    THE LENGTH RULE IS NOW A SEPARATE PURE PREDICATE, `aes128_gcm_limits(aadn,
    n)`, over two `uint64_t` byte counts with no buffer and no key. That is not
    tidiness: the bounds are 64 GiB and 2 EiB, so no test can allocate them, and
    the predicate is the only thing an assertion can drive to EXACTLY the bound
    and one byte past it on both sides. The sealer and opener call it rather
    than carrying a second copy of the comparison.

    WHAT IS OPEN, precisely. `aes128_gcm_seal` does NOT carry
    `warn_unused_result`, which every other bounded primitive in lib/ does. The
    attribute was attached temporarily and the compiler named all four sites
    that discard the result: srv/tls.c:451 (send_record) and srv/tls.c:1119
    (ticket_seal), both off limits under items 56-57, plus
    srv/test/cryptotest.c:210 and :241, the two pre-existing NIST vector calls
    in a file another item was mid-append on. The patch for all four is in the
    item 64 report. Until they are taken, those two TLS sites cannot notice a
    refusal and would transmit a record built from an uninitialised stack
    buffer if one occurred. It cannot occur there -- `inner` is at most
    REC_MAX + 1 and the ticket plaintext is 40 bytes -- so this is a latent
    hole, not a live one.

    NOTHING IN THIS REPOSITORY CAN REACH EITHER BOUND TODAY, and the item reads
    as though something could. Stated because overstating it would be its own
    dishonesty: the bounds guard lib/gcm.c as the generic primitive its own
    header says it is, and the next caller to compute a length from something a
    peer sent.

    WHAT IS VERIFIED BY EXECUTION AND WHAT IS NOT. 58 assertions in
    srv/test/cryptotest.c. Every REFUSAL is driven through the real sealer and
    opener, which works only because the refusal precedes any write -- a bogus
    length goes in beside a 64-byte buffer and no memory is touched. Every
    ACCEPTANCE at the exact bound is asserted against the predicate ALONE and
    cannot be asserted against the sealer at all; two mutants that make the
    sealer one byte stricter than the predicate SURVIVE the suite, which is the
    honest shape of that gap rather than a claim about it.

    ALSO FIXED HERE, AND IT BELONGS TO ITEMS 56-57 RATHER THAN TO THIS ITEM:
    `gf_mul` branched on secret data twice -- on the bits of the GHASH
    accumulator, and on the low bit of H = AES(key, 0), the authentication
    subkey. The second is the worse one: the 128-value lsb sequence is a
    function of H alone, so it was the same timing signature on every block of
    every record of a connection, and recovering H is tag forgery. Unlike
    lib/aes.c's S-box this needed no co-resident attacker. Both are now
    branchless via ct.h's `ct_mask64`. Measured by operation count: the old code
    ran 2113 to 4161 byte operations as popcount(x) went 0 to 128, a 1.97x
    readout of the accumulator, and 4096 to 4218 across choices of H; the new
    code runs 4224 for every one of them, identically. Equivalence was checked
    as a differential transcript against the pristine function over 401024
    inputs -- every single-bit pattern at all 128 positions on both sides,
    crossed against zero, all-ones and the reduction-triggering shapes, plus
    400000 random pairs -- with zero mismatches, and the NIST vectors still
    pass. Cost: +1.5% operations at the worst case, wall clock within noise
    (the branch it removes was unpredictable, which paid for the extra XOR).

    Summary: Enforce GCM counter and bit-length limits before sealing.

65. [DONE] Rejection-sample nonzero P-256 secret scalars uniformly. J-PAKE and TLS
    ECDHE reduce one random 256-bit value modulo the group order and do not
    reject zero, introducing bias and admitting the invalid zero scalar. Provide
    one scalar generator that rejection-samples the canonical interval
    `[1,n-1]` and use it for every ephemeral/private scalar and proof nonce.

    Summary: Rejection-sample nonzero P-256 secret scalars uniformly.

    PARTLY DONE. The generator exists, all four call sites use it, and every
    rule in it is pinned by execution. What is open is one test, not one line
    of behaviour: the failure return at the FOURTH call site
    (`app/dexcom.c`'s ECDSA key-challenge nonce) is checked in the code and
    exercised only on its success path, because the tests that link that file
    live in `app/test/` and belong to item 45.

    `p256_sc_rand()` in lib/p256.c draws 32 bytes and refuses the value if it
    is zero or >= n, up to `P256_SC_RAND_TRIES` (64) times. Rejection
    probability is 2^-32 exactly to four figures, so the expected count is
    1.000000000233 draws; the cap is not for bad luck (64 straight rejections
    is 2^-2048) but for a source stuck at a constant, where the difference
    between a bounded failure and `while (1)` is a handshake that fails versus
    a worker that stops answering. It lives with the curve rather than in
    lib/rand.c because it is a P-256 fact, and lib/rand.c deliberately names
    no algorithm; lib/jpake.c and app/dexcom.c dropped their "rand.h" includes
    so raw bytes are no longer in scope where a scalar is wanted.

    FOUR SITES, NOT THREE, and lib/ecdsa.c is none of them -- it takes `k` as
    a parameter and always refused `k == 0`, so ECDSA's zero handling was
    already right and only its bias was not. The other three (lib/jpake.c's
    five scalars per exchange, srv/tls.c's ECDHE scalar, srv/tls.c's
    CertificateVerify nonce) admitted zero outright.

    THE ZERO ADMISSION WAS NOT REACHABLE, and the reason is worth recording
    because nobody chose it: a zero scalar makes the emitted point infinite,
    an infinite point has no affine encoding, and `p256_to_xy` refuses it --
    so lib/jpake.c's `cert_byteify` and srv/tls.c's shared-secret step each
    failed before a byte was transmitted, both before `send_server_hello`.
    Two unrelated functions were holding the line that `p256_sc_from_be(&d,
    priv)` looked like it was holding. So this is hardening plus a bias fix,
    not a live exploit -- and the bias itself (the bottom 2^-32 of the
    interval arriving twice as often) supports no practical attack on P-256.

    30 assertions in srv/test/cryptotest.c, driven by a SCRIPTED entropy
    source through `rand_set_source`: all-zero refused, exactly n refused, n-1
    and 1 accepted unchanged, two out-of-range draws followed by a good one
    where the accepted scalar is the LATER draw, an always-out-of-range source
    failing at the cap rather than hanging, and a dead source propagated
    without a second ask. Failure reaches `jpake_new` (NULL) and a real
    `tls_handshake` run over a socketpair against a hand-built ClientHello --
    which fails having sent the client nothing, and whose CertificateVerify
    nonce is shown to go through the generator by its 2 + 64 draw count.

    14 of 15 mutants killed. ONE SURVIVOR, PROVABLY EQUIVALENT: deleting the
    `if (!p256_sc_rand(priv))` check in srv/tls.c's ECDHE is undetectable from
    outside, because the generator zeroes its output on failure, 0*G and 0*P
    are both the point at infinity, and `p256_to_xy` refuses to encode either
    -- still before anything is written. The two differ only in where the
    abort happens. That is the zeroing being fail-safe rather than an
    assertion that could be written better, and it is why the failure value is
    zero: it is the one scalar every consumer here independently refuses. One
    further mutant (removing the cap) is killed by a timeout rather than by a
    named assertion, which is inherent -- non-termination cannot be asserted.

66. [DONE] Make every HTTP assertion consume a fresh successful response. The shared
    `req()` test helper does not remove its body/header/status files before curl;
    if curl fails before rewriting them, later assertions can reuse the prior
    response. Clear all response artifacts first, make curl failure set the
    suite failure, and require an expected status for every request whose body
    or headers are asserted.

    Summary: Require fresh successful responses for every HTTP assertion.

    WHAT CURL ACTUALLY LEAVES BEHIND, measured rather than assumed (curl 8, a
    connection refused on a closed port): the status file becomes `000` because
    the SHELL truncates it before curl runs, the header dump is truncated to
    NOTHING because curl opens it and then fails, and the body file IS NOT
    TOUCHED AT ALL. So the one artifact every assertion reads through `$(req
    ...)` was the one artifact that kept the previous response.

    Three shapes of green-over-nothing followed from that, all of them live in
    `srv/test/synctest.sh` before this change:

      * line 115, "a session cookie smuggled in the request body does not sign
        you in", asserts the answer says "Sign in". The request before it is a
        failed login, whose page also says "Sign in". A curl that failed there
        passed the assertion against the earlier page -- a security control
        reported as working by a request the server never saw.
      * line 220, `nk "a bare round number is still routed"`, is a FORBIDDEN
        string assertion. Those pass on an empty body, so clearing the
        artifacts makes them pass more reliably, not less. Only a required
        status catches it.
      * `nk_hdr` had the same fail-open shape at the header level: curl leaves
        the dump empty, so "...and it sets no session cookie" and "...and is
        not even typed as a GIF" were true of every request that never
        happened. Ten `req` call sites asserted a body or a header with no
        status assertion at all.

    `req` is now `req <expected-status> <METHOD> <path> [curl args...]`. The
    status is the FIRST argument and is validated as three digits, so the old
    two-argument form is a named failure rather than a request expecting a
    status called "GET": a caller physically cannot ask for a body without
    stating what the answer should be. All 54 call sites in synctest.sh were
    converted and every expected status is confirmed by execution. The 41
    `ck_code` lines that follow requests were kept -- they name the rule in the
    transcript, which a check inside a subshell cannot do.

    THE SUBSHELL IS WHY THIS NEEDED A MECHANISM AND NOT AN `if`. `req` is used
    inside `$(...)`, and a subshell cannot set `fail`. So a failure raised
    there is written to `$T_TMP/.deferred` and printed to stderr immediately
    (stderr, because stdout inside `$(...)` IS the response body), and `t_end`
    -- called by all seven suites that source testlib.sh, in the parent shell,
    just before the verdict -- folds those records into `fail`. That is what
    makes "curl failure sets the suite failure" true even for a request nobody
    asserted anything about.

    Also fixed here, same family: `nk_hdr` and `ck_hdr` now fail when there are
    NO headers instead of reporting a forbidden header absent from a response
    that never arrived, and `req` fails when `$T_TMP` is not a directory
    instead of writing every response to `/.body`.

    PROVED BY A TEST OF THE HARNESS. `srv/test/harnesstest.sh` (new, wired in
    as `make harnesstest` and into `check`) runs suites as child processes and
    requires them to FAIL. Its isolating case for this item is the two-request
    one: a successful request, then one to a closed port, with the second
    assertion looking for text only the FIRST response contains. It also pins
    the case a required status cannot catch on its own -- a 200 whose body was
    cut off mid-transfer, where curl exits 18 and `%{http_code}` still says 200
    -- which is why curl's exit status is checked separately rather than
    trusted to disagree with the code.

    EACH RULE WAS MUTATED AND WATCHED TO FAIL, in an isolated copy: not
    clearing the artifacts, ignoring curl's exit status, accepting any first
    argument, accepting a wrong status, skipping the T_TMP check, dropping the
    deferred record, having t_end not drain it, and both header helpers made
    fail-open again. Every one is killed by a named assertion. Two needed the
    test sharpened first, and both are worth recording:

      * the stale-body case originally matched the assertion NAME in the child
        suite's output, and a name appears there whether it passed or failed --
        so that check could not fail. It reads "FAIL <name>" now, as does every
        other verdict this file asserts on.
      * `ck_hdr` over an empty header block fails either way, because there is
        nothing for the regex to match. The empty check changes only the
        DIAGNOSTIC, so the case pins the diagnostic: "wanted Content-Type, got:
        " sends the reader to the routing code and "there are NO headers to
        look at" sends them to the request. Without that, it is an equivalent
        mutant.

    NOT COVERED: this makes no claim about a response the server sent WRONGLY
    but completely. It closes the gap between "the assertion read a response"
    and "the assertion read THIS request's response".

67. [DONE] Reserve test ports until the intended server owns them. `pick_port()` binds
    port zero, prints the selected number, then closes the socket before server
    startup; parallel suites or unrelated processes can claim it meanwhile.
    Pass an open reserved listener to the child where practical, or retry the
    bind/start sequence on collision and verify the launched process owns the
    endpoint before testing it.

    Summary: Reserve test ports until the intended server owns them.

    THE LISTENER CANNOT BE PASSED, so it is retry-and-verify. `sync` takes a
    port NUMBER on its command line and calls socket()/bind()/listen() itself
    (srv/http.c, `http_listen`); there is no argument, environment variable or
    fd convention by which a test could hand it an already-bound socket, and
    inventing one would put test-only plumbing in the server that ships. The
    same is true of `srv/test/pairproxy.py`.

    WHICH LEAVES THE VERIFICATION AS THE WHOLE OF THE FIX. Three things can
    happen in the window between the probe socket closing and the server
    binding, and only the third is what the old code got wrong:

      1. our server binds it. Fine.
      2. somebody else binds it first and ours exits on EADDRINUSE. The suite
         says "the server did not start" -- red, and naming the wrong cause.
         This is the flake three separate agents have chased in this tree.
      3. somebody else's SERVER binds it and ours dies. Our pid has not been
         reaped yet, so `kill -0` succeeds; the poll curls the port and the
         OTHER suite's server answers 200; `wait_ready` reports ready. In
         faulttest that means every fault-injection assertion runs against a
         server with no faults injected. All green, nothing tested.

    A retry loop alone cannot tell case 3 from success, which is why the item's
    "verify the launched process owns the endpoint" is the load-bearing half.
    `port_owner()` reads /proc/net/tcp and /proc/net/tcp6 for a LISTENING
    socket on the port, matches its inode against /proc/<pid>/fd, and names the
    owning pids; `own_port()` answers whether a given pid is among them, and
    FAILS when the question cannot be asked at all (no procfs) rather than
    waving it through. `wait_ready()` now requires ownership before it reports
    a server ready -- it already had the pid and a url with the port in it, so
    all fourteen launch sites in the tree gained the check without a line
    changing at any of them. `ck_owns()` puts it in the transcript as a named
    assertion at the six sites that start a server everything after depends on.

    `serve()` wraps pick/launch/verify with a bounded retry, and retries ONLY
    when the kernel says the port is held by a process that is not ours.
    Retrying a server that died of its own accord would turn one honest failure
    into five, and faulttest has cases where refusing to start is the correct
    behaviour under test. `serve_refuses()` is the mirror for the tlstest cases
    that assert a server REFUSES to start: a run lost to EADDRINUSE is retried
    rather than reported, because "it exited" satisfies the assertion while
    proving nothing about the credential under test.

    `pick_port()` also stopped being able to hand the same number out twice.
    It records every port it issues in a memo keyed by the shell's pid -- a
    FILE, because `PORT=$(pick_port)` is a subshell where a variable would not
    survive -- and it probes 0.0.0.0 rather than 127.0.0.1, which is what
    srv/http.c binds, so a port held by another process on another interface is
    no longer reported free. restoredrill.sh asks for two ports back to back
    for two servers that run at the same time; measured, the old code repeated
    a port 2 to 10 times per 300 picks on this machine, and the memo takes that
    to zero.

    PROVED BY EXECUTION, in `srv/test/harnesstest.sh`: a squatter takes the
    port `pick_port` is about to return and `serve` must notice, move, and come
    back owning a different one; `own_port` must refuse to credit this shell,
    a process merely CONNECTED to the port, and -- the case that is not
    contrived at all -- a process using the port as the SOURCE of an outgoing
    connection, which is a real hazard because test ports and source ports come
    out of the same ephemeral range; and `wait_ready` must refuse a live pid
    plus a port an HTTP squatter is answering on, which is case 3 exactly.

    FIELD PROOF, and an honest limit to it: twelve suites were run at once
    (two srvcheck, srvasan, two tlstest, tlsasan, two faulttest, two
    restoredrill, interop, harnesstest) against a thief binding and releasing
    ports continuously -- 22,436 of them during the run. All twelve overlapped
    for 10.6 s by their own timestamps, so the concurrency is asserted rather
    than assumed. Every one passed. NOT observed even so: a single `serve`
    retry. The window is narrow enough that ordinary parallelism does not enter
    it often, which is exactly why it survived unnoticed for so long, and it is
    why the mechanism is pinned by the deterministic squatter case above rather
    than by the stress run.

    MUTANTS. `own_port` made to answer yes to everything, `port_owner` made to
    count any socket rather than a LISTENING one, `wait_ready` with the
    ownership check removed, `serve` made to try once, and `pick_port` made not
    to record what it issues: all five killed by named assertions, and all five
    run three times over to be sure the kill is not a flake, since these are
    the cases with real processes and real timing in them. The clean run was
    green all three times.

    ONE MUTANT COULD NOT BE KILLED, and it is the memo's skip branch itself
    (`if port in taken: continue`). Firing it needs the kernel to offer the
    same ephemeral port twice inside one run, which no test can arrange on
    demand -- so what is pinned by assertion is that every issued port IS
    recorded, and the branch that consults that record is covered only by the
    measurement above (2 to 10 repeats per 300 picks without it, 0 with it).
    Stated rather than papered over with a case that would pass either way.

68. [DONE] Require every concurrent request to succeed before timing the batch. A
    synctest concurrency case launches ten curls, ignores every `wait` status,
    and asserts only elapsed time; ten immediate connection failures therefore
    pass. Capture each exit status and independently require the intended HTTP
    status/body before evaluating the timing/fairness assertion.

    Summary: Require concurrent requests to succeed before timing them.

    FAILING WAS THE FAST PATH. A connection refused on a closed port comes back
    in microseconds, so with the server down, the port somebody else's, or curl
    unable to run, "ten simultaneous requests all complete promptly" passed at
    maximum speed having proved that nothing was answered. That is not a
    tolerated failure mode of the assertion, it is the EASIEST way through it.

    The single timed request above it had the same hole and is fixed with it:
    `T=$(curl ... -w '%{time_total}')` on a failure prints a number near zero,
    which is comfortably inside the "under half a second" bound, so "a page is
    served immediately while three peers sit silent" was also satisfied by not
    being served. Its exit status and its body are now checked first.

    `req_bg` was added to testlib.sh for the batch. Ten concurrent requests
    cannot share one set of response files -- that is item 66's staleness with
    a race on top, where the last writer decides what every assertion reads and
    nine of the ten answers are simply gone -- so every artifact is keyed by
    slot, and the helper exits nonzero unless its own request came back with
    the status it was told to expect. That makes `wait` on each background job
    an assertion rather than a formality. The case now requires ten answers of
    200, and ten bodies each carrying the real login form, BEFORE it looks at
    the clock; if fewer than ten came back it fails by name and says that ten
    failures are faster than ten answers.

    PROVED BY EXECUTION in `srv/test/harnesstest.sh`: the same batch against a
    dead port must fail the run, against a server that TRUNCATES its answers
    (ten perfect 200s, ten cut-off bodies) must also fail it, and against the
    live stub must pass with each slot having kept its own body -- the last of
    those is what stops the first two from being a helper that simply always
    says no.

    Mutated by making the ten slots share one set of response files again, and
    run three times: killed every time. WHICH assertion fires first is not
    deterministic and is not claimed to be -- with ten curls writing one file
    the loser varies, so it lands sometimes on "each with its own body" and
    sometimes on the batch count. The KILL is deterministic; the attribution
    inside it is not, which is in the nature of the defect being a race.

69. [DONE] Rebuild rolling statistics immediately after restoring readings.
    `pancra_logs_reload()` reloads sensors, readings, insulin, and weight after
    restore but never clears/rebuilds the rolling statistics loaded only at
    startup. History and plots update while TIR/average remain stale until
    restart. Add an idempotent locked rebuild from the restored reading log and
    invoke it before publishing restore completion.

    Summary: Rebuild rolling statistics after restoring readings.

70. [DONE] Reject future alarm stamps and age live data monotonically. Alarm freshness
    predicates currently accept any `now - stamp <= limit`, so a clock rollback
    makes negative ages fresh, can preserve alarms/predictions, and suppresses
    stale/stranded transitions. Require nonnegative persisted ages and use
    monotonic arrival/deadline stamps for in-process freshness and launch grace,
    retaining realtime only for record identity/display.

    Summary: Reject future alarm stamps and age live data monotonically.

71. [DONE] Drive calibration retries and live expiry with monotonic time. Calibration
    queue retry, asked/sent intervals, and pending rescale expiry compare realtime
    timestamps while the process is running, so wall-clock jumps can instantly
    expire work or postpone it indefinitely. Maintain monotonic in-process
    deadlines, keep realtime values only for restart reconciliation, and define
    negative/forward-skew handling when loading persisted state.

    Summary: Drive calibration retries and expiry with monotonic time.

72. [DONE] Evict the true oldest timestamp from bounded insulin and weight tails. Their
    push functions discard array element zero before sorting, which is oldest by
    arrival rather than timestamp. A late backdated import can therefore evict a
    newer record and then remain in the sorted tail. Insert/sort first and retain
    the newest-by-time records, while preserving insulin assertion replay
    requirements separately.

    Summary: Evict the oldest timestamp from insulin and weight tails.

73. [DONE] Never evict provenance still cited by historical readings. The permanent
    provenance file is loaded into a 64-record cache that discards the oldest
    record not referenced by a live slot, even though historical rows continue
    citing that source ID. Retain id-indexed provenance for every cited source or
    compact durable last-wins rows without dropping IDs; bound only UI snapshots,
    not historical attribution.

    Summary: Never evict provenance cited by historical readings.

74. [DONE] Synchronize alarm prediction publication and disconnect-latch reads.
    `alarm_note_pred()` writes prediction and timestamp outside `alarm_lk` while
    alarm evaluation reads them under that lock, and `alarm_disc_latched()` reads
    a latch unlocked while writers use the lock. Publish each value+timestamp
    coherently under the alarm lock (or one atomic snapshot) and synchronize the
    latch read so safety decisions cannot mix epochs or invoke C data races.

    Summary: Synchronize alarm predictions and disconnect-latch reads.

75. [DONE] Verify the second ClientHello PSK binder after HelloRetryRequest. TLS retry
    reparses ClientHello 2 but carries the resumed PSK decision from ClientHello
    1 without verifying the new binder over the synthetic retry transcript.
    Parse identities/binders again and authenticate the selected PSK against the
    complete required transcript before allowing resumed key schedule state.

    Summary: Verify the second ClientHello PSK binder after every retry.

76. [DONE] Never dispatch HTTP after a failed TLS ticket write. Handshake completion
    discards `send_ticket()` failure and returns success even if a timeout/reset
    left a partial protected record in the stream; request handling can then
    execute work for a client unable to parse any response. Propagate the write
    failure and close before HTTP dispatch, or explicitly defer ticket emission
    through buffered connection state.

    PARTLY DONE. The code change is in and complete: `send_ticket()` returns
    `enum ticket_fate`, which separates a ticket that was never issued (a
    derivation or seal that refused, so nothing reached the wire and the only
    cost is a full handshake next time) from a ticket whose WRITE failed part
    way, and `tls_handshake` now returns 0 for the second so https.c's
    prepare() never reports success. What is OPEN is verification by
    execution: no case in srv/test/tlstest.sh fails without the change, and
    the mutant that restores `(void)send_ticket(c)` leaves the whole suite
    green. That is not for want of trying -- both ways a write can fail take
    the connection with them before the difference becomes observable. A reset
    is what makes send() fail, and an arriving RST also flushes the receive
    queue, so the request the unfixed server would have executed is gone
    before it reads it; and the deadline predicate is consulted at the top of
    every full_read pass as well as every full_write pass, with a 1 s
    SO_RCVTIMEO to make a blocked read come back and check it, so any budget
    that expires in time to fail the ticket write has already failed the
    client Finished read microseconds earlier. Landing between those two is a
    window of a few microseconds, which is a race to arrange rather than a
    test. Reaching it deterministically needs a fault-injection hook and its
    own build (the shape srv/db.c uses behind -DDB_FAULTS), which is a larger
    change than this item asked for. The reasoning is recorded in a NOT TESTED
    HERE block in srv/test/tlstest.sh so the next reader does not have to
    rediscover it.

    Summary: Never dispatch HTTP after a failed TLS ticket write.

77. [DONE] Handle TLS KeyUpdate or reject it explicitly. The protected-record loop
    silently clears and ignores every non-application/non-alert record, including
    KeyUpdate. A conforming peer then switches keys while the server retains old
    keys and drops the next record. Implement traffic-secret update and sequence
    reset, or send `unexpected_message` and close deterministically rather than
    pretending the message was accepted.

    Summary: Handle TLS KeyUpdate or reject it explicitly.

78. [DONE] Do not drop meter state behind an older in-flight save.
    `meter_sync_save()` snapshots state before entering its single-flight guard;
    a concurrent caller that finds a writer active returns failure/skip, but its
    newer mutation is absent from the active writer's buffer and may never be
    persisted. Serialize callers through completion, or track a dirty generation
    and make the active writer re-snapshot and loop until it has committed the
    newest generation.

    SERIALISED THROUGH COMPLETION, which is the first of the two designs the
    item offers. msync_lk (app/meterstore.c) is taken around the render AND the
    write, so a second caller waits and then renders the table as it is when
    its turn comes -- which necessarily contains its own mutation. When
    meter_sync_save() returns 0 the bytes on disk are at least as new as the
    table was when the call began, and that is a contract a test can assert at
    the call.

    WHAT IT GIVES UP, plainly: a caller arriving during another save waits out
    one atomic_replace instead of returning at once, and N racing callers
    perform N writes where the single flight performed one. Affordable because
    of the caller list rather than as a general principle -- every caller is a
    BLE binder callback (pancra_meter_rssi from a connection's RSSI read,
    meter_note_advert from a scan result), the advert path is throttled to one
    per meter per minute, and MAIN never reaches it. The dirty-generation
    alternative keeps the non-blocking return but weakens the contract to "your
    value is on disk, or a writer that will include it is still running", which
    no caller can act on and no test can assert at the call.

    mrt_lk is still not held across the file: msync_lk is taken OUTSIDE it,
    which is calfile_lk's shape. Both are in app/thread.h's rank table and in
    app/test/lockorder.py (meter-sync-file, rank 3).

    Covered by execution in app/test/meterstoretest.c ("a save that loses a
    race must not lose the reading"): four saver threads, one meter id each so
    each id's stored time is monotone, plus a fifth meter written by a thread
    that never saves. Every successful save re-reads the file and demands its
    own value or newer; a watcher thread reads the file throughout and demands
    that no meter's stored time ever goes backwards.

    Summary: Persist meter updates that arrive during an in-flight save.

79. [DONE] Synchronize session-cache mutation, snapshots, and flush state. The
    session table, count, and save-rate state are plain globals accessed by
    render-side restore/put paths and service-heartbeat flush/save paths without
    one lock. Protect table and scheduling state, take a coherent snapshot under
    lock, perform `atomic_replace()` outside it, and reconcile a generation
    afterward so a concurrent update cannot create a torn file or be marked
    saved.

    Done exactly as the item describes, on app/settings.c's save_now/write_job
    pattern. sessc_lk covers the table, its count, the generation and the
    save-rate state; sess_render() takes the coherent snapshot under it;
    sess_write() performs atomic_replace under sessfile_lk with sessc_lk
    RELEASED, so no frame ever waits for flash; and the reconciliation
    afterwards marks the cache saved only when the generation has not moved
    since the render. A second rule falls out of the same generation: a job
    that is not newer than what is on disk is not written, so two flushers
    racing cannot land an older render on top of a newer one.

    The threads, named: sessc_put and sessc_restore are on the RENDER path
    (build_model, MAIN, once per sensor row per frame); sess_flush runs from
    sensor_reconcile on BOTH the activity's 1 Hz timer (MAIN) and the
    foreground service's "pancra-tick" HandlerThread (SERVICE), which is the
    one that outlives the activity. reconcile's single flight kept the two
    FLUSHERS apart and did nothing at all about the render path.

    sess-cache (rank 4) and sess-file (rank 3) are in app/thread.h's rank table
    and in app/test/lockorder.py. Covered by execution in
    app/test/meterstoretest.c ("the session cache under render/heartbeat
    overlap").

    Summary: Synchronize session-cache mutation, snapshots, and flushing.

80. [DONE] Replace history atomically when reloading restored logs.
    `pancra_logs_reload()` assumes `store_load()` replaces history, but the loader
    only inserts into the existing in-memory table. Rows removed from the
    restored file therefore remain visible, and dedup can retain old values over
    restored ones until restart. Load into a temporary history, validate the
    whole source, then swap it under the history lock; preserve the old table on
    failure.

    The history is now a VALUE (`struct hist_tab`, app/store.c:104) and the
    dedup/insert/evict rules live in `tab_insert`, which does not care which
    array it is filling. `store_load` calloc's a whole staging table, parses
    the entire file into it, and only then does one memcpy plus one store
    inside the hold its caller already has. Nothing derived is published
    before the swap: `hist_refresh_current` runs after it, so the big number
    comes from the restored table rather than from the one it replaced.

    PRESERVE-ON-FAILURE is the load-bearing half. A `read()` that FAILED is
    not end of file -- the staged table is a prefix, not a record -- so the
    stage is freed and the previous history, the previous big number and the
    previous stored-row count are all left exactly as they were. A row that
    was REJECTED is deliberately not that case: the whole file was still seen,
    so the user keeps everything that parsed, and the -1 reports the
    understatement. Cost of the staging: NHIST x 16 B = 78.75 kB held for the
    duration of one load on an arm64-only build, freed before the function
    returns, malloc'd rather than static so it is not a permanent doubling of
    the history's footprint. That is acceptable for a phone; a second static
    array would not have been.

    ORDERED AGAINST ITEM 69, which rebuilds the statistics from this same
    file: app/reading.c:313-317 prepares the statistics with NO lock held
    (the parse resolves each row's sensor through the registry, and the order
    is registry -> history), then takes the history lock ONCE and runs
    `store_load` -- the swap -- before `stat_reload_publish`. The restored
    history and the restored TIR/average therefore become visible in the same
    instant, and the statistics are computed over the replaced table rather
    than over the merged one. `make lockcheck` passes.

    PROVED BY EXECUTION, in `app/test/storetest.c`. The isolating case is that
    a row REMOVED from the restored file is GONE from the history afterwards,
    asserted without a reset so the reload happens over a live table -- every
    other loader assertion in the file passes an insert-only loader. With it:
    a corrected value at the same timestamp beats the resident row the dedup
    would have kept, AND the big number follows it; a log holding only a
    header replaces the history with nothing; and a load that cannot read its
    source leaves the table, the big number and the row count untouched.
    Nine mutants, each killed by a named assertion and none by a crash: the
    original insert-only loader; a stage seeded from the live table; a swap
    performed from a prefix; a clear-then-load; hoisting the big-number
    publish above the swap; a defensive `if (n > 0)` around the swap; letting
    a damaged row discard the whole record; and both RSSI publication rules.
    NOT covered by execution: the calloc-failure path (no fault injection
    reaches it) and `pancra_logs_reload` itself, which is shell-only and in no
    host binary -- its ordering is held by review, `make lockcheck` and the
    compile.

    Summary: Replace history atomically when reloading restored logs.

81. [DONE] Validate every load-bearing SQLite schema property. Version-zero shape
    checking covers basic columns and indexes but not foreign keys/cascades,
    collation, defaults, or `WITHOUT ROWID`. A superficially matching DB can be
    stamped current while permitting case-variant accounts or orphan rows. Check
    canonical schema semantics through `foreign_key_list`, `index_xinfo`, table
    metadata, and other explicit properties before stamping or opening.

    Summary: Validate every load-bearing SQLite schema property.

    THE PRAGMA WAS ALREADY ON, which is the one fact that decides how bad this
    was. `db_conf` sets `PRAGMA foreign_keys=ON` and REFUSES the connection if
    it cannot verify it, on every connection the server opens -- so the
    cascades were enforced, not decorative, and the orphan-row consequence was
    never live in a database this build wrote. What was unchecked was whether
    an ADOPTED file declared them at all. `dbmigtest` now proves both halves by
    execution: deleting a user through a server connection removes its app,
    pairing, session, share token and log rows, and the same deletion on a
    connection with the pragma at sqlite's default of OFF leaves all five.

    Checked now, each by building the canonical schema with EXACTLY ONE
    property changed and requiring a refusal: the collation of every column
    (`user.email` NOCASE, via sqlite3_table_column_metadata) AND of every
    indexed column (via `index_xinfo`, a separate fact -- a BINARY unique index
    over a NOCASE column is the case-variant-account shape); every foreign key
    with its ON DELETE and ON UPDATE actions (via `foreign_key_list`),
    including the two tables that must have NONE; every DEFAULT; whether each
    table is `WITHOUT ROWID`; and whether an index is PARTIAL, which enforces
    nothing outside its WHERE. A version-0 file is also scanned for rows that
    already point at a user that is gone.

    AND NOTHING IS STAMPED UNTIL IT PASSES. The after-check used to run once
    the version bump had committed, so a file refused for its indexes was left
    marked as this build's -- and the next start then skipped the version-0
    gate, where the collations and the orphan scan live. The check now runs
    inside the migration transaction, so a refusal rolls the tables, the
    indexes and the stamp back to the file as it was found.

82. [DONE] Reject incomplete SQLite index-shape scans. Schema validation loops while
    `sqlite3_step()==SQLITE_ROW`, finalizes, and compares accumulated indexes or
    columns without requiring terminal `SQLITE_DONE`. A BUSY/IOERR/CORRUPT after
    the expected prefix can therefore look like a complete matching schema.
    Retain and validate each terminal result before accepting the shape.

    Summary: Reject incomplete SQLite index-shape scans.

    Five scans in the schema gate now require terminal `SQLITE_DONE` through
    `db_finished`, which names the real result: `table_info`,
    `foreign_key_list`, `index_list`, `index_xinfo` and `foreign_key_check`.
    The comment on the `index_list` loop claimed a partial read was "caught by
    the OVERFLOW test below"; that test catches too MANY rows and cannot catch
    too few, which is what every failure of `sqlite3_step` produces.

    AN HONEST LIMIT ON THE COVERAGE. Only `foreign_key_check` walks data on
    disk; the other four are answered from the schema sqlite parsed at prepare
    time and cannot be made to stop part-way from outside the process. So the
    behaviour is pinned by execution on that one -- a canonical database with
    3000 readings, its second half overwritten with the schema page spared, is
    REFUSED and left unstamped, and removing that terminal check makes the
    assertion fail. Removing the other four changes no assertion. They are
    correct by construction and unreachable rather than untested.

83. [DONE] Verify full schema compatibility before accepting a backup. `db_verify()`
    runs integrity check and confirms only that three table names exist; an
    intact future or incompatible database can pass preflight, displace the live
    DB, and then fail server startup. Reuse a read-only form of the complete
    version/table/index/constraint compatibility validator used by `db_open()`.

    Summary: Verify full schema compatibility before accepting a backup.

    The version gate and the whole property check were split out of `migrate`
    into `version_supported` and `schema_usable`, which take a raw handle, run
    only PRAGMAs and SELECTs, and are now called by `db_verify` on its
    READ-ONLY connection. A preflight weaker than the open it precedes is a
    promise it is not entitled to make: it is what decides whether a backup may
    DISPLACE the live database, so a file it wrongly approves costs the good
    copy and then fails at startup.

    Pinned by execution with the isolating cases, each of which PASSES
    `integrity_check` and has all three table names -- so each would have been
    accepted before, and a corrupt-file case would have pinned nothing: an
    intact database from a FUTURE version, and an intact current-version
    database whose `session` has lost its foreign key. Both refused. A
    version-0 backup whose schema matches is still ACCEPTED, because `db_open`
    would migrate it in place and refusing it would reject every backup taken
    before the next schema step; the same file with one orphan row in it is
    refused.

84. [DONE] Fsync the backup directory before reporting publication success. Server
    backup creation renames the verified `.part` file to its destination and
    immediately reports success without syncing the parent directory. A power
    loss can therefore erase the acknowledged directory entry. Sync the parent
    after rename and report a distinct durability-uncertain result on failure.

    Summary: Fsync the backup directory before reporting success.

85. [DONE] Serialize every deployment, rollback, backup, and restore operation on the
    board. Scripts share staging names, PID/process state, and live DB files but
    acquire no common operation lock; concurrent invocations can overwrite one
    another's staged artifact, kill the replacement process, or interleave a DB
    move. Hold one atomic board-wide lock from staging through final health or
    rollback, use unique staging names, and reject contention clearly.

    Summary: Serialize deployment and restore operations on the board.

86. Pass deployment configuration as data, not generated remote shell source.
    Several scripts interpolate config paths and markers into quoted remote shell
    programs. A quote or metacharacter can break syntax or execute unintended
    commands. Validate configuration against a narrow grammar and transmit
    safely encoded positional arguments to a fixed installed/stdin script,
    never concatenate configuration values into remote shell language.

    Summary: Pass deployment configuration as data, not shell source.

87. [DONE] Fetch phone data atomically and fail on the first missing file.
    `app/fetch.sh` redirects adb output directly over destination files and lacks
    strict failure handling, so an adb error can truncate one file while later
    success makes the script exit zero. Fetch each file into a same-directory
    temporary, require adb success, rename atomically, and clean all temporaries
    through a trap.

    The script now asks the phone ONCE what it has (`run-as ... ls files`), so
    "the file is not there" and "adb could not be asked" are separate reports;
    every fetch lands in a `mktemp` staging file in the destination directory
    and is renamed only after adb exits zero; the temporaries are removed by a
    trap and by nothing else, deliberately, so that a trap which stopped
    working would be noticed. The list itself was wrong and is corrected:
    `pancra.csv`, `remote.pos` and `remote.pull` are gone (the first is the
    retired single-export name, the other two never existed in any build), and
    `session.cache` and `crash.log` are added. Under the old silent script a
    wrong list cost nothing; under "fail on the first missing file" it would
    have failed every pull on every phone, which is how a list nobody checks
    gets found out.

    VERIFIED BY EXECUTION, with no device attached: `make adbdrill` runs this
    script unmodified against a fake `adb` first on PATH (app/test/adbdrill.sh,
    the same arrangement srv/test/deploydrill.sh uses for the board). Six
    cases: a healthy pull; adb failing in the MIDDLE of the list; adb writing
    part of a file and then dying; a required file absent from the phone; the
    listing itself failing; and a run-as that succeeds over an empty data
    directory. Each asserts the exit status, the words of the message, the
    destination byte for byte, and that no staging file is left behind.

    THREE OF THOSE CASES ARE ISOLATING, and the drill says so in its output by
    running the OLD rule -- kept as an executable fixture rather than read from
    git, which would silently become a copy of the fix once this is committed
    -- against the same input: the mid-list failure, the partial transfer and
    the missing required file each produced a green run and exit 0 before.

    SIX MUTANTS, each removing one rule by its `#R:` marker, each run twice,
    all six killed by a named assertion: the staging-and-rename (killed by
    "sensors.csv now holds PARTIAL-BYTES-THAT-MUST-NOT-LAND"), the per-file adb
    status, the cleanup trap ("2 staging files left"), the missing-file
    pre-flight, the status of the listing, and the empty-listing rule. FOUR of
    those six are killed by the MESSAGE assertion rather than the exit status,
    because `set -eu` or a later check still fails the run -- it just blames
    the wrong thing. That is worth knowing: `set -e` was never the whole fix
    here, and a case that only checked the exit status would have passed over
    four of these six mutants.

    Summary: Fetch phone data atomically and fail on missing files.

88. [PARTLY DONE] Verify Android runtime permissions are actually granted.
    `devicesmoke.sh` greps package state for permission names, which appear
    even when requested but denied, then claims the permissions reached Android
    state. Explicitly grant or exercise the permission flow and parse granted
    flags/app-op state for every required permission before reporting success.

    DONE AND VERIFIED BY EXECUTION (`make adbdrill`, no device): the required
    set is now chosen by the device's API level rather than from the manifest
    -- minSdkVersion is 29, BLUETOOTH_SCAN/CONNECT exist from 31 and
    POST_NOTIFICATIONS from 33, and on anything older BLE scanning is gated on
    ACCESS_FINE_LOCATION, so requiring the modern three everywhere would fail
    every Android 10/11/12 device and requiring their PRESENCE passed
    everywhere and meant nothing. Each member is then explicitly `pm grant`ed
    (a refusal is a failure naming the grant) and its runtime `granted=` flag
    is read out of the dump, with `granted=false` winning over `granted=true`
    so a multi-user device that disagrees with itself fails closed. The
    package dump is collected through the same status-checked, .part-then-
    rename path as everything else, because half of `dumpsys package` still
    contains every permission NAME.

    THE ISOLATING CASE is a permission that is requested and denied while
    `pm grant` reports success -- a hard-restricted permission, or a policy
    that re-denies. Only the grant FLAG can see it. The drill runs the old rule
    against the same input and records that it PASSED; a granted permission
    passing proves nothing, since that is what already happened. A second
    isolating case covers a device with two users that disagree about one
    permission -- `granted=true` IS in the dump, for somebody else -- which is
    the only case that can see the rule that a `granted=false` anywhere wins.
    FIVE mutants (the grant flag reverted to the old name grep, the `pm grant`
    removed, the app-op check removed, the API-level set removed, the
    false-wins-over-true rule removed), each run twice, all killed by a named
    assertion; three of them by the top-level "reported a clean smoke run"
    assertion, i.e. they had restored a genuinely GREEN run.

    NOT CLOSED, AND IT NEEDS A PHONE: the app-op layer. A permission can be
    `granted=true` and its app-op ignored -- a restricted permission, an
    enterprise policy, a one-time grant that lapsed -- and the app then gets an
    empty scan result rather than an exception, which is indistinguishable from
    a sensor out of range. The check is written and its SHAPE is exercised (an
    op reported as `ignore` fails; an answer the parser cannot read fails
    closed), but the op strings in `appop_for` -- `android:bluetooth_scan`,
    `android:bluetooth_connect`, `android:post_notification`,
    `android:fine_location` -- are what the platform's OPSTR_ constants say and
    have NEVER been confirmed against what `cmd appops get` actually prints.
    The check fails closed on an answer it does not recognise, so a wrong
    string produces a red `make devicecheck` naming that comment rather than a
    green run that silently checked nothing; that is the safe direction and it
    is still unverified. One `make devicecheck` on a real phone closes it.

    Summary: Verify Android runtime permissions are actually granted.

89. [DONE] Fail device smoke when logcat cannot be collected. Its `adb logcat -d
    | grep` pipeline treats an adb failure like grep finding no fatal message
    and prints a clean result. Capture logcat to a temporary file, check adb
    status independently, and inspect only a confirmed snapshot.

    Every adb collection in `devicesmoke.sh` now goes through one `snap()`
    helper that enforces three separate rules with three separate messages:
    adb's own exit status is checked; the output lands in a `.part` and is
    renamed only when adb has finished; and an empty snapshot is refused,
    because the buffer was cleared and an app was started, so an empty logcat
    is a statement about the collection and not about the app. The scan itself
    no longer uses `if grep ...; then`, which reads grep's exit 2 as "no
    match" -- the same trap the Makefile's CRLF scan fell into, here in the
    one place where reading it as "no match" prints the all-clear.

    VERIFIED BY EXECUTION (`make adbdrill`, no device). THE ISOLATING CASE is a
    logcat that emits ordinary, non-fatal lines and THEN dies: what arrived is
    clean, so the old pipeline printed "no fatal Java/JNI error was logged" and
    exited zero, and the drill runs the old rule against that same input to
    record that it passed. The case asserts the failure, asserts the message
    says the collection failed, and asserts the all-clear line is NOT printed.
    A separate case pins the other verdict -- a collected log holding FATAL
    EXCEPTION must say "fatal Java/JNI error found" and must NOT say "could not
    be collected" -- because the whole item is that those two were one status.
    Two further cases cover the empty snapshot and a `dumpsys package` that
    arrives half way. FOUR mutants (the adb status check, the empty-snapshot
    rule, the .part-then-rename, and the fatal-error scan itself), each run
    twice, all killed by a named assertion.

    Summary: Fail device smoke when logcat cannot be collected.

90. [DONE] Require and verify the production signer for release APKs. The `release`
    target currently signs through the same generated public-password debug
    keystore as development and succeeds after merely reminding the operator to
    re-sign. Require explicit protected release keystore/alias inputs, produce a
    distinct release artifact, verify its signer fingerprint and non-debuggable
    manifest, and fail closed when production signing is unavailable.

    Summary: Require and verify the production signer for release APKs.

91. [DONE] Build DEX from a fresh exact set of Java class outputs. Java compilation
    writes into a persistent classes directory and D8 consumes every `*.class`;
    javac does not remove classes for deleted/renamed sources or obsolete nested
    types. Compile into a fresh temporary directory and atomically replace it,
    or maintain an exact output manifest, so clean and incremental APKs contain
    identical current bytecode only.

    Summary: Build DEX from a fresh exact set of Java class outputs.

92. [DONE] Rebuild packages when SDK and packaging-tool inputs change. Java, DEX, APK,
    and AAB recipes consume `android.jar`, R8, framework resources, aapt, and
    bundletool without declaring all of them as prerequisites. Replacing those
    inputs can leave Make reporting stale artifacts current. Declare every file
    input and fingerprint relevant discovered executable versions/options where
    paths alone do not capture tool changes.

    Summary: Rebuild packages when SDK and packaging tools change.

93. [DONE] Cap and prune each owner's live share tokens. Every authenticated mint adds
    another 14-day `share_token` without deleting expired/spent rows or enforcing
    an owner quota, permitting unbounded DB and settings-render growth. In one
    transaction, prune expired/spent tokens and enforce a small per-owner live
    cap by refusing or replacing the oldest, backed by an appropriate index.

    Minting moved out of the settings handler into `share_token_mint`
    (srv/auth.c), because the rule is about the TABLE and a rule that lives
    inside one handler is a rule the next mint site will not get. One durable
    transaction does three things in an order that is load-bearing: sweep every
    owner's expired and spent rows, THEN count, THEN insert. Counting first
    would refuse an owner on the strength of ten tokens that all died a
    fortnight ago -- a lockout with no way out, since an expired link is not on
    the page that has the Revoke button.

    THE CAP REFUSES; IT DOES NOT REPLACE. Both were permitted and they are
    different promises. By the time an eleventh link is minted the first ten are
    in other people's inboxes, so replacing the oldest would silently kill an
    invitation somebody was sent last week -- the owner reads "Share link
    created" and cannot tell they have just broken the link they mailed their
    doctor, who finds out by clicking it. Refusal is reversible, replacement is
    not. MAX_LIVE_TOKENS is defined AS MAX_FOLLOWERS so the two cannot drift: a
    live link is a follower in waiting, and more unredeemed links than the
    account can ever accept followers is by construction more than can be taken
    up.

    Backed by `share_token(owner_id, created_at)`, added as migration step 2
    (never by editing step 1) with the canonical INDEXES expectation updated to
    match, or the shape gate items 81-83 landed would refuse every database.
    Adding a second step exposed a latent defect in `migrate()`: the index
    check can only run at the final version, so a file with a wrong
    `user(email)` index sailed through step 1, committed, and was stamped
    version 1 before step 2 refused it -- leaving it marked as having passed a
    gate it had not, and skipping the version-0 collation and orphan scans on
    the next start. An abandoned run now puts the version stamp back where it
    found it. dbmigtest caught this; it was not found by reading.

    Summary: Cap and prune each owner's live share tokens.

94. [DONE] Prune expired sessions and cap active sessions per user. Each successful
    login/invite creates a year-long row, while expiry deletion happens only if
    that exact cookie is presented later. Prune global/per-user expiry during
    session creation or maintenance and transactionally cap active sessions per
    account, revoking oldest excess sessions.

    `session_new` is now also the sweep. In the same transaction as the insert
    it deletes EVERY expired session row in the table -- anybody's, no cookie
    required -- and revokes this account's oldest sessions down to
    MAX_SESSIONS. The global half is what the old rule structurally could not
    do: it only ever looked at the row it had just been handed the key to, so
    the sessions that got cleaned up were precisely the ones still in use, and
    a browser somebody signed in from once and never opened again left a live
    year-long credential nothing would ever reach. Item 54 made /logout fail
    closed rather than report a revocation that did not happen; this is the
    other half -- the sessions nobody ever logs out of at all.

    MAX_SESSIONS is 8. The thing being counted is COOKIES, not devices, and
    cookies are minted far more often than devices are acquired: about five
    machines somebody really signs in from, some of them holding two browsers,
    plus a cleared cookie jar or a private window minting a row without
    retiring the one it replaces. Five would sign people out of devices they
    are still using, which is the failure that makes a cap worse than none;
    much higher and a year of ordinary use never reaches it. Which row goes is
    decided by `last_seen` (tiebroken on rowid, so rows created in the same
    second are not sqlite's choice), so the credential revoked is literally the
    one that has gone longest without being used.

    NOT ON THE REQUEST PATH. `session_user` runs on every page view and a sweep
    there would charge every reader for every other account's housekeeping --
    item 35's shape on the nonce table, except the nonce prune is a bounded
    delete on a small sliding window and this is not. Creation is the right
    place: it is the event that adds a row, it happens a handful of times per
    account per year, and it already costs a quarter-second of PBKDF2.
    Fail-closed on purpose: step two REVOKES credentials, and a login that
    quietly declined to revoke the excess has handed out a ninth live cookie
    while the account's limit says eight.

    `db_in_transaction` was added because the invitation POST calls
    `session_new` from inside an already-open transaction that is creating the
    account, and sqlite has no nested BEGIN. Backed by `session(user_id,
    last_seen)`, in the same migration step 2 as item 93's index.

    Summary: Prune expired sessions and cap active sessions per user.

95. [DONE] Enforce one canonical email bound on every authentication surface. Normal
    login refuses email above 254 bytes, but invitation authentication accepts a
    much larger form field before throttle/account lookup and may record or
    create it. Apply the same exact email length/syntax/canonicalization before
    throttle lookup, account lookup, failure recording, and account creation in
    login, invite, CLI, and related paths.

    ONE RULE, IN ONE PLACE: `email_canon` (srv/util.c, declared in srv/util.h).
    It trims the outer whitespace, then requires 3..254 bytes, exactly one `@`
    with something either side, and no byte at or below 0x20 or equal to 0x7f;
    it folds `A-Z` to `a-z` and NOTHING else. The fold is A-Z only on purpose --
    that is exactly what `user.email COLLATE NOCASE` folds (srv/db.c), so this
    function's idea of "the same address" is the account table's idea, byte for
    byte. Bytes >= 0x80 pass through untouched for the same reason, and because
    refusing them would lock an existing account holder out of a server that had
    already accepted their address, with `sync passwd` refusing it too.

    THE SECOND DEFECT, WHICH WAS NOT ABOUT LENGTH. `login_fail.email` is a TEXT
    PRIMARY KEY with no collation, so it is BINARY, while the account lookup is
    NOCASE. One account therefore had as many throttle rows as there are
    spellings of its address: five failures as `bob@x`, five as `Bob@x`, five as
    `BOB@x`, and the counter never reached LOGIN_FAIL_MAX. Canonicalising makes
    the throttle key agree with the collation the lookup already used.

    CALLED FIRST ON EVERY SURFACE, before the throttle lookup, and its output is
    what every later call uses: `h_login_post` and the invitation form's `go`
    branch (srv/invite.c), `adduser`/`passwd`/`logout`/`invite` (srv/sync.c),
    pairing round 1 (srv/pair.c), and `synccli pair` (srv/synccli.c, where the
    round-1 body was built with an unchecked snprintf that TRUNCATED the J-PAKE
    packet and handed http_do a length past the end of its buffer). `user_create`
    and `login_failed` (srv/auth.c) additionally REFUSE a non-canonical address
    as a backstop -- unreachable while every caller behaves, and the difference
    between a 500 and a written row when one does not.

    VERIFIED BY EXECUTION: `make emailtest` (srv/test/emailtest.sh, 56
    assertions) against a real server and a real database. Every refusal is
    asserted against `login_fail` and `user`, never against the HTTP status: a
    401 is also what a wrong password gets, and the defect was always in the
    rows written on the way to the answer.

    THE ISOLATING CASES are the ones about what a row is KEYED ON, not the ones
    about a row's absence. An over-long address has no account, so on the
    invitation form the lookup misses and the failure branch is never entered --
    "no throttle row" would hold there even with the rule running last. What
    separates the two orders is a wrong password under a mixed-case spelling:
    the row must exist and must be keyed `jk@example.com`. At `/login` the
    absence case IS isolating, because that path records a failure even when no
    account exists, which is the unbounded-growth defect itself.

    Summary: Enforce one email bound across all authentication paths.

96. [DONE] Match calibration replies to the exact queued command. The Dexcom driver
    records only a boolean pending flag and later resolves whichever calibration
    queue entry is current. If the user replaces value A with B before A's reply,
    B can be marked accepted/rejected without being sent. Store sensor ID, value,
    and generation for each transmitted calibration; propagate that token with
    the reply and resolve only an exact live-queue match.

    WHAT IT LOOKED LIKE TO THE PERSON HOLDING THE PHONE. They take a fingerstick,
    misread the meter, type 100 and confirm; the app writes 0x34 for 100. Before
    the sensor answers they look again, see 180, and re-enter it. The ACCEPTED
    for the 100 arrives, `dc->cal_pending` says only "something is outstanding",
    and the shell resolves whatever is queued: the row reads LAST CAL 180
    APPLIED, nothing is left pending, and 180 was never sent. The sensor is
    calibrated to 100 and misreports against 100 for the rest of the session.

    THE TOKEN is (sensor id, value, generation). The generation is a counter the
    calibration module bumps on every queue (`g_calq_gen` / `g_calq_gen_next`,
    app/calib.c) -- deliberately NOT a clock, because a wall-clock stamp would
    make the IDENTITY of a write depend on an NTP step, and deliberately not
    named `g_cal_*_at`, which is the shape `make clockcheck` reserves for
    deadlines. It is part of what a queue IS, so it travels in `struct calq_undo`
    and a rolled-back queue keeps it; the counter behind it never goes backwards,
    so a number is never handed out twice. A load gives the restored entry a
    FRESH generation: a load defines the state, and a reply owed to what it
    replaced must not resolve what came off the disk.

    The driver's boolean became `dc->cal_tx` (app/dexdriver.c), written whole
    before the write goes out and read before it is cleared; `drv_cal_result`
    and `pancra_cal_result` carry all three back. The queue resolves only on an
    exact match of all four conditions and otherwise DISCARDS the reply with a
    warning -- and a discard costs nothing, because the queued value was never
    sent, its resend throttle was never stamped against it, and the next stream
    attempt writes it.

    VERIFIED BY EXECUTION: `make calibtest` (204 assertions) and `make
    drivertest`, both under ASan/UBSan and TSan.

    THE ISOLATING CASE is queue 100, transmit it, replace it with 180, then
    deliver 100's reply: 180 must NOT be marked resolved, must still be queued,
    and must go out on the next attempt. "The right one resolves" is every case
    that already passed. Three more separate the fields: the same value
    re-entered is a new calibration; a reply naming another sensor resolves
    nothing; a reply whose VALUE is not the one written resolves nothing even
    with the right sensor and generation. And one is the mirror -- a queue whose
    save FAILED is rolled back, so the write outstanding for it is still
    outstanding and its reply MUST resolve -- which is what stops the rule from
    being satisfied by discarding everything.

    Summary: Match calibration replies to the exact queued command.

97. [DONE] Count streaming only after accepting a usable reading. Dexcom notification
    handling marks a sensor streamed/remembered after any decoded EGV or nonempty
    backfill batch even when ingestion rejects every value or age. Return accepted
    status/count from reading callbacks and reset failure streak/persist identity
    only after at least one record enters the authoritative history.

    Summary: Count streaming only after accepting a usable reading.

98. [DONE] Project sensor session time from a monotonic receipt stamp. The driver adds
    `(uint32_t)(realtime_s() - last_clock_t)` to the last transmitter clock; a
    wall-clock rollback makes a negative delta wrap huge and corrupt warmup/end
    decisions. Store a monotonic receipt time, add only a bounded nonnegative
    monotonic delta, and retain realtime solely for persisted/display instants.

    Summary: Project sensor session time from a monotonic receipt stamp.

99. [DONE] Never write pairing secrets or sensor identifiers to logcat. The successful
    Dexcom pairing path logs the full J-PAKE code together with the sensor MAC.
    Remove all credential-bearing log arguments and log only a nonsensitive
    outcome/link category; audit adjacent diagnostic/crash logging for codes,
    keys, cookies, tokens, email addresses, and hardware identifiers.

    Summary: Never log pairing secrets or sensor identifiers.

100. Enforce private ownership and modes for all server state. SQLite live and
     backup files, operational directories/log/PID, and TLS keys rely on the
     invoking account's inherited umask and are not rejected for unsafe modes.
     Run under a dedicated account with `umask 077`, explicitly set private modes
     on DB/WAL/SHM/backups/log/PID, validate key/state ownership, and fail startup
     or deployment when same-host users can read or modify protected material.

     Summary: Enforce private ownership and modes for server state.

101. [DONE] Bound every plot CSV field before signed accumulation. `plotdata.c` parses
     fields after glucose by repeatedly computing `int n = n * 10 + digit`
     without a digit/range cap; corrupt long fields invoke signed-overflow UB
     before later normalization. Reuse a checked bounded integer cursor and
     reject the complete malformed row before mutating plot state.

     Summary: Bound every plot CSV field before signed accumulation.

102. [DONE] Range-check legacy meter indexes before accumulation. The legacy
     `meter.idx` loader accepts many leading digits into a signed int without a
     cutoff, so a corrupt/edited file can overflow during startup and publish a
     bogus index. Parse with checked arithmetic, require full canonical input,
     and reject values outside the real meter-index domain.

     Summary: Range-check legacy meter indexes before accumulation.

103. [DONE] Make string-builder capacity arithmetic overflow-safe. `sb_room()` computes
     `n + need + 1` and repeatedly doubles capacity without `SIZE_MAX` guards;
     wrap can approve an undersized allocation, overflow memcpy, or loop. Check
     each addition, reject impossible requested sizes, and use guarded doubling
     with a final exact capacity when the next doubling would overflow.

     Summary: Make string-builder capacity arithmetic overflow-safe.

104. [DONE] Validate GIF dimensions and pixel-count multiplication. `gif_encode()`
     accepts arbitrary positive int width/height, serializes only their low 16
     bits, and multiplies them through signed `long`. Require dimensions within
     the GIF 16-bit domain, validate pointers/palette/output, and compute the
     pixel count with checked `size_t` multiplication before reading pixels.

     Summary: Validate GIF dimensions and multiply pixel counts safely.

105. [DONE] Supervise and restart the server after crashes and board reboots. Deployment
     scripts launch a detached process and write a PID, but install no boot or
     crash supervisor. Use the configured service manager with restart/backoff,
     boot enablement, identity-bound health, and controlled stop/reload behavior;
     exercise unexpected exit and reboot recovery rather than only manual
     redeployment.

     WHAT IT LOOKED LIKE AT THREE IN THE MORNING. The deployment's entire story
     about the running process was one `setsid ... &` and a pid file. When the
     server exited -- a signal, a bug, an OOM kill on a board with 56 MB of RAM
     -- it stayed exited: the pid file went on naming the dead process, the front
     door went on forwarding the public port to a listener that was not there,
     and every phone got connection refused until a person noticed and ran `make
     duodeploy`. The recovery existed and worked; it needed somebody awake.
     After a REBOOT it was worse and quieter -- nothing on the board had ever
     heard of this service, because the only thing that had ever started it runs
     on the operator's laptop, so the board came up perfectly in every respect
     except the one that mattered and stayed that way indefinitely. And
     PANCRA_PID survives both events while meaning nothing after either: after a
     crash it names a dead pid the kernel may since have reissued, and after a
     reboot it names a pid that on a freshly booted board is very likely alive
     and belongs to ntpd or dropbear.

     NOT A UNIT FILE, and not because unit files are bad: the board is a Milk-V
     Duo running a buildroot image with BusyBox init, so there is no systemctl,
     and nothing in this repository can prove what any OTHER board runs. A
     deploy that wrote a unit would have installed supervision that never runs
     and reported success. So the mechanism is DECLARED, exactly as the front
     door is -- PANCRA_SUPERVISOR (watchdog|none) and PANCRA_BOOT_HOOK -- and the
     default is the one that needs nothing from the board at all:
     srv/deploy/supervise.sh, a poll loop installed beside the server. A board
     with a real service manager sets PANCRA_SUPERVISOR=none and points that
     manager at supervise.sh, never at the server: srv/deploy/start.sh holds the
     ONE copy of the start command and `make deploycheck` fails the build over a
     second one. The watchdog is handed a GENERATED copy -- start_template()
     emits it into supervise.env with the per-start tag left as @PANCRA_TAG@,
     refreshed by every deploy -- so the board never holds a hand-written start.

     IDENTITY-BOUND, in the sense health.sh's exe_is established: "the service is
     running" means the pid file names a live process whose /proc/<pid>/exe
     hashes to the INSTALLED binary. A `kill -0` alone fails at exactly the job
     this exists for, in exactly the case it exists for -- after a reboot the pid
     file holds a number from before it, the kernel has handed that number to
     something else, and a watchdog asking only "is something alive" finds
     something, concludes the server is up, and does nothing. The same rule
     guards the watchdog's own single-instance claim (a claim held by a pid that
     is alive and is not a supervisor is broken, or the first reboot disables
     supervision for ever), and each of its starts is judged by ITS OWN banner
     range in the log, because an append-only log answers "is there a readiness
     line" with the line the start that just died wrote.

     IT NEVER SIGNALS ANYTHING. It starts a service that is gone; it does not
     stop one that is there. The only handle it has is a pid file, a pid file
     survives a reboot, and killing what a stale one names is shooting
     bystanders -- which is item 156, and is why stop_block's kill is left
     alone here rather than half-fixed. For the same reason it does not restart
     a server that is up and answering nothing: that is a different fault whose
     remedy is a signal.

     A DELIBERATE STOP IS NOT FOUGHT, which is the rule that makes a supervisor
     safe to have at all. deploy, rollback, rotate and restore each stop the
     server on purpose for a second or two; a watchdog racing them starts the OLD
     binary in the middle of the swap and leaves two servers on one data
     directory with the pid file naming whichever start wrote it last. Two
     gates: the board LOCK, which the watchdog reads and never takes (it is not
     an operation, and a lock it held would be a lock the next deploy is refused
     by), and a stop MARKER raised by stop_block and cleared by start_block -- in
     the one stop and the one start every verb already shares, so no procedure
     has to remember. Both EXPIRE, because both are files and a file outlives
     what it was about: an ssh that dies between a stop and a start would
     otherwise leave the supervisor as the reason the board stayed down.

     AND BOOT ENABLEMENT IS ONE GUARDED BLOCK IN THE BOARD'S OWN STARTUP SCRIPT,
     between sentinels so ten deploys leave one launch line -- installed AFTER
     the `#!` line and not appended, which is the detail the whole thing turns
     on: the board's startup script ends by handing its process over to another
     service with `exec`, and a block appended to such a file is present,
     visible in review, and never reached. An undeclared or missing hook is
     reported in those words and NOT invented, because a startup script this
     deployment created is one the board's init never runs.

     VERIFIED BY EXECUTION: `make deploydrill` -- 169 assertions, 43 of them new
     -- and `make deploycheck`. The drill kills the server and requires it BACK;
     simulates a reboot (both processes gone, both pid files surviving and naming
     a live unrelated process) and requires the board's own boot script to bring
     the service up; holds the service down under a marker and under a lock and
     requires the watchdog not to fight either; backdates both and requires it to
     override them loudly; and runs a real deploy through a live watchdog,
     asserting exactly one server afterwards by /proc/<pid>/exe rather than by
     pid, because a pid recycled by a busy machine reads as a second server.

     THE ISOLATING CASES are the three that no earlier assertion could have
     passed for the wrong reason: the server killed and coming back WITHOUT a
     deploy; the reboot, where the only thing this drill does is run the board's
     boot script and where a `kill -0` liveness test would find the stale pid
     alive and do nothing; and the service held down under a lock with NO marker
     -- one mutant, one failed assertion. "A unit file was written" is asserted
     nowhere.

     NOT VERIFIED: nothing here has run on the real board. The fake board is a
     directory, its ssh is a shell function and its init is a shell running the
     hook script, so what is proved is the LOGIC -- that the block lands above
     the exec, that the watchdog survives a stale claim, that the gates hold and
     expire. Whether the Duo's init chain reaches the file PANCRA_BOOT_HOOK names
     can only be established by rebooting the Duo.

     Summary: Supervise the server after crashes and board reboots.

106. Save and restore bounded native activity state across recreation. Native
     startup ignores Android's saved-state buffer and registers no save callback,
     discarding screen/navigation and in-progress form state on activity/process
     recreation. Define a versioned bounded snapshot, validate it after durable
     domain data loads, restore only safe drafts/routes, and reject corrupt or
     incompatible state.

     Summary: Restore bounded native activity state after recreation.

107. Fail safely when foreground-service promotion is rejected.
     `PancraService` catches every `startForeground()` failure but proceeds to
     acquire a wakelock, tick alarms, schedule work, and return sticky as though
     monitoring were protected. Make promotion failure an explicit terminal or
     recoverable service state, stop/release resources safely, and publish a
     durable monitoring-stopped indication through an available channel.

     Summary: Fail safely when foreground-service promotion is rejected.

108. [DONE] Fail sync HTTP before calling Java after JNI allocation errors. `jni_http()`
     allocates strings and byte arrays and fills the body without checking each
     operation before invoking Java. OOM or `SetByteArrayRegion` failure can
     leave a pending exception/null body and still call `syncHttp`. Allocate and
     validate sequentially, unwind local refs and clear/report the exception on
     any failure, and return transport failure before entering Java.

     Verified by execution: `make jbridgetest` links syncjni.c whole and drives
     jni_http through the hook syncjni_wire installs, failing each of the five
     allocations on its own. Java is never entered, no local ref is leaked, the
     caller's buffer is emptied, and -1 is returned rather than the PREVIOUS
     request's 200 (which sync.c would have read as an upload it never made
     being accepted). Twelve mutants of these checks each fail a named
     assertion. NOT verified on a device: that CheckJNI aborts where the
     comments say it would is documented Android behaviour, MODELLED by the
     fake JNIEnv -- which counts every call ART would refuse -- and never
     observed on this project's phone.

     Summary: Stop sync HTTP before Java after JNI allocation failure.

109. [DONE] Publish BLE global references only after registration fully succeeds.
     `dexble_register()` assigns global refs without checking them and leaks or
     overwrites partial globals when `RegisterNatives` fails/retries. Construct
     refs in locals, check and clear each failure, delete both on every failed
     registration path, and publish the pair only after native registration has
     succeeded atomically.

     Verified by execution: `make jbridgetest` links dexble.c whole and fails
     each global ref, and RegisterNatives, on its own. dexble_ctx() stays NULL,
     both refs come back on every refusal, bondWatch/startService are never
     called on a class whose natives are not bound, and a second registration
     -- the activity relaunch -- leaves the live global count unchanged. Seven
     mutants each fail a named assertion. Not verified on a device (see 108).

     Summary: Publish BLE globals only after complete registration.

110. [DONE] Check and unwind every step of app class-loader lookup. `find_app_class()`
     chains class/method/object/string JNI calls and checks exceptions only at
     the end, so a null or pending exception is consumed by later calls and can
     abort under CheckJNI; successful locals also remain until callback return.
     Validate every step and release all acquired local refs through one cleanup
     exit, returning only a valid class.

     Done by moving the six JNI calls out of main.c into jb_app_class (see
     jbridge.h): they are a JNI sequence, not shell logic, and nothing could
     reach them while they were static in main.c. Verified by execution: each
     of the seven steps is refused in turn, both as a throw and as a null with
     NOTHING pending (getClassLoader legitimately answers null for a class the
     bootstrap loader owns), and the refused step is always the LAST step
     attempted -- which is exactly what the old code got wrong. Eight mutants
     each fail a named assertion. Not verified on a device (see 108).

     Summary: Check every step of app class-loader lookup.

111. [DONE] Abort immediately when the TLS transcript exceeds its bound. Transcript
     append currently sets a fatal flag but handshake processing continues into
     attacker-triggered ECDHE, ECDSA, and certificate output before checking it
     at final return. Make append return failure and propagate it before further
     state, crypto, or writes, or reject framed ClientHello length against the
     remaining transcript capacity up front.

     Summary: Abort immediately when the TLS transcript exceeds its bound.

112. [DONE] Refuse malformed local rows instead of deleting valid replicas. Local sync
     enumeration silently skips overlong or invalid nonempty rows, then derives
     canonical buckets from the smaller set and may PUT it or delete the server
     bucket. Treat any malformed/overlong local row as a hard scan failure,
     distinguish a genuinely empty complete file, and never infer authoritative
     deletion from parser rejection.

     Summary: Refuse malformed local rows instead of deleting replicas.

     VERIFIED BY EXECUTION: `make interoptest` (183 assertions against the real
     server, 62 of them new across items 112-114), `make storetest`,
     `make weighttest`. THE ISOLATING CASE: a log holding valid rows plus ONE
     malformed row, where the damaged row is the only row of its UTC day. The
     assertion is that the SERVER STILL HOLDS that day afterwards -- fetched
     back over the wire -- not merely that the sync returned an error, because
     an error raised after the deletion loop had run would satisfy the return
     code and still have destroyed the record. Both damage shapes are covered
     (a row row_ok refuses, and a line longer than a row) in both the streaming
     loop and the unterminated last line, and an EMPTY line is still skipped:
     a file of newlines is genuinely empty, which is the state item 113 needs
     told apart from this one.

     THE OVER-LONG FLAG EARNS ITS OWN CASE. row_ok's length bound already
     rejects a 513-byte truncation, so the flag looks redundant -- until the
     truncation's last kept byte is a '\r', which the carriage-return strip
     removes, leaving exactly SYNC_ROW_MAX characters that row_ok ACCEPTS.
     Without the flag the phone does not drop that line, it hashes and PUSHES
     the front of it: a fabricated reading at a fabricated time, on the server,
     indistinguishable from a real one ever after. Asserted directly.

     WHAT MUTATION FOUND, and it is the finding worth recording: the rule lives
     in TWO readers of the same file -- log_buckets, which lists the days, and
     log_scan, which builds the body -- and they MASK each other. Reverting the
     refusal in either one alone left every server-state assertion green,
     because the other refused the same bytes first. Reverting BOTH is what
     reproduces the original data loss, and it does: the server's day is
     deleted. Each site is now separately pinned as well, through the one state
     where the enumeration is acted on and the body builder never runs (a log
     the server holds NOTHING for, whose only row is damaged -- zero remote
     buckets means no deletion loop and no window to scan) and through
     sync_bucket_text, which reaches log_scan without log_buckets. Seven of the
     eight sites are killed by a named assertion. The eighth -- the over-long
     guard in log_buckets' unterminated-last-line block -- cannot be killed by
     any behavioural test: every path that reaches it also runs log_scan over
     the same bytes, and log_scan's identical guard refuses first. It is kept
     because the two readers agreeing about what a row is, is the invariant
     whose earlier violation deleted a day off the server.

113. [DONE] Represent deliberate deletion of a final log row with durable evidence.
     Sync correctly treats an unexpectedly empty local log as possible storage
     loss, but this makes intentional deletion of all rows impossible to
     converge and leaves the server copy forever. Persist a clear/tombstone
     generation in the deletion workflow and authorize empty replacement only
     from that evidence, retaining fail-safe behavior for bare missing/empty
     storage.

     Summary: Represent deliberate final-log deletion with a tombstone.

     WHERE THE TOMBSTONE IS WRITTEN, which was a design decision and not a
     lookup: there is NO "delete everything" workflow in this app. Insulin is
     append-only (a retraction is another row), and every log carries a '#'
     header that survives a rewrite -- so deleting the last DATA row leaves a
     one-row file, not an empty one. The state actually exists elsewhere:
     slots_save() rewrites the whole device registry and writes no header, so
     sensor_forget of the last device leaves slots.csv ZERO BYTES. That is an
     ordinary user action which the sync refused for ever -- and because
     sync_run stops at the first log that fails, it stopped the readings, doses
     and weights syncing behind it. The tombstone is therefore minted by
     slots_save (app/sensors.c) and by wt_rewrite (app/weight.c) when a delete
     removes the last line, the two places that can empty a synced log.

     DURABLE AND VERSIONED. `<log>.clear` holds one line, "pancra-clear
     <version> <generation>\n", written through atomic_replace -- stage, fsync,
     rename, fsync the directory -- and REPLACE_UNSYNCED travels as its own
     answer: the evidence is on disk and readable, only a power cut in the next
     moments could lose it, and reporting that as failure would leave the
     deletion looking unrecorded while the file records it. A version this build
     does not know reads as NO EVIDENCE, which is the fail-safe direction; an
     older build predates the file and never looks.

     THE TWO STATES THAT DECIDE WHETHER THIS IS SAFE. Tombstone present, log
     NON-EMPTY: it authorises nothing -- the guard is not even reached, the rows
     push as always, and sync drops the stale evidence so it cannot speak for a
     later emptiness. Tombstone absent, log empty OR MISSING: refused exactly as
     before. And a MISSING log is refused even WITH a tombstone: clearing a log
     leaves an empty FILE, losing the storage leaves a HOLE, and only the first
     is evidence. The evidence also lives NEXT TO the log on purpose -- kept
     anywhere else it would survive the storage loss it exists to be told apart
     from, and would then authorise deleting the backup.

     VERIFIED BY EXECUTION: `make interoptest`, `make storetest`,
     `make weighttest`. THE ISOLATING CASES: a deliberate delete-all WITH the
     tombstone CONVERGES (the server's rows are fetched back and are really
     gone); an empty log WITHOUT it leaves the server's copy byte-for-byte
     intact; a tombstone whose log has been deleted authorises nothing; and
     rows added back drop the evidence, so the next unauthorised emptiness is
     refused again. storetest drives the real workflow -- mint, claim, forget
     the last device -- and pins the format against five kinds of damage.

114. [DONE] Restore missing rows inside partially present buckets. Restore currently
     discards remote digest hashes/counts and skips a remote bucket solely because
     the bucket number exists locally. A torn partial local bucket is therefore
     never repaired and can later overwrite the complete server set. Compare
     canonical hashes, fetch mismatches, validate the fetched hash, and atomically
     merge missing server rows without removing valid local-only rows.

     Summary: Restore missing rows inside partially present buckets.

     VERIFIED BY EXECUTION: `make interoptest`. THE ISOLATING CASE: a local
     bucket that EXISTS and is SHORT of two rows the server holds, while also
     holding one row the server has never seen. The old test was
     `lb[k] == rb[r]` -- a bucket NUMBER -- so the bucket was skipped and the
     tear was never repaired; and the per-bucket hash that would have shown the
     damage was parsed out of the digest and thrown away. The assertions are
     that both missing rows are BACK, that the row only the phone had SURVIVED,
     and that a row both sides held was not duplicated into the file.

     WHY A TORN BUCKET IS NOT MERELY UNREPAIRED. The phone is authoritative, so
     the next sync PUTs that short bucket over the server's complete copy and
     deletes the missing rows there too. A restore that "succeeded" while
     leaving a tear had armed exactly the loss item 112 is about.

     The fetched body is validated against the hash the digest promised --
     driven by a fault, because a real server never sends a mismatch and the
     check could otherwise be argued for but never run -- and refusing it is
     asserted to leave the log byte-for-byte untouched. The merge is a UNION
     written in one call: local rows are already in the staging copy, only the
     rows this phone lacks are appended. A restore with nothing to add abandons
     the staging file rather than publishing an identical rewrite, asserted on
     the file's INODE, since a needless publish produces identical bytes.

     ONE MUTANT SURVIVED AND IS EQUIVALENT: disabling copy_into alone. The
     mid-download catch-up re-reads the original from offset `copied`, which is
     then 0, so it reproduces the copy exactly. The union property is pinned by
     a mutant that suppresses both.

115. [DONE] Decorate a hit box only when its append succeeds. `add_hit_ix()` calls
     `add_hit()` and then writes `box[n-1]`; when the hit table is full, append
     only sets overflow and leaves `n` unchanged, so the code of the previous
     valid control is overwritten. The same risk applies to immediate glow
     decoration. Return success/the appended index and decorate only that new
     entry.

     Summary: Decorate a hit box only when its append succeeds.

116. [DONE] Require a monotonically increasing Android release version code. The
     manifest hard-codes `versionCode=1` and both release APK/AAB paths consume
     it unchanged, so a second distributed artifact cannot update the first.
     Source version code/name from explicit release inputs, require the code to
     exceed the last published value, and inspect final packaged metadata before
     declaring release success.


    DONE. The four numbers that identify a package -- versionCode, versionName,
    minSdkVersion, targetSdkVersion -- are gone from app/AndroidManifest.xml and
    come from explicit build inputs (VERSION_CODE, VERSION_NAME, MIN_SDK,
    TARGET_SDK), injected by every packaging path. THE MEASUREMENT THAT DECIDED
    THE DESIGN: aapt v0.2-debian and aapt2 2.19-debian both treat
    --version-code/--version-name/--min-sdk-version/--target-sdk-version as
    INJECT-IF-ABSENT. An attribute in the manifest source WINS and the flag is
    accepted and discarded in silence -- so the AAB rule's
    `--target-sdk-version 34` had been dead text, and the two could never have
    been made to disagree by editing it. The manifest cannot carry them at all,
    and `make versioncheck` refuses the file if one comes back.

    The ledger of what has actually been distributed is app/published.mk, in
    version control: the rule compares two different builds, and only a
    reviewable file outlives the build that made the previous artifact. On a
    first release PUBLISHED_VERSION_CODE is 0 -- a written-down "nothing has
    shipped" sentinel rather than an absent value, so an unreadable ledger
    cannot compare true -- and every code from 1 up satisfies the rule.

    Verified by execution, on the FINAL PACKAGED metadata (aapt dump badging /
    bundletool dump manifest), never on the source: a release whose code equals
    or is below the ledger is refused and leaves no artifact behind; a build
    told VERSION_CODE=7 with android:versionCode="1" restored to the manifest
    packages 1, and apkcheck catches it; releasecheck refuses an already-signed
    release APK whose packaged code does not exceed the ledger. `make release`
    was driven end to end with a throwaway key held outside the tree.

    PARTLY DONE only in this: there is no production keystore in this
    repository, so the signed artifact that would actually be distributed
    cannot be produced here, and `make release` fails closed without one by
    design (item 90). Nothing was installed on a phone.

     Summary: Require an increasing Android release version code.

117. [PARTLY DONE] Target and verify the currently required Android SDK level. Release and AAB
     packaging fix target SDK 34, which no longer satisfies current Play update
     requirements and omits newer platform behavior. Make compile/target SDK
     explicit release inputs, validate against the current supported policy, and
     inspect the final APK/AAB metadata so stale manifest/link values cannot pass.


    PARTLY DONE. The mechanism is complete and tested; the NUMBER needs a Play
    Console lookup and a real device, and this session had neither.

    MIN_SDK, TARGET_SDK and COMPILE_SDK are explicit build inputs passed by the
    APK, release-APK and AAB paths alike, and MIN_SDK also drives d8's
    --min-api so the compatibility floor and the code contract cannot drift.
    TARGET_SDK moved 34 -> 35 and PLAY_TARGET_SDK_MIN refuses anything below
    the floor. COMPILE_SDK is stamped by aapt from framework-res.apk rather
    than passed, so it is asserted against the packaged value: replacing the
    framework now fails naming the variable instead of quietly shipping a
    different package.

    Both isolating cases are covered by execution, and they need DIFFERENT
    checks because of the inject-if-absent behaviour recorded under item 116:
      - link flags stale, manifest correct -> the packaged targetSdkVersion is
        wrong and apkcheck/aabcheck refuse it.
      - manifest stale, link flags correct -> the packaged value is RIGHT,
        because the manifest silently wins, and no packaged check can see
        anything wrong. Only versioncheck reading the manifest source finds it.
    A third defect was found while testing: the Makefile was not a prerequisite
    of anything it packages, so editing the aapt link flags by hand left the
    APK "up to date" and the edit never reached the artifact. Fixed
    (MAKEFILE_SELF), and the mutant is killed now where it survived before.

    WHAT IS NOT DONE. Play's required target API level is a dated external
    policy that moves every August; 35 satisfies it as of this writing and the
    next step is 36, but no gate in this repository can look that up. And
    targetSdkVersion 35 opts the app in to Android 15's enforced edge-to-edge
    insets, which is where this app draws the glucose number and the buttons --
    that must be looked at on a phone running that release, and it has not
    been. Both are stated in the Makefile beside the variable.

     Summary: Target and verify the required Android SDK level.

118. [DONE] Explicitly exclude sensitive app data from Android backup and transfer.
     The manifest sets `allowBackup=false` but supplies no modern
     `dataExtractionRules` or legacy full-backup exclusions; device-to-device
     behavior can vary while files contain medical history and credentials. Add
     explicit cloud/device-transfer exclusion rules for current and legacy
     Android and verify the packaged manifest.


    DONE. app/res/xml/data_extraction_rules.xml (API 31+) and
    app/res/xml/backup_rules.xml (API 30 and below, which ignore the modern
    attribute -- minSdkVersion is 29, so both generations are live) exclude
    every domain the platform can reach, on BOTH channels: cloud backup and
    device-to-device transfer, the second of which allowBackup="false" never
    governed. Domains are enumerated rather than summarised because whatever is
    not excluded is included, and the device_* twins are separate domains that
    "exclude root" does not cover. allowBackup="false" stays as the outermost
    of the three.

    Verified in the PACKAGED artifact, and the whole chain is read out of it:
    the manifest attribute, the resource id it points at, the name that id
    resolves to, the file that name maps to, its presence in the zip, and the
    rules decoded from inside that file. Checking that the source file exists
    on disk would have proved none of those links. Mutants killed by name:
    dropping one domain, dropping the whole <device-transfer> section, dropping
    either manifest attribute, emptying the legacy rules, turning one <exclude>
    into an <include>, and flipping allowBackup back to true. The same
    assertions run against the .aab through bundletool.

     Summary: Exclude all sensitive app data from Android transfer.

119. [DONE] Require one exact supported HTTP request-line grammar. Server parsing splits
     only on spaces around method/target and does not require the remaining bytes
     to be exactly a supported HTTP version followed by CRLF. Reject malformed,
     extra-token, and unsupported-version request lines before header parsing and
     close the connection, preventing proxy/origin boundary disagreement.

     Summary: Require one exact supported HTTP request-line grammar.

120. [DONE] Reject malformed, NUL, duplicate, or truncated URL-form fields. Form
     decoding preserves invalid percent escapes, admits `%00` as an embedded C
     terminator, silently clips to caller capacity, and does not expose duplicate
     ambiguity. Return typed absent/valid/malformed/too-long/duplicate outcomes
     and refuse bad escapes, decoded NUL, overflow, or duplicate security fields
     before authentication or mutation.

     Summary: Reject malformed, NUL, duplicate, or truncated form fields.

121. [DONE] Send browser security headers on every web response. Common responses lack
     a CSP/frame-ancestors policy, `X-Content-Type-Options`, and a referrer policy
     even on authenticated medical pages and state-changing forms. Add a reviewed
     central default policy to response construction, with narrowly documented
     route exceptions only where required.

     Summary: Send browser security headers on every web response.

122. [DONE] Bound sync response bodies before allocating them. `Ble.syncHttp()`
     reads until EOF into an unbounded `ByteArrayOutputStream`; native rejects an
     oversized body only after Java has allocated it. Enforce a Java-side maximum
     matching each native operation, validate Content-Length when present, abort
     on the first excess byte, and return transport failure without exhausting
     the app heap.

     DONE AND VERIFIED BY EXECUTION (`make boundaryjavatest`, no device). The
     read loop moved out of Ble.syncHttp into
     `BoundaryLogic.readBoundedBody`, which takes a `java.io.InputStream` and
     a clock and therefore RUNS ON THE HOST -- the whole reason it is not left
     beside HttpURLConnection, where every case below would have been testable
     only on a phone, which in practice means untested. syncHttp now calls it
     and keeps only what needs Android: the connection, the two idle timeouts,
     and item 123's watchdog.

     THE LIMIT IS NATIVE'S LIMIT, NOT A SECOND NUMBER. `SYNC_BODY_CAP` mirrors
     `SYNC_BUF_MAX` (app/sync.h), which is the `outcap` every sync.c call site
     passes to `jni_http`; `SYNC_BODY_MAX` is `CAP - 1`, because jni_http
     refuses `len > outcap - 1` (it NUL-terminates at `out[n]` and sync.c
     parses a C string). Drift is prevented MECHANICALLY, not by comment:
     `make javacheck` greps the C constant out of app/sync.h, evaluates both
     expressions and fails the build on a mismatch -- the same shape as the
     existing NET_* / `enum sync_net_fail` cross-check. That gate was itself
     mutation-tested three ways (Java cap halved; C cap doubled; the Java
     declaration renamed so the grep reads NOTHING) and refused all three,
     the last one because a gate that scans nothing must fail rather than
     pass.

     THE ISOLATING CASES, and why the obvious ones prove nothing:
       - A body ONE BYTE over the limit is refused while a body of EXACTLY the
         limit is accepted. A comfortably-oversized body would also be refused
         by a limit wrong by a hundred bytes; only the pair pins the exact
         off-by-one that has to agree with jni_http.
       - A LYING `Content-Length` -- the header says 12, the server then
         streams for ever. The header check passes it and the byte counter
         refuses it. Honouring the header alone is the obvious wrong fix and
         is worse than none, because it looks like one.
       - The ABORT POINT is asserted, not the return value. Returning null
         proves the caller was told no; it says nothing about the heap, and
         the heap is the item. Against a ten-megabyte answer the suite asserts
         at most 100 bytes accumulated, at most 101 read, and at most 101
         bytes served by the stream. The loop asks for exactly one byte more
         than the budget allows (`bodyReadSize`), so the abort is on the first
         excess byte rather than within a 4 kB bufferful of it.
       - A body with NO `Content-Length` at all (chunked, which is what a
         streaming middlebox actually sends) is bounded identically.
       - A refusal returns NO bytes, never a prefix: half a bucket that parses
         is worse than no bucket (lib/wirevec.h's asymmetry rule).
     A refusal also sets `sSyncCode` back to -1, which is load-bearing:
     syncjni.c reads the status separately from the array, so a null return
     with a live 200 still in the field reads there as a successful request
     with an EMPTY body -- a bucket fetch treated as a bucket that IS empty,
     which is the one answer that drives the loop that deletes.

     TEN MUTANTS for this item, all killed by a NAMED assertion except where
     noted: `>` to `>=` in the budget test; the limit doubled; the budget test
     removed entirely; the Content-Length check neutered; the read-size `+1`
     dropped; the never-below-1 read clamp removed; the size check MOVED to
     after the append (killed on the abort-point assertion, which is the one
     that matters); a size refusal handing back the partial body. ONE MUTANT
     SURVIVES AND IS DELIBERATE: removing the `> 19 digits` guard in
     `contentLengthRefused` fails nothing, because the `NumberFormatException`
     catch below it returns the same answer. Removing BOTH is caught only by
     the exception escaping the suite -- a CRASH, not a named failure. The
     guard is kept and the source says exactly this, rather than pretending to
     a coverage it does not have.

     NOT VERIFIED BY EXECUTION: that HttpURLConnection hands
     `getHeaderField("Content-Length")` and the real body stream to this code
     on a phone. That wiring is compile-checked and pinned by javacheck greps
     (readBoundedBody must be called; the old unbounded loop must not
     reappear), but no test runs `syncHttp` itself -- it needs a device and a
     TLS server.

     Summary: Bound sync response bodies before allocating them.

123. [PARTLY DONE] Enforce an absolute deadline for every sync exchange. The
     Java read timeout applies separately to each blocking read, so a server
     sending one byte within every timeout can occupy the sole sync/pair/restore
     executor forever. Add a monotonic whole-request deadline and cancellation in
     addition to connect/read idle timeouts, closing the exchange when either
     expires.

     PARTLY DONE, and the split is between the two halves of "cancellation".

     THE DEADLINE IS DONE AND VERIFIED BY EXECUTION (`make boundaryjavatest`,
     no device). `deadlineRemainingMs`/`deadlineExpired` take two
     `System.nanoTime()` readings, and `readBoundedBody` re-checks them BEFORE
     the first read and on EVERY iteration -- which is the whole item, since
     `setReadTimeout` restarts on each byte. The suite drives a fake stream
     that dribbles one byte per read while advancing a fake monotonic clock
     ten seconds a byte: it stops after five bytes and fifty seconds against a
     forty-five second budget, reports NET_TIMEOUT, and discards the partial
     body. The suite also asserts that no single ten-second gap trips a
     twenty-second idle timeout -- i.e. that nothing BUT the absolute deadline
     could have ended it. A slow-but-finishing exchange inside the budget
     still delivers its body, so the rule is not "never sync".

     MONOTONIC, per item 70, and the failure mode here is the nastier one.
     There, a negative `now - stamp` made stale state read as fresh. Here a
     wall clock stepping backwards makes the REMAINING budget larger than the
     budget -- the wedged request this exists to kill would be granted more
     time by the clock moving. The guard is on the SIGN OF THE DIFFERENCE
     (which is also the overflow-correct way to compare nanoTime), a
     non-positive elapsed yields zero remaining, and `javacheck` fails the
     build if `currentTimeMillis` appears in syncHttp's code.

     ISOLATING CASES: a deadline already expired when the body starts refuses
     WITHOUT READING A BYTE (asserted on the stream's own counter -- a stream
     never touched cannot have been refused for its size, which is what makes
     this about the clock and nothing else); a clock that ran backwards reads
     as expired rather than as time remaining; a zero budget expires
     immediately rather than meaning "for ever".

     SIX MUTANTS, all killed by a named assertion: the backwards-clock guard
     removed; `<= 0` weakened to `< 0`; the pre-read check removed; the
     in-loop check removed; a deadline refusal handing back the partial body;
     and the zero clamp removed. That last one SURVIVED the first drill --
     `deadlineExpired`'s `<= 0` masks a negative return -- and is now pinned
     by its own assertion, because a remaining budget is the natural argument
     to `setReadTimeout`, where a negative value is an error and a zero one
     means INFINITE: this item's hang, restored by its own fix.

     NOT VERIFIED BY EXECUTION, and this is why the item is not [DONE]:
     THE SOCKET-LEVEL CANCELLATION. Arithmetic between reads cannot interrupt
     a thread already blocked inside one, and the request write has no timeout
     of any kind, so syncHttp now arms a timer on a shared daemon
     `ScheduledThreadPoolExecutor` (`syncWatch`) at the moment the connection
     is created -- before any byte moves, so the budget covers connect, TLS
     handshake, request body, status line, response body and the gaps -- and
     the timer calls `disconnect()`, which closes the socket and makes the
     blocked call return. A socket WE closed is reported as NET_TIMEOUT rather
     than the NET_OTHER a bare `SocketException` would produce.

     None of that paragraph is executed anywhere. Proving it needs a real
     server that accepts a connection and then goes silent, and a device or a
     live TLS endpoint to run syncHttp against; there is neither here. What
     IS checked is structural: `javacheck` fails if syncHttp stops arming the
     watchdog. So the claim "a server that dribbles is cut off" is tested; the
     claim "a server that accepts and then says nothing has its socket closed
     and frees pushExec" is reasoned and gated, not demonstrated. The
     executor-is-free-afterwards assertion the item deserves cannot be written
     without running the real thing: `pushExec` lives in Ble.java and needs
     android.jar.

     Summary: Enforce an absolute deadline for every sync exchange.

124. Reject redirects for signed sync requests. HttpURLConnection redirect
     handling is not disabled, although the native signature authenticates one
     exact method and target; a redirect may change the destination or rewrite
     POST to GET and a later 2xx can be reported as success for the original
     operation. Disable automatic redirects and treat every 3xx as protocol
     failure unless a future policy explicitly re-signs a validated target.

     Summary: Reject redirects for signed sync requests.

125. Close sync request and response streams on every path. The Java transport
     does not explicitly close its output or input streams and its `finally`
     block intentionally does nothing. Use try-with-resources, close/finish the
     request body before reading the response, and close the fully consumed or
     aborted response so connections and descriptors can be safely released or
     pooled.

     Summary: Close sync streams on success, failure, and cancellation.

126. Keep the HTTP pool alive until every worker has stopped. Detached workers
     retain a pointer to a caller-stack `http_pool`; if the calling-thread worker
     returns after a permanent accept error, the serve function returns and
     destroys that stack object while other workers still dereference it, also
     leaving the listener open. Give the pool process-owned lifetime and
     coordinate stop/join/close, or terminate on an unrecoverable accept failure.

     Summary: Keep the HTTP pool alive until every worker has stopped.

127. Parse every pairing reply with exact bounded framing. Pairing rounds decode
     fixed hex lengths without first proving the returned body length, allowing a
     short reply to read stale response-buffer bytes; confirmation accepts a
     prefix, and UID parsing accepts overflow/trailing junk. Carry the transport
     body length, require exact round grammar and terminators, exact hex widths,
     and an overflow-safe positive UID with no surplus bytes.

     Summary: Parse every pairing reply with exact bounded framing.

128. Validate signed-request pointers and lengths before hashing. Public
     `sync_request()` casts signed `blen` to `size_t`; a negative length becomes
     an enormous out-of-bounds hash read, and a positive length with null body is
     likewise invalid. Validate method/path/output pointers, require `blen >= 0`,
     and require a body whenever length is nonzero before hashing or transport.

     Summary: Validate signed-request pointers and lengths before hashing.

129. [DONE] Close every export stream and discard failed snapshots. Export input and
     output streams are closed only on the normal Java path; any read/write/close
     exception jumps to a broad catch, leaks descriptors until GC, and leaves a
     partial share file. Use try-with-resources for every stream, publish/grant
     only after the complete output closes successfully, and remove/quarantine
     failed temporary snapshots.

     Summary: Close every export stream and discard failed snapshots.

130. [DONE] Snapshot exports only at complete CSV row boundaries. Java reads live CSV
     files without coordinating with native append/rewrite operations, and
     `readLine()` treats an EOF fragment as a complete line which export then
     terminates with newline. Snapshot each source under its storage contract or
     copy only through a proven complete newline boundary, so exported sections
     cannot contain torn rows or unrelated instants.

     Summary: Snapshot exports only at complete CSV row boundaries.

131. [DONE] Resolve edited timestamps with the target date's UTC offset. Insulin and
     weight forms split/recombine arbitrary dates using the current mutable
     offset, so a target across a DST boundary is persisted an hour wrong. Resolve
     the zone offset at the original/target civil instant, define ambiguous and
     nonexistent transition-time behavior, and persist the resolved target
     offset rather than today's.

     app/civil.c is the new leaf that owns the conversion: a civil time is
     resolved by SOLVING (an offset o is right only if zone(naive - o) == o),
     so the three answers -- one instant, two, or none -- fall out of counting
     the candidates rather than being special-cased. The zone is a callback,
     which is what makes it testable over transitions months away. forms.c
     splits and recombines through it, in the offset in force at the instant
     being edited, and CONFIRM persists the offset resolved at the instant
     being written instead of g_tz_off.

     THE TWO RULES, chosen and pinned:
       AMBIGUOUS (the repeated hour of a fall-back) -> the EARLIER instant,
         the first time the clock read it. It is what java.time's ofLocal and
         Python's fold=0 choose, and it is the only choice that makes
         re-editing a stable no-op instead of walking the entry an hour later
         each time. The rejected instant is reported, not discarded.
       NONEXISTENT (the skipped hour of a spring-forward) -> SHIFT FORWARD by
         the gap, so 02:30 becomes 03:30 and the form redisplays 03:30. Not a
         refusal, because the civil time can become nonexistent without anyone
         typing it -- change only the DATE of an 02:30 dose -- and "NOT A
         TIME" against a date the user just typed is unexplainable.

     Verified by execution: insulintest (133 assertions) and weighttest (121)
     assert the stored INSTANT across a boundary in both directions, both
     rules above, the offset persisted in the tz_offset_s column, and that all
     288 ordinary local times across a year are unchanged. Mutation-tested:
     recombining with the starting offset, choosing the other ambiguous
     instant, accepting a nonexistent time as valid, splitting in UTC, and
     persisting the arithmetic offset instead of the one in force at the
     stored instant are each caught.

     PARTLY DONE in one respect, stated plainly: forms.c is linked into no
     host suite (it reaches nav, shell, settings, sensors, remote and the
     renderer), so the CALL SITES in kp_commit_datetime and the two CONFIRM
     paths are covered only by compilation. Everything they delegate to is
     under test; a mutant planted in forms.c itself would not be caught.
     Nothing was run on a phone.

     Summary: Resolve form timestamps with the target date's UTC offset.

132. [DONE] Disambiguate meter readings in the repeated DST hour. OneTouch records carry
     naive local time; the current fixed-point offset guess chooses one of two
     valid fall-back instants, so distinct readings can collide or shift an hour.
     Consider both valid offsets and use record index/order, neighboring readings,
     and import time to select a monotonic sequence; retain ambiguity metadata or
     request confirmation when evidence cannot decide.

     meter_tz_for's fixed-point iteration is gone; meter_zone is now a plain
     "what was the offset at this instant" callback and civil_resolve does the
     solving. meter_stamp_step (meterlogic.c, pure) takes the decision across
     the WALK rather than per record: the meter hands records over in ascending
     index order and an index is assigned when the fingerstick is taken, so the
     instants are known to increase even where the clock readings do not. A
     clock reading that fails to ADVANCE is the fall-back observed, and from
     that record on the second pass of the repeated hour is in force. Import
     time supplies the other constraint -- a candidate instant later than the
     import cannot be one -- and is used only as a bound, never as an ordering.

     WHEN NOTHING DECIDES IT the ambiguity is retained rather than settled:
     civil.h's documented choice supplies the stamp so the import still
     completes (refusing the record would lose a fingerstick outright), and the
     flag plus the instant that was NOT chosen travel with it into the meter's
     runtime record (meter_rt_ambiguous), cleared per import so the count
     always describes the sync in front of the reader.

     Verified by execution: metertest (78 assertions) drives a run of
     fingersticks across the 2025 fall-back and asserts the instants are
     strictly increasing, that no two share an instant, and that each is the
     instant it was actually taken at -- including two records with the
     IDENTICAL clock reading, which the old code collapsed onto one timestamp
     where the log's exact-match BGM dedup silently dropped the second. It also
     asserts the undecidable run is flagged with its alternative retained, that
     import time rules out a future candidate, and that 288 ordinary records
     decode to EXACTLY what the old fixed-point conversion gave.
     meterstoretest (79) pins the retention. Mutation-tested: a decoder that
     picks one fixed offset again, one that never forces the second pass on a
     collision, and one that drops the ambiguity flag or the stored metadata
     are each caught.

     Nothing was run against a real meter or a phone; the zone is a fixture
     implementing the post-2007 US rule, and the record ordering assumption
     (ascending index = chronological) is the one otble.c's walk already
     documents.

     Summary: Disambiguate meter readings in the repeated DST hour.

133. Require exact ClientHello vector and PSK-binder framing. TLS parsing does not
     consistently require cipher, compression, extension, versions, signatures,
     key-share, identity, and binder vectors to exhaust their declared lengths;
     binder verification accepts a prefix rather than matching identities and
     binders exactly. Use bounded nested cursors, reject duplicates/trailing
     bytes, and require one correctly framed binder per selected identity.

     Summary: Require exact ClientHello vector and PSK-binder framing.

134. Refuse TLS tickets when monotonic time is unavailable. Ticket time maps a
     failed `clock_gettime()` to zero, indistinguishable from a valid timestamp;
     persistent failure can make zero-issued tickets pass the age check forever.
     Return clock status explicitly and refuse both ticket issuance and resumed
     acceptance whenever monotonic time cannot be obtained.

     Summary: Refuse TLS tickets when monotonic time is unavailable.

135. Close the prior thread-local connection before each database open.
     `db_open()` overwrites the calling thread's live SQLite pointer directly;
     opening context B after A loses A's initial handle, which later context
     switching cannot recover or close. Route opens through one checked
     connection-switch helper, honoring close failure, or make handles explicitly
     context-owned and close every one during context teardown.

     Summary: Close the prior thread-local connection before database open.

136. Preserve insulin form identity when writes or deletes fail. Confirm/update
     paths navigate away and clear `g_ins_edit` even when persistence fails; a
     failed delete returns to the populated form with edit identity cleared, so a
     later confirm appends a duplicate rather than retrying the update. Retain
     draft, original-row identity, and retry screen on failure, clearing and
     navigating only after durable success.

     Summary: Preserve insulin form identity after failed persistence.

137. Keep failed weight deletions targeted and retryable. The delete-confirm path
     reports failure but unconditionally clears `g_wt_edit` and returns to the
     log, discarding the exact original row needed for a safe retry. Preserve
     `g_wt_orig`/edit identity and remain in confirmation or edit state until the
     deletion commits.

     Summary: Keep failed weight deletions targeted and retryable.

138. Retain calibration drafts when confirmed persistence fails. Calibration and
     rescale confirmation clear the pending entered value and leave the screen
     even when `calib_queue()` or `calib_rescale_set()` returns failure. Keep the
     value and confirmation state intact with the failure status so retry does
     not require re-entry; clear/navigate only after durable acceptance.

     Summary: Retain calibration drafts after failed persistence.

139. Reject noncanonical ECDSA scalars before reduction. Signing and verification
     decode private/nonces and signature r/s through a helper that subtracts the
     group order, so out-of-range external values can become accepted aliases
     despite the API contract. Add a checked canonical scalar decoder requiring
     `1 <= x < n` for d, k, r, and s; reserve modular reduction for hashes and
     internal coordinates.

     Summary: Reject noncanonical ECDSA scalars before reduction.

140. Reject unsupported J-PAKE passwords instead of truncating them. Construction
     silently clips passwords above 32 bytes, making distinct inputs with the
     same prefix equivalent, and accepts empty/all-zero inputs that reduce to an
     invalid secret. Return failure for null-with-length, oversized, empty, or
     zero-reduced values and document the exact password byte encoding/domain.

     Summary: Reject truncated or zero J-PAKE password scalars.

141. Enforce ordered single-use J-PAKE transcript phases. The API permits
     duplicate/overwriting peer rounds and round three before an accepted round
     one; later derivation checks only selected flags. Track explicit phases,
     reject duplicates and out-of-order packets before parsing/publication, and
     derive only after exactly one accepted packet of every required round.

     Summary: Enforce ordered single-use J-PAKE transcript phases.

142. Validate plot framebuffer geometry before pixel writes. The public plot
     framebuffer accepts arbitrary stride/width/height, while pixel output clips
     only x/y and indexes through signed `y * stride + x`. Reject negative or
     undersized stride, impossible dimensions/rectangles/radii, and use checked
     `size_t` multiplication/addition for every buffer offset.

     Summary: Validate plot framebuffer geometry before pixel writes.

143. Validate public string-buffer pointers and capacities before use. Helpers
     such as `html_esc()`, `form_get()`, `hdr_get()`, `clampn()`, and
     `str_snapshot()` write index zero or compute `cap-1` without consistently
     rejecting zero/negative capacity or null pointers. Define null-with-zero-
     length semantics where useful and otherwise return typed failure without
     touching output.

     Summary: Validate public string pointers and capacities before use.

144. Leave hex output untouched when any input pair is invalid. `hex_to()` writes
     each decoded byte immediately and only later reports a malformed pair, so a
     failed call exposes a partially attacker-controlled result. Prevalidate the
     complete input or decode into temporary storage and publish only on full
     success.

     Summary: Leave hex output untouched when decoding fails.

145. Reject null signing and route inputs before dereferencing them. Signature
     construction validates output only but passes required method/target/nonce/
     hash pointers to `%s`, while `route_of()` unconditionally clears its output.
     Validate every required pointer and capacity at the public boundary and
     change void APIs to status returns where refusal must propagate.

     Summary: Reject null signing and route inputs before dereferencing.

146. Make random-hex length exact or reject unsupported requests. `rnd_hex()`
     promises exactly the requested character count plus NUL but floors odd
     lengths, silently caps large requests, and accepts no output capacity.
     Redesign around requested byte count plus explicit destination capacity, or
     reject odd/oversized lengths rather than returning a shorter security token.

     Summary: Make random-hex length exact or reject unsupported requests.

147. Synchronize and snapshot the shared native status string. BLE binder writers
     and main rendering concurrently modify/read plain `g_status`; the model also
     retains a pointer that can change while rendering. Protect writes and copies
     with a dedicated leaf lock or atomically publish immutable buffers, and keep
     captured text inside each UI snapshot; give signal-time crash logging a
     separately safe published snapshot.

     Summary: Synchronize and snapshot the shared native status string.

148. Read administrative passwords without command arguments. `adduser` and
     `passwd` require plaintext in argv, exposing it to process listings, shell
     history, and audit logs. Accept secrets through a no-echo terminal prompt,
     explicit stdin mode, or inherited secret descriptor, and ensure usage/help
     never encourages argv credentials.

     Summary: Read administrative passwords without command arguments.

149. Change passwords and revoke sessions in one transaction on every surface.
     The CLI commits the new hash and only afterward separately deletes sessions;
     failure leaves old stolen cookies valid with the new password installed.
     Share one transactional auth operation between web and CLI and report
     success only after hash update and revocation commit together.

     Summary: Change passwords and revoke sessions in one transaction.

150. Refuse backup destinations that alias the live database. The backup command
     can rename its snapshot over the open live DB pathname, leaving the server
     writing an unlinked old inode/WAL and losing subsequent work at restart.
     Resolve and compare canonical path/inode identities and reject the live DB,
     WAL, SHM, temporary aliases, and destinations within unsafe staging names.

     Summary: Refuse backup destinations that alias the live database.

151. Never unlink live SQLite sidecars during backup verification. Verification
     checks for absent WAL/SHM, opens the supplied path, then unlinks sidecars by
     pathname; another server can create real sidecars in that TOCTOU window.
     Verify an immutable private scratch copy and delete only files created and
     identity-checked by that invocation.

     Summary: Never unlink live SQLite sidecars during verification.

152. Distinguish failed account lookup from a missing user in admin commands.
     Invite/password/logout callers discard `user_by_email()`'s explicit DB
     failure output and diagnose prepare/step faults as “no such user.” Preserve
     the typed result, return an operational failure code and accurate diagnostic,
     and avoid directing an operator toward a logically wrong retry.

     Summary: Distinguish failed account lookups from missing users.

153. Preserve notification dirtiness until a refresh succeeds. `notify_tick()`
     clears its dirty flag before calling the renderer, but rendering can return
     early when JNI is unavailable or its single-flight lock is busy. The only
     refresh request is then lost and an old glucose may remain until another
     event happens. Return render success and reassert dirty on every failure, or
     acquire render ownership before consuming the flag.

     Summary: Preserve notification dirtiness until refresh succeeds.

154. [DONE] Scan every first-party text input for CRLF line endings. The format gate's
     CRLF pass covers C/H, Makefile, and the manifest, but omits shell, Java,
     Python, deployment config, and other executable text. Build one manifest of
     tracked plus untracked nonignored first-party text with explicit binary/
     generated exclusions, fail on unreadable inputs, and scan all of it so a
     CRLF shebang cannot pass `make check` and later fail execution.

     Summary: Scan every first-party text input for CRLF endings.

155. Distinguish invalid signatures from authentication storage faults.
     Signature verification returns one false result for a bad signature, app-key
     prepare/step failure, and nonce insertion failure; routing maps all to 401.
     Return typed valid/invalid/replay/error states, require exact SQLite terminal
     results, and map operational DB failure to retryable 500/503 rather than
     falsely telling a correctly paired app its key is invalid.

     Summary: Separate invalid signatures from auth storage faults.

156. Verify process identity before signaling a deployment PID. Deployment reads
     a PID file and sends TERM/KILL without proving that the live process is the
     installed Pancra instance; PID reuse after crash/reboot can kill an unrelated
     process. Prefer service-manager ownership, or validate executable inode/hash
     and process-start identity before signaling and quarantine stale PID files.

     Summary: Verify process identity before signaling deployment PIDs.

157. Bound and rotate the deployed server log. Every start appends stdout/stderr
     to one permanent log with no size/retention policy; sustained diagnostics can
     fill the state filesystem and then break SQLite durability or startup. Put
     output under bounded service-manager journaling/rotation while preserving
     recent per-start readiness/error evidence and exercise disk-pressure behavior.

     Summary: Bound and rotate the deployed server log.

158. Reject slot files whose unread tail exceeds the load buffer. `slots_load()`
     performs one fixed-size read and can return success when byte 1023 is a
     newline, treating its injected terminator as EOF while silently ignoring all
     later device slots. Use a bounded streaming parser or probe/read one extra
     byte and report oversize/damage before publishing any truncated registry.

     Summary: Reject slot files with unread data beyond the load buffer.

159. [DONE] Require successful expected-status responses for every web assertion.
     Several synctest checks invoke silent curl directly and assert only negative
     body properties; a connection failure produces empty output that passes
     “contains no script/handler.” Route every asserted exchange through a helper
     that preserves transport status, clears artifacts, and requires the intended
     HTTP status before any body/header assertion.

     Summary: Require successful responses for every web assertion.

160. Bound retained deployment releases with a safe prune policy. Each distinct
     deployed binary is copied into the releases directory forever, while only DB
     backups have retention. After a healthy deploy, retain active and immediate
     rollback artifacts, then prune older identity-verified releases by count,
     age, and available-space policy; exercise rollback selection under low disk.

     Summary: Bound retained releases with a safe prune policy.

161. Publish backups under unique no-replace names. Backup and deploy-preflight
     names have only one-second timestamp resolution, while publication renames
     over an existing destination and local transfer does likewise. Two attempts
     in one second can silently destroy the earlier recovery point. Generate a
     collision-resistant identifier or use exclusive no-replace publication and
     propagate the actual chosen name through verification/transfer/retention.

     Summary: Publish backups under unique no-replace names.

162. Require exact outcomes for every help-listed CLI verb. `clitest` invokes
     each verb with `|| true` and passes whenever output lacks “not a subcommand,”
     so a crash, signal, silence, or unrelated fatal error proves dispatch.
     Capture exit status/stdout/stderr and specify the exact safe no-argument
     outcome for each verb, rejecting signals and unexpected empty output.

     Summary: Require exact outcomes for every help-listed CLI verb.

163. Track rollback order independently of binary modification time. Default
     rollback chooses the newest archived release through `ls -t`, but deploy
     archives with `cp -p`, preserving each binary's original source mtime rather
     than displacement order. Record an atomic deployment sequence/previous-hash
     pointer (or explicit archive timestamp metadata), validate selection, and
     test releases whose source mtimes disagree with deployment order.

     Summary: Track rollback order independently of binary mtime.

164. [DONE] Parse statistics only from complete valid reading rows. Statistics loading
     accepts timestamp/glucose prefixes and tolerates missing separators/fields,
     so a fresh malformed row such as `epoch,100junk` can affect TIR/average even
     though history rejects it. Use the same exact bounded row decoder/schema as
     authoritative reading load and handle malformed input consistently rather
     than computing a plausible statistic from it.

     Summary: Parse statistics only from complete valid reading rows.

165. Drive BLE liveness and retry deadlines with monotonic time. CGM silence,
     reconnect throttling, RSSI freshness, and DIS retry use realtime deltas; a
     backward correction makes ages negative and can suppress recovery until wall
     time catches up. Keep sample/persisted identity in realtime, but stamp and
     compare live receipt/retry/freshness deadlines exclusively with `mono_s()`
     and define clock-unavailable reset behavior.

     Summary: Drive BLE liveness and retries with monotonic time.

166. Fsync deployed binaries and restored databases before reporting success.
     Deploy/rollback move staged executables and restore moves DB/sidecars into
     place without syncing staged files and affected directories. A power loss
     after healthy-success output can lose/revert the acknowledged publication.
     Use one remote durability helper for file fsync, atomic rename, and directory
     fsync, with a distinct post-rename uncertainty result.

     Summary: Fsync deployed binaries and restored databases before success.

167. [DONE] Require the server-only dry run itself to succeed. `srvonlycheck` pipes a
     nested `make -n srv` diagnostic stream to `grep -q .`; without pipefail, a
     failing make that prints an error satisfies grep and passes the check.
     Capture output and exit status separately, require status zero, and print
     the captured diagnostic only when the dry run fails.

     Summary: Require the server-only dry run itself to succeed.
