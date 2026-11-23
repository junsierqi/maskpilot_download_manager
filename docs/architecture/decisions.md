# Engineering Decisions

This page preserves design evolution by topic. Database versions and earlier platform boundaries describe decisions at that time, rather than a standalone current feature list. See the [architecture overview](overview.md) for module relationships and the [feature matrix](../product/feature-matrix.md) and [support matrix](../product/support-matrix.md) for current support. The later sections on explicit origin authorization, complex HLS, and native macOS adaptation describe subsequent behavior.

## Native Linux platform and local delivery

Keep existing domain/application interfaces and independent protocol engines. Platform differences belong in native adapters, Qt/filesystem boundaries, and build modules. Linux uses streaming Qt SHA, libsecret/Secret Service, local metadata reads without following links, and private process groups bound to a session and pidfd. If the owner is killed, a guardian retaining group membership cleans up descendants. Helpers must remain trusted; this is not a malicious-code sandbox. Qt/filesystem conversions explicitly use UTF-16; native filename composition avoids locale-dependent wide-string conversion.

File opens retain exclusive creation and read/write modes. After verification, scanning, and synchronization, BT publishes its private directory through `renameat2(RENAME_NOREPLACE)`, without falling back to an ordinary rename that could replace an empty destination. Native Messaging isolates standard channels when starting the persistent desktop; Windows also restricts inherited handles so the desktop cannot retain browser response pipes. Affected GNU test targets use explicit rescan link groups for static-library resolution, without adding application-to-infrastructure dependencies.

Linux local packages use a dedicated CMake install component, adjacent BT shared libraries, and complete notices. Matching system packages provide Qt and transport dependencies; Windows DLL/version manifests are not reused. Tests use a disposable ordinary-user HOME, bus, and encrypted keyring. Packaging rejects existing outputs. Manifests establish byte consistency and provenance checks, not publisher authentication. User-space, kernel, graphical, and browser qualification remains limited to recorded environments; formal distribution requires separate acceptance.

## Native bounded live recording

The separate `Live` type and `LiveRecordingOptions` preserve the media-processing component's local-file restrictions. A private playlist parser handles exact source URLs, sequence numbers, keys/ranges/maps, and track boundaries. Native adapters reuse curl lifetime, proxies, file publication, and scanning. Ordinary HLS appends only complete MPEG-TS segments; FLV appends only complete tags. Playlists are limited to 1 MiB/4096 segments, each segment to 32 MiB, and FLV memory by tag length.

Recording requires duration and file-size limits. The separate `finishRecording` application interface uses `TransferControl.finish`, then verifies, scans, and publishes without replacement. Pause, cancellation, and source errors do not publish. Restart begins again at the current source, without automatic retry or a recording-resume guarantee. Task-owned recording caches participate in exact cleanup; body quotas survive restarts. Database v18 backs up the old database and stores only public format/limit settings; origin login material remains in memory.

## Offline installation, updates, and recovery

Installation, update, and removal share PowerShell package-ownership and path rules. The offline ZIP's external SHA-256 comes from an independent trusted channel. Verification and extraction use the same read-only locked handle, followed by internal manifest, fixed required-file, and version checks. Paths, reparse points, sizes, entry counts, case-insensitive duplicates, and file/directory conflicts are checked before replacement. Destination mutexes and actual executable-path checks protect cooperating operations without automatically killing user processes.

Updates use a controlled transaction directory beside the installation. Persist preparation first, then write and verify backups and replacement files. Once applying, replace files atomically one at a time and write the installation marker last. One journal contains the immutable plan, avoiding recovery gaps from cleaning up several metadata files. Interruption recovery and explicit rollback first verify every current/backup file, accepting only planned old, new, or legitimately missing states. External changes preserve the scene. Recovery does not imply an atomic transaction across the entire directory and registry.

Successful upgrades retain one rollback set. Explicit completion removes only owned, hash-matching transaction files and empty directories. Original browser configuration/registration journals retain their original bytes and ownership hashes, so user changes are not reclaimed as removable files. Payload rollback does not downgrade the task database; registry changes use the existing exact-ownership journal separately.

Caught write failures clean up only files exclusively created by that invocation; pre-existing temporary files are not owned by it. Unverifiable truncated staging files or journals left by process termination remain as conflicts requiring explicit manual inspection. Recovery guarantees apply only to valid persisted checkpoints. Preserve external changes and diagnostic material rather than deleting mismatches to manufacture recovery success.

Release scripts reject existing staging directories and ZIPs. Qualification baselines freeze native source, runtime files, and actual tools. Installation/release scripts may evolve independently, but packaged runtime file sets, sizes, and hashes must match the frozen endurance run. Private build caches and machine paths remain in test evidence; public packages record only versions, source identifiers, and qualification summaries.

## 2026-11-18: Site project coordination and immutable revisions

The project coordinator reuses the download coordinator. A single atomic JSON manifest stores project intent, stable request identifiers, the complete child-task plan, and publication receipts. It adds no transfer engine and holds no database locks across coordinators. Preview fetches HTML/CSS to discover resources; confirmed execution handles identified binary resources. A specific resource role takes precedence over a generic page link for the same URL. The body budget combines verified bytes with full reservations for unfinished children; zero remaining budget must not become unlimited.

SQLite 13 stores immutable project/revision associations. Before queue policies run, the native coordinator pauses recovering project children so waiting-window tasks cannot start before project verification. Creation acknowledgments, recovery, content reads, and publication verify ownership, request identifiers, full paths, URLs, queues, proxies, limits, response types, and hashes. Deleted, cancelled, or edited tasks are not recreated automatically. Manifest-write failure disables project scheduling and pauses associated work; ordinary downloads remain available.

Before publication, persist the staging token and hash manifest, write to a controlled same-volume staging directory, then rename to a revision directory without replacement. Restart accepts a renamed result only when the immutable marker and every hash match. Failure, cancellation, and incomplete writes preserve earlier publications. Cleanup deletes only manifest-owned staging files with unchanged hashes and never recursively removes unknown directories. Updates explicitly refetch content; old local hashes do not prove remote freshness. Removing history preserves original downloads and exports.

The Qt project view drains command results asynchronously. The main window counts drafts, preview confirmations, pauses, errors, and discovery/publication gaps as unfinished projects. Cancelling a project cannot trigger successful completion actions. Destruction closes the window, stops the project coordinator, then stops the download coordinator, preventing dangling callbacks and child redispatch during exit.

## 2026-11-18: Bounded static content processing

The site-content port accepts downloaded bytes, the actual response URL, and its type; it exposes no network or file loader. HTML uses a pinned libxml2 in-memory parser context that denies every external resource per context, without relying on ineffective NONET flags or changing global loaders. Bound input, tree depth, nodes, attributes, references, and serialized output; CSS tokenization/rewriting checks the output budget incrementally. Re-escape decoded identifiers and use CSS escapes for angle brackets in strings so HTML raw-text regions cannot reopen. Distinguish unsupported syntax, resource limits, and caller failures.

Retain only allowed static markup, same-origin references, and validated relative export paths. Remove active content and insert strict CSP. Known images, fonts, and media must also match supported file signatures; SVG/HTML declared as an image cannot be exported directly. Signature checks are neither complete media decoding nor general malware scanning. Basic Unicode/Latin-1 encodings are supported; others are explicitly rejected. At this stage, the complete project state machine, previews, and immutable revision publication belonged to a later independent module.

SQLite 12 persists `response_content_type` alongside the response URL. This observation aggregates successful probing and consistent actual responses. Missing, conflicting duplicate, parameter-varying, or unsafe values become empty without failing ordinary downloads. An error-page type from a 416 empty-resource response does not describe the resource; complete caches use only the current successful probe's type. Retries, restarts, request edits, and invalid events clear old observations. Task lists cannot restore or inject type authority.

## 2026-11-17: Interface and candidate packages

Continue using Qt Widgets. `ui/src/style/appearance.*` centralizes styles and vector icons. Task delegates render immutable snapshots; protocol, scheduling, and filesystem work stays out of painting. Narrow windows hide secondary columns while retaining name, progress, status, and rate. Persistent status-bar rates are separate from timed notifications, so refreshes cannot overwrite messages.

CMake generates a domain version header shared by the desktop, default User-Agent, and validation client. Packaging may select an independently verified Release directory, checks versions and SDKs, and collects actual additional runtime libraries and complete dependency notices. Protocol fixtures may sanitize only product child-process environments, leaving the parent to provide local TLS/seeding services. Real downloads from the package must establish deployment correctness.

## 2026-11-18: Explicit proxy routing and credential lifetime

The domain model retains compatible credential-free proxy addresses. One parser validates protocol, host, IPv6, port, and credential lengths. HTTP/FTP share private configuration helpers: an empty address explicitly connects directly; a nonempty address explicitly disables environment bypass. Proxy and origin authentication stay separate. SOCKS5 resolves target DNS through the proxy; the system may resolve the proxy itself. HTTPS proxies retain certificate validation. Active FTP combined with a proxy is explicitly rejected.

The BT factory shares active sessions by immutable endpoint and in-memory credential identity, without changing global network settings between tasks. Idle sessions are released; expired route keys are removed on the next acquisition or factory destruction. Proxy mode disables UDP discovery, uTP, and automatic port mapping; HTTPS proxies are unsupported. Numeric-IP proxy endpoints receive an additional cancellable five-second TCP connectivity check. Hostnames use the native resolver, so operating-system DNS latency is not covered by that bound. Origin or individual-peer failures do not invalidate the whole shared route.

Schema 5 appends only a marker requiring proxy credentials and backs up before migration. Configuration validation allows paused/imported tasks without secrets; adapters check credentials before networking. Usernames/passwords never enter the task database, task lists, logs, or helper arguments. The marker constrains credential availability, not the server's authentication method. Authenticated media proxies use the private standard-input channel shared with probing.

## 2026-11-17: Independent BT engine

The domain explicitly distinguishes HTTP, FTP, media, and BT; persisted state values are append-only. Application ports carry immutable metadata/file descriptions, upload statistics, file selection, and stop-seeding controls without exposing library types. The independent `transfer/transfer_router.cpp` routes protocols to adapters; the media module handles only media transfers. A shared factory supplies one lazy BT session and alert dispatcher. The coordinator retains concurrency/priority ownership; do not create a complete client per task.

Schema 4 stores bounded torrent metadata, selections, upload policy, and publication markers, preserves old media fields, and migrates previous protocol types. Local torrent addition first captures bytes safely through the adapter, then persists acknowledgment. Imports validate as a whole. Later loss of the original path does not trigger rereading. Raw metadata may contain private Tracker information and is protected local task data.

BT uses an adjacent task-owned temporary directory and identity marker, rejecting unsafe paths, file conflicts, and reparse paths. After selected files pass piece checks, wait for the pause alert and disk release, synchronize files, then publish the entire directory without replacement. Resume data cannot choose storage paths or bypass verification. Seeding cannot repair or redownload published content; cancelling seeding preserves the published directory. At this stage, unsupported proxy operation failed explicitly.

Magnets first acquire and verify hash-bound metadata, release the metadata-stage handle, then start payload transfer with complete file priorities to avoid zero-selection completion and reconnect backoff. Incomplete v2 piece layers are not written as complete torrents. Retain the authoritative info dictionary and let the engine acquire and verify required hashes. Carry forward only bounded currently connected peers; never trust network destinations from disk resume data.

If automatic listening fails due to port permissions with no successful listener, try at most 8 random non-ephemeral ports. Do not rewrite explicit configuration or an existing successful listener. Before adding a task, wait at most 5 seconds while honoring pause/cancel; report a clear failure. Initialize control state before starting the network dispatcher thread. Persist the publication marker immediately rather than delaying it throughout seeding.

## 2026-11-17: Protocol adapters and FTP

FTP/FTPS uses independent `network/ftp_transfer.cpp`, without reusing HTTP status, Range, or ETag logic. `filesystem/transfer_files.*` supplies existing synchronous writes, atomic resume-metadata replacement, and publication without replacement; protocol decisions and retry policies remain adapter responsibilities. Transfer ports, the serial task coordinator, and the Qt task table remain shared.

FTP defaults to a single passive stream. Explicit TLS requires encryption of both control and data channels; `ftps://` selects implicit TLS. Usernames/passwords stay in memory and cannot be embedded in URLs. Schema 3 adds only TLS, active-mode, and CA-path settings, backing up the old database. Adding a protocol does not persist old credentials.

FTP SIZE/MDTM are not strong content identities. Cache reuse requires an expected SHA-256 plus matching URL, size, modification time, and TLS settings. Failed final verification prevents publication. Otherwise, or without server REST support, restart the download. Do not imply reliable resume without proof; unverified server combinations remain explicit limitations. BT has its own shared session and adapter; its upload/seeding lifecycle does not enter FTP or HTTP state machines.

Keep focused local commits by stage for rollback. Database migrations do not downgrade automatically. Before returning to an older program that cannot read the new schema, exit every instance, back up current data separately, and validate an upgrade-time `.before-v*-*.db` copy in an isolated data directory. Old backups omit later task states. Code rollback does not delete downloads or unknown user files.

## 2026-11-16: Development baseline

The user authorized autonomous implementation against the overall blueprint: Windows x64 first, C++20, Qt 6 Widgets. Local Qt 6.11.0, MSVC 2022, and CMake 3.31 were inspected; native dependencies use a pinned baseline. Major directories and dependency directions follow the blueprint. Reference research stays outside the repository; reference implementation source, names, and identifiers are not copied.

## Networking, disk writes, and threading

One coordinator thread serializes task-controller and database access. Each active task has one bounded worker that exclusively owns its multi transport and files; segments do not get individual threads. Compared with a separate disk pool, this reduces cross-thread buffering and backpressure protocols, bounds memory, and isolates disk blocking from the UI. Add a disk executor only when measurements justify it.

Write only after strict checks of status, Content-Range, byte length, and resource identity. Strong validators support reliable cross-session resume; without reliable identity, restart. After segment synchronization, atomically persist recoverable offsets; recovery truncates uncertain tails to those offsets. Assemble on the destination volume, then publish without overwriting existing files.

## Sensitive information and persistence

Cookies and authentication headers live only in memory, outside task tables. After restart, login-dependent tasks may need resending from the browser. Raw URLs can contain short-lived credentials and remain in the current user's local database. User-exported task lists also contain raw URLs; the export dialog warns about this. Lists exclude Cookies and authentication headers, but users must protect exported files. Diagnostics hide query parameters.

## Browser acknowledgment ordering

Pause the browser download first. Only after native validation/persistence and acknowledgment matching the request ID may the extension cancel the original. If acknowledgment fails, resume the browser download to avoid losing the task.

## Native resource constraints and response observations

Site children reuse the native HTTP engine. Domain requests append only body-size and same-origin redirect constraints. Check known lengths before opening work files, unknown lengths per chunk before writing, and totals again during assembly. Per-stream/progress counts cannot substitute for the total limit. A separate traffic ledger counts received probe/retry bodies. Redirects compare the initial scheme, host, and effective port while preserving stricter exact-URL login grants. Data connections still forbid automatic redirects.

The HTTP adapter reports actual response URLs through a trusted port. The coordinator accepts only the current generation and clears observations before a new attempt/request edit. Invalid observations terminate rather than becoming input authority. Schema 11 persists constraints and response URLs; task lists export only request constraints. Replay checks size, origin policy, digest, proxy-login requirements, and media selection, preventing weaker old tasks from acknowledging a new request. Preserve filename, ownership, and route checks.

## Browser pages and platform boundaries

Browser entries share TypeScript behavior with separate Chromium service-worker and Firefox module-event-page manifests. Background logic composes independent Cookie contexts and bounded resource sessions. The page renderer is an isolated classic script without module imports, `postMessage`, or native bridges. Trusted scanning creates resource candidates, opaque identifiers, and stable request identifiers. Actual selections remain in an isolated closure; asynchronous Cookie lookup is followed by document/session revalidation. Firefox's fixed adjacent manifest and extension identifier stay separate from Chrome/broker branches. Registration journals restore values by current ownership. Both extensions still use independent download areas.

## Media isolation and recovery boundaries

The domain stores bounded format descriptions and typed selections, not raw parser JSON or format URLs. A separate application probe port obtains short-lived execution snapshots through existing routing and credential resolution. Desktop probing is only for the independent download area; environment-authorized tasks cannot use it to expand origin grants. Qt probes run on a separate cancellable worker, with generation invalidation preventing stale results from overwriting new URLs. Recheck explicit formats on the same route before transfer. Convert exact IDs to filters so reserved words cannot act as selection expressions.

The shared process module sanitizes environments and handles private input, Windows jobs, and exit cleanup. Two packaged static-runtime launchers without Qt dependencies cover explicit/implicit FFmpeg and ffprobe input, allowing only regular task-directory files and non-playlist containers. Local postprocessing cannot reconnect or read arbitrary directories. Trusted startup binds actual component paths; the parser uses its own launcher directory. These restrictions cover supported component calls, not untrusted-executable sandboxing. Schema 10 appends mode, primary format, audio track, and container columns, backing up first; old tasks retain automatic presets.

Media runs in external processes, attached to a Windows job at creation so cancellation cleans up the tree. Independent tasks may explicitly supply in-memory authentication headers, Cookies, and custom headers. Default authenticated probing/downloading shares a restricted temporary local gateway allowing only the original scheme, host, and effective port; independent tasks may explicitly authorize extra origins. HTTP requests and HTTPS CONNECT validate random proxy credentials and destinations before connecting. HTTPS bytes remain encrypted, with component-side certificate verification. Selected upstream proxies never fall back to direct connections; HTTP proxies must allow CONNECT to the target port. The gateway owns at most 16 connections, bounded headers/buffers, and the process-operation lifetime. Process groups and the media gateway remain separate. Private input carries source material and temporary routes, never arguments or the task database. Browser exact-URL grants do not expand. Missing login material stops execution before launching components; public media retains existing multi-origin behavior. This is not an operating-system sandbox for untrusted components.

Components cannot prove content identity across runs. Every restart therefore clears the task-owned cache; ordinary component resume is not treated as safe recovery.

Media child processes cannot change rate limits in place; actual budget changes safely restart the task. Scheduling retains stable media slots within a batch, reserves against existing active counts when reducing concurrency, and shrinks after draining. This avoids bandwidth over-allocation and repeated restarts from ordinary queue fluctuations.

## Distribution layout and maintenance

UI private state, table presentation, and dialogs live in `windows/`, `models/`, and `dialogs/`, preserving public interfaces. Do not create empty directories in advance. System completion actions and extraction tools use absolute Windows system-directory paths rather than searching download directories for matching executables.

Portable and current-user installations use the same dynamic runtimes. Packages include licenses, Qt module source, SBOM, and file hashes. Users install optional media components or download them separately through Tools; they are not redistributed by default. Installation/removal touches only the dedicated application directory, preserving databases and downloads. External publication, signing, update origins, and browser stores are outside local-candidate completion claims.

## Stability diagnostics and dependency patches

An older runtime raised an access violation during mixed stress testing, with the stack in request-validation regex matching; the root cause remains unconfirmed. Two 15-minute AddressSanitizer runs of project code completed 5,300/5,400 file verifications without reporting out-of-bounds access or use-after-free. These diagnostics do not replace release stability acceptance. Windows allocation/deallocation category checking remains disabled by default; allocation tags from uninstrumented dependencies are not a confirmed root cause.

Dependencies moved to Qt 6.11.3, which fixes an independently reproduced container access-after-cleanup defect. That defect has not been shown reachable from the old stress scenario, so the upgrade is not presented as identification of the original crash. Matching source, licenses, runtimes, and validation must use the same version. Release retains separate PDBs for optimized-build diagnostics; published programs record only PDB filenames.

Later endurance runs exposed a history-deletion timeout. Isolated measurements of individually synchronized writes did not explain that stall, so no general transaction batching that changes new-task acknowledgment ordering was introduced. Skip only redundant deletion requests for nonexistent cache directories while independently cleaning verified task-owned assembly files. Existing final files remain intact. Stress failures collect isolated desktop thread dumps while retaining the 10-second deletion assertion and all frozen budgets to investigate the historical stall.

Test-framework report publication and terminal-state regressions live in `tests/system/stress_report_test.py`, separate from Native Messaging integration. Windows report publication retries brief reader contention within bounds; persistent errors still fail. Live monitoring reads bounded process-output summaries rather than opening reports being replaced. Acceptance cleanup begins only after the terminal report publishes successfully. Any exception revokes acceptance so later cleanup cannot mislabel a failure as success.

## Optional native credential vault foundation

The application defines `ICredentialVault` independently of Windows. Its native adapter stores only explicitly supplied proxy credentials in current-user Windows Credential Manager (`CRED_TYPE_GENERIC`, `CRED_PERSIST_LOCAL_MACHINE`). Random installation and reference IDs form a private target namespace. An entry binds its owner and canonical proxy route; mismatching reads and deletes fail. HTTP and HTTPS remain distinct; SOCKS5 aliases share the product's remote-DNS semantics. Native blob limits are checked before writes; malformed entries fail closed. Other platforms report unavailable without plaintext fallback.

This protects persisted credentials at the Windows logon-user boundary; it does not isolate hostile processes running as the same user. The adapter does not enumerate entries or accept caller-selected native target names. Native-read buffers are cleared before release, but complete in-process secret zeroization is not claimed. Callers must store a returned reference transactionally, clean up a failed reference commit, and revoke active use before deleting owned entries. Factory creation itself performs no writes.

The context core connects optional storage to task/policy persistence, desktop Remember/Forget controls and scoped IPC. Browser cookies are not stored in this vault. Tests use random owned installation IDs, real native writes/reads/deletes and a second process; they never enumerate or delete unrelated credentials.

## Actor-owned download contexts

One coordinator owns context policies, task changes, capability sessions and both SQLite repository facades. Task ownership, scoped request ID and route revision are server metadata outside editable download specifications. Existing standalone task/cache IDs remain unchanged. Profile submissions receive independent task IDs and replay within their owner, source, requested filename and pinned route identity. Ordinary projections contain only names, IDs, state and byte counts; owner filtering happens before pagination.

Context changes invalidate old caller sessions and affect new tasks. Accepted tasks retain their previous proxy snapshot/reference until explicitly rebound while inactive. Credentials are resolved into a worker copy before transport creation. Browser-origin grants are exact-URL, owner-bound and short-lived; only a required marker survives restart. Grant-bearing HTTP redirects stop before requesting any changed URL. Actual browser storage must supply correctly filtered grants; an extension origin alone never identifies a profile.

Optional native writes have a durable ownership journal before the OS call. A separate per-write nonce binds that journal to the native entry, preventing failed-write cleanup from claiming an older reference. SQLite and the OS vault are not one transaction: failed cleanup retains metadata for retry; startup loads all task/policy references before exact unused-entry cleanup. Completed history intentionally retains a referenced credential until removal or context revocation. No credential enumeration is used.

Revocation first persists disabled policy and denies new work, then pauses owned workers. Acknowledgement waits for their teardown and exact owned credential cleanup; cleanup errors keep the context disabled and report incomplete cleanup. Published files survive. Failed unused-entry cleanup reports a warning without disabling unrelated downloads. Per-context processes, schedulers and SQLite writers are unnecessary. BT sessions include owner identity even when route credentials match.

The typed core is tested independently of desktop composition. Framed broker authorization and actual profile/Remember/rebind/revoke widgets have separate product-path tests. Genuine sibling-browser issuance remains separate acceptance work; downloader fixtures alone do not establish that path.

## Scoped native broker composition

Desktop startup supplies the shared task/context store and native vault to the single coordinator. A Qt Core protocol adapter replaces window-owned native request handling. Administrative broker authority and legacy extension authority have separate per-start tokens; ordinary bearer requests use the core session registry and never fall back to another authority. The native host has a deliberate local broker invocation distinct from browser-origin invocation. Credentials use framed standard input, not command-line arguments.

The current extension remains a standalone v1 client. v2 fixtures establish downloader-side context readiness; they do not establish actual fingerprint-browser Profile/Storage/Bridge issuance. The UI remains trusted local administration, while browser projections and controls are owner-scoped inside the application actor.

## Context controls and authentication recovery

Profile administration and narrow profile submission live in a dedicated dialog implementation. QObject-bound future polling keeps actor persistence, worker teardown and vault calls off the UI thread. Accepted tasks retain their route snapshot; only explicit inactive rebind adopts current policy. Profile selection filters presentation, while the actor independently authorizes every scoped call. Revocation cleanup clears the durable policy reference before successful acknowledgement; incomplete cleanup cannot reactivate a profile.

Source-login requirements are part of the specification and task persistence contract. Cookie/Authorization/custom header material and FTP credentials derive the marker automatically. Restart and interchange remove material but retain the marker. Standalone edits can deliberately clear it; profile rebind retains an earlier grant requirement. Each adapter checks requirements before network/helper creation. Bound HTTP requests stop at any changed URL, including same-origin changes, until fresh authorization is supplied.

Task-list JSON is handled by a bounded codec separate from dialogs. Version 2 exports authentication markers and advisory context provenance without authorities or secrets. Import validates everything, obtains explicit standalone preview acceptance, then persists sequentially with stable request IDs. A retained in-memory batch makes partial-save retry idempotent without pretending the entire batch is transactional.

## Durable adaptive HTTP extents

HTTP recovery uses a bounded versioned layout containing the strong ETag, resource length, effective URL, generation and complete ordered extent partition. Stable numeric IDs address `part-N` and `offset-N`; they do not determine assembly order. At most 256 stored extents coexist with the task's configured 1–8 live connections. Valid legacy count-based layouts migrate without renaming their existing parts. Invalid or changed identities invalidate owned numeric artifacts before new-resource writes; unknown-length or unvalidated resources remain single-stream.

After completed messages are drained and pending extents admitted, an idle connection may split a large active remainder. The transport detaches that handle outside callbacks, synchronizes and checkpoints its prefix, closes the file, then atomically commits a shortened parent and a fresh child. Child data files are created after that commit. A crash before commit retains the old parent range; a crash after commit uses the new partition and treats missing child data as zero. No retained prefix is copied or assigned twice. Historical HTTP requests may overlap abandoned tails; durable ownership never overlaps.

The private layout codec enforces bounded metadata, unique IDs and complete contiguous coverage. A prospective child ID's unreferenced numeric artifacts are cleared before committing its ownership. Layout replacement retries the same generation for at most about 240 ms of delays to tolerate transient readers; persistent failure reports attention and keeps the previous generation authoritative. Curl manages only active handles, and bandwidth allocation uses their count. Assembly follows byte position. Application cleanup enumerates existing canonical owned names within the shared numeric limit; restart reuses the same cleanup path. Unrelated cache entries and published files survive. This protocol change does not require a database migration or another scheduling actor.

## Named queues and manual intent

Strict daily native payload accounting uses a separate worker-safe facade on the same SQLite backend. Schema 9 stores immutable transfer-run identities and UTC days, cumulative observed and reserved high-water marks, per-queue charged usage and a monotone period floor. Absolute reservation targets make duplicate/stale requests idempotent. Credits commit before a shared native lease admits callback bytes; all HTTP extents and FTP retries share that lease. No worker waits for the actor while the actor may join it. Callbacks accept only a credited prefix; deliberate short writes become policy holds rather than network retries. Existing recovery prefixes and local merge/hash work are excluded.

Each run belongs to one day. Every callback checks the effective day before spending cached credit. Midnight stops and settles the old run; later admission opens a new run, so delayed settlement cannot refund today's allowance or carry yesterday's credit forward. Clock rollback cannot lower the persisted day. Clean settlement refunds only provably unused reservation once, after callbacks stop; abandoned runs retain uncertain charged credit. History deletion, queue moves and policy edits do not erase usage. Confirmed observations may lag a credit batch and are displayed separately from charged/outstanding values. Budget edits require inactive queue jobs. Positive budgets reject BT/media before factory creation; local torrent imports are rejected before ACK to preserve their durable metadata contract. This metric covers admitted native body bytes, not ISP/wire traffic or estimated external-engine usage.

Weekly queue windows use explicit IANA time zones and a pure local-minute predicate. The scheduling actor obtains one UTC instant through an application port; the QtCore adapter converts that instant to civil time, so DST gaps have no instants and repeated minutes follow the same predicate twice. Overnight and equal-endpoint 24-hour windows belong to their starting weekday. Missing zones hold closed without a local/UTC fallback. A per-UTC-minute cache is invalidated on forward or backward clock changes and policy updates. Window closure preserves Run intent, while manual pause and source authorization retain their existing authority. Schema 8 appends window policy to queue CAS records. Desktop global limits are supplied before the coordinator starts; widget construction is not an admission gate. This clock port does not replace the credential/session security clock.

Queue membership is trusted task metadata independent of source owner, context, proxy, grants and recovery identity. The existing coordinator owns a focused queue registry; queues add no worker coordinators or database connections. The shared SQLite backend exposes a queue facade alongside task/context facades. Migration 7 installs the default queue and persists membership and Run/Paused intent without moving task files or changing old state ordinals.

Queue configuration uses revision checks. Moving an inactive selection validates every member and conditionally updates all queue IDs and timestamps in one transaction; any mismatch rolls back the batch. Task owners, route snapshots, credentials and paths remain unchanged. The default queue cannot be removed, and a named queue must contain no task records before removal. Browser drafts and interchange files cannot choose queue policy or membership. Existing scoped submissions and imports use the default queue; trusted desktop controls can subsequently move inactive tasks.

The actor admits work in queue turns, preserving priority within a queue and intersecting global and queue active limits. Persistent manual intent distinguishes a user pause from queue policy waiting. Disabling a queue cooperatively pauses its running work; enabling it reopens only eligible policy-held Run tasks, preserving retry and one-shot gates. Completed, failed, revoked and manually paused work does not start through queue enable. Startup retains safe recovery semantics.

Rates are allocated across global, queue and task caps with unused-cap redistribution. Media helpers retain their actual launch reservation until exit; incompatible reductions return an ordinary pause-first error. Settings await that result before updating saved preferences. Finite exhausted allocations must never become the existing zero/unlimited control value. HTTP reduces live handles when a tiny positive allocation cannot fund one byte per handle; the durable extents remain pending. These are bandwidth controls, separate from the later traffic ledger and payload allowances. Completion notification and power countdown also check unfinished work, including tasks that have never been active.

## Portable Release build policy

Release dependencies use a separate `x64-windows-portable` triplet and install
tree. A shared CMake policy supplies empirically checked MSVC prefix mapping and
basename-only PDB references to both the application and dependencies. Debug and
AddressSanitizer remain diagnostic builds. Relative source locations retain their
filenames and lines; native tests run from the source root.

Pinned overlay patches change only libxml2's implicit portable catalog default
and OpenSSL's public build-information rendering. Existing per-context resource
denial remains in force. Signed supplier libraries are preserved byte-for-byte;
their SDK hashes and expected valid publisher signatures define narrowly reported
provenance exceptions. Private host paths remain forbidden in all runtime files.

## SFTP and native remote-file transfer

FTP/FTPS and SFTP use typed factories over one remote-file stream/cache/publication
implementation. Protocol admission and connection/authentication remain explicit;
SFTP requires a canonical SHA-256 server-key pin before credentials are sent.
Password and explicit local private-key modes cannot fall back to agents, default
keys or one another. Credentials and private-key contents are never persisted or
exported; only public fingerprint/method/key-path configuration is stored.

Remote size and modification time are weak validators. Reusing a durable prefix
requires an expected final SHA-256, matching source metadata and, for SFTP, the
same server pin. Assembly, scanner and no-replace publication remain shared.
A single process-lifetime curl initializer selects Windows Schannel before global
initialization, preserving the existing TLS trust backend when SSH crypto adds
another TLS backend. Runtime protocol capability, the SSH DLL and its license
notice are checked alongside ordinary deployment files.

## Explicit origin authorization and complex HLS

Explicit VOD mode uses an operation-owned adapter containing only public code. After Cookie processing, every request/redirect clears inherited headers and injects separate material by scheme, host, and effective port. An in-memory capability channel delivers policy; the gateway opens only after actual backend/handler preflight. Only GET/HEAD are allowed; other backends and system proxy bypass are disabled, preserving end-to-end TLS. SQLite v19/task lists store only origins and login requirements. Material and confirmation are not persisted; URL edits, imports, and restarts require reauthorization. Browser-context authority does not expand. This is a trusted-component protocol, not a malicious-component sandbox.

Native Live reuses the public policy, selecting origin material individually for curl playlists, keys, maps, segments, and redirects. AES-128 CBC uses the existing crypto library, memory-only keys, strict padding, and explicit or sequence-derived IVs. Keys are fetched again for the current segment to support rotation at the same URI. Ranges require exact 206/Content-Range/body lengths. Ignore low-latency hints and consume complete segments only; reject delta updates, skipped segments, and DRM.

fMP4, audio tracks, and discontinuity sequences retain complete decrypted segments in a bounded private temporary directory. Generated local playlists reference only owned filenames. Trusted local muxing allows only file/pipe protocols and constrained containers; it never receives remote playlists, URLs, keys, or login environments. Separate audio requires matching sequence numbers, durations, discontinuity sequences, and explicit PROGRAM-DATE-TIME clocks; alignment is never guessed. Process groups are cancellable with bounded time, diagnostics, and output. Final output still passes digest checks, scanning, and publication without replacement. Ordinary TS/FLV needs no muxing component; the native engine retains all networking and quota accounting.

## Native macOS adaptation and CI qualification

Darwin discovers native development dependencies independently. Linux/macOS share the pinned BT source fallback; Windows retains its toolchain entry. Apple credentials use the user Keychain through Security.framework. Random installation/reference namespaces isolate entries; reads/deletes verify owner, canonical route, and write nonce. Writes only add entries. Deletion uses the verified entry's native identity, with a private lock serializing each reference. Locked/denied access fails explicitly without blocking prompts or plaintext fallback; other entries are not enumerated. This neither isolates malicious same-user processes nor promises complete memory zeroization.

Private components use an EOF guardian in a separate session. Only the owner retains the close-on-exec write end. The guardian closes other descriptors, especially Qt startup pipes and secret input, and retains group membership until termination. Owner exit/stop terminates the owned group without signaling a reused numeric group ID. The protocol assumes trusted components and cannot constrain malicious code that deliberately escapes the session.

Directory publication uses native Darwin `renamex_np(RENAME_EXCL)`, without a check-then-overwrite emulation. Local torrent reads filter known network/automounted sources from a nonblocking mount snapshot, then reject links and special files through descriptors. Only system `/var` and `/tmp` aliases receive explicit conversion; verify the same file before/after reading. Media launchers locate themselves through Qt's absolute application path rather than Linux `/proc`.

Native tests in dual-architecture GitHub CI must establish actual Apple API, kernel, and SDK behavior. This increment delivered preparation awaiting execution; Windows regressions and script simulations do not count as macOS passes. See [macOS acceptance](../testing/macos-acceptance.md) for qualification boundaries and gaps.
