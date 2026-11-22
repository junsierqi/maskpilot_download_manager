add_library(maskpilot_infrastructure
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
target_link_libraries(maskpilot_infrastructure PUBLIC maskpilot_application Qt6::Core
    PRIVATE CURL::libcurl unofficial::sqlite3::sqlite3 LibtorrentRasterbar::torrent-rasterbar LibXml2::LibXml2 Qt6::Network)
if(WIN32)
  target_link_libraries(maskpilot_infrastructure PRIVATE bcrypt advapi32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(maskpilot_infrastructure PRIVATE PkgConfig::LIBSECRET)
endif()
