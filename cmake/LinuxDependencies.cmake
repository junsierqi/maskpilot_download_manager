# Use distribution development packages without imposing Windows vcpkg target names.
find_package(CURL REQUIRED COMPONENTS HTTP HTTPS FTP FTPS SFTP)
find_package(SQLite3 REQUIRED)
if(NOT TARGET unofficial::sqlite3::sqlite3)
  add_library(unofficial::sqlite3::sqlite3 ALIAS SQLite::SQLite3)
endif()
# The offline site parser relies on the per-context resource-loader API.
find_package(LibXml2 2.14 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

# The 2.1 API provides the private part-file layout used by task publication.
# Do not silently substitute distribution 2.0 packages and lose that contract.
function(maskpilot_linux_torrent)
  find_package(LibtorrentRasterbar 2.1.2 CONFIG QUIET)
  if(LibtorrentRasterbar_FOUND)
    return()
  endif()
  option(MASKPILOT_FETCH_LIBTORRENT "Build the pinned compatible BT library if it is not installed" ON)
  if(NOT MASKPILOT_FETCH_LIBTORRENT)
    message(FATAL_ERROR "Linux requires libtorrent >=2.1.2; supply its CMAKE_PREFIX_PATH or enable MASKPILOT_FETCH_LIBTORRENT")
  endif()
  include(FetchContent)
  # Function scope keeps upstream options from changing our own library types.
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
maskpilot_linux_torrent()
