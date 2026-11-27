# Feature and acceptance mapping

IDs follow the development blueprint. An implementation does not qualify every platform/environment; release acceptance records external/continuing checks.

## Current coverage

The candidate covers core downloading but **does not complete the original full product goal**. An entry point alone cannot mark a blueprint item complete. Remaining work below requires implementation/independent acceptance, not an assumption that the user cancelled it.

| Capability | Implemented | Remaining |
|---|---|---|
| Segmentation | Initial and idle-connection tail splitting, up to 8 active connections/256 durable extents, single-stream fallback | Public-server throughput compatibility and current-version long runs |
| Queues/schedules | Named queues, concurrency/rates, manual versus policy waiting, fairness/priority, one-time start/stop, zoned weekly windows, UTC-day/conservative rolling HTTP/FTP/SFTP/ED2K/native-live budgets | BT/on-demand budgets, per-byte rolling timestamps, ISP traffic limits |
| Batches/sites | Text/templates/page discovery, confirmation/retry; bounded public same-origin recursion, depth/path/type filters, preview, offline rewrite, immutable revisions, manual/finite periodic full sync | Other-origin scope, dynamic/login reconstruction, incremental deduplication, old-version cleanup |
| Protocol/authentication | HTTP/HTTPS, explicit standalone single-attempt POST, FTP/FTPS, SHA-256-pinned SFTP password/key downloads, BT/magnets, standalone native ED2K with configured index/direct sources, custom headers/cookies, memory-only origin login, optional native proxy vault and profiles; isolated BT HTTP CONNECT/SOCKS5 TCP and explicit-origin media/live login | Interactive origin auth, BT HTTPS/UDP proxies, browser POST-body interception, DASH/RTMP live, DRM, partial segments, unaligned audio |
| ED2K | Native TCP index/source discovery, remote queues, bounded classic/extended/compressed frames, 32/64-bit requests, MD4 parts/file and optional AICH, verified restart, Qt/CLI/store/import-export, shared per-route peer arbitration | Kad, low-ID callbacks, upload/seeding, required encryption, IPv6, HTTP source fallback; public-network and long-run acceptance |
| Browser/media | Chrome/Edge developer and Firefox 150+ temporary extensions; observed-GET interception/recovery, browser-retained POST, optional panel/resources, public and desktop-authorized origin media, actual format/ID/container checks | Store signatures, other browsers, private/partitioned login, POST-body interception, ongoing site qualification |
| Desktop | Categories/search/history/import/export/tray, English-default English/Simplified Chinese, external scan settings/results | Broader toolbar/column/theme customization |
| Completion scan | Trusted optional tool, staged pre-publication checks, durable requirement/results, bounded cancellable process group, Qt/CLI/detail/waiver controls | Disabled by default, no bundled antivirus/efficacy/network sandbox guarantee, no retroactive published-BT scan |
| Delivery | Windows local portable/current-user installation and independently hashed offline update/recovery/rollback; Linux native build/private tests/system-library local package | Online discovery/signing/store/clean-system qualification; no Linux installer/updater; current long runs required before release |

Master supports bounded HLS/HTTP-FLV recording, duration/size limits, finish/save, per-origin login, proxies, budgets, and scanning. Pause/restart records current live content afresh. HLS supports non-DRM AES-128, ranges, fMP4, aligned separate audio, discontinuities, and complete-segment low-latency fallback; complex containers need a trusted local muxer. DRM, partial/delta HLS, unaligned audio, DASH live, RTMP, webpage live extraction, and protected-media bypass are unsupported. macOS 15 arm64/x86_64 native builds and selected core/platform paths pass recorded CI; visual, optional media, browser installation, and formal distribution remain separate; Linux qualification is system/architecture-specific. Validate each version; this is not certification of complete equivalence with another product.

## Implementations and evidence boundaries

| ID | Entry point | Validation and boundary |
|---|---|---|
| F-01 | Native transport/request options | HTTP faults, redirects, credentials/proxies; authentication scope in support matrix |
| F-02 | Multi tail splitting/durable layout/fallback | Real uneven Range, forced exit after layout commit/resume, legacy migration, 64-bit/error boundaries; no universal speedup |
| F-03 | Offset/identity sidecars, SQLite | Forced restart, changed resource identity, no-ETag redownload |
| F-04 | Coordinator queue registry/settings/manage/move/priority | Global/queue/task limits, fairness, media reservations, transactional moves/manual intent; old stress results need current reruns |
| F-05 | History/categories/import/export | Full preview/validation then individual durable saves/retry; secrets excluded, v2 requirement flags retained |
| F-06 (partial) | Named queues, one-time start/stop, zoned weekly windows, UTC-day/1–168-hour conservative native/live budgets, completion actions | Rolling charge from attempt end; active reservations never expire early; BT/helper media require ordinary queues; explicit project-cycle opt-in; manual pauses never auto-resume; real sleep/shutdown not triggered |
| F-07 | MV3/native host/user IPC | Real interception/fallback; production policy/store installation remain external |
| F-07 recovery | Source-page action, trusted recovery page, ten-minute task token/durable receipt | Real Chromium/Firefox default/container Cookie recovery, same ID/path/full hash/no duplicate; cancellation/process stages also use fixtures; no private/partitioned Cookie or POST replay |
| F-08 | Qt windows/details/settings/tray | Actual control flows/native screenshots; multi-DPI/screens separate |
| F-09 | Batch/resources/project coordinator/atomic manifest/content handling/no-replace publish/Qt/CLI | Bounded public page/style/resource work, explicit start, origin/type limits, pause/restart, offline navigation, manual update/explicit finite full sync; app-open only, pause/failure disables cycles |
| F-10 | Independent inspection/private process/shared adapter and native live engine | Actual MP4/HLS/DASH IDs/containers/audio merge; public/explicit-origin login with authenticated HTTP/SOCKS5, guarded local processing, redownload on resume. TS/AES-128/ranges, optional local fMP4/aligned audio/discontinuities, complete low-latency fallback and HTTP-FLV; live resumes afresh |
| F-11 (partial) | Optional parser/component management | Missing/broken tools diagnosed; no maintained site compatibility list |
| F-12 | URL/request refresh/identity checks | Changed URLs cannot mix resource versions; refreshed credentials remain process-local |
| F-13 | SHA/same-volume publication/error state | Bad ranges/digests never publish; no overwrite; full disk-failure environments separate |
| F-14 | Bilingual UI/install/uninstall/offline recovery/rollback/migrations | Upgrade backups; smoke removes development PATH; rollback covers files only; clean systems separate |
| F-15 (partial) | FTP/FTPS adapter/request options | Local FTP/TLS faults; no remote browsing/recursive FTP crawl |
| F-17 (partial) | Independent BT/unified tasks/selection/seeding/isolated TCP proxies | Local v1/v2/hybrid/magnets/tracker-only metadata/resume/rate-limited seeding; mirrors/remote control/plugins/BT HTTPS/UDP proxies missing |
| F-16 (partial) | Firefox 150+ temporary extension/panel/native bridge | Real local browser flows; signatures/production policy external |
| F-18 (partial) | Linux native build/adapters/local package; macOS native build/adapters/core CI | [Linux](../testing/linux-acceptance.md): Ubuntu 26.04 x64 and kernel 6.9+ scope only, not all systems/headless service. [macOS](../testing/macos-acceptance.md) macOS 15 arm64/x86_64 native core CI passes; manual visual/deployment/optional-tool and release qualification remain |

## Context increments

Native credential storage, actor-owned contexts, and v2 broker composition are separate foundations. One coordinator owns identity/deduplication/route snapshots/source grants/revocation; ordinary interfaces expose only current-context summaries. Existing extensions remain v1 standalone clients. Real Qt/native regressions cover vault save/forget, profile management/filtering/creation/rebind/revoke/reactivate/import retry. Successful revocation clears durable references; failures are retryable. Browser-project issuance, multi-DPI, and clean-system acceptance remain separate.
