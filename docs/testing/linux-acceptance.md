# Linux build and acceptance

## Qualification scope

Linux shares C++20/Qt 6 task, transfer, and persistence layers with Windows. The recorded environment is an isolated Ubuntu 26.04 x64 container on Ubuntu 24.04 x64 with host kernel `7.0.0-43-generic`; it does not prove Ubuntu 24.04 native userspace binary compatibility.

Full helper protection requires Linux **6.9+** pidfd process-group signaling. Earlier recorded SDK: GCC 15.2, CMake 4.2.3, Qt 6.10.2, curl 8.18.0, SQLite 3.46.1, libxml2 2.15.2, libsecret 0.21.7, OpenSSL 3.5.5, and pinned BT 2.1.2. Minimum source contracts are Qt 6.8, libxml2 2.14, and protocol-capable curl; older distribution packages may not meet them.

## Build and full tests

Prepare native development packages listed in `.github/workflows/linux.yml` within matching userspace, plus Node.js 22 and npm on the ordinary build user's PATH. CI supplies Node separately with `actions/setup-node`; the OS package list alone is insufficient. Do not upgrade a business host for builds. Run source commands from the repository root as an ordinary user; runtime packages do not contain build scripts:

```sh
python3 -m venv out/python-linux
out/python-linux/bin/python -m pip install -r tests/requirements.txt yt-dlp==2026.10.26
export PATH="$PWD/out/python-linux/bin:$PATH"
export PYTHON="$PWD/out/python-linux/bin/python"
sh scripts/verify-linux.sh
```

The script builds Release, tests the extension/full CTest, and runs real media cases. `scripts/linux_test_session.py` uses temporary HOME/session bus/encrypted test keyring, not user credentials, and refuses root. In containers, wrap it with `tini -s --` to reap detached test descendants.

Set `MASKPILOT_BUILD_DIR` for a different build. Supply matching verified offline BT sources with `sh scripts/verify-linux.sh -DFETCHCONTENT_SOURCE_DIR_MASKPILOT_TORRENT=/absolute/source`. For a compatible preinstalled BT library, packaging also requires `MASKPILOT_TORRENT_NOTICES_DIR` with complete matching `LICENSE` and `COPYING`.

## Local acceptance package

```sh
python scripts/package_linux.py --build-directory out/build/linux-gcc-release \
  --output-directory out/package/MaskPilot-0.3.0-linux-x64-local
```

Packages keep BT libraries in adjacent `lib/`, rely on system Qt/transport libraries, and retain notices. They are not universal cross-distribution bundles, installers, or signed releases. Existing staging/archive/digest outputs are refused; failures retain evidence. Manifests describe actual bytes/internal links, source digest, and verifiable Git state. A declared Git-less snapshot commit is not a verified commit, and a digest does not authenticate a publisher.

Run `./maskpilot` on matching systems, keeping CLI and `media_guard/` adjacent. Native files, BT, and bounded live work without media helpers. Optional on-demand tools are system-installed/user-selected; Linux does not download Windows binaries. Remembered proxy credentials need an unlocked Secret Service, failing explicitly without plaintext fallback when absent.

Ubuntu 26.04 runtime packages:

```sh
sudo apt-get install libqt6widgets6 libqt6network6 libqt6svg6 qt6-qpa-plugins \
  libxcb-cursor0 libcurl4t64 libsqlite3-0 libxml2-16 libsecret-1-0 libssl3t64 fonts-noto-cjk
```

These package names do not apply to older distributions. System-managed libraries/licenses/sources come from distribution channels. The desktop must provide a Secret Service session/unlocked keyring. Shutdown uses system-session authorization; no actual shutdown was exercised.

## Manual browser configuration

Linux has no installer/uninstaller. This development example uses ordinary-user default paths and does not replace real browser/enterprise-policy acceptance. Native host/Chrome user paths/Firefox manifest rules follow [Chrome native messaging](https://developer.chrome.com/docs/extensions/develop/concepts/native-messaging) and [Mozilla manifests](https://developer.mozilla.org/en-US/docs/Mozilla/Add-ons/WebExtensions/Native_manifests). Custom profiles, sandbox packages, and policy may change discovery.

Run inside the extracted, current-user-owned program directory. If any of the three files exists, stop without overwriting configuration:

```sh
umask 077
python3 - <<'PY'
import json
from pathlib import Path
root = Path.cwd().resolve()
assert (root / 'maskpilot').is_file() and (root / 'maskpilot_native_host').is_file()
home = Path.home()
chrome = home / '.config/google-chrome/NativeMessagingHosts/com.maskpilot.downloads.json'
firefox = home / '.mozilla/native-messaging-hosts/com.maskpilot.downloads.json'
config = root / 'native-bridge.json'
paths = (config, chrome, firefox)
assert not any(path.exists() or path.is_symlink() for path in paths), 'Existing configuration retained'
origin = 'chrome-extension://epdfgbhpbpkpephcfmemmadlblpodnfo/'
gecko = 'downloads@maskpilot.invalid'
host = dict(name='com.maskpilot.downloads', description='MaskPilot downloads',
            type='stdio', path=str(root / 'maskpilot_native_host'))
values = (
    dict(allowedOrigins=[origin], allowedFirefoxExtensions=[gecko],
         firefoxManifestPath=str(firefox), desktopPath=str(root / 'maskpilot'),
         dataDirectory=str(home / '.local/share/maskpilot-downloads')),
    dict(host, allowed_origins=[origin]),
    dict(host, allowed_extensions=[gecko]),
)
for path, value in zip(paths, values):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with path.open('x', encoding='utf-8') as stream:
        json.dump(value, stream, indent=2)
        stream.write('\n')
PY
```

Load `browser_extension/chromium/` in Chrome developer mode and `browser_extension/firefox/manifest.json` temporarily in Firefox. IDs must match manifests. Manually start with `./maskpilot --data-dir "$HOME/.local/share/maskpilot-downloads"` so browser/desktop share the same area. The host rechecks Firefox manifest/extension/program binding. Revoke only unchanged owned configuration; never remove browser profiles/download data.

## Current hosted CI

The [2026-11-23 CI snapshot](scenarios.md#hosted-ci-snapshot) records 329/329 native cases and 48/48 extension cases, with Node 22 available to the ordinary build user. It is separate from the earlier server acceptance and short-load measurements below.

## Earlier server acceptance

Native fixes are at `a0a5e37`. Independent build/test directories retain early failures and repair logs.

| Check | Recorded evidence |
|---|---|
| Full Linux entry | `sh scripts/verify-linux.sh`: one **309/309 CTest** run (670.13 seconds), extension **48/48**, **14 media groups**, including deployed-package/keyring/process-lifetime cases |
| Windows preservation | Independent **323/323** full run, later **10/10** affected cases, plus **1/1** native auto-start and segmentation fixtures, retaining failure counterexamples |
| Asset protection | `AGENTS.md` and 2,460 protected files kept matching digests; original 25 business containers retained identity; owned test mount only, no public ports |
| Graphics | Actual 1280×860 Chinese Qt view checked in Xvfb, not physical desktop/GPU qualification |
| Short load | Installed 300-second HTTP profile: 304.86 seconds, 44 cycles, 4,400 independently hashed files, 106 pause/resumes, peak 8 active; desktop IPC P95 4.20 ms, peak private 50.90 MiB/RSS 62.03 MiB; processes/files cleaned |
| CI | Native Linux YAML/Shell checked and the same full entry passed on the server; hosted remote CI was not triggered |

Private logs are in `out/qa/linux-port-20261120/`, with `linux-final-verification.log` for the full entry. Package `linux-build-info.json`/`manifest.json` record runtime contracts and byte/source summaries. Server limits were 1 CPU, 1,536 MiB, and one build/test job; the business host was not upgraded.

Recorded Windows/Linux native source digest: `fd563785f21f39dab9a1b29a9c7f8a7d66e7ebe387cb8e005fc0040f2dad13ab`. Paths are sorted as relative POSIX paths, independently of host case sorting. Load evidence covers local HTTP Range only, not media/TLS/public-network long-run qualification. Adjacent `.sha256` files describe final archives.

Physical GPU/Wayland, multi-screen/DPI, production browser policy, other distributions/architectures and an 8-hour Linux run remain unqualified by this Linux evidence. For independently recorded native Mac core CI and its remaining gaps, see [macOS](macos-acceptance.md). Automatic updates, formal release, and browser-project integration are separate future scope.
