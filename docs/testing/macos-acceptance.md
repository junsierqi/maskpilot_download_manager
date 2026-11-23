# macOS build and acceptance

## Current status

Native builds and selected core/platform paths pass on macOS 15 arm64 and x86_64 at `40aadd9`: 320/320 native cases, 48/48 extension cases, and a six-case prerequisite gate per architecture. See the [actual CI snapshot](scenarios.md#hosted-ci-snapshot). Windows regression or Python simulations do not qualify macOS. Native visual behavior, clean deployment, optional media/browser compatibility, signing, formal release, and online updates remain separate.

## After pushing

Push/pull request triggers `.github/workflows/macos.yml`; Actions → macOS → Run workflow also starts it manually. Jobs use `macos-15` (arm64) and `macos-15-intel` (x86_64) and verify native architecture. Either failure needs repair. No Apple developer account/signing secret is required.

Jobs explicitly select Xcode 26.3 through `DEVELOPER_DIR`, install Qt 6.11.3, native development dependencies, and Python/Node tools, then build all CMake targets. Fetching the checksum-pinned BT source adds initial time. Actual system/Homebrew versions are recorded in `macos-sdk.txt`; these are not a frozen release toolchain.

Native configuration and the loaded macOS network runtime require version 8.22 or newer. Older runner images can retain 8.21 despite a normal package install, causing a known IPv6 forward-proxy request-construction failure. CI first retains existing SDK prefix directories and installs missing base packages using runner metadata, then explicitly refreshes package metadata for the network capability check. It upgrades `curl` only when its actual client/library versions or HTTP/HTTPS/FTP/FTPS/SFTP capabilities are insufficient, then rechecks them. Project CMake/runtime guards and full native tests remain mandatory; required package dependencies may still need source builds on Intel. The original expanded-address HTTP/SOCKS tests remain mandatory; failed tests can print a bounded owned-client comparison without credential/header/body logging.

The first hosted runs failed with the runner's default Xcode 16.4 because its C++ library did not expose `std::jthread`/`std::stop_token`. Xcode 26 or newer is required for their stable implementation; experimental-library flags are not used. CMake checks compilation/linking before dependency setup and reports an unsupported SDK directly.

`macos-clang-release` selects `CMAKE_OSX_SYSROOT=macosx` before compiler initialization. This avoids the Apple compiler default `/usr/local/include` priority shadowing the selected SDK. CMake runs the selected crypto library to reject header/runtime major mismatches before building targets. The verifier also clears inherited C/C++/Objective-C header search overrides; the manual example does the same without changing global package links.

## Automated scope

- Native C++ GoogleTest: domain, scheduling, persistence, Qt controls, BT, hashes, media policy, and safety boundaries.
- Local HTTP, FTP/FTPS, SFTP, POST, budget, scanning, site publication, native messaging, browser recovery, and adaptive segmentation product paths.
- Apple-specific: Unicode vault reopen, owner/route/write-nonce binding, duplicate refusal/exact deletion, locked-vault failure, local torrent/symlink refusal, exclusive directory publication, descendant cleanup after stop/forced owner death, and private-input EOF.
- Browser-extension Node tests and portable disposable-keychain failure/recovery fixtures.

Keychain tests write fake credentials only in owned random namespaces. CI creates a private temporary keychain, temporarily changes search/default settings, restores/verifies originals on completion/error, and deletes only its keychain. Execution is restricted to GitHub-hosted macOS; never fake these environment variables on a personal machine. Forced job cancellation relies on hosted VM destruction, since in-process finally execution cannot be guaranteed.

The same guarded session prepares `127.0.0.2` on `lo0` when absent, probes TCP/UDP binding, and removes only its added alias on exit. Existing addresses remain untouched. The distinct address is required for peer-isolation/seeding fixtures; macOS does not implicitly bind the entire IPv4 loopback range. A short prerequisite gate runs before the complete native selection; either gate or restoration failure fails the job. Test origins use literal local names without reverse DNS, and Firefox fixtures use the actual adjacent-manifest policy.

Vault operations retain the query-level no-authentication option and temporarily suppress legacy Keychain interaction under a shared process mutex, restoring the exact previous setting before returning. Locked access must fail without automatically unlocking or prompting. The isolated CI probe prints stage names only (seed, lock, locked read, cleanup unlock); it never logs credential values. Native assertions cover restoration after successful and failed operations, including an already-disabled setting.

## Logs and failures

Download `macos-<architecture>-qualification` artifacts: `macos-ci.log`, SDK/link records, CTest `LastTest.log`, and CMake configure logs. For configuration failures check dependency discovery; for build/link failures start at the first diagnostic; for runtime failures inspect the exact test/library paths. Share failed steps/full logs with maintainers. Rerun both architectures after native source, test, or SDK repairs; documentation-only changes do not alter previously tested binaries.

Successful `*-unpackaged.tar.gz` output is an inspection build requiring the build machine’s Qt/native libraries, not a distributable `.app`, DMG, or standalone installer. Early failures may leave only partial diagnostics.

## Manual native build

Install Xcode 26 or newer with its command-line tools, Qt 6.11.3, Python 3.13, Node.js 22/npm, and development packages `cmake ninja pkgconf boost curl libxml2 sqlite googletest openssl@3`, including `curl` 8.22 or newer. Refresh package metadata and upgrade that package if an existing installation is older. Select that Xcode through `DEVELOPER_DIR` (CI uses `/Applications/Xcode_26.3.app/Contents/Developer`) before creating a fresh build directory, and point `QT_ROOT_DIR` at a compatible native Qt SDK:

```bash
python3 -m venv out/python-macos
out/python-macos/bin/python -m pip install -r tests/requirements.txt
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH OBJCPLUS_INCLUDE_PATH
export CMAKE_PREFIX_PATH="$QT_ROOT_DIR:$(brew --prefix curl):$(brew --prefix libxml2):$(brew --prefix sqlite):$(brew --prefix googletest):$(brew --prefix boost)"
export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
cmake --preset macos-clang-release -DPython3_EXECUTABLE="$PWD/out/python-macos/bin/python"
cmake --build --preset macos-clang-release
QT_QPA_PLATFORM=offscreen ctest --preset macos-clang-release -R '^(Checksums\.|macos_ci_fixture$)' --output-on-failure
```

`scripts/verify-macos.sh` orchestrates full CI, including its hosted-only keychain session. The manual commands above run a non-keychain-writing subset. Do not run complete credential tests against a personal keychain.

## Remaining qualification

Beyond the passing core CI on both architectures, separately qualify native UI/fonts/DPI/tray, real browser installation/permissions, optional parsers/muxers, actual recording, long load, sleep/wake, and clean deployment. Initial CI does not install optional media components or run their complete product cases; native unit coverage does not qualify them. Packaging, signing, and notarization remain deferred.
