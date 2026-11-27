add_library(maskpilot_infrastructure
    src/ed2k/protocol.cpp
    src/ed2k/integrity.cpp
    src/ed2k/connection.cpp
    src/ed2k/messages.cpp
    src/ed2k/checkpoint.cpp
    src/ed2k/verification.cpp
    src/ed2k/peer.cpp
    src/ed2k/transport.cpp
    src/ed2k/identity.cpp
    src/scheduling/queue_civil_time.cpp
    src/network/curl_transfer.cpp
    src/network/http_layout.cpp
    src/network/remote_file_transfer.cpp
    src/network/curl_runtime.cpp
    src/site/site_content.cpp
    src/site/css_content.cpp
    src/site/site_project_repository.cpp
    src/site/site_project_publisher.cpp
    src/transfer/transfer_router.cpp
    src/filesystem/transfer_files.cpp
    src/persistence/sqlite_repository.cpp
    src/security/credential_vault.cpp
    src/bridge/download_bridge.cpp
    src/media/live_playlist.cpp
    src/media/live_assembly.cpp
    src/media/live_transfer.cpp
    src/media/media_tools.cpp
    src/media/media_process.cpp
    src/media/media_origin_gateway.cpp
    src/process/owned_process.cpp
    src/security/completion_scanner.cpp
    src/media/media_inventory.cpp
    src/torrent/torrent_engine.cpp
    src/torrent/torrent_metadata.cpp)
target_include_directories(maskpilot_infrastructure PUBLIC include)
qt_add_resources(maskpilot_infrastructure "media_scope"
    PREFIX "/maskpilot/media" BASE "${CMAKE_CURRENT_SOURCE_DIR}/src/media"
    FILES src/media/scoped_requests.py)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(maskpilot_infrastructure PUBLIC maskpilot_application Qt6::Core
    PRIVATE CURL::libcurl OpenSSL::Crypto unofficial::sqlite3::sqlite3 LibtorrentRasterbar::torrent-rasterbar LibXml2::LibXml2 Qt6::Network)
target_link_libraries(maskpilot_infrastructure PRIVATE ZLIB::ZLIB)
if(WIN32)
  target_link_libraries(maskpilot_infrastructure PRIVATE bcrypt advapi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(maskpilot_infrastructure PRIVATE PkgConfig::LIBSECRET)
elseif(APPLE)
  target_sources(maskpilot_infrastructure PRIVATE
    src/process/macos_process_group.cpp src/security/macos_keychain.cpp)
  find_library(MASKPILOT_SECURITY_FRAMEWORK Security REQUIRED)
  find_library(MASKPILOT_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
  target_link_libraries(maskpilot_infrastructure PRIVATE
    ${MASKPILOT_SECURITY_FRAMEWORK} ${MASKPILOT_COREFOUNDATION_FRAMEWORK})
endif()
