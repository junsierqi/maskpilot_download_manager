# Current architecture

This document describes the current source and CMake targets. See the [blueprint](../DEVELOPMENT_BLUEPRINT.md) for product/evolution rules and the [feature map](../product/feature-matrix.md) for support boundaries. Planned directories are not implemented modules.

## Directories and responsibilities

| Path | Current responsibility |
|---|---|
| `apps/desktop/` | Qt startup, single instance, dependency composition, user-data directory, and local IPC |
| `apps/cli/` | Verification client and site-project commands; not a headless background service |
| `apps/native_host/` | Browser native-message framing, identity checks, and desktop forwarding |
| `apps/media_guard/` | Restrict trusted media helpers to owned local input/output |
| `src/domain/` | Pure C++ task, request, queue, proxy, media, live, and context models/validation |
| `src/application/` | Use cases/ports, task coordination, queue/budget/context policy, site projects, and browser recovery |
| `src/infrastructure/src/` | Concrete adapters in `network/`, `transfer/`, `torrent/`, `media/`, `site/`, `persistence/`, `filesystem/`, `security/`, `process/`, `bridge/`, and `scheduling/` |
| `src/ui/src/` | Code-built Qt Widgets in `windows/`, `dialogs/`, `models/`, and `style/` |
| `browser_extension/` | TypeScript extension, browser manifests, build scripts, and Node tests; generated folders are ignored |
| `database/migrations/` | Ordered SQL migrations, upgrade backups, and future-schema rejection |
| `tests/` | Unit/integration/system tests, local services, and Linux/macOS fixtures |
| `resources/`, `packaging/` | Application assets, installation, and browser registration |
| `cmake/`, `scripts/` | Native dependencies, builds, validation, dependency sources, and packaging |
| `third_party/` | Actual dependency/distribution notices; retain names and licenses |
| `out/` | Ignored builds, packages, and local validation output |

Public headers live under each layer's `include/maskpilot/<layer>/`. CMake creates a target for each of the four layers. Do not add empty directories or placeholder implementations for hypothetical future features.

## Dependency direction

`maskpilot_domain` depends only on build options; `maskpilot_application` depends on domain; `maskpilot_ui` depends on application and Qt; `maskpilot_infrastructure` implements application ports using curl, SQLite, BT, crypto, content processing, and native APIs. Desktop/CLI composition roots combine application and infrastructure; UI does not implement protocol engines.

Domain/application source does not depend on QWidget, curl, SQLite, or OS credential APIs. Infrastructure does not depend on UI. Application objects receive transport, storage, clock, and scanner implementations through interfaces. Layer `CMakeLists.txt` files define the actual links; tests can compose real adapters or injected failures.

## Task flow and ownership

1. Desktop, CLI, or browser bridge validates inputs and explicitly determines ownership, route, login requirements, and output path.
2. The application coordinator owns state, scheduling, queue windows, budget reservations, and persistence. Workers transfer data and return events; UI consumes state and asynchronous command results through its event loop.
3. Routing selects native file, BT, trusted media helper, or native live adapters. Login material remains in memory/private input; persistence stores public configuration, requirement flags, and explicitly approved vault references.
4. Content is staged in owned files/directories, checked for resource identity/integrity, optionally scanned, and published without replacement. Pause, cancellation, errors, late callbacks, and restart are reconciled by the state machine rather than forced into success.

Site projects reuse the download coordinator and an atomic manifest for preview, children, and immutable revisions; there is no separate network engine. Committed summaries and changed immutable details are published under one view lock, preventing a completed detail from preceding its committed summary. Browser recovery uses bounded original-task sessions to refresh links/login without expanding context authority. See the [browser interface](../protocols/browser.md) and [task list contract](../protocols/task-list.md).

## Platform and validation boundaries

Platform differences live mainly in native dependencies, credential vaults, process lifetime, and publication adapters. Windows uses Credential Manager/job objects; Linux uses Secret Service/session/pidfd protection; macOS has Keychain, EOF guardian, and exclusive publication code, with compilation and selected core/platform paths qualified on macOS 15 arm64/x86_64; visual, clean-deployment, and formal release checks remain separate. No plaintext vault fallback is provided. Helpers must be trusted; lifetime controls are not a hostile-code sandbox.

The desktop defaults to English, honors saved English/Simplified Chinese choices, and applies a language change after restart. Application labels and Qt translator startup share the same fallback.

See [source builds](../../README.md#building-from-source), [Linux](../testing/linux-acceptance.md)/[macOS](../testing/macos-acceptance.md) qualification, [testing](../testing/scenarios.md), and [decisions](decisions.md). Source boundaries and historical results do not replace current-commit machine/browser/long-run/release qualification.

## ED2K transport

Domain `ed2k.h/.cpp` owns bounded file-link/proof parsing and the common endpoint grammar. `DownloadSpec::ed2k` carries public servers/sources; kind 6 and the appended source-wait/remote-queue states preserve earlier numeric values. Migration 0020 appends `ed2k_servers` and `ed2k_sources` to schema 20, with the existing pre-upgrade backup. Their canonical newline grammar is independent of Qt and reused by UI/CLI/storage. Version-2 task lists use an optional strict `ed2k` object with `servers`/`sources` string arrays.

Infrastructure `src/ed2k/` separates bounded framing/messages, explicit-route connections, peer negotiation/queues, identity, integrity, checkpoint and orchestration. One `Ed2kResources` belongs to each unified transport factory: FIFO endpoint leases prevent competing tasks from evicting each other's stable owner/public-route identity; a shared eight-part memory allowance bounds concurrent buffers. Connections and proxy credentials remain task-local. No index defaults, listener, Kad, UDP or upload service is started.

Each task caches only verified parts in a native-file-identified sparse payload, an immutable hash proof and a bounded checkpoint bitmap. Reopen rehashes cached candidates. A separately identified exclusive stage is streamed, checked, optionally scanned and published without replacement. The coordinator delegates ED2K cleanup before generic stage removal, preserving unknown/replaced files. Persistent route identity contains no proxy credentials and survives task removal. Unbudgeted queues use the existing null budget contract; configured budgets charge received payload, including corrupt, duplicate and compressed wire data. Displayed speed uses logical file progress for ETA.
