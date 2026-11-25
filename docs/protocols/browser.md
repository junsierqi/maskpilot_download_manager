# Browser and native protocol

This describes development code. Frozen 0.2.0 packages keep their original protocol and do not gain new capabilities from this document. Interception/Cookie forwarding default off; existing extensions remain v1 standalone clients.

The native host starts the desktop with `--background`. Initialization errors exit with failure instead of waiting for an error dialog; normal desktop launches retain interactive diagnostics. Failure before durable `prepare` acknowledgement never authorizes cancelling the browser-owned download.

## Framing and identity

Chromium uses a service-worker manifest; Firefox 150+ uses a module event page with shared code. Firefox ID is `downloads@maskpilot.invalid`; service `com.maskpilot.downloads` uses adjacent `com.maskpilot.downloads.firefox.json`. Host checks absolute manifest path, current executable, and `allowed_extensions`; `native-bridge.json` also needs `allowedFirefoxExtensions`. Source arguments grant neither broker administration nor fingerprint-profile identity.

The opt-in panel injects an isolated classic script only into top-level HTTP(S). Bounds: 200 resources, 64 page sessions, 10-minute lifetime. Pages submit only random document/session tokens and selected opaque resource IDs. Extra fields, iframes, wrong/expired documents/tokens, and arbitrary native/admin commands fail. Reload/navigation/close/disable/lost background state requires rediscovery.

ShadowRoot organizes styles, not authority. Selection stays in an isolated closure and changes only on real clicks; page edits to `value`/`checked` or synthetic clicks add no authority. Recheck page/session after asynchronous Cookie work and before native send. Per-item accepted/remaining results and stable batch `requestId` provide retry idempotence, not cross-task transactions or restart-persistent batches.

Query only selected URL/trusted container, inspect partitions with empty `partitionKey`, then reject partitioned/first-party-isolated identity without downgrade queries. Firefox missing trusted `cookieStoreId`/private windows refuse forwarding. Another partition’s cookie for the same URL can cause refusal. Disabling forwarding is explicit.

Windows registration touches only three fixed HKCU 64-bit service keys. `browser-registration.json` records original default type/value/existence before writing; repeat registration preserves the first snapshot. Unregister restores only still-owned values and retains external changes/unrelated values/subkeys. Installation tracks dynamic hashes; failed restoration retains logs and stops uninstall. Temporary-extension tests do not qualify signed distribution/system isolation.

Native messages/local pipes use a 4-byte unsigned little-endian length followed by UTF-8 JSON. Maximum frame: 1 MiB; one request per connection. `requestId`, `taskId`, context/session IDs are 32-character lowercase hex. Fields enforce types/bounds/command allowlists.

Desktop IPC is current-user only. Startup generates distinct `ipc.token` (v1 standalone) and `ipc-admin.token` (trusted local administration); ordinary extension hosts never read admin tokens. Ordinary v2 uses a context-issued `sessionToken`; failure never falls back to standalone/admin authority. Extension registration proves extension identity, not browser-profile ownership.

No public local HTTP control port exists. Admin/session/grant Cookie material never enters task lists/diagnostics. Windows vault/pipe boundaries are the logged-in user, not hostile same-user processes.

## v1: standalone area

Example request and response:

```json
{"version":1,"requestId":"0123456789abcdef0123456789abcdef","command":"prepare","url":"https://example.test/file.zip","filename":"file.zip"}
{"version":1,"requestId":"0123456789abcdef0123456789abcdef","ok":true,"taskId":"0123456789abcdef0123456789abcdef"}
```

`ping` checks connection; `list` returns up to 1000 standalone summaries; `add` creates, `prepare` durably pauses, and `resume`/`pause`/`cancel`/`remove` take `taskId`. Other contexts are invisible/uncontrollable. Remove cleans history/owned cache but preserves final files; active tasks need pause/cancel first.

Draft fields include `filename`, `referrer`, `cookie`, `userAgent`, `media`, and `audioOnly`, HTTP/HTTPS only. Trusted desktop settings supply destinations/routes/tools; browser requests cannot choose arbitrary paths/proxies/executables. This browser media entry refuses Cookie/Authorization/custom headers; public on-demand resume refetches.

## v1: browser-assisted recovery (development code)

Standalone adds `recovery.begin` (`taskId`), `recovery.apply` (`recoveryToken`, `url`, optional `cookie`/`referrer`/`userAgent`), and `recovery.resume`/`recovery.cancel` (token only). Only unfinished paused worker-free standalone HTTP files qualify, not context/site/media/FTP tasks. Neither ordinary v2 nor admin can expand authority through these commands. `canRecover` is a UI hint, not authorization.

Begin returns original ID/name/source page/token/expiry. At most 64 memory sessions last ten minutes; beginning again replaces the old token. Edits/restart/queue move/removal/process restart/backward clock invalidate sessions. Tokens/apply receipts are not durable/exported.

Apply changes only source/current browser material, retaining ID/target/proxy/queue/hash/policy and clearing old origin/custom headers. Required login needs new material, never anonymous downgrade. Acknowledge only durable success. Same-token/same-material retry acknowledges the original save; changed replay fails. Task stays paused until browser duplicate cancellation and separate resume; failures retain browser download. New URLs do not prove reusable extents; native HTTP identity decides.

Trusted popup explicitly chooses the original, pauses/waits for persistence, then opens source in the current container. Only post-begin downloads in that container are listed; user selects an exact record, never filename/global-next matching. `storage.session` stores token/original/source-tab/container/expiry/stage only, no Cookie or content-script access. Pages/panels cannot call recovery controls; private/missing-container/partitioned login refuses.

Pause selected browser record, apply/save/acknowledge and record stage before cancelling it. Then `recovery.resume` validates the same live receipt, never ordinary-ID fallback. Cancel failure leaves original paused for explicit retry/cancel; lost acknowledgement never starts it. Manual URLs stay paused. Worker reload can recover memory stages, browser/desktop restart needs a new session. Explicitly cancel invalid sessions before re-enabling interception. Real Chromium/Firefox default/container Cookie recovery is tested; worker interruptions use controller fixtures, not universal site/POST/media replay.

## v2: ordinary context client

Through native host, `sessionToken` authenticates transport and becomes desktop-pipe authentication, never admin-token substitution. Example IDs are placeholders:

```json
{"version":2,"requestId":"0123456789abcdef0123456789abcdef","sessionToken":"11111111111111111111111111111111","command":"prepare","params":{"url":"https://example.test/file.zip","filename":"file.zip"}}
```

| Command | `params` | Result |
|---|---|---|
| `ping` | Empty object | Session validity |
| `prepare` | `url`, `filename`, optional `originGrantId` | Paused `taskId` |
| `add` | Same, optional `paused` | `taskId` |
| `list` | Optional `limit` (1–1000), `cursor` | `tasks`, `nextCursor` |
| `resume`/`pause`/`cancel`/`remove` | `taskId` | Accepted task ID |

Sources are HTTP/HTTPS or magnets; richer media options are separate scope. Clients cannot supply context IDs, Cookie, proxy, vault references, destinations, or tools. Sessions determine ownership. Lists filter ownership before paging and contain only ID/name/state/bytes. Foreign/missing IDs have identical refusal results.

Retries with the same context/request ID return the durable task; other contexts may reuse IDs independently. Retry never replaces original route/grant; refresh is explicit administration.

## v2: trusted local administration

A trusted Browser Bridge can start `maskpilot_native_host.exe --broker --data-dir <absolute-app-data-directory>` and exchange binary frames through stdin/stdout, not raw text JSON. Secrets belong inside frames, not arguments. Ordinary browser sources/JSON fields cannot select broker mode.

| Command | Parameters/result |
|---|---|
| `context.configure` | `externalKey`, `label`, `downloadRoot`; optional `proxyEndpoint`, `proxyAuthenticationRequired`, `originAuthenticationRequired`, `proxyCredentials:{username,password}`, `rememberProxy`. Updates require `id`/`expectedRevision`; revoked contexts need explicit `reactivate`. Returns safe context data |
| `context.list` | Context information/`hasStoredCredentials`, never native references/ownership nonce |
| `context.issue` | `contextId` → `sessionToken` |
| `origin.grant` | `contextId`, exact `url`, `cookie`/`authorization`, `expiresAt` → `originGrantId`; at most five minutes |
| `context.revoke` | `contextId`; persist disabled state, stop work, wait for exit/credential cleanup, then acknowledge |
| `task.rebind` | `taskId`, optional `originGrantId`; inactive unfinished tasks only, destination preserved |

Configuration changes invalidate old sessions; new tasks use new configuration while accepted tasks retain routes until explicit rebind. Sessions do not survive restart. Revocation preserves final files; cleanup failure remains disabled/incomplete. Ordinary session expiration does not revoke accepted background work.

Trusted Browser Storage/Bridge filters grants by URL/Domain/Path/Secure/partition. Downloader checks context/exact URL/expiry and never reads private browser databases. URL-changing redirects pause exact-grant tasks for new approval; missing grants after restart never downgrade anonymous. Authenticated v1 standalone also retains login-required and refuses any changed-URL redirect, including same-origin. Real browser issuance needs independent project acceptance, not local-fixture substitution.

## Interception and recovery

1. Pause the browser download.
2. `prepare` validates/persists a paused task before acknowledgement.
3. Cancel the browser original, then `resume`.
4. Prepare failure restores browser work. Cancel failure cancels the native paused draft and restores the browser. Native-resume failure retains the durable task and reports the error.

Control acknowledgement means the coordinator accepted a transition; query terminal results through `list`. Administrative revocation has an additional worker-exit/cleanup barrier. Unexecuted UI/production-browser/store/fingerprint-browser integration cannot be marked qualified.
