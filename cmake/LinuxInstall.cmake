# Local Linux qualification package; system Qt and transport dependencies are not bundled.
set_target_properties(maskpilot maskpilot_cli maskpilot_native_host PROPERTIES INSTALL_RPATH "$ORIGIN/lib")
set_target_properties(maskpilot_media_ffmpeg maskpilot_media_ffprobe PROPERTIES
  INSTALL_RPATH "$ORIGIN/../lib")
install(TARGETS maskpilot maskpilot_cli maskpilot_native_host
  RUNTIME DESTINATION . COMPONENT LinuxRuntime)
install(TARGETS maskpilot_media_ffmpeg maskpilot_media_ffprobe
  RUNTIME DESTINATION media_guard COMPONENT LinuxRuntime)
install(FILES "$<TARGET_FILE:LibtorrentRasterbar::torrent-rasterbar>"
  "$<TARGET_SONAME_FILE:LibtorrentRasterbar::torrent-rasterbar>"
  DESTINATION lib COMPONENT LinuxRuntime)

get_target_property(torrent_target LibtorrentRasterbar::torrent-rasterbar ALIASED_TARGET)
if(NOT torrent_target)
  set(torrent_target LibtorrentRasterbar::torrent-rasterbar)
endif()
get_target_property(torrent_imported ${torrent_target} IMPORTED)
set(default_notices "")
if(NOT torrent_imported)
  get_target_property(default_notices ${torrent_target} SOURCE_DIR)
endif()
set(MASKPILOT_TORRENT_NOTICES_DIR "${default_notices}" CACHE PATH
  "Matching libtorrent source directory containing complete LICENSE and COPYING")
if(EXISTS "${MASKPILOT_TORRENT_NOTICES_DIR}/LICENSE" AND
   EXISTS "${MASKPILOT_TORRENT_NOTICES_DIR}/COPYING")
  install(FILES "${MASKPILOT_TORRENT_NOTICES_DIR}/LICENSE" "${MASKPILOT_TORRENT_NOTICES_DIR}/COPYING"
    DESTINATION licenses/libtorrent COMPONENT LinuxRuntime)
else()
  message(STATUS "Linux packaging requires MASKPILOT_TORRENT_NOTICES_DIR with matching LICENSE and COPYING")
endif()
install(FILES LICENSE DESTINATION . COMPONENT LinuxRuntime)
install(FILES docs/testing/linux-acceptance.md DESTINATION . RENAME README.md COMPONENT LinuxRuntime)
install(DIRECTORY docs/ DESTINATION docs COMPONENT LinuxRuntime FILES_MATCHING PATTERN "*.md")
install(DIRECTORY browser_extension/dist/ DESTINATION browser_extension/chromium COMPONENT LinuxRuntime)
install(DIRECTORY browser_extension/dist-firefox/ DESTINATION browser_extension/firefox COMPONENT LinuxRuntime)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/linux-build-info.json" CONTENT
  "{\n  \"version\": \"${PROJECT_VERSION}\",\n  \"system\": \"Linux\",\n  \"architecture\": \"${CMAKE_SYSTEM_PROCESSOR}\",\n  \"compiler\": \"${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\",\n  \"qt\": \"${Qt6Core_VERSION}\",\n  \"curl\": \"${CURL_VERSION}\",\n  \"sqlite\": \"${SQLite3_VERSION}\",\n  \"libxml2\": \"${LIBXML2_VERSION_STRING}\",\n  \"libsecret\": \"${LIBSECRET_VERSION}\",\n  \"minimumKernel\": \"6.9\",\n  \"runtimePolicy\": \"System Qt/transport libraries; bundled compatible BT library only\"\n}\n")
install(FILES "${CMAKE_BINARY_DIR}/linux-build-info.json" DESTINATION . COMPONENT LinuxRuntime)
