# Support matrix

## Platform qualification

| Platform | Current scope |
|---|---|
| Windows x64 | Native implementation, portable/current-user installation and offline recovery; current regressions and frozen-release qualification are separate |
| Linux x64 | Native C++/Qt/engines/vault/native messaging; Ubuntu 26.04 userspace, kernel 6.9+ for complete helper protection; see [Linux](../testing/linux-acceptance.md) |
| macOS 15 arm64/x86_64 | All native targets build and 320 core/platform cases pass per architecture; see [recorded CI](../testing/scenarios.md#hosted-ci-snapshot). Manual visual/deployment, optional media tools, real browser installation, and formal release remain separate |
| Other environments | Ubuntu 24.04 native userspace, other distributions/architectures, physical GPU/Wayland/screens, production browser policy, and other macOS versions remain unproven; no headless service/formal release |

Linux checks extension logic and real native-message/desktop/download paths, not completed real-browser installation, store release, or enterprise-policy qualification. Read capabilities below with platform limits.

Live uses an independent native HTTP(S) engine for direct HLS/HTTP-FLV, producing `.ts`/`.flv`. Ordinary TS/FLV/non-DRM AES-128 TS need no helper; fMP4/aligned separate audio/discontinuities use optional trusted local packaging. Bounds are 1 second–48 hours and 1 MiB–1 TiB, default 1 hour/1 GiB. Source end/limits/explicit finish save complete content after hash/scan/no-replace publication. HLS starts at up to the last three segments and refreshes/deduplicates by sequence. Missing segments, rollback, unsupported tags, and transfer errors do not publish.

Pause/cancel never publish; resume/restart records current live anew. Exact-origin memory login, selected proxy, dynamic rates, and native budgets apply; exhaustion waits without refunding consumed bytes. Metadata is excluded but received incomplete media units count. Default redirects/lists/segments/keys remain initial-origin; Media access can approve up to 16 extras without credential inheritance. AES-128 CBC/rotation/explicit or sequence IV, exact 206 ranges, maps/fMP4/discontinuities and sequence/duration/discontinuity/PROGRAM-DATE-TIME-aligned audio work. Low-latency uses complete segments, not hints/partials; delta/skip, DRM/SAMPLE-AES/non-identity keys/unaligned audio fail. No DASH live/RTMP/webpage-live extraction. HLS caps each track at 8192 complete segments then finishes; local packaging caps at 5 minutes/1 MiB diagnostics. Final size still applies: refuse, never truncate.

POST through HTTP/HTTPS proxies authenticates CONNECT before submission and requires source-port permission. SOCKS5 keeps proxy-side DNS/authentication. No tunnel/auth failure falls back direct.

Standalone desktop/CLI HTTP(S) POST supports confirmed single attempts, UTF-8/already encoded bodies up to 1 MiB, custom types, and explicit empty bodies. Body/confirmation are memory-only; databases/exports retain method/type. Pause/error/restart requires new material/approval. No POST probe/segmentation/resume/automatic retry; same-origin 301/302/303 become bodyless GET, while 307/308/cross-origin fail. Size/hash/scan/no-replace gates remain. Browser bodies/context/site/media POST and undoing server effects are unsupported.

Master supports bounded public static projects: create, page/CSS preview, explicit resource start, pause/resume, verified Open, immutable updates, and file-preserving history removal. Same-origin HTTP(S) only, with depth/count/body/output/path/type limits. Unknown encodings, SVG/XHTML, and unsupported active CSS are refused/removed. Images/fonts/media require supported types/signatures; signatures do not replace decoding/scanning. No dynamic-app/login-session reconstruction.

| Capability | Behavior and limits |
|---|---|
| Windows target | Windows 10 1809+/Windows 11 x64, Qt Widgets; tested host 10.0.26300.0. Windows 10/clean systems unqualified; no release packages for other systems |
| HTTP files | Initial/dynamic tail extents, at most 8 active connections/256 durable segments, single-stream fallback/unknown length/empty files. Splitting needs strong ETag/known length/valid Range/large enough remainder; TLS checks on |
| FTP/FTPS | Single-stream adapter, anonymous/account, active/passive, explicit/implicit TLS/private CA; hostname/certificate verification, no downgrade, memory-only login |
| FTP resume | REST needs expected SHA-256 and matching URL/size/modification/TLS, with final hash; otherwise/refused REST restart. SIZE/MDTM are not strong identity |
| SFTP | Shared native remote-file cache/hash/scan/publish, trusted SHA-256 host pin before explicit password/local-key auth, encrypted keys/Unicode paths. No agents/ambient keys/interactive auth/upload/remote browsing. Login/passphrase memory-only; method/pin/key path public |
| SFTP resume/route | Expected file SHA-256 plus URL/pin/size/modification/cache match and final hash, otherwise restart; native budget/scan/no-overwrite. HTTP/HTTPS proxies need SSH-port CONNECT; SOCKS5 remote DNS, no direct fallback |
| BT/magnets | Independent shared sessions; local v1/v2/hybrid/magnets/tracker metadata/selection/piece checks/pause/resume/seeding/upload stats/task-global limits. Metadata ≤16 MiB, files ≤20,000; initial peer/tracker/web-seed lists each ≤256. Loopback does not prove public DHT/all seeds |
| BT files/privacy | Owned new directories/no overwrite, history deletion or seed cancellation preserves published content. Metadata persists/exports and may contain private tracker/passkeys; local capture rejects network/device/reparse paths |
| Recovery | Strong ETag/length/actual URL/full layout verification, atomic layout/synced per-segment offsets and legacy layout compatibility. Weak identity/corruption restarts; crashes restore paused |
| Login/proxy | File Cookie/custom Authorization/headers and HTTP/HTTPS/SOCKS5 with separate credentials. Memory-only by default; explicit current-user native proxy saving, origin login not saved. Empty is direct, explicit route overrides environment bypass, SOCKS5 target DNS remote; no interactive origin login |
| BT proxy | Isolated HTTP CONNECT/SOCKS5 TCP sessions by route/credentials; no DHT/local discovery/uTP/automatic mapping/UDP trackers/HTTPS proxies. Native retries/alternates retained; never bypass direct |
| Proxy recovery | Public endpoint/login-required flag persist; unsaved/imported credentials need reentry. Flag does not force server challenges. Proxy host uses system/native DNS; extra five-second reachability probe is numeric-IP only. Active FTP plus proxy rejected |
| Scheduling | Global/per-queue 1–8 active, queue rotation/priority, task/queue/global rates, one-time and explicitly zoned weekly windows. Lower concurrency drains active tasks; helper startup rate fixed, conflicting reductions need pause |
| Completion | Notify/exit/shutdown; last two use a 60-second cancellable countdown revoked by new work. Only explicit action values accepted |
| Browser | Chrome/Edge MV3 and Firefox 150+ manifests/native messaging/send/discovery/opt-in interception/panel, standalone only. Interception/recovery require matching observed GET URL/time/container; POST/unknown/conflict/private stay in-browser. At most 256 memory observations/ten minutes, no bodies/headers; restart may need new records. Developer/temporary load, no signed store package; failed interception restores browser transfer and reports restoration failure |
| Browser recovery | Explicit original unfinished standalone HTTP plus session/container record, refresh/save acknowledgement before duplicate cancellation/token resume, retaining ID/path. Manual URLs stay paused. Ten-minute memory session, restart anew; no POST/private/partitioned/media/site/context recovery |
| Completion scan | Trusted Qt/CLI tool checks HTTP/FTP/SFTP/media staging and BT private directories before publication. Refusal/timeout/crash/mutation blocks completion. Durable requirements/results, UI details/waiver and missing-tool repair. Default off, no bundled antivirus/efficacy/network isolation guarantee, no retroactive published-BT scan |
| Browser login | Selected URL/trusted tab or download container; ordinary/Firefox containers tested. Private/partitioned/first-party-isolated login refused, no default-container fallback; another partition's matching Cookie also refuses |
| Batch | Text/static resource links/numeric templates/deduplication/bounds; stable per-batch request IDs retain accepted items/retry leftovers. Not a complete recursive crawler |
| Media | Public MP4/HLS/DASH on-demand, async actual-format inspection/exact audio/video IDs/container validation and automatic quality caps. MKV/MP4/WebM/M4A/MP3/Opus/FLAC depend on source/tools; resume redownloads |
| Webpage media | Optional public-URL parser, no universal-site or maintained external compatibility list. Missing components/refusal/extraction failures are explicit |
| Media login | Standalone memory Cookie/Authorization/custom headers; initial-origin default. Explicit up to 16 extra HTTP(S) origins with independent login/anonymous access. Match protocol/host/effective port on requests/redirects; no inherited secrets. Only public origin/login requirements persist; restart/import/link edits need approval/material. Explicit mode GET/HEAD only; other protocols/POST/unapproved origins refused, browser/context grants never expand automatically |
| Media safety | Trusted helpers only, private secret input not database/arguments; bounded gateway retains proxy, HTTPS encrypted/certificate-verified. Not a hostile-code OS sandbox. On-demand helper does not record live, bypass DRM/paywalls, or install Node if absent |
| Media proxy/local processing | Authenticated HTTP/SOCKS5 and remote origin DNS, private stdin credentials. Unified FFmpeg/ffprobe guard admits ordinary local containers in owned directories only, no network fallback; missing guard fails. Helpers must be trusted |
| Publication | Same-volume staging, optional SHA-256/no same-name overwrite; record deletion preserves files |
| Data | Versioned SQLite/upgrade backups, unknown-future-version refusal; validated imports create paused tasks |
| UI | Qt Widgets, English default and saved English/Simplified Chinese, protocol/category filters, filename/source/download/upload. Narrow windows hide secondary size/category/remaining columns; details retain all information |
| Updates | Manual new packages preserve data; no published online-update service/signing certificate/store entry |

macOS native dependencies/adapters and Apple Silicon/Intel core paths pass the [recorded native CI](../testing/scenarios.md#hosted-ci-snapshot). This entry covers selected automated paths, not universal platform compatibility. See [macOS](../testing/macos-acceptance.md) for remaining qualification.

Future scope includes mirrors, remote control, plugins, formal macOS release, and unqualified systems/architectures. Basic BT is not every advanced setting of a professional torrent client; public-network/firewall/private-tracker compatibility needs separate acceptance.

Weekly windows select weekdays/local times in explicit IANA zones and support midnight crossing. Closed windows retain running intent; reopening never starts manually paused/completed tasks. One window per queue is not recurring task creation or project synchronization.

Native budgets cover HTTP/FTP/SFTP/live only, with UTC-day or conservative 1–168-hour rolling windows. Admitted probes/retries count; overhead/cache do not. Connections share durable reservations; active rolling reservations never expire early, settled payload remains a window from attempt end, and crash uncertainty a window from recovery. This is not per-byte timestamp metering, but cannot release potentially used credits early. UI distinguishes confirmed/charged/reserved; midnight does not reset and durable clock high-water prevents rollback credit. Strict queues refuse BT/magnets/helper media, ordinary queues allow them; no ISP/BT/on-demand traffic guarantee.

Native HTTP `maxStoredBytes` (0 unlimited) and `sameOriginRedirectOnly` are task policies. Site children use positive caps/same-origin. Caps check probed length/writes/prepublication; redirects check initial protocol/host/effective port before connection, allowing valid relative hops. Strict grants still require exact URLs. Current response observations are stored and cleared before failed retry; overhead remains outside these policies.

Projects support manual update and explicit post-success periodic full sync: 5–10080 minutes, 1–100 runs, missed cycles coalesce, old revisions remain. App-open only; pause/failure disables cycles, interrupted revisions reopen paused. Tools configures completion scan after pausing active work; disabling it never waives existing tasks. Offline ZIP update uses independent trusted hashes, interruption recovery/file rollback/explicit cleanup; online discovery/signing/store are separate. Formats describe only current URL/route/tools and are revalidated, never universal site support. See [remaining gaps](feature-matrix.md).

Explicit MP4/WebM requires reported compatible codec metadata. Unknown-codec direct links refuse that choice; use default/MKV or a source reporting codecs. Never guess codecs or silently change format.

Minimum Windows targets follow [Qt 6.11 Windows support](https://doc.qt.io/qt-6.11/windows.html); dependency support is not product qualification.

Native messaging is current-user local IPC with startup tokens/source checks, not a remote listener. Tokens do not isolate malicious software running as the same Windows user.

## Context and credential development boundaries

The application supplies immutable task ownership, context-local deduplication, route snapshots, short-lived exact-URL grants, and revocation exit barriers. Desktop v2 broker/admin, v1 standalone, and ordinary context sessions are separate. Native vault/failure recovery is tested and saving is opt-in. Profile/vault/filter/create/rebind/revoke/reactivate controls exist; profile creation is HTTP/HTTPS/magnets, while advanced media/FTP is standalone. Existing extensions remain v1; extension identity is not fingerprint-profile identity. Browser-side issuance is integrated separately within its own project boundary.

Authenticated native HTTP retains source-login requirements; exact-URL-changing redirects need new grants and cannot send credentials or downgrade anonymously. FTP/SFTP requirements survive restart without saving passwords. Standalone media/live allow same-origin memory login; BT rejects origin-auth flags. Missing material fails before connection. Standalone users can explicitly choose anonymous login except SFTP always requires explicit credentials. Task-list v2 retains public configuration/requirements but restores no identity/authority; see [format](../protocols/task-list.md).
