# MaskPilot Download Manager

A C++20/Qt 6 Widgets download manager for Windows and Linux. HTTP/HTTPS, FTP/FTPS, SFTP, native live recording, media adapters, and an independent BT engine share task scheduling and persistence across desktop, CLI, and browser entry points. See the [Linux guide](docs/testing/linux-acceptance.md) for the tested systems/libraries; support is not universal across distributions.

**Current `master`** contains completed increments, including browser-assisted link/login recovery, periodic site synchronization, rolling native-payload budgets, and optional completion scanning. Daily work uses current `master` and focused verified commits, not divergent long-lived feature branches. Frozen 0.2.0/0.3.0 packages remain separate: their long-run evidence does not qualify current development code. Local validation/candidate packages are the present scope; online updates and formal release are deferred. See [features and gaps](docs/product/feature-matrix.md).

Native macOS configuration, Apple adapters, and dual-architecture GitHub Actions entry points exist, but **actual macOS compilation/runtime qualification remains unverified**. The `macOS` workflow builds all targets, runs core/platform tests, and uploads logs on Apple Silicon and Intel. Failures require fixes/reruns; configuration alone does not prove support. See [macOS acceptance](docs/testing/macos-acceptance.md).

## Reading guide

| Goal | Entry points |
|---|---|
| Usage and support | [Usage](#usage), [protocols and limits](docs/product/support-matrix.md), [features and gaps](docs/product/feature-matrix.md) |
| Build and development | [Source builds](#building-from-source), [current architecture](docs/architecture/overview.md), [blueprint](docs/DEVELOPMENT_BLUEPRINT.md), [contributing](CONTRIBUTING.md) |
| Validation and platforms | [Test scenarios](docs/testing/scenarios.md), [Linux](docs/testing/linux-acceptance.md), [pending macOS qualification](docs/testing/macos-acceptance.md) |

## Language

The desktop defaults to **English**, including on a Chinese-language system when no preference is saved. Choose English or Simplified Chinese in **Settings → Language (restart)**, save, and restart. Existing saved choices remain in effect; upgrades do not reset Chinese preferences. Canonical repository documentation and contribution templates are English. This is two-language desktop support, not a claim that every language or diagnostic is localized.

## Usage

### Live recording

In a new download, choose a direct HLS or HTTP-FLV URL on the live tab. HLS produces `.ts`; HTTP-FLV produces `.flv`. Ordinary TS/FLV and non-DRM AES-128 TS need no helper. fMP4, discontinuities, and aligned separate audio require a trusted local processor under Media access. Defaults: 1 hour/1 GiB; bounds: 1 second–48 hours and 1 MiB–1 TiB. Finish/save an active recording from its context menu, or let the source end/a limit stop it. Only complete segments/FLV tags are saved, without transcoding.

Pause, cancellation, network errors, missing segments, or unsupported formats do not publish a final file. Resume records the current feed afresh, without joining old cache or recovering missed content. HLS starts at up to the last three segments and deduplicates by sequence. Each track retains at most 8192 complete segments, then finishes/saves. MPEG-TS, AES-128 CBC key rotation/explicit or sequence IVs, exact byte ranges, initialization maps/fMP4, discontinuities, and separate audio aligned by sequence/duration/discontinuity/PROGRAM-DATE-TIME are supported. Low-latency HLS falls back to complete segments; partial, delta/skip, DRM, SAMPLE-AES, and unaligned audio are rejected. DASH live, RTMP, and webpage live extraction remain unsupported.

Redirects, playlists, keys, and segments stay on the initial protocol/host/effective port by default. Explicitly authorize additional origins under Media access; each has separate login material and never inherits another origin's credentials. Advanced Cookie/Authorization values remain in memory and need reentry after restart. Selected proxy, dynamic throttling, native-payload budget, final digest, pre-publication scan, and no-replace publication apply. Budget exhaustion causes policy waiting; later recording starts afresh and consumed bytes are not refunded. Metering includes media bytes that never form a final complete unit, excluding playlists/network framing.

```powershell
maskpilot_cli.exe "https://example.test/live/index.m3u8" "D:\Downloads\record.ts" ".\live-data" --live-hls --live-duration 1800 --live-max-bytes 1073741824
maskpilot_cli.exe "https://example.test/live/feed.flv" "D:\Downloads\record.flv" ".\flv-data" --live-flv --source-cookie-env LIVE_COOKIE --source-authorization-env LIVE_AUTH
```

Supply credential variables only as needed; never script real secrets. Run `ctest --test-dir out/build/windows-msvc-protocols -R 'Live|live_recording_product_path' --output-on-failure` for live checks. Real video fixtures need `ffmpeg`/`ffprobe` on the test PATH. Complex HLS is packaged locally; native code performs all network requests. `--live-muxer <absolute-path>` selects the processor; `--live-origins-env MASKPILOT_MEDIA_ACCESS` reads the origin JSON below.

### Files and site downloads

Static site projects handle public same-origin HTTP(S) pages and rewrite local references using libxml2 and a CSS tokenizer. Actual response types determine classification; absent/conflicting/unsafe types are not guessed from filenames. UTF-8, UTF-16, Latin-1/ASCII are supported. Scripts, forms, embedded frames, and external automatic loads are removed and restrictive content-security policy is applied. Dynamic apps, login sessions, and protected content are not reconstructed.

Extract a candidate ZIP and run `maskpilot.exe`, or use `Install.cmd` for current-user installation without administrator privileges. Exit installed programs before upgrading. Manual offline upgrades provide recovery records; `Uninstall.cmd` retains databases, settings, and downloads.

1. Choose New download, enter a URL/destination, and set advanced connections, digest, proxy, headers, task rate, or schedule as needed.
2. Use toolbar/context actions to pause, resume, reprioritize, redownload, or refresh links/login. Removing records preserves final files; cancellation cleans owned cache.
3. Settings control concurrency, global rates, category directories, English/Simplified Chinese, tray behavior, and completion actions. Exit/shutdown has a cancellable countdown.
4. Batch add extracts links or expands numeric templates. Task-list import previews paths/routes/authentication before adding paused independent tasks; failed saves allow retrying leftovers. See the [list format](docs/protocols/task-list.md).

HTTP byte resume requires verifiable strong resource identity and Range support; otherwise restart safely. Idle connections can split a sufficiently large remaining extent, up to 8 active connections and 256 durable extents per task. Small remainders stay unsplit; throughput improvement is not universal. Authentication headers/cookies are process-local. Signed original URLs persist in the local database/explicit exports; protect data directories and exported files.

## Static site projects

Create a static site project from Tools, choosing a public root, output directory, queue, depth, path prefix, and resource types. Creation makes no request. Preview fetches candidate pages/CSS, including stylesheet imports, to plan resources. Recognized images/fonts/media download only after explicit Start. Bounded inspection may identify other types; preview shows skipped reasons.

Children share queues, windows, daily budgets, rates, and recovery and appear in the ordinary task list. Project pause/restart requires explicit resume. Removed/cancelled/edited children are reconciled, not silently recreated. Project proxy passwords remain process-local and need reentry after restart; source cookies/login are not broadcast.

Completed exports are `<selected-directory>/site-<project-id>/r-<revision>/index.html`. Open verifies markers/hashes. Update fetches/previews anew and confirmation publishes a new revision; error/cancellation preserves the previous one. Old revisions, original downloads, and task records remain; deleting project history preserves files. Per-revision stored-body/output caps are not traffic/total-disk quotas.

After the first export, explicitly enable synchronization with 5–10080 minute intervals and 1–100 runs. This authorizes complete automatic revisions within the **saved scope**; other draft changes need a manual Update first. Next time is measured from successful completion; missed cycles coalesce into one and the finite count stops automatically. Old versions are retained even for unchanged sources, so allow disk space.

Synchronization runs only while the app is open. Completed projects retain waiting schedules across closure; interrupted revisions restart paused and need resume. Manual/project-child pause, cancellation, or failure disables cycles. Missing proxy credentials stop with an error, never direct fallback. A backward clock jump restarts the interval. Stop cycle affects future runs, while Pause stops current work. Waiting for a cycle blocks all-complete exit/shutdown.

The local verification client uses `maskpilot_cli.exe --project REQUEST_JSON DATA_DIRECTORY` with `create`, `preview`, `start`, `pause`, `resume`, `cancel`, `update`, `status`, `list`, `open`, and `remove`. Requests are bounded JSON; proxy secrets use named environment variables. UI/CLI share the data-directory lock and cannot concurrently open the same directory.

## Upgrade and recovery

Obtain the ZIP SHA-256 independently from a trusted channel, then use the updater from an already trusted installation or verified extraction:

```powershell
$expectedHash = 'REPLACE_WITH_INDEPENDENTLY_VERIFIED_64_HEX_SHA256'
./scripts/update.ps1 -Action Apply -Package D:/downloads/MaskPilot-0.3.0-windows-x64.zip -PackageSha256 $expectedHash -InstallDirectory D:/Apps/MaskPilot
./scripts/update.ps1 -Action Recover -InstallDirectory D:/Apps/MaskPilot
./scripts/update.ps1 -Action Rollback -InstallDirectory D:/Apps/MaskPilot
./scripts/update.ps1 -Action Finalize -InstallDirectory D:/Apps/MaskPilot
```

`Apply` hashes the ZIP through the same read-only handle, then checks extraction paths, full manifest, and prior ownership. Installed programs must exit first. Unknown conflicts, modifications, or junctions stop and retain evidence. The internal manifest checks consistency; the independently obtained external hash selects the trusted package.

`Recover` restores the old installation after interruption. Successful upgrades retain rollback material: `Finalize` cleans it after acceptance, or `Rollback` restores files. Resolve that record before the next upgrade. Settings, downloads, and old browser registration retain ownership rules.

Verifiable durable checkpoints support automatic recovery. Caught write failures clean only partial files exclusively created by that call. Unverifiable truncated staging/journals from termination/storage errors preserve installation, backup, and evidence for manual inspection, not guessed deletion. Register new browsers separately below.

`Rollback` restores installation files only. Older-program compatibility after migration is separate. Backups are `tasks.db.before-v<version>-<id>.db`; the updater never overwrites task databases or automatically rolls back downloads. Online discovery, signing, and store release remain separate work.

## Proxy and credentials

New/edit requests have a separate Proxy tab; URL-embedded credentials are forbidden. Files support HTTP/HTTPS/SOCKS5, proxy-side target DNS for SOCKS5, and certificate/hostname checks for HTTPS proxies. Empty means explicit direct. Selected proxies cannot be bypassed by environment `NO_PROXY`.

Credentials default to memory only. Explicit Remember proxy credentials uses current-user Windows Credential Manager/unlocked Linux Secret Service with no plaintext fallback. macOS Keychain exists but real runtime qualification is pending. Databases/exports never contain passwords. Unsaved required credentials need reentry after restart/import and block transfer until supplied. Forget clears saved/in-memory username/password but retains the authentication-required flag. Negotiation follows the proxy protocol.

BT uses HTTP CONNECT/SOCKS5 with separate route/credential sessions. Proxied BT uses TCP and disables DHT, local discovery, automatic mapping, and uTP; UDP trackers/HTTPS proxies are unsupported. Proxy hostname resolution may be local; anonymity is not promised. Active FTP cannot use proxies. Authenticated HTTP/SOCKS5 media passes secrets/original URLs through private stdin, not helper arguments.

CLI `--proxy-user-env <name>`/`--proxy-password-env <name>` read process credentials. `--proxy-user`/`--proxy-password` also work but can appear in process inspection or shell history.

## Download queues (development code)

Manage named queues from Tools: create/edit/pause/enable/delete empty queues. Each admits 1–8 active tasks and download/BT-upload rates within global/task limits. Queue/profile filters are independent. New standalone downloads can choose queues; browser/profile tasks use Default. Move paused tasks without changing ownership, route, vault references, or cache. Move/remove records before deleting nonempty queues.

Queue pause preserves manual task intent. Enabling resumes only policy-waiting tasks, not manually paused, failed, or completed tasks. Media rates are reserved at startup; conflicting reductions require pausing, rather than pretending to update active helpers live.

Queues can set weekly weekdays/local start/end and explicit IANA zone, e.g. `Asia/Shanghai`. End at/before start crosses midnight; equal times mean 24 hours from selected dates. Outside windows, running intent waits; inside, schedules/retries/profile authority/manual pause still apply. Missing DST minutes do not run; repeated minutes both follow local windows. Unknown zones block, not system fallback. Windows do not recreate completed downloads; project cycles are separate.

Native-payload budget 0 disables it. Positive budgets allow HTTP/FTP/SFTP/native live. Window 0 means UTC day; 1–168 means conservative rolling hours. Admitted payload includes probes/retries/repeated data, not cached extents/hashing/assembly again. Exhaustion retains running intent for later credit; manual pause never auto-resumes. Pause all active queue tasks before edits. BT/magnet/helper media can use ordinary queues but strict budgets reject them before engine startup; local torrent import needs an ordinary queue.

This is not an ISP traffic meter: TLS/headers/socket buffers/overhead are excluded. UI separates confirmed, charged, and reserved/unverified bytes; confirmation can lag. Reservations persist first; normal completion refunds only unused credit, crashes do not refund unverifiable reservations. Removing history/moving tasks/editing configuration does not erase usage; clock rollback restores no old credit.

Rolling mode continuously counts active reservations. Confirmed payload remains charged for a window from **that attempt's end**; crash reservations remain for a window from recovery. Early bytes in long transfers can therefore expire later than byte-timestamp metering. Midnight does not reset rolling budgets, clock rollback adds no credit, and deleting history changes no usage.

## FTP / FTPS

Enter `ftp://`/`ftps://` and configure login, active mode, and private CA on FTP / FTPS. `ftp://` defaults to cleartext/passive; require explicit TLS for AUTH TLS. `ftps://` uses implicit TLS. Certificates/hostnames are always checked, without cleartext downgrade. Never embed login in URLs. Credentials are not saved; reenter after restart.

FTP uses one data connection. REST resume requires expected SHA-256 and matching URL/size/modification time/TLS mode, with final digest verification. Refused resume or insufficient identity restarts safely. File downloads only: no upload/remote directory browser. Extension interception remains HTTP/HTTPS.

```powershell
maskpilot_cli ftp://example.test/file.zip C:/Downloads/file.zip C:/Downloads/state --ftp-user user --ftp-explicit-tls --sha256 <expected-sha256>
```

Prefer desktop password fields; command arguments may be visible in process listings/history.

## SFTP

Enter `sftp://`, a trusted SHA-256 server fingerprint, and password or explicit local private-key login. Common unpadded fingerprints accept optional `SHA256:`. Pins are checked before sending credentials; unknown hosts are never auto-trusted. Keys must be ordinary local files; encrypted-key passphrases work. No SSH agent, ambient keys, or interactive authentication.

Username/password/key passphrase remain in memory and need request/login editing after restart. Only method, fingerprint, and key path persist, not key contents. Single-stream resume needs expected file SHA-256 plus matching URL/pin/length/modification time/cache and final digest; otherwise restart from zero. Queues, rates, native budgets, and scanning apply. HTTP/HTTPS proxies must permit SSH-port CONNECT; SOCKS5 keeps proxy-side DNS, without direct fallback.

```powershell
$env:SFTP_LOGIN = 'example-user'
$env:SFTP_SECRET = 'example-password'
maskpilot_cli sftp://example.test/files/archive.zip C:/Downloads/archive.zip C:/Downloads/state --sftp-host-key 'SHA256:BASE64_FINGERPRINT' --sftp-user-env SFTP_LOGIN --sftp-password-env SFTP_SECRET
Remove-Item Env:SFTP_LOGIN, Env:SFTP_SECRET
```

Use `--sftp-private-key C:/Keys/download.pem` and optional `--sftp-key-passphrase-env NAME` for keys; `--sha256 DIGEST` permits verified resume. Replace sample fingerprints with independently trusted values. File download only: no remote browsing, upload, or recursive synchronization.

## BT / magnets

Tools opens local `.torrent` files or `magnet:?` links. The BT tab supports all files or metadata-first selection, seeding, and upload rate. Details expose selection/upload/connections; Settings controls global upload. Local real-transfer checks cover v1/v2/hybrid torrents, magnets, and tracker-only metadata. The 0.2.0 candidate includes FTP/FTPS/BT; see [acceptance](docs/releases/acceptance.md).

Each task owns a new destination directory, retaining torrent paths. Verified selected content publishes as a whole, never replacing existing same-name directories. Pause keeps cache; cancel removes owned unpublished cache while preserving final files. Seeding uses an active slot and can be paused/stopped. Resume rechecks pieces.

Metadata persists with tasks; deleting the original torrent does not block recovery. It may contain private tracker URLs/passkeys and **is included in explicit exports**; protect databases/lists. Local torrent sources must be on directly mounted disks, not network shares/devices/reparse paths. Copy them locally first if necessary.

Per-torrent metadata/file bounds are 16 MiB/20,000. Isolated HTTP CONNECT/SOCKS5 TCP routing follows the limits above; browser interception remains HTTP/HTTPS. Public reachability depends on network/OS ports/trackers/seeds; local fixtures do not prove every public torrent works.

```powershell
maskpilot_cli C:/Downloads/example.torrent C:/Downloads/example C:/Downloads/state --select-files 0,2 --upload-limit 65536
```

CLI is a bounded verification client; use desktop for sustained downloading/seeding. Ordinary non-live verification pauses unfinished tasks after 180 seconds; live tasks use recording bounds and finalization.

Native HTTP can cap stored payload and same-origin redirects, for example:

```powershell
maskpilot_cli https://example.test/docs C:/Downloads/page.html C:/Downloads/state --max-stored-bytes 1048576 --same-origin-redirects
```

`--max-stored-bytes 0` is unrestricted; positive values are checked against known total and writes. `--same-origin-redirects` requires original protocol/host/effective port; actual response URL is an observation. These are native-HTTP policies, not traffic/disk quotas. Lower caps preserve cache but block oversized publication. Site projects apply them to ordinary children.
Prefix `--bt-no-discovery` disables DHT/local discovery/automatic mapping; explicit trackers/peers remain enabled. This is not an offline switch.

## POST file downloads

HTTP/HTTPS proxies must allow source-port CONNECT and authenticate before POST. Proxy/tunnel failures never fall back direct; SOCKS5 keeps proxy-side DNS/authentication.

Choose POST on the request tab, enter already encoded form/JSON/other body and content type, and confirm one submission. Body cap is 1 MiB, memory only, never database/export/settings. Source/route/login/body edits revoke approval. CLI reads UTF-8 content from an environment variable:

```powershell
$env:DOWNLOAD_REQUEST = '{"format":"zip"}'
maskpilot_cli https://example.test/export C:/Downloads/export.zip C:/Downloads/state --post-body-env DOWNLOAD_REQUEST --post-content-type application/json
Remove-Item Env:DOWNLOAD_REQUEST
```

`--confirm-empty-post` explicitly submits empty content; `--post` only selects the method. No preliminary probe, segmentation, byte resume, or automatic retry. Same-origin 301/302/303 become bodyless GET; 307/308/cross-origin redirects fail. Pause/error/restart/redownload requires new body/approval: the server may already have processed the request and cancellation cannot undo it. Existing targets fail before submission. Digest/size/scan gates remain. Imports are paused/body-free. Only standalone HTTP(S) files support POST, not browser body interception, media, site projects, or context grants.

## Browser integration

Installation registers current-user Chrome/Edge/Firefox channels. For portable installations:

```powershell
./scripts/register-browser.ps1 -InstallDirectory 'C:/Apps/MaskPilot'
```

Enable Chrome/Edge developer mode and load packaged `browser_extension/`. Firefox 150+ temporarily loads `browser_extension_firefox/manifest.json` through `about:debugging#/runtime/this-firefox`, disappearing on browser restart. No signed store package is published. `npm --prefix browser_extension run build` generates `dist/` and `dist-firefox/`.

Send links by context menu or discover resources through the extension. Auto-interception, page panel, and Cookie forwarding are separately opt-in. Partial batches retain accepted requests; retrying leftovers avoids duplicates. Reload/navigation requires rediscovery.

Interception/recovery requires observed GET with matching URL/time/container. POST/other/conflicting/unknown records stay in-browser, never replayed as GET. `webRequest` observes bounded in-memory method/matching metadata, not bodies/headers, and never modifies requests. At most 256 observations live ten minutes; restart/eviction may require a new browser download. Confirmed recovery can continue/cancel. Private records are not auto-intercepted. Manual ordinary-link sending is explicit GET; configure POST on desktop.

Cookie queries use only selected URL/browser container. Private-window login, partitioned cookies, and first-party isolation are refused, retaining browser downloads rather than using defaults. Another partition's cookie for the same URL also causes refusal. Unregister restores only installation-owned values, preserving external edits. Both extensions use standalone downloads; see [protocol/fallback rules](docs/protocols/browser.md).

For expired/relogin standalone HTTP tasks, pause and start browser link/login recovery. Explicitly select the original desktop task, log in on the opened source tab, and trigger download again. Select the exact session record on the recovery page to preserve ID/destination. Enable Cookie forwarding if needed. Durable update acknowledgement precedes cancelling the browser duplicate, token validation, and resume. Cancel failure keeps desktop paused. Manually supplied links update but remain paused for review.

Recovery lives ten minutes in extension/desktop memory; browser restart requires a new session. Auto-interception pauses meanwhile. Cancelling recovery deletes neither original task nor final file. Only reacquirable HTTP(S) GET files are supported, not form/POST replay or arbitrary authenticated/media/context recovery. HTTP identity checks still decide cache reuse.

## Authenticated media and explicit origins

Standalone media can explicitly supply Cookie/Authorization/custom headers before inspection/selection. Secrets stay process-local and require reentry after restart, without anonymous downgrade. Refresh missing login material before resume.

Authenticated requests default to the initial protocol/host/effective port. Media access can explicitly approve up to 16 extra HTTP(S) origins for standalone on-demand/live tasks, each with independent Cookie/Authorization/headers or anonymous access. Redirects never inherit another origin's credentials. Origins/login requirements persist; secrets/approval do not, requiring review/approval/material after restart/import/URL edits. CLI `--media-origins-env MASKPILOT_MEDIA_ACCESS` (live: `--live-origins-env`) reads JSON such as `[{"origin":"https://cdn.example.com","authenticationRequired":false}]`; names start `MASKPILOT_MEDIA_`. Never script real secrets. Explicit on-demand mode permits GET/HEAD, rejects POST/other protocols/unapproved origins, and uses a bounded temporary gateway preserving proxy routes. HTTPS remains encrypted and helper-validated, without local root installation. HTTP proxies need target-port CONNECT. Ordinary public media retains multi-origin behavior. Browser auto-media does not expand exact-URL Cookie authority into whole-origin grants; configure on desktop. DRM, interactive login, and universal site extraction are unsupported.

With trusted test components, CMake registers `authenticated_media_product_path`/`media_origins_product_path`. Owned MP4/HLS/DASH and HTTP/SOCKS5 services test login, origin/redirect isolation, refusal, and output without real sites.

## Completion scanning (development core)

Optional external scanning checks staged HTTP/FTP/media files and BT directories after transfer/integrity checks, before publication. Off by default, without bundled antivirus. On Windows choose a trusted local command-line `.exe`; pass separate arguments with exactly one complete `{file}`, without shell/URL/login material. Scan actual staging instead of skipping temporary extensions. Exit 0 is the tool's reported pass, not a malware-free guarantee.

Scanner options precede the verification-client command, for example:

```powershell
maskpilot_cli.exe --scan-program 'C:/Tools/scanner.exe' --scan-arg '--check' --scan-arg '{file}' --scan-timeout 120 'https://example.test/file.zip' 'D:/Downloads/file.zip' 'D:/AppData/MaskPilot-test'
```

Timeout is 1–3600 seconds including before/after content checks; pause/cancel terminates the owned group. Rejection/crash/timeout/content mutation prevents completion/publication and retains recovery. Requirements/results persist; missing configuration waits for repair. `--waive-scan --resume TASK_ID DATA_DIRECTORY` records an explicit waiver for that stopped task, not a scan pass. Arguments are not exported.

Configure trusted scanner, one literal argument per line, and timeout through Tools. Exactly one line is `{file}`, without shell quotes; BT supplies a staging directory. Settings reload at startup. Pause active tasks before editing; a missing program remains repairable. Disabling global scanning does not waive prior requirements. Explicit waive/require actions keep stopped tasks paused for manual resume. Lists show progress and details/tooltips show result/UTC time. Exit 0 is not a safety guarantee. Published BT content is not rescanned externally.

Scanning is not retroactive for completed/published BT content. Previously scanned BT can seed after content checks. Unverifiable requirements on published content wait for an explicit decision instead of handing the user's final directory to a scanner again.

## Media

Tools selects trusted media components. Windows can also download checksum-verified parser/muxer binaries; Linux uses native system/user tools. Optional components are not bundled. Inspect actual audio/video formats and select exact IDs/compatible containers, or use automatic quality caps. MKV/MP4/WebM and M4A/MP3/Opus/FLAC depend on sources/components. Source/network edits invalidate inspection; download revalidates and fails explicitly if selections disappear.

Inspection is asynchronous, cancellable, and bounded. The parser uses its native downloader; `media_guard/` limits FFmpeg/ffprobe to ordinary files in owned task directories, denying external-download fallback/disguised remote playlists. This is not a hostile-executable sandbox; tools must be trusted. Pause/resume/retry/rate changes redownload. Login follows the origin rules above. On-demand helpers do not record live; the native engine does. DRM is unsupported; site behavior depends on source/components. See [support limits](docs/product/support-matrix.md).

## Building from source

### Windows

Windows x64, Visual Studio 2022 C++ desktop tools/Windows SDK, CMake ≥3.25, Ninja, **Qt 6.11.3 MSVC x64**, vcpkg, Python 3, and Node ≥22 are required. Set these variables; repository scripts initialize compiler tools:

Full tests need `ffmpeg`/`ffprobe` on PATH for owned video fixtures. They are not ordinary TS/FLV runtime dependencies; complex HLS needs a trusted processor. Install Python dependencies before CMake, preferably in a venv. CI pins/checksums media tools in `.github/workflows/windows.yml`; optional login-media cases need a trusted parser. See [scenarios](docs/testing/scenarios.md).

```powershell
$env:QT_ROOT_DIR = 'C:/Qt/6.11.3/msvc2022_64'
$env:VCPKG_ROOT = 'C:/Tools/vcpkg'
python -m pip install -r tests/requirements.txt
./scripts/build.ps1 -Configuration Debug
./scripts/verify.ps1 -Configuration Release
npm.cmd --prefix browser_extension ci
npm.cmd --prefix browser_extension test
./scripts/dependency-sources.ps1
./scripts/package.ps1
./scripts/smoke.ps1 -PackageDirectory out/package/MaskPilot-0.3.0-windows-x64
```

Select existing Release output with `./scripts/package.ps1 -SkipBuild -BuildDirectory out/build/windows-msvc-portable`. Release uses `x64-windows-portable`/`out/dependencies-portable/`; Debug retains separate dependencies. Scripts check project/client/Qt/compiler versions and dependency configuration.

Release probes relative source-path mapping; private PDBs stay in builds. Packaging audits private paths, PE debug records, and manifests. Qt/MSVC bytes must match SDK/toolchain originals with valid signatures. Supplier build identifiers remain preserved/reported rather than modifying signed binaries. Required patches/licenses accompany packages.

Pinned Windows dependencies come from vcpkg; generated content stays in `out/`. Qt changes require matching source manifests/hashes; packaging refuses mismatch. Windows packages contain used Qt module sources/licenses. Original code is [MIT](LICENSE); dependencies are in [NOTICES](third_party/NOTICES.md). Linux uses system libraries and its own build/BT-license materials, not Windows version lists.

Scripts choose the newest installed MSVC toolset. After Qt/toolset changes, run `cmake --fresh --preset windows-msvc-release` in an initialized compiler environment and rebuild. Packaging checks actual Qt/compiler/Release configuration and refuses instrumented builds.

### Linux

Qualified userspace is Ubuntu 26.04 x64. Full helper protection needs Linux 6.9+; source requires Qt ≥6.8, libxml2 ≥2.14, and HTTP/HTTPS/FTP/FTPS/SFTP curl. Prepare native SDK/test prerequisites as an ordinary user:

```sh
python3 -m venv out/python-linux
out/python-linux/bin/python -m pip install -r tests/requirements.txt yt-dlp==2026.10.26
export PATH="$PWD/out/python-linux/bin:$PATH"
export PYTHON="$PWD/out/python-linux/bin/python"
sh scripts/verify-linux.sh
```

This builds, tests extension/full CTest in private keyrings, and runs real media checks. Preset: `linux-gcc-release`. Package with `python scripts/package_linux.py --build-directory out/build/linux-gcc-release --output-directory out/package/MaskPilot-0.3.0-linux-x64-local`; existing output is refused. See [Linux](docs/testing/linux-acceptance.md) for runtime/source/browser prerequisites and qualification limits. Formal release is deferred.

### macOS (qualification pending)

Use `macos-clang-release`; [macOS instructions](docs/testing/macos-acceptance.md) cover Qt/native dependencies, manual subset, and dual-architecture CI. Full CI uses a disposable keychain; never impersonate CI on a personal machine. No distributable installer or actual successful Mac build is claimed.

## Engineering and validation

`apps/` contains desktop/CLI/native-host/media-guard roots; `src/domain/` models; `src/application/` coordination; `src/infrastructure/` network/SQLite/media; `src/ui/` windows/models/dialogs; `browser_extension/` browser integration; `tests/` unit/fault/product cases; `packaging/` installation. Business logic does not depend on Qt UI/concrete network implementations.

See [architecture](docs/architecture/overview.md) for real paths/dependencies/flow, [blueprint](docs/DEVELOPMENT_BLUEPRINT.md) for goals/invariants, [scenarios](docs/testing/scenarios.md) for tests, and [acceptance](docs/releases/acceptance.md) for evidence/pending items. Unexecuted stress/clean-system/store qualification is not a pass.

The [historical Windows audit](docs/testing/repository-audit.md) records fixes/evidence at named commits, not full qualification of later code. Current matrices define support; regression passes do not deliver the original complete product goal.

Use C++20, four spaces, `.clang-format` (LLVM/110 columns), `PascalCase` types, and existing `camelCase` functions/locals. `.clang-tidy` is available but not automatically run by `verify.ps1`. GoogleTest uses `TEST(Suite, DescriptiveBehavior)`; prefer local fault services and explain new-behavior validation. Use imperative commits, describe behavior/issues/results in PRs, and attach UI screenshots. `AGENTS.md` stays unchanged by instruction; current guidance is here, in the blueprint, and [Contributing](CONTRIBUTING.md).

## Local context protocol (development code)

Current code has v1 standalone and v2 context interfaces for trusted local browser brokers. Admin authority configures directories/routes/short-lived source grants; ordinary clients get one context session without authority fallback. See [protocol](docs/protocols/browser.md). Profile/credential controls create/edit/revoke/reactivate profiles and retry cleanup. Filters limit list/new-task ownership; accepted tasks retain routes. Rebind applies only to inactive unfinished tasks. Profile creation supports HTTP/HTTPS/magnets; independent media stays standalone. Revocation stops work before vault cleanup; failures retain disabled state/retry requirements. Actual browser-side issuance needs separate browser-project acceptance.
