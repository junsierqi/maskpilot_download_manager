# Contributing

Read the [README](README.md), [current architecture](docs/architecture/overview.md), and [feature scope](docs/product/feature-matrix.md) first. Product goals and qualified features are recorded separately; actual macOS CI qualification remains pending. See the [documentation index](docs/README.md) for the full map.

The desktop defaults to English and supports saved English/Simplified Chinese choices through Settings (restart required). Canonical repository documentation is English.

The existing `AGENTS.md` is preserved by project instruction. Its empty-workspace/toolchain descriptions are early records; use this guide and the documents above for current directories, commands, and style. Do not rewrite it or remove dependency notices during documentation work.

## Before editing

- Check `git status --short`, the current branch, and the latest mainline; preserve unrelated changes and user data.
- Maintainers work directly on `master` and commit focused, verified increments. Do not leave accepted features scattered across long-lived branches. External contributors may use a fork and Pull Request; accepted work ultimately belongs on `master`.
- Explain the objective, boundaries, and validation before implementation. Protocol, persistence, permission, release, or platform changes must identify compatibility and acceptance requirements.
- Evolve directories around actual responsibilities. Avoid empty modules, cross-layer dependencies, and unrelated formatting; update affected documentation.

## Build and test

Prepare dependencies using [Building from source](README.md#building-from-source). Windows uses `scripts/build.ps1`/`scripts/verify.ps1`; Linux uses `scripts/verify-linux.sh`. See the [macOS guide](docs/testing/macos-acceptance.md) for its native preset, hosted CI, and personal-machine boundaries. Run commands from the repository root.

For an existing Windows Release preset build, a focused check might be:

```powershell
ctest --test-dir out/build/windows-msvc-release -R 'Checksums\.' --output-on-failure --no-tests=error
```

Choose the actual build directory and affected cases. Full validation and external media prerequisites are in [Testing scenarios](docs/testing/scenarios.md). Browser extension checks use `npm --prefix browser_extension ci` and `npm --prefix browser_extension test`. Never describe skipped, simulated, or unexecuted checks as real platform passes.

## Code and data conventions

- Use C++20 and Qt 6. Follow `.clang-format`: four spaces, a 110-column target, and existing module naming. Match public types/methods to their module; avoid bulk renaming.
- `.clang-tidy` supplies applicable diagnostics; `verify.ps1` does not automatically run either clang tool. Perform appropriate formatting/static checks and review compiler warnings.
- Keep domain/application independent; network, storage, and OS APIs belong in infrastructure, and Qt controls in UI. Add numbered SQL migrations instead of rewriting released migrations.
- Cover success, failure, and applicable boundaries for behavior changes. Prefer owned local download services, including interruption, recovery, file conflicts, and errors.
- Keep builds, downloads, and temporary data in ignored locations such as `out/`. Never commit credentials, private keys, login headers, secret-bearing URLs, or real browser profiles. Retain actual dependency names, copyrights, and licenses.

## Commits and Pull Requests

Use concise imperative subjects, such as `Fix queue selection after restart`, for a complete focused change. Describe the problem, resulting behavior, related issue, scope, and commands/platforms actually checked. List unverified items explicitly.

Include normal/narrow-window screenshots for UI changes, compatibility notes for protocols/migrations, and package/environment details for release work. Check `git diff --check` and generated content before committing. Investigate failed CI logs; configuration files alone do not prove a passing run.
