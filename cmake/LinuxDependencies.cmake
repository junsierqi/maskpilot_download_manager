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
include("${CMAKE_CURRENT_LIST_DIR}/TorrentDependency.cmake")
maskpilot_find_torrent()
