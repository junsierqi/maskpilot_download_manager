# Release candidate acceptance

The **historical frozen 0.3.0 baseline** below passed local functionality and an independent eight-hour run. Later authenticated media/POST/SFTP/live/audit code does not inherit that qualification. Judge current commits/builds from specific [audit evidence](../testing/repository-audit.md). Old 0.2.0/0.3.0 packages remain frozen; online updates/formal release were deferred. External platforms/production browsers/stores/signing remain unverified; these candidates were not publicly released. The complete original goal remains unfinished: see [gaps](../product/feature-matrix.md).

## Final 0.3.0 runtime qualification (2026-11-19)

Tested source `a982b6fa3be245173d224038e7ff900b8fded0bb`, clean at startup. No-overwrite private baseline `out/qa/qualification-0.3-portable-final.json` binds **154** native/build-policy inputs, **38** runtime files, and **3** explicit external media tools; SHA-256 `9bf147b0c13dd3810b68b73e9640addab5633a46c8576c1d86e7a5d04f2df525`. Desktop SHA-256 `eeb9605233a8347ce8a729ce3f017d4532baad22e1d809b39cd98e2aa879edd9`. Machine paths remain private.

| Check | Evidence for those runtime bytes |
|---|---|
| Regression | **269/269 CTest**, 626.40 seconds; extension **26/26**; **13** actual media/format/container/proxy/Qt cases; real Chromium offline pages made **zero remote requests** |
| Browsers | Chromium **8**, Firefox **9** assertions: durable handoff/recovery/Cookie isolation/trusted panel/host fallback; registrations restored. Not production sandbox/store qualification |
| Large file | **4,294,967,553 bytes**, 61.39 seconds; SHA-256 `c1a2510f41f6fc63bd169116662b1a514c66b43fab60b3dc656d114cfe83145a` |
| Native UI | Windows-native profile/queue controls **2/2**, PNGs inspected. Synthetic settings illustrate UI, not transfer |
| Old data | Actual frozen 0.2.0 created completed/paused HTTP tasks; isolated stopped data migrated **schema 4→13**, preserving IDs/old fields/defaults and matching backup. No automatic contact; explicit same-ID resume verified hashes; original DB/package unchanged. Not every legacy protocol/credential/cache combination |
| Continuous load | **28,824.66 seconds**, **28,831.14** including final checks; 908 cycles, **90,800 hashes**, **2,716 pause/resumes**, peak 8; 91 media cycles/**273 outputs** |
| Memory/response | Peak private **23.49 MiB**, working set **45.97 MiB**, first/last-quarter drift **1.70 MiB**, IPC p95 **19.64 ms**, max observation gap **3.82 seconds**, within original budgets |
| Cleanup/parity | Native processes/temp cleaned; source/policy/runtime/helper hashes and mapped modules matched before/after, exit 0 |

Long-run report `out/qa/stress-0.3-portable-final.json`, SHA-256 `a2b4db432da747d49a6617a0817ecd8469dd889fbb60223c5393a67973be029c`. Isolated offscreen HTTP loops/local synthetic MP4/HLS/DASH are not eight-hour public FTP/BT/real-site qualification. Budgets stayed 512 MiB memory/64 MiB quarterly drift/200 ms GUI IPC p95/30-second observations. Earlier short/interrupted records remain; timings are never joined.

Final packaging used `scripts/package.ps1 -SkipBuild` with explicit portable build/baseline/qualified runtime. All **38 runtime files must match byte-for-byte**. Docs can make `sourceCommit` later than `qualifiedSourceCommit` but cannot change frozen native inputs. Staging/ZIP must pass paths/PE/supplier provenance/full manifests/licenses/source-patch audits, then `scripts/smoke.ps1` checks desktop/CLI/native handoff/install/reinstall/offline update/rollback/cleanup/uninstall without development PATH, retaining unknown files/registration ownership. External ZIP digest stays outside internal docs to avoid circular self-hashing.

Remaining observation: two earlier site-completion waits had unconfirmed causes. Diagnostics/assertions/five-second budget stayed intact; 70 repeats/full regression later passed without claiming root-cause repair. Clean Windows/production sandbox/store signatures/release signing/trusted online discovery/long real-site compatibility/browser integration remained unverified; no renaming/public release at this stage.

## 0.3.0 freeze: historical work record

Versions were unified across CMake, extension manifests/lock, Windows assemblies/native resources. `scripts/release_manifest.py` creates no-overwrite local baselines binding native inputs/runtime/explicit helpers/plugins. Tester checks sets/hashes/its own scripts before/after; private machine paths are not public release manifests.

Update/recovery tools, final package, environment functionality, and a new eight-hour run were required before acceptance, regardless of version labels. Internal manifests never replace independently trusted package hashes/signatures.

Five native 0.3.0 programs had version resources checked; **269/269** regression passed in 667.09 seconds. Isolated runtime preflight requested 300 seconds/ran about 329.12, verified 1,000 files/30 pause-resumes/2 media cycles, IPC p95 18.65 ms/private drift 2.04 MiB, cleanup and before/after/mapped-module parity passed. **Not eight-hour qualification**.

At this historical point, offline-tool/candidate acceptance was ongoing. Successful upgrades retained file rollback materials until explicit finalization; external modifications blocked replacement/rollback and kept journals. `release.json`, complete manifest, and external ZIP digest describe version/consistency/bytes, not signatures or online service.

Windows fixtures passed replacement/removal/rollback/checkpoint faults/real locks/junctions/process occupancy/child termination. Mid-write errors clean only newly exclusive partials; unverifiable truncation preserves the scene for manual repair. Fixture-owned repairs are not automatic recovery. Real full-package/old-database acceptance remained separate.

Candidate smoke passed desktop/CLI/bridge/install/reinstall/offline ZIP apply/rollback/finalize/uninstall without development PATH/Qt, retaining unknown downloads. Artifact `out/package/preflight-0.3-001/MaskPilot-0.3.0-windows-x64.zip`, SHA-256 `576127b7716f0a546ad910badd67bab247456c41fc81cb8a317eeff3351516b3`, accurately dirty source state; a tool-validation artifact, not final release.

That runtime’s long-run attempt was deliberately stopped after private compiler/diagnostic paths were found: about 2,411.18 seconds/7,400 hashes, failed/Interrupted, not eight hours. Portable-build repair/new byte qualification/new timing from zero followed; old programs/packages stayed intact.

## 2026-11-18: native proxy development slice

Independent Release `out/build/windows-msvc-completion/`, desktop SHA-256 `9f3e46f354d5ccb80312e5075c20a1c3da999c30fae2f21ff041b0310500bbc0`. **77/77 CTest** in 323.72 seconds and extension **14/14** passed. Actual Qt tested password fields/required-login/task creation, screenshot `out/qa/completion-proxy-dialog-final.png`.

Actual CLI recording HTTP/SOCKS5 proxies checked HTTP/FTP/remote DNS/redirects/no proxy-auth leakage/environment `NO_PROXY` contamination/wrong-missing credentials/no direct bypass/both FTPS modes. Required-login survived forced exit and refused secret-free resume before contact; SQLite/journals/task lists had no fixture credentials. Migrations backed up, preserving tasks/protocols/data.

Real BT metadata/pieces used CONNECT/SOCKS5 with concurrent direct/different proxies/same-endpoint different-credential isolation. Four boundary groups—bad password, explicit-peer trackerless magnet/cached metadata, bad-node/tracker alternates, and same-route bad-job isolation—each passed **3** repeats in 37.73 seconds. URI web seeds/deduplication/256-entry caps had separate parser/round-trip tests, not public web-seed qualification.

Failure/repair history remained: auth errors initially lacked owning-task mapping; outbound alerts meant attempts, not successful connection; port 1 was silently ignored, not a real bad node. Final fixtures bound non-listening ports and checked actual bad/good attempts. Original 30-second watchdog/hash/isolation assertions stayed; temporary diagnostics removed. Logs: `out/qa/completion-proxy-ctest-final.log`, `out/qa/completion-proxy-final-repeats2.log`.

This was only stage 1 native proxies. Context/vault, adaptive queues, media/browser UX, and delivery followed. Authenticated media proxies were then explicitly refused pending private stdin. BT HTTPS/UDP proxies, system DNS/public topology/clean systems/browser integration/new long run were unqualified. Frozen 0.2.0 ZIP was neither overwritten nor lent its eight-hour evidence.

## 0.2.0 protocol/UI/deployment candidate

Release `out/build/windows-msvc-protocols/`, Qt 6.11.3/same MSVC. Desktop SHA-256 `3877ce864625bdb0ce2c4ac92bb506dd7d96fc59318cf850e4b15d41f1dc7b46`. Package `out/package/MaskPilot-0.2.0-windows-x64.zip`; project/Windows/client/extension versions all 0.2.0.

| Check | Recorded result |
|---|---|
| Regression | **63/63 CTest**, 240.70 seconds; extension **14/14** |
| Seeding repeats | **3** strict repeats, 66.65 seconds; original 60-second bound/rate/full hash/pause-file preservation |
| Packaged protocols | Actual CLI without development PATH/Qt: FTP/explicit-implicit FTPS **7 groups**, BT metadata/verified download **1/1**, 5.08 seconds |
| Runtime/install | Desktop/CLI/native messages/install/upgrade/uninstall passed without developer environment, unknown files preserved |
| Browser | Isolated Chromium **7** assertions: Cookie isolation/no persistence/durable-before-cancel/fallback/popup; registration restored, `out/qa/browser-1794940249334/evidence.json` |
| Short mixed load | 60-second workload, 61.02 total, 300 hashes/9 pause-resumes/peak 8/2 media cycles/6 outputs; IPC p95 19.92 ms/private drift 0.10 MiB; cleanup passed |
| Native visuals | 1180×750 empty/Chinese tasks, 850×560 narrow/FTP dialog; readable compact navigation/source/progress/upload, unclipped empty hint |
| Dependencies/source | Seven native transport/storage DLLs, Qt/MSVC, full notices/Qt sources/SBOM/hashes; formatting/diff passed, `AGENTS.md` unchanged |

Logs: `out/qa/protocols-02-ctest-final.log`, `out/qa/stress-protocols-02-final.json`. Paused visual samples are marked synthetic and prove layout only; transfers come from actual product/protocol fixtures.

Initial 63-case run had a seed-receiver timeout. Listener worked, then EOF; original seed/receiver shared loopback IP, conflicting with same-IP deduplication. Separate loopback receiver passed three strict repeats/full regression without changing product policy or claiming alerts established every historical disconnect cause. Schema-4 binary-column JSON failure was fixed while retaining all-column/raw DB-journal credential checks. Early package/environment failures were repaired/rerun.

No new eight-hour run/public DHT/public-private tracker/clean Windows VM/multi-DPI/production sandbox qualification. Browser fixtures explicitly disabled sandboxing. BT proxies and original full-feature gaps were still missing at this stage.

## BT/magnet regression stage

Protocol build `out/build/windows-msvc-protocols/`: **62/62 CTest**, 232.98 seconds, including 18 real local BT engine/service/CLI and existing HTTP/FTP-TLS/media/native/Qt cases. Magnet v1/v2/hybrid, tracker-only, global upload, and desktop task-list roundtrip each passed 3 repeats, 82.53 seconds total. Extension **14/14**.

One later native-listener failure case passed **1/1**, 6.19 seconds: clear errors/startup pause-cancel/staging retention, with unchanged functional bytes. Thus 62 full cases plus one separate negative check, not an already executed 63-case suite.

Desktop SHA-256 `7524011b2d6fa4e6a842727d97be5ee60828fbc58971b3f2b215af930757db19`. Old ZIP lacked these additions; UI/new package were later. No new eight-hour/public DHT/tracker/clean-system/production-policy qualification.

Initial failures remained recorded: zero-selected metadata reconnect backoff and incomplete v2 piece-layer serialization were fixed/retested. Different Windows TCP/UDP exclusions caused auto-listener failures; fixtures checked both, product added bounded port recovery/cancellable preparation. File-dialog timeout came from focused editor text; tests now operate actual input/check paths, preserving transfer bounds/full hashes.

Local torrent/private-tracker/passkey storage limits are in the support matrix. Missing BT proxy/mirror/remote features were not counted complete by these passes.

## FTP/FTPS regression stage

Independent Release `out/build/windows-msvc-protocols/`, **36/36 CTest**, 102.59 seconds, extension **14/14**. Real FTP covered anonymous/auth/active-passive/empty-Unicode/missing metadata/bad login/truncation/change/hash/conflict/SHA resume/no-hash restart/REST refusal/forced recovery/explicit-implicit TLS/CA-hostname refusal. Direct adapter checks covered transfer/finalizing pause-cancel and resumed digest.

Desktop checked password fields/paused task; DB/export excluded FTP login. Schema 1/2 backed up before 3. Desktop SHA-256 `2232ac4358f9ac7c0dac92845a3f77924f69f58b1b6440dfc6f3e35eb7bde9b5`. No new eight-hour/package/public FTP matrix; BT/UI remained future work.

## 2026-11-17 source-audit regression

Fixed SQLite NUL truncation, case-variant IDs sharing Windows caches, and media publication ignoring pause/cancel. Preserve full text length for validation; reject non-lowercase IDs without altering valid existing ones.

Release `out/build/windows-msvc-audit/`, desktop SHA-256 `45bbfc1e41d4ca93f520e7b58d209c95e98325284f1a50ee01dfd7d5088ac012`. **30/30 CTest**, 63.85 seconds; UTF-8 media-path repair then **2/2** Chinese-cache checks. Extension **14/14**, isolated Chromium **7** assertions, explicit unsandboxed fixture.

60-second short load: 67.16 total, 300 hashes/9 pause-resumes/peak 8/2 media cycles/6 outputs; IPC p95 14.39 ms/drift 0.55 MiB/cleanup pass, `out/qa/stress-audit-report.json`. **No new eight-hour run/package**; historical frozen ZIP/results below exclude these fixes.

An audit never proves no other defects in every environment. Dynamic splitting/multiple queues/periodic scheduling/recursive crawling were not implemented at this historical point; fixes were not full-feature acceptance.

## Frozen historical candidate

| Check | Recorded evidence |
|---|---|
| Release regression | 25/25 CTest: real create/media list roundtrip/mixed rate/cancel cache/missing-cache assembly cleanup/invalid Unicode-device/corrupt paths/final-report failure |
| Extension | 14/14 Node tests |
| File paths | Actual HTTP fault matrix, Chinese target/DB paths, filesystem errors/login/proxy/conflict/forced recovery |
| Large file | 4,294,967,553 bytes, 62.25 seconds; SHA-256 `c1a2510f41f6fc63bd169116662b1a514c66b43fab60b3dc656d114cfe83145a` |
| Media | Real parser/muxer MP4/HLS/DASH/audio/login refusal |
| Browser | Isolated Chromium 7 assertions: authenticated Cookie/isolation/no persistence/durable-before-cancel/fallback/popup |
| Deployment | Desktop/CLI/native messages without developer PATH; manifest install/upgrade/uninstall preserved unknown/modified files |
| Short preflight | 60 requested/70.35 seconds total, 400 hashes/9 pause-resumes/peak 8/1 media cycle; IPC p95 32.93 ms/drift 1.92 MiB |
| Full stability | 28,809.97 seconds (8 hours 9.97), 2,098 cycles/209,800 hashes/5,674 pause-resumes/210 media cycles/630 outputs; cleanup passed |
| Memory/response | Peak private 19.51 MiB/working set 39.93 MiB/drift 3.50 MiB/IPC p95 24.88 ms/max gap 10.08 seconds, original budgets passed |
| Visuals | Native Windows Qt screenshots inspected; offscreen images not font/DPI evidence |

Desktop SHA-256 `b236e713bf630ca3743af1de2daea3cf49e4492ee06efc7601cd9dc64f86f852`. Report `out/qa/stress-final-report-release.json`, SHA-256 `27a4101c69ff4cf5c8e1ad78589b98536c59d6e9a3b62484ec6ff3ccb1695c2a`. After cleanup, 33 runtime files/81 inputs/helpers matched frozen fingerprints; `AGENTS.md` unchanged.

Earlier candidate access violation occurred around 382 seconds/2,400 hashes, retained in `out/qa/stress-frozen-release.json`. Dump located local request-ID regex matching, but original root cause was unconfirmed. Two 15-minute project ASAN diagnostics processed 5,300/5,400 hashes without bounds/use-after-free reports. Qt 6.11.3/toolset unification removed an independently reproduced dependency defect; final long run did not repeat the crash, without claiming proven original causality.

Early Qt 6.11.3 candidate also failed around 2,193 seconds/14,700 hashes: deletion of 100 completed histories exceeded 10 seconds with 65 DB records remaining. Desktop was alive before cleanup; `out/qa/stress-qt6113-release.json` retained failure. Root cause remained unconfirmed; optimized frozen candidate later passed full load without relaxing 10 seconds.

Isolated 25-round batch removal passed, original worst about 3.21 seconds, without reproducing timeout. Reducing repeated absent-cache deletes yielded another 25 rounds/worst about 2.70 seconds. Not a controlled benchmark or root-cause proof. Stress now captures owned desktop thread dumps after failure/before cleanup; actual dump generation verified, original 10-second assertion retained.

Another run lasted about 3 hours 43 minutes, 95,100 hashes/2,603 pause-resumes/96 media cycles, interrupted by Windows read-sharing during atomic report replacement (`out/qa/stress-cleanup-release.json`). Desktop lived and DB was empty; no observed product failure, but interrupted timing was not eight hours. Fixture sharing conflict was reproduced/fixed; real load plus final-report injection also reproduced/fixed a possible false-pass bug.

A fresh complete long run passed with fixed 100 queue/8 active/8 segments and periodic public-media samples. Budgets: sampled memory <512 MiB/drift <64 MiB/IPC p95 <200 ms/gap ≤30 seconds. Isolated offscreen UI released sleep prevention on completion. Failed/interrupted preflights remain unqualified, never joined.

GUI IPC measures main-event-thread responsiveness, not every visible operation/render. Browser fixtures explicitly disable their sandbox, not proof of production compatibility.

Still unverified externally: clean Windows VM, production browser sandbox/signed store package, release signatures, long-term real sites, online updates. Missing release credentials do not block local runnable candidates, but cannot justify a public-release claim.

## Historical 0.3.0 qualification boundary

The first 0.3.0 long-run attempt was deliberately stopped after 2,411.18 seconds
when release inspection found local compilation paths in runtime binaries. Its
7,400 verified files do not constitute eight-hour acceptance. The failed report
is retained at `out/qa/stress-0.3-long.json`.

Portable compilation, artifact audits and a new unchanged runtime baseline must
pass before a fresh 28,800-second run. Earlier 0.2.0 results above apply only to
that older frozen artifact. Current qualification must identify its own source
commit, runtime hashes, package hash and complete observed duration.

### Portable preflight evidence

The corrected default, explicit and trailing-separator source roots passed six
actual MSVC source/header/Boost diagnostic probes. The portable dependency graph
and applications were rebuilt. Stage and ZIP audits verified 38 runtime files,
basename-only local PDB references, unchanged authenticated supplier DLLs, and
the complete pinned source/patch payload. The frozen preflight records 154
source/policy inputs and three external media tools.

The corrected runtime passed a 307.38-second workload: 800 file hashes, 24
pause/resume operations, eight active transfers and real media processing.
GUI IPC p95 was 27.14 ms and quarter memory drift was 2.65 MiB; process and
fixture cleanup passed. Actual media inventory/container/proxy/Qt checks and
offline HTML/CSS rendering with zero remote contacts also passed. These short
runs do not qualify the required eight-hour release run.

One loaded full-suite run timed out in
`SiteProjectCompletion.PreviewAndCancellationCannotTriggerSuccessfulCompletionCountdown`;
a later loaded repeat failed at another wait in the same test. The cause remains
unconfirmed. Failure-only state diagnostics preserve the original assertions
and five-second waits. Subsequent 30- and 40-run repetitions passed, including
40 runs alongside real download load. The failures remain recorded; these
repetitions do not prove a root cause or justify a guessed production change.
The subsequent complete suite passed 269/269 tests in 626.40 seconds with the
original waiting budgets and new failure diagnostics retained.
