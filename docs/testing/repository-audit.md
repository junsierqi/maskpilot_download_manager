# Repository audit and acceptance

> Historical Windows audit baseline: `d1a4658`. Later Linux implementation/deployment limits are in [Linux acceptance](linux-acceptance.md). Current requirements follow [feature](../product/feature-matrix.md)/[support](../product/support-matrix.md) matrices; historical platform rows below are not current conclusions.

This audit covered the C++20/Qt Windows project and its committed scope. **Increment completion is not the complete original product, nor proof of no undiscovered defects.** Online updates/formal release were deferred; renaming/fingerprint-browser integration remained later scope.

## Confirmed fixes

| Finding | Fix/evidence |
|---|---|
| HLS rollback with overlapping windows could succeed incorrectly | Check sequence monotonicity independently; old program actually published, fixed version refuses/no later segment request. `e5c2662` |
| Empty FLV audio/video tags counted as media | Require nonempty payload; empty/metadata-only never publish, valid media with empty surrounding tags remains playable. Same commit |
| Windows SHA object storage destroyed before native handle | OS-managed storage, hash handle released before provider; 8-thread known vectors/cancel/read-error recovery. `f9c1d58`, API-lifetime correction, not a fabricated guaranteed crash |
| Clean CI lacked mandatory prerequisites | Install pinned Python dependencies before configure; SHA-256-checksum-pinned media archive before extraction in owned runner temp |
| Current support confused with old qualification | Correct login-media/native-live budgets and evidence; frozen long runs never qualify later code |

## Original requirement audit

IDs follow the [blueprint](../DEVELOPMENT_BLUEPRINT.md); detailed limits are in [feature](../product/feature-matrix.md)/[support](../product/support-matrix.md) maps.

| ID | Scope/gaps at this historical audit |
|---|---|
| F-01–03 | Native HTTP/adaptive splitting/verifiable resume, weak identity safely restarts; public compatibility ongoing |
| F-04–05 | Queues/concurrency/rates/priority/categories/history/import preview/retry |
| F-06 | One-time/weekly/daily/conservative rolling budgets/completion; no BT/helper budgets/per-byte metering |
| F-07 | GET interception/link-login recovery; no POST capture/private/partitioned login/store release |
| F-08 | Qt/tray/bilingual/management; themes/columns/multi-DPI/screens incomplete/unverified |
| F-09 | Public same-origin crawl/rewrite/revisions/finite full sync; no dynamic/login/incremental deduplication |
| F-10 | Public/same-origin authenticated media/direct TS-HLS/HTTP-FLV; complex HLS/DASH live/RTMP/page-live not yet implemented at this baseline |
| F-11 | Optional components; no maintained external-site acceptance list |
| F-12–14 | Request/link refresh/hash/no-replace/scan/migration/portable/current-user/offline recovery; online/formal release deferred |
| F-15 | FTP/FTPS and pinned SFTP; no remote browsing/recursive sync/interactive SSH |
| F-16 | Temporary Firefox/containers; signatures/production policy external |
| F-17 | BT/magnets/selection/resume/seeding/TCP proxies; no mirrors/remote/plugins/BT HTTPS/UDP proxies |
| F-18 | Other desktops/headless services not delivered at this baseline |

Explicit gaps are not cancelled user requirements. This audit did not silently become new protocol/platform development.

## Checks and evidence

Independent build `out/build/windows-msvc-audit-20261120`, logs `out/qa/repository-audit-20261120/`. Source review covered domain/application/network/media/BT/storage/Qt/CLI/native bridge/extension/build/package and every shared-hash caller.

Static analysis checked 60 distinct runtime translation units plus the media-probe variant/repaired hash file without build failure. The BT optional-access warning was refuted by control flow: job assignments follow successful storage initialization and pre-initialization exceptions never enter cleanup. This does not prove every path correct.

| Acceptance | Recorded result/log |
|---|---|
| Native regression | **322/322 passed, 951.10 seconds**, `full-ctest.log`; actual HTTP/FTP/FTPS/SFTP/POST/live/BT/media/site/Qt/native/registration paths |
| Extension | **48/48**, `npm-test.log` |
| Release counterexamples | Paths **15/15**, manifest **12/12**, `release-paths.log`/`release-manifest.log`; controlled examples, not actual package qualification |
| Offline recovery | **15 fault/recovery groups** plus suite acceptance, `package-update.log`; real locks/termination and simulated package faults, not old-database compatibility |
| Chromium | **10 groups**, registration restored, `chromium/evidence.json`; explicit no-sandbox testing, not production policy |
| Firefox | **12 groups**, independent `firefox/` evidence, temporary extension/isolated profile, registration restored |
| Media/Qt | **14 groups**, `media-inventory-r2.log`, actual IDs/containers/auth proxies/private input/scan/Qt output |
| Native hash ASAN | Instrumented test framework, **4/4 strict ASAN**, Checksums in `asan-instrumented-acceptance.log` |
| Limited address checks | **51/51, 58.24 seconds**, `asan-address-only-acceptance.log`, explicit `alloc_dealloc_mismatch=0`; not full strict ASAN |
| CI prerequisites | Clean Python reproduced missing deps then installed; corrupt archives refused before extraction, `ci-prerequisite-checks.json`; hosted CI unexecuted |

Expanded strict ASAN reported allocation-type mismatches in 42 of 51 cases at uninstrumented Qt-returned standard-library boundaries; all 42 allocation stacks originated in QtCore. The initial uninstrumented test framework had the same issue; the new instrumented framework replaced only the owned ASAN copy, retaining original dependencies. Complete allocation checks remain unqualified; failures are preserved, not hidden, and vendor/system instrumentation is not claimed.

The local CI-media download timed out at 180 seconds after about 11.9 MB; the incomplete archive was neither verified nor extracted. Workflow now uses a bounded 900-second download/pinned hash/owned temp/fail-fast processing, source-reviewed but awaiting hosted download-chain validation. Functional checks used trusted local 8.1.1 tools rather than public downloads.

Extra media checks first completed 13 groups then timed out at Qt launch because manual invocation omitted plugin paths for copied runtime libraries. With owned `QT_PLUGIN_PATH`, independent Qt and all 14 groups passed, without longer assertions/product edits. Original `media-inventory.log` is retained as failure.

Unexecuted clean Windows/production browser sandbox/store-release signatures/fingerprint integration/public environments/new eight-hour load are not passes. Review was same-agent, not independent human/multi-agent review.

## Portable delivery recheck

A new package used `out/package/repository-audit-local-20261120-r1/`, preserving old output. Packaging required a clean workspace and checked actual Release binaries/licenses/sources/SDK provenance/paths/PE/full ZIP manifest. `release.json` records source commit/dirty state; external ZIP hash is adjacent `.sha256`.

Acceptance used `scripts/smoke.ps1 -PackageDirectory <new-package-directory> -PackageArchive <new-ZIP>`; `portable-smoke.log`/`package.log` record actual status. Removing developer PATH/Qt variables, it checks desktop/CLI/bridge/current-user install/reinstall/offline apply/rollback/finalize/uninstall and preserves unknown downloads. Local delivery cannot substitute for new-version long-run/clean-system qualification.

## Protection boundaries

`AGENTS.md` stayed unchanged and the adjacent browser project read-only. Old packages/builds/dependencies/user programs/downloads were retained; new outputs were isolated. Existing file inventories/hashes were rechecked. Owned local services/data only, no real accounts or actual shutdown.
