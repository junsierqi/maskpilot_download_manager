# Use distribution development packages without imposing Windows vcpkg target names.
find_package(CURL REQUIRED COMPONENTS HTTP HTTPS FTP FTPS SFTP)
find_package(SQLite3 REQUIRED)
if(NOT TARGET unofficial::sqlite3::sqlite3)
  add_library(unofficial::sqlite3::sqlite3 ALIAS SQLite::SQLite3)
endif()
# The offline site parser relies on the per-context resource-loader API.
find_package(LibXml2 2.14 REQUIRED)
