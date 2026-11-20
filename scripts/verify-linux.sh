#!/bin/sh
# Run from a native non-root Linux SDK; no host package or profile changes.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
python=${PYTHON:-python3}
build=${MASKPILOT_BUILD_DIR:-out/build/linux-gcc-release}
parser=$(command -v yt-dlp)
muxer=$(command -v ffmpeg)
command -v ffprobe >/dev/null
cmake --preset linux-gcc-release -B "$build" \
  "$@" -DBUILD_TESTING=ON "-DPython3_EXECUTABLE=$(command -v "$python")" \
  "-DMASKPILOT_TEST_MEDIA_PARSER=$parser" "-DMASKPILOT_TEST_MEDIA_MUXER=$muxer"
cmake --build "$build" --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-1}"
npm --prefix browser_extension ci --ignore-scripts
npm --prefix browser_extension test
"$python" scripts/linux_test_session.py ctest --test-dir "$build" --output-on-failure --no-tests=error -j1
"$python" scripts/linux_test_session.py "$python" tests/integration/media_inventory_test.py \
  --exercise "$build/maskpilot_cli" --parser "$parser" --muxer "$muxer" \
  --ui "$build/tests/maskpilot_media_system_tests" --scan-probe "$build/tests/maskpilot_scan_probe"
