# Dependency Notices

MaskPilot's original source is MIT licensed. Dependency licenses apply to their
respective files. This document records actual dependencies, not design references.

| Component | Use | License / source |
|---|---|---|
| Qt 6.11.3 Core, Gui, Widgets, Network, Svg and deployed plugins | Dynamic desktop runtime | LGPL-3.0; bundled upstream notices, SBOMs and corresponding module source archives |
| Qt 6.11.3 translations | Desktop translations | BSD-3-Clause; matching module source and license texts bundled |
| curl 8.23.0 | HTTP/FTP/FTPS transfers, dynamically linked | curl license; https://curl.se/ |
| nghttp2 1.71.0 | HTTP/2 support | MIT; https://nghttp2.org/ |
| zlib 1.4 | Transport compression dependency | zlib license; https://zlib.net/ |
| SQLite 3.54.1 | Task persistence | Public domain; https://sqlite.org/ |
| libtorrent 2.1.2 | Independent BitTorrent session, metadata and verified piece transfer | BSD-3-Clause and bundled Zlib, BSL-1.0, APSL-2.0, BSD-4-Clause notices; https://github.com/arvidn/libtorrent/tree/v2.1.2 |
| Boost 1.92.0 | BitTorrent engine dependencies | Boost Software License 1.0; https://www.boost.org/LICENSE_1_0.txt |
| OpenSSL 3.6.5 | BitTorrent engine cryptographic dependency | Apache-2.0; https://www.openssl.org/ |
| Microsoft Visual C++ runtime | Windows runtime DLLs | Microsoft redistribution terms supplied with the build toolchain |

Packages include the installed dependency copyright files under `licenses/`,
Qt SBOMs and complete source archives under `dependency-sources/`. Qt is dynamically
linked; compatible modified Qt libraries may replace the DLLs. Do not remove these
materials when redistributing. Source builds use unmodified dependency sources
selected by `vcpkg-configuration.json`; no proprietary Qt modules are used.
The installed libtorrent copyright file includes its bundled helper notices;
retain the complete file, along with notices for the deployed Boost and OpenSSL
dependencies. Build-time Perl is not part of the application runtime.

The 0.2.0 Windows candidate bundles the seven native transport/storage libraries
and the Qt/MSVC runtime. Boost header-library port notices are retained even
though no Boost DLL is required. Versioned CMake configuration folders are build
metadata and are not runtime libraries.

Optional media tools are **not bundled**. The application can download pinned
yt-dlp and FFmpeg binaries from their distributors, or use user-selected executables.
Their own licenses and third-party notices apply. Keep the full downloaded FFmpeg
distribution, including its license/readme/source instructions. If redistributing
these optional tools yourself, supply their corresponding license and source
materials separately; the default MaskPilot package does not redistribute them.

GoogleTest, TypeScript and browser type definitions are build/test dependencies;
their binaries and source are not included in the desktop runtime package.
