# Dependency Notices

MaskPilot's original source is MIT licensed. Dependency licenses apply to their
respective files. This document records actual dependencies, not design references.

| Component | Use | License / source |
|---|---|---|
| Qt 6.11.3 Core, Gui, Widgets, Network, Svg and deployed plugins | Dynamic desktop runtime | LGPL-3.0; bundled upstream notices, SBOMs and corresponding module source archives |
| Qt 6.11.3 translations | Desktop translations | BSD-3-Clause; matching module source and license texts bundled |
| curl 8.23.0 | HTTP transfers, dynamically linked | curl license; https://curl.se/ |
| nghttp2 1.71.0 | HTTP/2 support | MIT; https://nghttp2.org/ |
| zlib 1.4 | Transport compression dependency | zlib license; https://zlib.net/ |
| SQLite 3.54.1 | Task persistence | Public domain; https://sqlite.org/ |
| Microsoft Visual C++ runtime | Windows runtime DLLs | Microsoft redistribution terms supplied with the build toolchain |

Packages include the installed dependency copyright files under `licenses/`,
Qt SBOMs and complete source archives under `dependency-sources/`. Qt is dynamically
linked; compatible modified Qt libraries may replace the DLLs. Do not remove these
materials when redistributing. Source builds use unmodified dependency sources
selected by `vcpkg-configuration.json`; no proprietary Qt modules are used.

Optional media tools are **not bundled**. The application can download pinned
yt-dlp and FFmpeg binaries from their distributors, or use user-selected executables.
Their own licenses and third-party notices apply. Keep the full downloaded FFmpeg
distribution, including its license/readme/source instructions. If redistributing
these optional tools yourself, supply their corresponding license and source
materials separately; the default MaskPilot package does not redistribute them.

GoogleTest, TypeScript and browser type definitions are build/test dependencies;
their binaries and source are not included in the desktop runtime package.
