# Keep the private part-file API and fallback archive identical across native SDKs.
function(maskpilot_find_torrent)
  find_package(LibtorrentRasterbar 2.1.2 CONFIG QUIET)
  if(LibtorrentRasterbar_FOUND)
    return()
  endif()
  option(MASKPILOT_FETCH_LIBTORRENT "Build the pinned compatible BT library if it is not installed" ON)
  if(NOT MASKPILOT_FETCH_LIBTORRENT)
    message(FATAL_ERROR "This platform requires libtorrent >=2.1.2; supply CMAKE_PREFIX_PATH or enable MASKPILOT_FETCH_LIBTORRENT")
  endif()
  include(FetchContent)
  set(BUILD_SHARED_LIBS ON)
  set(build_tests OFF)
  set(build_examples OFF)
  set(build_tools OFF)
  set(python-bindings OFF)
  set(webtorrent OFF)
  FetchContent_Declare(maskpilot_torrent
    URL https://github.com/arvidn/libtorrent/releases/download/v2.1.2/libtorrent-rasterbar-2.1.2.tar.gz
    URL_HASH SHA256=3362546d9cd71b9e49ee6cac7d3f1f914ce9cdb217c86b63d5b22cbed0334dbc
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE)
  FetchContent_MakeAvailable(maskpilot_torrent)
  add_library(LibtorrentRasterbar::torrent-rasterbar ALIAS torrent-rasterbar)
endfunction()
