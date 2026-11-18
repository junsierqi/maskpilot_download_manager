add_library(maskpilot_infrastructure
    src/scheduling/queue_civil_time.cpp
    src/network/curl_transfer.cpp
    src/network/http_layout.cpp
    src/network/ftp_transfer.cpp
    src/transfer/transfer_router.cpp
    src/filesystem/transfer_files.cpp
    src/persistence/sqlite_repository.cpp
    src/security/credential_vault.cpp
    src/bridge/download_bridge.cpp
    src/media/media_tools.cpp
    src/torrent/torrent_engine.cpp
    src/torrent/torrent_metadata.cpp)
target_include_directories(maskpilot_infrastructure PUBLIC include)
target_link_libraries(maskpilot_infrastructure PUBLIC maskpilot_application Qt6::Core
    PRIVATE CURL::libcurl unofficial::sqlite3::sqlite3 LibtorrentRasterbar::torrent-rasterbar)
if(WIN32)
  target_link_libraries(maskpilot_infrastructure PRIVATE bcrypt advapi32)
endif()
