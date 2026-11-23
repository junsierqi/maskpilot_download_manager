# MaskPilot Download Manager: Product and Engineering Blueprint

> Status: user-authorized product and engineering baseline. `CMakeLists.txt` defines the source version; acceptance records define support.
> Confirmed: open source, C++, Qt, and planning the complete workflow and major directories before implementation.
> Initial defaults: Windows 11 x64 first, C++20, Qt 6 Widgets, owned task management and HTTP scheduling. Current qualification follows the feature matrix and platform guides.
> Existing `AGENTS.md` remains unchanged. This blueprint supplements constraints and cannot override subsequent explicit user instructions.

## Mainline development and commits

- Develop directly on the latest `master`. All completed, verified functionality must converge on mainline rather than remain scattered across long-lived feature branches.
- Check the workspace and commit relationships before work; confirm mainline includes existing results. Do not create feature branches or worktrees unless explicitly requested.
- Make focused commits after each complete stage passes implementation, validation, and review. Distinguish pending changes from accepted commits; do not describe unfinished work as delivered.
- Bound rollback to explicit commits or the current diff, preserving other capabilities and user data. Before deleting old branches, prove their commits are included in mainline.
- Check remote state when configured. Without a remote, claim only local mainline completeness, never remote synchronization.

## 1. Goals and completion boundaries

Build a maintainable, complete open-source download manager for daily use, with room for automation and extension. Prioritize file correctness, reliable recovery, browser takeover, usability, performance, then feature count.

Complete delivery means implementing and accepting the entire confirmed version scope, including installation, configuration, normal/error flows, and documentation. Build and validate internal milestones without presenting prototypes as the final product. Neither defect-free first-pass coding nor completion within one uninterrupted session is guaranteed.

Feature completeness is a long-term goal, not a current compatibility promise. Map capabilities individually in `docs/product/feature-matrix.md`, recording implementation, acceptance examples, and gaps. Unevaluated features cannot be marked supported. Deferred goals must not silently disappear because implementation is difficult.

## 2. Version scope and feature acceptance matrix

The table proposes scope. Product identifiers `F-*` are separate from workflow `REQ-*` and `TASK-*`; implementation plans must map them rather than merely check feature names.

This table preserves product goals and original staging, not current completion status. Mainline now includes dynamic segment splitting, named queues/weekly windows, and recurring site synchronization. Linux has environment-specific acceptance; macOS build/adaptation and CI await real execution. A maintained media-site compatibility list, additional platforms/services, and formal distribution remain gaps. See the [feature matrix](product/feature-matrix.md). Version acceptance does not imply completion of the entire product goal.

| ID | Capability and delivery boundary | Proposed version | Stage | Minimum acceptance evidence |
|---|---|---|---|---|
| F-01 | HTTP/HTTPS, redirects, proxies, headers, authentication | 1.0 | M2 | Local success, timeout, authentication, proxy, and redirect fixtures |
| F-02 | Adaptive segments, connection limits, single-stream fallback | 1.0 | M3 | Matching hashes with correct, ignored, and invalid Range responses |
| F-03 | Pause, resume, crash recovery, remote-change detection | 1.0 | M3 | Identical recovered bytes after forced termination/restart |
| F-04 | Queues, priorities, concurrency, global/task rate limits | 1.0 | M4 | Quotas respected; rate error meets the test contract |
| F-05 | History, search, categories, directory rules, batch import/export | 1.0 | M4 | Persistence across restart; corrupt imports preserve tasks |
| F-06 | Scheduled start/stop, scheduled queues, completion actions | 1.0 | M4 | Timezone, wake, missed-schedule, and exit tests |
| F-07 | Chrome/Edge extension, context-menu sending, download takeover | 1.0 | M6 | Installed-extension public/login downloads and browser fallback |
| F-08 | New task/details/settings, tray, notifications, shortcuts | 1.0 | M5 | Real main paths, high DPI, and keyboard checks |
| F-09 | Static page links, batch filtering, URL templates | 1.0 | M6 | Previews, deduplication, count limits, cancellation |
| F-10 | Web media discovery, HLS/DASH VOD, merging | 1.0 | M7 | Owned fixtures for audio, quality, missing segments, merge failures |
| F-11 | Supported-site media extraction adapters | 1.0 | M7 | Explicit supported list and diagnosable failures; no arbitrary-site promise |
| F-12 | Temporary-link refresh and safe retry | 1.0 | M3/M6 | Revalidate identity; never combine resource versions |
| F-13 | File verification, conflicts, disk failures, error logs | 1.0 | M2/M3 | Determinate full-disk, permission, duplicate, and digest-failure outcomes |
| F-14 | Chinese/English, installation, portable package, upgrade migrations | 1.0 | M5/M8 | Clean-machine operation; readable tasks after upgrade |
| F-15 | FTP/FTPS, site rules, recursive site capture | Separately versioned after comparison | Later | Explicit protocols/depth; link extraction is not complete site capture |
| F-16 | Firefox integration and further browser interaction | Separately versioned | Later | Independent compatibility tests and recorded differences |
| F-17 | BT/magnets, mirrors, remote control, plugins | Extension release | Later | Separate projects, threat models, revocable permissions |
| F-18 | Linux/macOS, NAS, headless services | Extension release | Later | Independent build, installation, platform acceptance |

Version 1.0 excludes DRM bypass, arbitrary paid-content access, unbounded crawling, cloud accounts, and unrestricted plugin execution. Media downloads are limited to content the user can access and tools explicitly support. Accept media extraction and browser takeover separately; neither substitutes for the other.

## 3. Technical baseline and dependency policy

| Area | Proposed choice | Boundary |
|---|---|---|
| Language | C++20; C primarily for existing C libraries | No duplicate core implementations |
| Interface | Qt 6 Widgets, Model/View, resources, translations | No download/database logic in widget slots |
| Network | libcurl multi behind transport adapters | Own task/segment policy, not TLS/HTTP stacks |
| Persistence | SQLite with versioned migrations | One write owner; no scattered business/UI SQL |
| Build | CMake + Ninja + MSVC; vcpkg manifest | Pin compatible Qt acquisition, compiler, CRT |
| Tests | CTest + GoogleTest; Qt Test for UI adapters | Controlled local network services |
| Media | Separate optional yt-dlp/FFmpeg adapters | Structured arguments, versions, timeouts, cancellation |
| Browser | TypeScript extension + C++ Native Messaging host | Browser language required; core remains C++ |

M0 must prove a locally buildable combination, then record exact versions, sources, hashes, and platforms. Do not claim ready commands before locking versions. QtNetwork may serve limited Qt needs but cannot duplicate the main libcurl engine.

Original project code uses the root MIT license. Maintain dependency inventories, license texts, and distribution methods before release; check actual Qt module/media-tool versions and builds. Dependency source/binary distributions retain real provenance and required notices. Do not assume every dependency shares one license. The project has not copied reference implementation code.

## 4. Major directories: planning and controlled evolution

Current major directories follow below; see the [architecture overview](architecture/overview.md) for responsibilities, subdirectories, and CMake dependencies. Do not populate unimplemented modules with empty classes or fake interfaces. Adjust directories when responsibilities and maintenance costs justify it; explain benefits/impact, update documentation, and verify boundaries and existing behavior first.

```text
.
├── CMakeLists.txt / CMakePresets.json   # Targets and shared presets
├── vcpkg.json / vcpkg-configuration.json
├── .clang-format / .clang-tidy
├── .github/workflows/                  # Platform builds and validation
├── apps/
│   ├── desktop/                        # Qt, single instance, composition
│   ├── cli/                            # Validation client and site commands
│   ├── native_host/                    # Native Messaging and local IPC
│   └── media_guard/                    # Local media component launchers
├── src/
│   ├── domain/                         # Models and invariants
│   ├── application/                    # Coordinators, use cases, ports
│   ├── infrastructure/                 # Protocol, storage, system adapters
│   └── ui/                             # Windows, dialogs, models, styles
├── browser_extension/                  # TypeScript, manifests, build/tests
├── resources/                          # Static application resources
├── database/migrations/                # Incremental SQL migrations
├── tests/                              # Unit/integration/system/platform tests
├── benchmarks/                         # Performance scenarios and reports
├── cmake/                              # Native dependencies and build/package config
├── scripts/                            # Build, validation, packaging entry points
├── packaging/                          # Installation and browser registration
├── docs/                               # Usage, product, architecture, protocols, acceptance
├── third_party/                        # Actual dependency notices
└── out/                                # Ignored generated content
```

Directory rules:

1. Give each library a CMake target. Public headers live in `include/maskpilot/...`, private implementations in the module's `src/`; declare dependencies through `target_*`.
2. `apps/` assembles dependencies; `ui/` presents data. Use public interfaces across modules, never relative paths into private headers.
3. Organize tests by validation layer. Names describe module/behavior, such as `download_recovery_test.cpp`.
4. Avoid ownerless `common/`, `misc/`, universal `utils`, and giant managers. Identify ownership/callers before shared abstractions.
5. Build into `out/build/<preset>/` and package into `out/package/`; commit neither. Keep `CMakeUserPresets.json` and machine paths private.
6. Databases, logs, credentials, and downloads belong in system user directories or user-selected locations, never source directories. Tests use isolated temporary directories.
7. Adding files within existing modules needs no new approval. New major modules, dependency reversals, or public-interface moves require an ADR, impact scope, and migration plan first.

## 5. Module dependencies and interface contracts

Allowed compile-time dependency direction:

```text
apps/desktop ──> ui + application + infrastructure
ui ───────────> application ──> domain
infrastructure ──────────────> application ports + domain
apps/native_host ────────────> minimal protocol/platform adapters; no full UI linkage
domain ─────────────────────> C++ standard library
```

`domain` and `application` depend on neither QWidget, libcurl, SQL, nor Windows APIs. Keep Qt/dependencies in adapters. Review CMake/source dependencies for cycles; there is no separate automated architecture-rule checker currently. Do not reimplement mature library capabilities merely to claim pure C++.

Proposed core ports include `ITransferTransport`, `ITaskRepository`, `IFileStore`, `ICredentialStore`, `IClock`, and `IEventDispatcher`. Introduce interfaces for established boundaries, not mechanically for every small class.

Commands carry stable task IDs, request IDs, and attempt generations; events carry sequence numbers and generations. Late callbacks cannot alter new attempts. Commands return explicit results/error codes; the UI does not poll the database for business state.

Initially, one desktop process hosts background downloads. Closing the window hides it to the tray by default; Quit performs controlled shutdown. The native host validates messages, finds/starts the single instance, and forwards requests rather than starting a second manager. A separate daemon is a later extension.

## 6. Threads, resources, and C++ conventions

- The UI thread handles widgets/presentation only. A coordinator serializes state and SQLite writes; the network reactor exclusively owns libcurl handles.
- The original M1/M2 prototype plan proposed a bounded disk work queue, with backpressure pausing/resuming reception instead of dropping buffers or growing memory. The implementation choice is recorded under networking, disk writes, and threading in the architecture decisions.
- Pass values, immutable snapshots, and explicitly owned buffers across threads. No callbacks under locks, UI blocking on networking, or uncontrolled detached threads.
- Controllers, networking, and disk work communicate through events/cancellation tokens. Define timeouts, shutdown order, capacity, and late-event handling for every stage.
- Use RAII. Raw pointers are non-owning; prefer `unique_ptr`, using `shared_ptr` only for shared lifetimes.
- QObject parenting and smart pointers cannot both own one object. Never manipulate QWidget across threads.
- Files use `snake_case`, types `PascalCase`, functions/variables `camelCase`, private members a trailing `_`, constants `kCamelCase`, and namespace `maskpilot`.
- Use 4-space indentation and clang-format. Apply clang-tidy/compiler diagnostics to new code. Exceptions cannot escape C callbacks, thread entries, or IPC boundaries.
- Record versions, thread defaults, and capacities in configuration/architecture documents rather than magic numbers. More connections cannot substitute for performance analysis.

## 7. Task states and recovery invariants

Persisted states: `Queued`, `Scheduled`, `Probing`, `Downloading`, `Pausing`, `Paused`, `RetryWaiting`, `Verifying`, `Finalizing`, `Completed`, `Failed`, `Cancelling`, `Cancelled`, `NeedsAttention`.

| Trigger | Allowed main path | Required behavior |
|---|---|---|
| Immediate download | Queued → Probing → Downloading → Verifying → Finalizing → Completed | Success follows verification and publication |
| Pause | Active → Pausing → Paused | Stop new requests, settle pending writes, commit safe progress |
| Resume | Paused/RetryWaiting → Queued → Probing | Revalidate resource and local files |
| Automatic retry | Retryable error → RetryWaiting → Queued | Bounded count/time, exponential backoff, honor Retry-After |
| User attention | Identity change, expired authentication, disk issue → NeedsAttention | Explicit continue, rebuild, or refresh-link options |
| Cancel | Nonterminal → Cancelling → Cancelled | Stop requests/writes; file deletion is separate and explicit |
| Abnormal-exit recovery | Scan unsettled tasks → inspect → Paused/NeedsAttention | Never mark uncertain data complete automatically |

One controller owns transitions. Complete the transition table before implementation for repeated pause/cancel, failures at every stage, exit, and recovery. Reject unlisted transitions or define explicit idempotence.

Required invariants:

1. Use 64-bit offsets and half-open `[begin, end)` ranges. Segments cannot overlap or exceed bounds. Unknown lengths use one stream without invented percentages.
2. Enable segmentation only after validating Range and resource identity. On 200, invalid Content-Range, 416, or changed length, reprobe or safely fall back; never write blindly to expected offsets.
3. Prefer strong ETag/If-Range for resume. Weak validators are not strong identity guarantees. Restart or request intervention when consistency cannot be established.
4. Segmented requests use the same byte representation; transparent decompression cannot desynchronize remote/local offsets.
5. SQLite transactions cannot make file writes atomic. Separate received, written, and confirmed-recoverable progress. Persisted recoverable offsets cannot precede file synchronization.
6. Recovery cross-checks database, temporary files, and segments, redownloading uncertain ranges. Forced termination and power loss are different validation levels; no absolute power-loss guarantee on all hardware.
7. Temporary/final files share a destination volume. Verify length/available digests, record finalization intent, publish, then commit completion. Restart reconciles intermediate states.
8. Preserve existing files and generate a new name by default. Overwrite, delete, task cleanup, and file cleanup are separate; never silently delete existing user files.
9. A refreshed URL's filename alone cannot establish identity. Never forward authentication/headers unconditionally across origins.

## 8. Data, protocols, and security

The model includes at least task, source, segment, queue, schedule, category, settings, and schema_version. Do not append unbounded logs/transient events to task tables. Store paths, validators, lengths, attempts, and confirmed segments; never plaintext passwords or long-lived Cookies.

Number migrations incrementally, such as `0001_initial.sql`. Back up metadata before transactional migration. Preserve the old database and report failure. Older programs cannot silently downgrade/open newer schemas.

Specify browser contracts in `docs/protocols/` first: version, message-size limits, request ID, commands, field validation, responses, and errors. Native Messaging validates callers; desktop IPC is current-user-only with connection authentication. An unauthenticated local HTTP port is not the default.

Collect only Cookies/headers needed for authorized downloads, never the entire browser Cookie database. Redact URL queries, authentication headers, Cookies, and sensitive local paths from logs; no telemetry by default. TLS verification is enabled by default; exceptions cannot become silent global settings.

Launch media tools with structured arguments, never shell command concatenation. Pin sources/versions, bound output/runtime, clean up process trees on cancellation, and retain understandable errors. Design authenticity checks/rollback before automatically upgrading executable code. Online update discovery and formal distribution are deferred; manual offline updates do not mean an online service is delivered.

## 9. Interface and complete user workflows

Plan empty, loading, error, and disabled states for task lists, new tasks, batch-import previews, details, queues/schedules, category rules, media selection, settings, diagnostics/about, and extension popups. Fresh desktop settings default to English; saved English/Simplified Chinese choices are preserved. Changing language requires restart.

Main flow: add/take over → validate URL/destination → choose immediate/queued/scheduled → probe/download → pause/retry/resolve errors → verify/complete → open file/folder.

Details show source, destination, speed, remaining work, connections/segments, recovery capability, and recent errors. Distinguish network, authentication, resource changes, disk, and missing-tool failures with actionable options; internal enum names are not user explanations.

Takeover follows user-enabled file/site rules, never all browser requests. Provide deduplication and acknowledgment. On failure, retain the browser path rather than losing a cancelled original. Sensitive requests cannot reuse authentication across sites.

Schedules use timezone ID + rule + next execution point, defining daylight-saving, missed-run, and wake behavior. Execute once after wake by default without duplicate replay. Shutdown/similar actions are disabled by default, explicitly enabled, and cancellable during a countdown.

## 10. Development sequence and stage gates

| Stage | Deliverables | Exit gate |
|---|---|---|
| M0 Baseline | Feature comparison, PRD, directories/dependencies, ADRs, toolchain, pins, test design | Confirmed baseline, clear build/validation entries, tasks for unknowns |
| M1 Foundation | CMake targets, startup, logs/settings, SQLite migration, fixtures, CI | Clean configuration/build/startup, dependency/thread prototypes pass |
| M2 Single stream | F-01/F-13, command → network → disk → completion/failure | Normal, timeout, authentication, permission, disk failures verified |
| M3 Reliable engine | F-02/F-03/F-12/F-13, segments, recovery, verification, publication | Fault matrix passes; repeated operations/changes preserve files |
| M4 Management | F-04/F-05/F-06, queues, categories, history, schedules | Persistence, quotas, time boundaries, completion actions pass |
| M5 Desktop | F-08/F-14, main screens, tray, translations | Real main paths, responsive UI, high DPI checks |
| M6 Browser/batch | F-07/F-09/F-12, extension, host, IPC, filtering | Installed Chrome/Edge authentication, deduplication, fallback |
| M7 Media | F-10/F-11, support matrix, adapters, HLS/DASH | Owned media fixtures pass; missing-tool/cancel handling |
| M8 Candidate | F-14, regressions, performance, installation/portable, migration, licenses/docs | Evidence for section 12; remaining differences disclosed |

Small interface-validation prototypes may cross stages when labeled and archived; they cannot become unreviewed production code. Keep every stage buildable. Complete current validation/records, then advance to the next authorized task without repeated continue replies.

## 11. Test strategy and commands

Tests validate requirements rather than mirror implementation. Each F-* needs applicable success, failure, boundary, persistence, and cancellation cases. Control 200/206/416, redirects, disconnects, throttling, authentication, remote changes, and malformed responses. Establish fixtures in M0; public networks cannot be the sole acceptance source.

Prioritize zero-byte, unknown-length, and greater-than-4 GiB files; Chinese/long paths; duplicate directories; full disks; deletion/late callbacks; network changes; repeated takeover; missing databases/temporary files; corrupt imports; tool exit; and forced termination.

Run existing entries below from the root. See [README build instructions](../README.md#building-from-source) for Windows SDK, Qt, native dependencies, and test prerequisites. Linux/macOS use their own presets/guides rather than these Windows commands:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug --output-on-failure
powershell -File scripts/verify.ps1
powershell -File scripts/package.ps1
powershell -File scripts/smoke.ps1 -PackageDirectory out/package/MaskPilot-0.3.0-windows-x64
```

`verify.ps1` aggregates Windows build, CTest, release path/manifest checks, offline update recovery, and extension tests. Failures return nonzero. It does not automatically run clang-format/clang-tidy; apply repository formatting and applicable diagnostics separately. The extension uses a lockfile with `npm ci`, `npm run build`, and `npm test`. CI definitions do not prove executed CI. Record local, CI, real-machine, and manual evidence separately.

Avoid misleading repository-wide coverage percentages initially. Critical state, range, retry, and recovery branches need positive/negative cases and disclosed gaps. Performance records include CPU, disk, network, dependency versions, and fixtures; more connections alone do not prove performance.

## 12. Release acceptance and definition of done

1. Map every in-scope F-* to tasks/evidence. No silent deferral or placeholders for failed requirements.
2. Install/start on clean Windows without developer tooling. Check portable operation, host registration, upgrades, and uninstall separately.
3. Pass correctness, recovery, resource-change, repeated-operation, authentication, and disk-failure matrices. Digest mismatches, silent task loss, credential leaks, and arbitrary command execution block delivery.
4. Default stress scenario: 100 queued tasks, 8 active, at most 8 connections per task under a global cap, and 8 hours of mixed downloads without crashes/unbounded growth. Freeze the machine, fixtures, and memory budget in M0; never loosen them before running.
5. Candidate responsiveness: common UI actions at p95 ≤ 200 ms on the baseline machine, with cancellable long operations. Freeze measurement methods in M0.
6. Packages contain complete dependencies, licenses, versions, and resources, with no machine absolute paths, credentials, or test downloads.
7. README, installation/development guides, support tables, known issues, and recovery instructions match behavior, without unexplained TODOs, fake data, or dead entries.
8. Deliver source, a runnable package, reproducible build steps, reports, and limitations. Missing certificates, store accounts, or publication permissions are explicit external blockers, never a published-availability claim.

## 13. Execution, change, and session recovery rules

- Before coding, establish scope, directories, dependency direction, state/protocol contracts, and acceptance matrices. Evolve them through recorded responsibilities, maintenance needs, and evidence.
- Refactoring/layout changes retain layering, security boundaries, and acceptance rules. Record reasons, benefits, impact, and migration; update design/layout documentation first. Keep candidates consistent during acceptance so evidence corresponds to one version.
- Work items identify requirements, files, dependencies, normal/error behavior, commands, and completion conditions. Read relevant interfaces before editing.
- Implementers decide routine details. Record reasons, impact, and alternatives before changing scope, data formats, public protocols, major modules, or acceptance criteria. Convenience cannot replace user product decisions.
- Diagnose failed checks. Never remove tests, weaken assertions, or change gates to manufacture completion. Repeat affected checks after fixes without purposeless full-suite repetition.
- Resolve current work before switching features. Milestones record completed/failed items, evidence, risks, and the next action.
- Advance authorized reversible work automatically. Pause dependent steps only for missing critical decisions, credentials/tools, destructive actions, or external publication conditions; independent work may continue.
- Context boundaries do not require replanning. Read `AGENTS.md`, this blueprint, records selected by `.idea-to-code/current.json`, and the latest task, then resume.
- Planning approval does not automatically authorize commits, pushes, or publication. Existing explicit authorization persists.
- Distinguish source inspection, test doubles, local integration, real product paths, and external validation. One executor's self-review is not independent review.

## 14. Baseline confirmation and remaining unknowns

The user selected C++, Qt, and planning first. The original proposed baseline was Windows 11 x64 first, Qt Widgets, the 1.0 scope above, and later FTP/full-site capture/Firefox/cross-platform boundaries. Consult current matrices for subsequent implementations.

Original code uses MIT; new dependencies need separate checks. Engineering configuration and acceptance records maintain versions, installer tooling, fault fixtures, and budgets. Never assume untested combinations work. If they fail, compare alternatives under the same goal rather than silently switching to Go or another UI stack.

This blueprint does not require every decision resolved immediately; resolve associated decisions before relevant implementation. Continue after confirmation until agreed release acceptance or a concrete external blocker, rather than stopping at the first runnable window.

## 15. Technical and distribution evidence

Configuration, architecture decisions, and dependency notices record contracts, versions, licenses, and build outcomes. External-product research stays outside the repository; do not copy product names, identifiers, or research notes.
