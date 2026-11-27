# Testing scenarios

## macOS native core entry

GitHub `macOS` builds all arm64/x86_64 targets and runs selected core/platform cases, extension units, and a six-case prerequisite gate. See [current-runtime acceptance](current-acceptance.md) for the latest verified counts; the snapshot below is historical. `macos_platform_product_path` exercises real keychain/no-replace/local-torrent/process-tree checks. Disposable keychains restore original search/default settings even on failure. `macos_ci_fixture` proves simulated script failure/recovery only. Native visual, clean deployment, optional media tools, and browser installation require separate acceptance; see [macOS](macos-acceptance.md).

## Hosted CI snapshot

Recorded on 2026-11-23. All four native jobs qualify the same code commit; each row links its actual successful run.

| Environment | Commit | Native cases | Extension cases | Run |
|---|---|---:|---:|---|
| Windows x64 | `40aadd9` | 340/340 | 48/48 | [Actions](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45213171865/job/137827120975) |
| Linux x64 | `40aadd9` | 329/329 | 48/48 | [Actions](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45213171975/job/137827122720) |
| macOS 15 arm64 | `40aadd9` | 320/320 | 48/48 | [Actions](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45213172114/job/137827122000) |
| macOS 15 x86_64 | `40aadd9` | 320/320 | 48/48 | [Actions](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45213172114/job/137827121394) |

All four jobs qualify the same application, test, and build inputs at `40aadd9`. Both Mac jobs also pass the six-case prerequisite gate; Linux uses Node 22 under the ordinary build user. Subsequent documentation-only changes do not rebuild or extend this evidence.

These runs do not constitute a new eight-hour load result, clean-machine deployment, native Mac visual acceptance, complete media-site compatibility, or signed public release. Earlier version-specific acceptance remains historical evidence.

## Native Linux checks

Run `sh scripts/verify-linux.sh` as an ordinary user: `linux-gcc-release`, isolated HOME/bus/encrypted keyring, native build/extension/full CTest/real media cases. `linux_package_product_path` checks installed bytes/licenses/internal links/no-overwrite and Qt/native bridge/BT CLI outside build paths. It clears development library overrides, not a substitute for a clean machine.

See [Linux](linux-acceptance.md) for kernel/SDK/runtime/logs/unqualified scope. Old Windows manifests/long runs cannot replace Linux results, and 300 seconds cannot be called eight hours.

## Windows qualification baseline

`python scripts/release_manifest.py create --root . --build-directory <build> --output <new-baseline.json>` creates a no-overwrite local Release manifest covering native source/programs/dependencies/guards/version/environment. Add actual helpers/Qt plugins with repeated `--external name=absolute-path`. `verify --root . --build-directory <build> --manifest <baseline.json>` checks the complete set/hashes. Private manifests include machine paths, stay out of packages, and do not authenticate publishers.

Long runs use `python benchmarks/run.py --desktop <build>/maskpilot.exe --native-host <build>/maskpilot_native_host.exe --build-directory <build> --release-manifest <baseline.json> --duration 28800 --report out/qa/stress-0.3.json`, adding `--media-parser`, `--media-muxer`, and `--media-fixtures` for media. Use `--isolated-runtime` for self-contained qualification runtimes: only product child overrides are removed, retaining Git for source checks. Copied caches are private evidence, never distributed.

Verify baseline before startup/after cleanup; any change fails. Inspect actually mapped desktop DLLs, exempting only Windows system binary directories. First run a 300-second preflight; short/interrupted runs are not eight-hour passes. Independent release-script/docs edits need not alter frozen native inputs; new/changed native inputs fail.

`python tests/system/release_manifest_test.py` covers hash/set/path/cache/version counterexamples, not real load. Final reports must bind the same package bytes. Historical long runs, current regressions, and new long runs are separate.

## Functional regression

Before configure, install `python -m pip install -r tests/requirements.txt` in the same Python environment and verify `ffmpeg -version`/`ffprobe -version`. SFTP/TLS use test dependencies; live tools generate/verify owned fixtures. Optional authenticated media needs trusted parser/muxer: check registration with `ctest --test-dir <build> -N -R authenticated_media_product_path`; unregistered is not passed.

Replace `<build>` with the current independent build directory; old binaries do not qualify new work:

| Increment | Product path and boundaries |
|---|---|
| Explicit-origin authenticated media | `ctest --test-dir <build> -R 'MediaAuthenticated\|MediaOrigins\|authenticated_media_product_path\|media_origins_product_path' --output-on-failure --no-tests=error`: local MP4/HLS/DASH, actual Qt/auth proxies, no persisted secrets, origin/redirect isolation, anonymous origins, zero unapproved connections; not real login sites |
| Explicit POST | `ctest --test-dir <build> -R 'HttpPost\|http_post_product_path' --output-on-failure --no-tests=error`: single request/body/method/cancel/no-retry/files; browser POST bodies stay in-browser |
| SFTP | `ctest --test-dir <build> -R 'Sftp\|sftp_product_path' --output-on-failure --no-tests=error`: real local SSH, bad pin, password/encrypted keys/proxies/resume/restart/budget/scan, no auto-trust |
| Bounded native live | `ctest --test-dir <build> -R 'Live\|live_recording_product_path\|live_compatibility_product_path' --output-on-failure --no-tests=error`: TS-HLS/HTTP-FLV/AES-128/rotation/exact ranges/fMP4/aligned audio/discontinuity local mux/complete fallback/origin login/proxy/time/size/finish/restart/scan/budget; rollback/empty-only media refused, valid padding remains playable |
| Shared hash lifetime | `ctest --test-dir <build> -R '^Checksums\.' --output-on-failure --no-tests=error`: Windows provider/independent vectors/empty/Unicode/1 MiB/8 threads/cancel/read-error recovery |

Historical full-audit evidence and original gaps are in [repository audit](repository-audit.md).

Offline recovery: `powershell -NoProfile -ExecutionPolicy Bypass -File tests/system/package_update_test.ps1 -SourceRoot .`. Owned temp cases cover old/new replacement/removal/rollback/finalization/hash/ZIP paths/case collisions/modified-unknown files/actual locks/stage faults/termination recovery. Internal callbacks are fixture-only; ZIP/locks/atomic replace/recovery use real Windows files. Test payloads do not prove program/database compatibility; actual upgrades are separate.

Also cover failures after 64 KiB writes, exclusive partial cleanup, and preexisting-temp preservation. Real termination after a full checkpoint proves recovery; truly truncated staging proves retained scene/original payload, resuming only after explicit fixture-owned repair. That case is not automatic repair or proof against arbitrary power/disk failures.

`scripts/package.ps1 -SkipBuild -BuildDirectory <build> -OutputDirectory out/package/preflight-03 -QualificationManifest out/qa/qualification-0.3-long.json -QualificationRuntimeDirectory out/qualification/MaskPilot-0.3.0-runtime` verifies frozen qualification and creates independent candidate/ZIP/external `.sha256`. Existing outputs refuse; retries need a fresh output parent. Private baselines are excluded.

`scripts/smoke.ps1 -PackageDirectory <extracted-directory> -PackageArchive <ZIP>` removes development PATH/Qt overrides, runs actual desktop/CLI/bridge, checks libraries/plugins/version/licenses, and tests install/reinstall/offline update/rollback/finalization/uninstall. Downloads survive, owned processes/directories are cleaned. Smoke/regression cannot replace current-version long runs.

`python tests/integration/site_project_test.py --exercise <cli.exe>` covers create/no contact, HTML/CSS preview/plan, same-origin/path/cycle bounds, explicit start, no-replace, updates, old revision after failure/cancel, forced exit/resume. Set `MASKPILOT_TEST_BROWSER=<chromium.exe>` and `--browser tests/system/site_project_test.mjs` to check published files/image decoding/CSS imports/round-trip navigation/zero-origin requests/no active scripts after shutting down the source.

`ctest --test-dir <build> -R 'SiteProject|ProjectTask|site_project_' --output-on-failure --no-tests=error` includes SQLite 12→13/atomic manifests/receipts/real Windows junction refusal/Qt controls. Owned clocks and real curl drive windows/daily budgets, never system time. Save faults/old callbacks/exact interleavings are fixture-only supplements. `maskpilot_site_project_system_tests.exe --screenshot <absolute-path.png>` captures the view; `MASKPILOT_TEST_NATIVE_UI=1` uses native windows. No real shutdown.

`python tests/integration/response_metadata_test.py --exercise <cli.exe>` covers extensionless HTML/CSS/charset/final URLs/probe-body conflicts/duplicate-unsafe-missing headers/empty resources/8 ranges/dynamic splits/full-cache recovery. `ctest --test-dir <build> -R 'SiteContent|ResponseContentType' --output-on-failure --no-tests=error` covers SQLite 11→12/current-old events/HTML-CSS escaping/caller errors/output amplification/type-signature mismatch.

`node tests/system/site_content_test.mjs --build-directory <build> --browser <chromium.exe>` checks production parsing in real `file://` browser after origin closure: local images/imports/anchors, zero page network, no active code. Owned outputs/screenshots/`evidence.json` remain, without real browser/registry edits. This foundation does not replace full project/update state-machine acceptance.

`python tests/integration/transfer_constraints_test.py --exercise <cli.exe>` uses actual servers for known/chunked/unknown length, 8 ranges, crash/reduced-cap recovery, no-replace, relative redirect final URL, and zero foreign TCP connections; proxy default-port/case/IPv6 origin equivalence is checked. Probes may receive/count payload. CTest `TransferConstraints|RequestConstraints|ResponseMetadata|TransferConstraintStorage` adds hidden Qt fields/strict lists/SQL 10→11/observation lifetime; coordinator stubs are fixture-only.

Run `node tests/system/browser_test.mjs --build-directory <build> --browser <chromium.exe>` and `python tests/system/firefox_test.py --build-directory <build> --extension browser_extension/dist-firefox --browser <firefox.exe> --evidence-directory out/qa/firefox`. Real isolated extensions/desktops cover durable pause acknowledgement/hash/interception/fallback/containers/panel. Trusted clicks and page DOM/synthetic tampering are distinct. Firefox private/partition refusal retains browser files. Temporary loading is not signed release.

`tests/system/browser_registration_test.ps1` checks original types/repeat registration/external changes/journal failure; CTest marks it serial. Three fixed HKCU defaults restore in `finally`. Never run alongside other real-browser fixtures: restoration journals could interleave. `npm --prefix browser_extension test` negative state/Cookie modules do not replace real-browser checks.

Ordinary regression uses `scripts/verify.ps1 -Configuration Release` and `npm.cmd --prefix browser_extension test`. Scheduler stubs are fixture-only; local HTTP/native host/real components/browser tests execute actual product binaries.

| Scenario | Command/evidence |
|---|---|
| Native file faults | CTest `local_http_product_path`: Range/fallback/empty-unknown length/identity/redirect credential stripping/proxy/login/retry/conflicts/hash refusal |
| Adaptive extents | `adaptive_http_product_path`: uneven local transfers, real new tail split, 2/8 connection bounds, forced exit after layout commit then one-stream resume, old offsets/corrupt layout/Windows replace failure. Distinct blocks/hashes catch reordered assembly |
| Named queues | `QueuePolicy.*`, `QueueStorage.*`, `QueueUi.*`: rotation/priority/concurrency/rates/media reservations/manual-policy intent/retries/schedules, actual SQLite 6→7/version checks/transactional moves, Qt and TCP payload checks |
| FTP/FTPS | `ftp_product_path` or `python tests/support/ftp_fixture.py --exercise <cli.exe>`: real anonymous/login/active-passive/explicit-implicit TLS/CA-hostname refusal/hash/change/REST fallback/forced recovery; controlled local scope |
| Native proxy | `proxy_product_path` or `python tests/integration/proxy_test.py --exercise <cli.exe>`: actual CONNECT/SOCKS5/auth/remote DNS/environment contamination/bad-missing credentials/no direct fallback/FTPS modes/forced-restart flags |
| BT proxy isolation | `TorrentProxy.*`: actual metadata/pieces through proxy, concurrent direct/different endpoints/same endpoint different credentials; bad peers/trackers do not poison alternates or other jobs |
| BT/magnets | `ctest --test-dir <build-dir> -R TorrentEngine --output-on-failure`: real local seed/v1-v2-hybrid/magnets/tracker-only/selection/resume/concurrency/seeding/rates/publication/CLI/SQLite, public discovery off |
| Forced recovery | Kill actual CLI, read durable offset, resume/check full file; not merely extent arithmetic |
| Large file | `python tests/support/http_fixture.py --exercise out/build/windows-msvc-release/maskpilot_cli.exe --large`: actual 4 GiB+257 bytes/SHA-256 |
| Native messaging | `native_bridge_product_path`: source/size/durable ACK/resume/remove preserving final files |
| Media | `python tests/integration/native_host_test.py <host.exe> <desktop.exe> --media-parser <parser.exe> --media-muxer <ffmpeg.exe>`: MP4/HLS/DASH/audio/login-header refusal |
| Media formats/proxy | `python tests/integration/media_inventory_test.py --exercise <cli.exe> --parser <parser.exe> --muxer <ffmpeg.exe> --ui <build/tests/maskpilot_media_system_tests.exe>`: owned multi-quality DASH/streams/containers/auth HTTP-SOCKS5/remote DNS/no bypass/private input/Qt. Build requires `media_guard/` |
| Browser installation | Node ≥22, `node tests/system/browser_test.mjs --browser <test-chromium.exe> --build-directory <build-or-package-dir>`, isolated profile/owned registration backup-restoration |
| Independent deployment | `scripts/smoke.ps1 -PackageDirectory <dir>` clears development PATH and runs desktop/CLI/native messages; not a fresh VM |
| Stability | `python benchmarks/run.py --help` lists fixed budgets; short runs never replace eight hours |
| Stress reporting | `stress_report_publication_failure`: actual Windows read contention/atomic reports and injected report failure at real desktop-load end; failure/diagnostics must survive, in `tests/system/stress_report_test.py` |

Some automated browser environments cannot enable sandboxing. Only explicit `--allow-unsandboxed-fixture` disables the test instance sandbox; this proves no production-sandbox compatibility and is never a product launch option.

Isolate app data/downloads/services. Do not change real downloads/browser profiles or trigger actual shutdown to test countdowns. Native Windows screenshots prove visuals; offscreen control tests do not prove font/DPI correctness.

FTP TLS fixtures generate short-lived certificates without system trust changes. Require Python `cryptography` or PATH OpenSSL; absence fails, never silently skips TLS. These are fixture tools, not product Python/OpenSSL-CLI runtime requirements.

BT seeds normally verify all test data, never skip checking. Probe loopback TCP/UDP together to avoid different Windows exclusions; bind retries stay within original preparation limits. Real file-dialog tests operate filename fields/check resulting paths with bounded diagnostics, not unreliable focused-dialog `selectFile()`.

Prepare owned `source.mp4` under `out/qa/media-fixtures/` or generate the synthetic sample below. Default 300 seconds is a short check; full timing needs `--duration 28800`:

```powershell
New-Item -ItemType Directory -Force out/qa/media-fixtures
ffmpeg -y -f lavfi -i testsrc2=size=160x90:rate=10 -f lavfi -i sine=frequency=440 -t 3 -c:v libx264 -c:a aac out/qa/media-fixtures/source.mp4
python benchmarks/run.py --desktop out/package/MaskPilot-0.1.0-windows-x64/maskpilot.exe --native-host out/package/MaskPilot-0.1.0-windows-x64/maskpilot_native_host.exe --duration 28800 --media-parser out/tools/yt-dlp.exe --media-muxer 'C:/Tools/ffmpeg/bin/ffmpeg.exe'
```

Freeze budgets before running: sampled desktop private/working set <512 MiB, first/last-quarter private drift <64 MiB, GUI IPC p95 <200 ms, observation gap ≤30 seconds. Incremental JSON includes binary hashes/reproduction. Helper/fixture memory is outside desktop budgets; no external-site/TLS/all-visible-UI stress claim. Default offscreen; native visual qualification is separate.

`maskpilot.exe --data-dir <isolated-dir> --window-size 850x560 --screenshot <file.png>` captures Windows-native windows, or the active modal. Sample tasks illustrate layout, not successful transfer.

For package tests, `MASKPILOT_TEST_CLEAN_CLI_ENV=1` clears only product-child PATH/Qt in FTP/actual BT CLI cases; `MASKPILOT_TEST_CLI` points BT at the packaged client. Parent TLS/seeding environment stays intact.

Seeding fixtures use two loopback addresses to distinguish seed/receiver instead of treating same-IP local processes as independent remotes. Keep the 60-second bound, rate limit, and full verification, with listener/connection diagnostics. Browser credential audits inspect all SQLite columns/binary fields and database/journal bytes.

Before release also check clean Windows 10/11, DPI/screens, sleep/wake, constrained/unwritable disks, and real Chrome/Edge policies. Unexecuted items stay in records.

## Context/native-message validation

`ctest --test-dir <build> -R 'PayloadBudget|TrafficStorage|QueueTraffic|QueueUi|native_payload_budget_product_path' --output-on-failure` tests actual CLI/local HTTP1/8 budgets/cache/FTP REST re-receive/forced reservations/no-contact ledger failures/pre-engine magnet refusal. SQLite covers concurrent last-credit/high-water idempotence/one-time refund/history-move retention/UTC rollover/backward clocks. Actual Qt/curl uses owned test clocks for reopening/manual pause/UI usage, never system time. `MASKPILOT_TEST_TRAFFIC_SCREENSHOT=<absolute-path.png>` captures the dialog.

`ctest --test-dir <build> -R 'QueueSchedule|QueuePolicy|QueueStorage|QueueUi' --output-on-failure` checks actual QtCore New York missing/repeated DST minutes and Shanghai date changes with owned scheduler clocks. Qt saves/reopens weekday/time/zone; real HTTP preserves cache across closed windows and verifies full output after reopening, without contacting manually paused sources. Also schema7→8 backup/CAS/first-worker limits. Work stubs prove scheduling only, not transport/grants.

`ctest --test-dir <build> -R scoped_native_bridge_product_path --output-on-failure --no-tests=error` runs real desktop/host: separate admin/two sessions/deduplication/cross-context refusal/v1 isolation/revoke-reissue-restart/real proxy-Cookie routing. Large histories are seeded only while the owned desktop is stopped, not by another runtime DB writer. It checks no anonymous v1 restart/no contact after exact-URL change/rebound optional grants still required. Browser-side issuance is not inferred.

`ctest --test-dir <build> -R 'ContextUi\.|TaskList|DesktopFlow\.|DownloadContexts\.' --output-on-failure --no-tests=error` covers real profile/filter/create/rebind/revoke/reactivate/cleanup, standalone native remember/forget/reopened requirements, import cancel/save failure/retry. Random installation namespaces clean only owned entries. Default offscreen; `MASKPILOT_TEST_NATIVE_UI=1` uses native Windows, `MASKPILOT_TEST_CONTEXT_SCREENSHOT=<absolute-path.png>` captures profiles.

`ctest --test-dir <build> -R '^Queue' --output-on-failure --no-tests=error` checks queues. Native captures: `MASKPILOT_TEST_NATIVE_UI=1`, `MASKPILOT_TEST_QUEUE_SCREENSHOT=<absolute-path.png>`, `QueueUi.ActualManagerAddFilterMoveAndReopenPreservePausedMembership`. Real HTTP verifies enable does not start manual pauses and retains/completes data. Save-order failure uses controlled media stubs, not real-helper regression. Tests never trigger actual exit/shutdown.

## Desktop language preferences

Run `ctest --test-dir <build> -R '^LanguageUi\.' --output-on-failure --no-tests=error`
to check fresh English defaults, cancelling settings without creating a preference,
and saving/reopening both English and Simplified Chinese through the real settings dialog.
The fixture uses an isolated data directory and a Chinese process locale; it does not
simulate changing the operating system locale. Native startup screenshots separately
check unset and saved-language preferences. Existing user settings must not be reset.

## Portable build verification

Run `scripts/probe_portable_compiler.ps1` in the initialized MSVC environment.
It compiles and runs actual source/header paths with spaces and both slash styles,
using default, explicit and trailing-separator source roots,
checks `__FILE__` and `std::source_location`, and exercises Boost diagnostics when
headers are available. Ignored compiler options fail the probe.

Run `python tests/system/release_paths_test.py` for deterministic PE/path/archive
counterexamples and `python tests/system/release_manifest_test.py` for baseline
drift checks. Synthetic binaries and mocked signature/version seams are fixture
evidence only. Packaging independently audits actual runtime files and the final
ZIP against the selected Qt SDK and Microsoft CRT source directory. Qualification
hashes include CMake presets, overlay patches and audit/build policy inputs.

## Native ED2K

`Ed2k*` native cases cover bounded links/frames/hashsets, independent hash vectors, checkpoint ownership and restart rehash, schema-19 to 20 upgrade/backup, strict endpoint persistence, task-list roundtrip and actual new-download controls. `ed2k_peer_product_path` drives the compiled transport against owned loopback peers/indexes, including queueing, malformed/corrupt failover, compression, pause/reopen, large-offset requests, quota/rate, proxy DNS/failure and scan/no-clobber checks. `ed2k_product_path` drives the public CLI and actual unified service, including two concurrent files through the shared factory and persistent quota exhaustion.

An isolated independent peer also served exact 3-byte, 9,728,001-byte and 19,456,001-byte legal payloads. The last case paused after a verified part and resumed in a new process; final bytes and independently produced auxiliary hashes matched. This qualifies the native peer path, not public index availability, WAN throughput, a complete >4-GiB transfer, new cross-platform CI or long-duration operation. Earlier failed soak/IPC evidence remains unresolved and is not replaced by these checks. Long qualification is deferred while implementing ED2K at the user's request.

Local Windows integration verification: the initial full 376-case run passed 370; six old schema fixtures/assertions were corrected for schema 20 and all passed on rerun. Final focused 46/46 passed in 29.53 seconds, including 31 native ED2K cases and both ED2K product fixtures; the new immutable-identity regression brings discovery to 377. This is combined run/rerun evidence, not a new single 377-case pass. Normal/narrow new-download controls were also inspected with the actual Windows platform and product stylesheet. The macOS script simulator passed 18/18; native macOS ED2K and new hosted runs remain unexecuted here.

### CI repair regressions

Desktop startup now stages private token files early and commits them only after storage, IPC listening and callbacks are ready. The actual-startup fixture holds SQLite exclusively to verify that no ordinary token advertises premature readiness, then releases the lock and requires a clean real smoke run. Existing token-failure/no-database, future-schema byte-preservation and final-report failure assertions remain unchanged; the protected stress harness is not modified.

ED2K retains its original 25-second dual-file product test, complete payload checks and exclusive endpoint/identity assertions. Busy reads yield in bounded turns rather than sleeping after each receive; idle and throttled waits still sleep. Verified-part notifications preserve pause/restart control independent of the periodic UI timer. The fixture reports phase elapsed time and bounded task state/progress on failure, without printing request URLs or credentials. Hosted success requires all four ordinary native jobs on the same repair commit; local passes alone do not qualify macOS.

Native bridge acceptance also withholds the real running desktop's token until after the listener is available, requires an authenticated response after publication, and rejects a malformed token. Publication and connection share the existing 9.5-second host budget. Scanner startup requires both persisted scan policy and the ready token within its existing 20-second deadline, preserving missing-tool repairability and no-download assertions.
