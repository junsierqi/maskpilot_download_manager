# macOS build and acceptance

## Current status

Native configuration, Apple adapters, and executable CI entry points are prepared. Hosted compilation passes with Xcode 26.3, but **native runtime acceptance remains incomplete**: arm64 improved from 262/319 at `114848e` to 316/320 at `ce167b1`. The remaining failures concern native fixture prerequisites; follow-up fixes require a fresh run on both architectures. Windows regression or Python simulations do not qualify macOS. Formal release, signing, online updates, and complete media/browser compatibility are not claimed.

## After pushing

Push/pull request triggers `.github/workflows/macos.yml`; Actions → macOS → Run workflow also starts it manually. Jobs use `macos-15` (arm64) and `macos-15-intel` (x86_64) and verify native architecture. Either failure needs repair. No Apple developer account/signing secret is required.

Jobs explicitly select Xcode 26.3 through `DEVELOPER_DIR`, install Qt 6.11.3, native development dependencies, and Python/Node tools, then build all CMake targets. Fetching the checksum-pinned BT source adds initial time. Actual system/Homebrew versions are recorded in `macos-sdk.txt`; these are not a frozen release toolchain.

The first hosted runs failed with the runner's default Xcode 16.4 because its C++ library did not expose `std::jthread`/`std::stop_token`. Xcode 26 or newer is required for their stable implementation; experimental-library flags are not used. CMake checks compilation/linking before dependency setup and reports an unsupported SDK directly. Selecting a newer SDK is a repair awaiting a successful native rerun, not proof of platform acceptance.

## Automated scope

- Native C++ GoogleTest: domain, scheduling, persistence, Qt controls, BT, hashes, media policy, and safety boundaries.
- Local HTTP, FTP/FTPS, SFTP, POST, budget, scanning, site publication, native messaging, browser recovery, and adaptive segmentation product paths.
- Apple-specific: Unicode vault reopen, owner/route/write-nonce binding, duplicate refusal/exact deletion, locked-vault failure, local torrent/symlink refusal, exclusive directory publication, descendant cleanup after stop/forced owner death, and private-input EOF.
- Browser-extension Node tests and portable disposable-keychain failure/recovery fixtures.

Keychain tests write fake credentials only in owned random namespaces. CI creates a private temporary keychain, temporarily changes search/default settings, restores/verifies originals on completion/error, and deletes only its keychain. Execution is restricted to GitHub-hosted macOS; never fake these environment variables on a personal machine. Forced job cancellation relies on hosted VM destruction, since in-process finally execution cannot be guaranteed.

The same guarded session prepares `127.0.0.2` on `lo0` when absent, probes TCP/UDP binding, and removes only its added alias on exit. Existing addresses remain untouched. The distinct address is required for peer-isolation/seeding fixtures; macOS does not implicitly bind the entire IPv4 loopback range. A short prerequisite gate runs before the complete native selection; either gate or restoration failure fails the job. Test origins use literal local names without reverse DNS, and Firefox fixtures use the actual adjacent-manifest policy.

## Logs and failures

Download `macos-<architecture>-qualification` artifacts: `macos-ci.log`, SDK/link records, CTest `LastTest.log`, and CMake configure logs. For configuration failures check dependency discovery; for build/link failures start at the first diagnostic; for runtime failures inspect the exact test/library paths. Share failed steps/full logs with maintainers. Both architectures must rerun after repairs.

Successful `*-unpackaged.tar.gz` output is an inspection build requiring the build machine’s Qt/native libraries, not a distributable `.app`, DMG, or standalone installer. Early failures may leave only partial diagnostics.

## Manual native build

Install Xcode 26 or newer with its command-line tools, Qt 6.11.3, and development packages `cmake ninja pkgconf boost curl libxml2 sqlite googletest openssl@3`. Select that Xcode through `DEVELOPER_DIR` (CI uses `/Applications/Xcode_26.3.app/Contents/Developer`) before creating a fresh build directory, and point `QT_ROOT_DIR` at a compatible native Qt SDK:

```bash
python3 -m venv out/python-macos
out/python-macos/bin/python -m pip install -r tests/requirements.txt
export CMAKE_PREFIX_PATH="$QT_ROOT_DIR;$(brew --prefix curl);$(brew --prefix libxml2);$(brew --prefix sqlite);$(brew --prefix googletest);$(brew --prefix boost)"
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --preset macos-clang-release -DPython3_EXECUTABLE="$PWD/out/python-macos/bin/python"
cmake --build --preset macos-clang-release
QT_QPA_PLATFORM=offscreen ctest --preset macos-clang-release -R '^(Checksums\.|macos_ci_fixture$)' --output-on-failure
```

`scripts/verify-macos.sh` orchestrates full CI, including its hosted-only keychain session. The manual commands above run a non-keychain-writing subset. Do not run complete credential tests against a personal keychain.

## Remaining qualification

Even after both CI architectures pass, separately qualify native UI/fonts/DPI/tray, real browser installation/permissions, optional parsers/muxers, actual recording, long load, sleep/wake, and clean deployment. Initial CI does not install optional media components or run their complete product cases; native unit coverage does not qualify them. Packaging, signing, and notarization remain deferred.
