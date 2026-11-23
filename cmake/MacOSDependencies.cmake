# Use the explicitly selected native SDK, never the reduced system curl/SQLite.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  message(FATAL_ERROR "macOS dependencies require a native Darwin toolchain")
endif()
find_package(CURL 8.22 REQUIRED COMPONENTS HTTP HTTPS FTP FTPS SFTP)
find_package(SQLite3 REQUIRED)
if(NOT TARGET unofficial::sqlite3::sqlite3)
  add_library(unofficial::sqlite3::sqlite3 ALIAS SQLite::SQLite3)
endif()
find_package(LibXml2 2.14 REQUIRED)
find_package(OpenSSL 3 REQUIRED)
include(CheckCXXSourceRuns)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
set(CMAKE_REQUIRED_LIBRARIES OpenSSL::Crypto)
# Recheck after an SDK or inherited compiler environment changes in this build.
unset(MASKPILOT_CRYPTO_SDK_MATCH CACHE)
check_cxx_source_runs("#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include <cstdio>
int main() {
  const auto runtime = OpenSSL_version_num() >> 28;
  if (runtime == OPENSSL_VERSION_MAJOR) return 0;
  std::fprintf(stderr, \"Crypto SDK major mismatch: headers %u, runtime %lu\",
               static_cast<unsigned>(OPENSSL_VERSION_MAJOR), runtime);
  std::fputc(10, stderr);
  return 1;
}" MASKPILOT_CRYPTO_SDK_MATCH)
cmake_pop_check_state()
if(NOT MASKPILOT_CRYPTO_SDK_MATCH)
  message(FATAL_ERROR
    "Native crypto headers and runtime must match. "
    "Select an explicit macOS SDK (CMAKE_OSX_SYSROOT=macosx), clear inherited header search paths, "
    "and use a consistent OpenSSL SDK; inspect CMakeConfigureLog.yaml.")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/TorrentDependency.cmake")
maskpilot_find_torrent()
