#!/bin/bash
# Native SDK build and explicit initial CI coverage; no signing or publishing.
set -euo pipefail
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
test "$(uname -s)" = Darwin
build=${MASKPILOT_BUILD_DIR:-out/build/macos-clang-release}
architecture=${MASKPILOT_MACOS_ARCH:-$(uname -m)}
test "$architecture" = "$(uname -m)"
python=${PYTHON:-python3}
brew_prefix=$(brew --prefix)
qt_root=${QT_ROOT_DIR:?The selected official Qt SDK is required}
export PATH="$PWD/out/python-macos/bin:$brew_prefix/bin:$qt_root/bin:$PATH"
export PKG_CONFIG_PATH="$(brew --prefix curl)/lib/pkgconfig:$(brew --prefix libxml2)/lib/pkgconfig:$(brew --prefix sqlite)/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig"
mkdir -p out/qa
{
  sw_vers
  uname -m
  xcodebuild -version
  cmake --version
  "$python" --version
  brew list --versions
  "$(brew --prefix curl)/bin/curl" --version
} > out/qa/macos-sdk.txt 2>&1
cmake --preset macos-clang-release -B "$build" \
  "-DCMAKE_OSX_ARCHITECTURES=$architecture" \
  "-DCMAKE_PREFIX_PATH=$qt_root;$(brew --prefix curl);$(brew --prefix libxml2);$(brew --prefix sqlite);$(brew --prefix googletest);$(brew --prefix boost)" \
  "-DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)" \
  "-DPython3_EXECUTABLE=$(command -v "$python")" \
  -DMASKPILOT_TEST_MEDIA_PARSER:FILEPATH= -DMASKPILOT_TEST_MEDIA_MUXER:FILEPATH=
cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
npm --prefix browser_extension ci --ignore-scripts
npm --prefix browser_extension test
export QT_QPA_PLATFORM=offscreen
export MASKPILOT_TEST_CLI="$(cd "$build" && pwd)/maskpilot_cli"
export MASKPILOT_TEST_HTTP_CLIENT="$(brew --prefix curl)/bin/curl"
# Official Qt can dynamically load OpenSSL; its Homebrew keg is not in /usr/lib.
export DYLD_LIBRARY_PATH="$(brew --prefix openssl@3)/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
# Native cases include domain/UI/storage/torrent/scanner/crypto. Optional full
# media-component/live-video/browser-install/packaging acceptance is separate.
selection='^[A-Z]|macos_platform_product_path|local_http_product_path|ftp_product_path|sftp_product_path|http_post_product_path|native_payload_budget_product_path|completion_scan_product_path|bounded_http_product_path|http_response_metadata_product_path|site_project_product_path|site_project_qt_product_path|native_bridge_product_path|scoped_native_bridge_product_path|browser_recovery_product_path|adaptive_http_product_path|macos_ci_fixture'
# Catch known native prerequisites early; successful qualification still runs
# the complete selection below with every original assertion and timeout.
preflight='^(native_bridge_product_path|DownloadContexts\.ActualHttpAndSocksRoutesCarryOnlyTheirOwnedOriginGrants|TorrentProxy\.PeerOnlyHttpMagnetReportsRejectedTunnelsWithoutBypassingTheProxy|TorrentEngine\.PublishedSeedingUploadsWithLimitAndPausesWithoutRemovingFiles|macos_platform_product_path|bounded_http_product_path)$'
"$python" scripts/macos_test_session.py --platform-executable "$build/tests/maskpilot_macos_platform_tests" -- \
  ctest --test-dir "$build" -R "$preflight" --stop-on-failure --output-on-failure --no-tests=error -j1
"$python" scripts/macos_test_session.py --platform-executable "$build/tests/maskpilot_macos_platform_tests" -- \
  ctest --test-dir "$build" -R "$selection" --output-on-failure --no-tests=error -j1
{
  for executable in maskpilot maskpilot_cli maskpilot_native_host media_guard/ffmpeg media_guard/ffprobe; do
    file "$build/$executable"
    otool -L "$build/$executable"
  done
} > out/qa/macos-linked-libraries.txt 2>&1
tar -czf "out/qa/macos-$architecture-unpackaged.tar.gz" -C "$build" \
  maskpilot maskpilot_cli maskpilot_native_host media_guard
