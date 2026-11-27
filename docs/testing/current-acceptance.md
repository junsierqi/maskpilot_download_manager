# Current-runtime acceptance

This record qualifies a local validation candidate, not a formal release. Historical 0.2.0/0.3.0 packages keep their original evidence; they do not qualify newer code.

Status on 2026-11-26: **in progress**. The FTPS repair passed local and all four hosted checks, but its fresh long-duration run failed during HTTP recovery-cache reset. The user subsequently prioritized ED2K implementation and deferred remaining long qualification. The IPC/soak failure remains unaccepted; formal browser integration and maintained site compatibility remain separate follow-up work.

## ED2K development cohort

Native ED2K is now integrated with standalone Qt/CLI tasks, schema 20 storage and strict version 2 task-list endpoint arrays. The actual native engine downloaded legal 3-byte, 9,728,001-byte and 19,456,001-byte payloads from an isolated independent peer; the largest paused after one verified part and resumed in a new process with exact bytes and independently generated auxiliary hashes. Public CLI direct/index discovery, two concurrent unified-service downloads sharing endpoint arbitration, native quota exhaustion, safe removal, upgrade/backup and Qt controls/import-export have local Windows evidence. See [ED2K scenarios](scenarios.md#native-ed2k).

Windows validation: the initial full run passed 370 of 376 cases in 461.49 seconds. Its six failures were stale historical-schema fixtures/expected-version assertions; their corrected inputs and original assertions all passed on rerun. The final build passed 46 focused checks in 29.53 seconds, including 31 native ED2K cases, both protocol/product fixtures, all six migration cases and affected request/UI paths. One added file-identity edit regression brings discovery to 377 cases. These are a full run plus focused final reruns, not a fresh 377-case single-cohort pass.

This cohort uses Qt 6.11.3/MSVC 14.44 on Windows. Previous hosted/core qualification below does not qualify this new engine on other platforms. macOS CI now selects its bounded peer and unified-product fixtures; configuration alone is not a passing run. Kad, low-ID callbacks, uploading, required encryption, IPv6, web-source fallback, a complete >4-GiB ED2K transfer, public-network compatibility and long-duration acceptance remain unqualified or unsupported as documented. The prior failed long run is retained below, and no eight-hour pass is claimed.

## Latest repair cohort and retained long-run failure

Commit `f722441d4735212eead941b9f52fdff1b8f7f806`, before the HTTP cache repair, passed 344/344 local cases in 1260.46 seconds and the expanded FTP/FTPS fixture in 226.79 seconds. Its hosted checks all passed:

| Environment | Native cases | Extension cases | Hosted job |
|---|---:|---:|---|
| Windows x64 | 341/341 | 48/48 | [Windows](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45745854635/job/139661612432) |
| Linux x64 | 330/330 | 48/48 | [Linux](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45745854592/job/139661612649) |
| macOS arm64 | 321/321 plus 6 prerequisites | 48/48 | [ARM](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45745854613/job/139661613275) |
| macOS x86_64 | 321/321 plus 6 prerequisites | 48/48 | [Intel](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45745854613/job/139661612972) |

Its fresh ZIP SHA-256 was `0b9bae3b701e11bdebd1b8f9e11f5c3331bfbe7084e52f7d3724ed5e023713ff`. Scrubbed deployment/offline recovery, isolated Chromium/Firefox, legacy migration, public HTTPS/FTP/FTPS/media, authorized SFTP and an actual 4,294,967,553-byte transfer passed. The five-minute preflight also passed with before/after baseline and cleanup checks.

The subsequent long run **failed after 9,456.09 seconds**, when cycle 283 exceeded its unchanged 180-second deadline. It had verified 28,200 files, completed 849 pause/resume operations and 29 media cycles, and reached eight active tasks. Owned processes were cleaned; failed workload files, database and thread dump were deliberately retained. The after-cleanup baseline gate did not pass, and this run is not eight-hour qualification.

All eight remaining worker stacks were inside the HTTP reset loop's filesystem deletion calls, although their private cache directories were empty. That loop checked 1,024 possible numeric artifact paths per reset. The exact cause of the unusually slow OS calls is unproven. A new real-client empty-file regression independently fails on the old native binary: three startup-normalized I/O deltas were 1,298, 1,308 and 1,305, above its broad 768-operation bound.

The narrow repair enumerates present canonical owned artifacts before replacing identity and checks pause/cancel between operations. Its rebuilt Windows native passed 344/344 cases in 1239.41 seconds, including the expanded adaptive fixture in 48.23 seconds; extension units passed 48/48 with none skipped. Source and CLI hashes were unchanged throughout validation. The new empty-cache I/O deltas were 284, 280 and 274, below the unchanged 768-operation bound. Locked/high-ID temporary files, unrelated names and platform case behavior passed. Current-repair hosted CI and renewed eight-hour qualification remain pending; the preserved run above remains failed. No benchmark deadline, resource threshold, route or integrity check is relaxed.

The following sections retain the original cohort's historical evidence and boundaries; they do not qualify this changed native code.

## Original native cohort and verified scope

Native source is bound to commit d36b711112e852989e535878d800574d8dfd9ab1. The Windows Release uses Qt 6.11.3, MSVC 14.44, and portable x64 dependencies. The candidate ZIP SHA-256 is c796703bdcdd26c2b6779f6562ae42eea499746b3d9e69d2ad294be9b4097fb5.

- Full local regression: 344/344 passed in 1041.80 seconds without Qt plugin-path overrides. Subsequent fixture-only relocation repair passed the original native bridge case and actual scrubbed deployment test; application binaries were unchanged.
- Actual deployment: desktop/CLI/native handoff, installation, reinstallation, offline apply/rollback/finalize and uninstall passed with developer PATH removed and downloaded content preserved.
- Real Chromium and Firefox: durable prepare before interception, scoped Cookie forwarding, recovery, unavailable-host fallback and trusted panel checks passed using isolated profiles. Prior host registration defaults were restored. These are standalone extension paths, not fingerprint-browser v2 issuance.
- Public HTTPS: immutable repository document downloaded directly and through a redirect with expected SHA-256. A deliberately wrong digest failed without publishing the final destination.
- Public media: a [public documentation sample](https://developer.mozilla.org/en-US/docs/Web/HTML/Reference/Elements/video) passed deployed media inspection, explicit source selection and default MKV output. Independently fetched MP4 reference and output had identical decoded video and audio hashes; container bytes differed as expected. Forcing MP4 with unknown codec metadata correctly refused publication; the original diagnostic failure is retained. This covers one public file, not login or streaming sites.
- Legacy data: a frozen 0.2.0 executable created completed and durable partial tasks. Current migration from schema 4 to 19 preserved task identities/specifications and backup rows, made no automatic origin request, and restored partial work paused. Explicit resume used nonzero Range and verified the final hash. Original data/package/completed bytes remained unchanged.
- Public FTP/FTPS: the deployed client completed a small read-only demo file over FTP, explicit FTPS and implicit FTPS with the independent trusted-TLS reference hash. Wrong-digest FTPS did not publish the final file. The service required TLS data-session resumption; both modes also passed the strengthened local receiver policy, whose non-resuming control received 425 with zero payload.
- Remote SFTP: an existing trusted SSH identity and password-only reference authenticated to the authorized QA host. The client downloaded an existing QA license with the same hash, rejected a wrong pin, preserved remote bytes and left no plaintext password in the checked local logs/data. The native client selected another server-approved host-key algorithm than the reference; a pin must match this client's negotiated key. This qualifies one real server, not universal account or host-key compatibility.
- Large file: 4,294,967,553 bytes downloaded and hashed, exercising actual 64-bit filesystem and transfer behavior.

## Long-duration acceptance

The original native cohort completed 28,803.07 seconds of local HTTP and periodic synthetic MP4/HLS/DASH load after its five-minute preflight. All original thresholds passed:

| Observation | Actual result |
|---|---:|
| Verified files / transfer cycles | 93,900 / 939 |
| Pause/resume operations | 2,811 |
| Peak active tasks / segment layouts checked | 8 / 93,900 |
| Media cycles / resource samples | 94 / 26,215 |
| Private / working memory peaks | 29.48 / 52.18 MiB |
| Private-memory quarter drift | 2.93 MiB; limit below 64 MiB |
| IPC latency p95 | 18.63 ms; limit below 200 ms |
| Maximum sample gap | 13.34 s; limit 30 s |

All 23 mapped runtime modules were checked before and after load. The source/runtime/helper baseline verified before launch and after cleanup; the owned process exited and temporary workload files were removed. The coordinator exited successfully. Native source is the exact `d36b711` cohort above; later test/document commits did not change those application inputs. This qualifies the bounded HTTP/synthetic-media workload, not long-running FTPS, all public sites or a future repaired binary. A native repair requires its own candidate baseline and acceptance.

## Boundaries and retained failures

The Linux test server's Docker storage is read-only. A resource-limit update was rejected before container startup; original stopped state/limits were independently confirmed unchanged. The successful hosted jobs below qualify commit `2b85802`, whose application/build inputs match the frozen native cohort. Later `b3c2d9d` strengthens the FTPS fixture only: Linux and both Mac architectures passed; Windows passed 340/341 but failed the first strict explicit-FTPS transfer with network code 18. Local controls, including the same Python 3.13.17 TLS runtime and 20 fixed secure transfers, passed both modes.

Diagnostic commit `09baeb6` again passed Linux and both Mac architectures. Windows passed 339/341: the strict FTPS receiver recorded four TLS 1.3 data connections without session resumption, rejected each before sending payload, and completed TLS shutdown. The cause of missing resumption remains unproven. The second failure was HTTP recovery's nonzero-Range assertion. A controlled real-client counterexample confirmed that the test observer could accept a temporary `offset-0.new` before any checkpoint was committed. Commit `fc175ad` restricts observation to committed checkpoint names and matching part lengths; the original complete recovery test passed locally, preserving its Range and integrity assertions. This does not prove the exact observer state in the earlier CI failure.

Commit `c8d177d` adds bounded, certificate-verified system-client controls only after the original strict FTPS assertion fails. Linux passed 330 native and 48 extension cases; both Mac architectures passed 321 native cases, six prerequisites and 48 extension cases. The [Intel run](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45624538807/job/139241134913) completed its main selection in 1032.47 seconds. Windows passed 340/341 in 743.41 seconds: HTTP recovery passed, while strict FTPS still failed. On that Windows Server 2022 host (build 20348), the native client and independent system client both negotiated TLS 1.3 without data-session resumption and received zero payload. The system client's default and minimum-TLS-1.2 cases failed; exactly TLS 1.2 resumed successfully and transferred all 8,388,864 bytes with certificate verification. This establishes a shared interoperability failure in the tested configuration, not its precise underlying cause.

A separate mandatory native TLS 1.2 matrix now precedes the unchanged default-negotiation cases. Both explicit and implicit modes retain exact content, session resumption, unknown-CA refusal and hostname refusal. The complete expanded fixture passed locally in 43.21 seconds using Python 3.13.17. Its hosted result is pending. A passing compatibility case cannot qualify a failing default-negotiation transfer; no application TLS policy, trust check, retry limit or deadline has changed.

A subsequent native repair candidate adds Windows-only compatibility negotiation after an affirmative 425/522 session-resumption refusal with no new payload. It permits one verified TLS 1.2 transition and one independent REST restart, capped at three local negotiations, while retaining configured task retries and integrity checks. The complete local FTP/FTPS fixture passed in 167.58 seconds, including nonzero REST recovery in both TLS modes, both restart/compatibility orders without scheduler retries, and trust/length/hash rejection. An intermediate repair failed combined recovery because ignoring FTP size also bypassed the library's REST step; explicit REST with a required 350 confirmation fixes that path. Failed evidence is retained. Extension tests passed 48/48. Full local regression passed **344/344 in 1214.04 seconds** with Qt plugin overrides removed and adapter/CLI hashes unchanged throughout. Hosted results and fresh deployed long-duration qualification remain pending. The completed eight-hour run above qualifies the original native binaries and must not be used to qualify this repair.

Follow-up review found that a negated reply containing `is required` could still trigger compatibility. A real-client counterexample confirmed the transition; explicit negative-phrase handling now prevents it. The expanded FTP/FTPS fixture passed in 226.79 seconds within its unchanged 300-second limit, and the updated native binaries passed **344/344 in 1260.46 seconds** with unchanged source/CLI hashes and no Qt plugin overrides. A first deployment attempt was rejected because the qualification helper added `CMakeCache.txt` to the package-shaped runtime before inventory verification. Comparison found that single extra file, with no missing or modified package members. The next qualification keeps deployment verification ahead of adding build metadata; hosted and deployed long-duration results remain pending.

| Environment | Native cases | Extension cases | Successful hosted job |
|---|---:|---:|---|
| Windows x64 | 341/341 | 48/48 | [Windows](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45583364436/job/139098681354) |
| Linux x64 | 330/330 | 48/48 | [Linux](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45583364370/job/139098681222) |
| macOS arm64 | 321/321 plus 6 prerequisites | 48/48 | [ARM](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45583364558/job/139098681994) |
| macOS x86_64 | 321/321 plus 6 prerequisites | 48/48 | [Intel](https://github.com/junsierqi/maskpilot_download_manager/actions/runs/45583364558/job/139098682514) |

The preceding Intel attempt hit its 90-minute job ceiling after about 65 minutes of SDK setup and five prerequisites; it never qualified the full selection. The successful replacement used a 150-minute outer Intel job allowance, with native operation deadlines, assertions and test selection unchanged. The interrupted evidence remains retained.

Physical macOS UI/deployment, clean Windows machines, general public FTP compatibility and public BT peers, authenticated commercial media sites, extension production distribution and fingerprint-browser integration are unverified by this acceptance.

Initial failed regression/deployment logs remain evidence. Confirmed repairs isolate scanner Qt dependencies, prevent unattended startup dialogs, give eight-connection fixtures adequate server backlog and preserve deployed Qt plugin layout in relocation tests. Sporadic original startup/acknowledgement timeouts have no proven attribution; later passing runs do not establish their cause.


Twenty additional isolated real desktop/native-host startups passed with the original 15-second token and 25-second request deadlines. All tracked desktop and auto-start processes were released, and the successful private test directory was removed. An earlier diagnostic attempt received all replies but failed cleanup; its evidence is retained separately. This startup diagnostic does not establish graceful GUI shutdown or explain the original acknowledgement timeout.
