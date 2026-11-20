add_executable(maskpilot_tests unit/domain_test.cpp integration/engine_test.cpp integration/credential_vault_test.cpp integration/context_test.cpp unit/context_ui_test.cpp unit/task_list_test.cpp)
target_sources(maskpilot_tests PRIVATE unit/http_layout_test.cpp)
target_sources(maskpilot_tests PRIVATE unit/browser_recovery_test.cpp)
target_sources(maskpilot_tests PRIVATE unit/browser_recovery_ui_test.cpp)
target_sources(maskpilot_tests PRIVATE unit/completion_scan_test.cpp)
target_sources(maskpilot_tests PRIVATE integration/media_auth_test.cpp)
target_sources(maskpilot_tests PRIVATE integration/http_post_test.cpp)
find_package(OpenSSL REQUIRED)
target_link_libraries(maskpilot_tests PRIVATE OpenSSL::Crypto)
target_sources(maskpilot_tests PRIVATE unit/completion_scan_ui_test.cpp)
add_dependencies(maskpilot_tests maskpilot)
target_compile_definitions(maskpilot_tests PRIVATE MASKPILOT_TEST_DESKTOP="$<TARGET_FILE:maskpilot>")
add_executable(maskpilot_scan_probe support/scan_probe.cpp)
target_link_libraries(maskpilot_scan_probe PRIVATE Qt6::Core maskpilot_options)
add_dependencies(maskpilot_tests maskpilot_scan_probe)
target_compile_definitions(maskpilot_tests PRIVATE MASKPILOT_TEST_SCAN_PROBE="$<TARGET_FILE:maskpilot_scan_probe>")
target_sources(maskpilot_tests PRIVATE unit/queue_test.cpp integration/queue_store_test.cpp unit/queue_ui_test.cpp)
target_sources(maskpilot_tests PRIVATE unit/queue_schedule_test.cpp)
target_sources(maskpilot_tests PRIVATE unit/traffic_budget_test.cpp integration/traffic_store_test.cpp)
target_sources(maskpilot_tests PRIVATE unit/media_selection_test.cpp unit/media_ui_test.cpp integration/media_store_test.cpp)
target_sources(maskpilot_tests PRIVATE unit/transfer_constraints_test.cpp unit/transfer_constraints_ui_test.cpp integration/transfer_constraints_store_test.cpp)
target_sources(maskpilot_tests PRIVATE unit/response_metadata_test.cpp unit/site_content_test.cpp integration/response_metadata_store_test.cpp)
target_sources(maskpilot_tests PRIVATE unit/project_task_test.cpp unit/site_project_test.cpp integration/site_project_store_test.cpp)
add_executable(maskpilot_site_project_system_tests integration/site_project_system_test.cpp)
target_link_libraries(maskpilot_site_project_system_tests PRIVATE maskpilot_ui maskpilot_infrastructure Qt6::Network)
add_executable(maskpilot_site_content_probe support/site_content_probe.cpp)
target_link_libraries(maskpilot_site_content_probe PRIVATE maskpilot_infrastructure)
add_executable(maskpilot_media_system_tests integration/media_ui_system_test.cpp)
add_dependencies(maskpilot_media_system_tests maskpilot_media_ffmpeg maskpilot_media_ffprobe)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # GNU ld scans static archives once. The application site coordinator calls
  # policy functions supplied by the content adapter; resolve the complete group.
  target_link_libraries(maskpilot_media_system_tests PRIVATE
    "$<LINK_GROUP:RESCAN,maskpilot_ui,maskpilot_application,maskpilot_infrastructure,maskpilot_domain>"
    Qt6::Network)
else()
  target_link_libraries(maskpilot_media_system_tests PRIVATE maskpilot_ui maskpilot_infrastructure Qt6::Network)
endif()
target_link_libraries(maskpilot_tests PRIVATE Qt6::Network)
target_link_libraries(maskpilot_tests PRIVATE maskpilot_application maskpilot_infrastructure GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(maskpilot_tests DISCOVERY_MODE PRE_TEST WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME linux_package_product_path COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/system/linux_package_test.py --build-directory ${CMAKE_BINARY_DIR})
  set_tests_properties(linux_package_product_path PROPERTIES TIMEOUT 240 RUN_SERIAL TRUE)
  add_executable(maskpilot_linux_platform_tests linux/platform_test.cpp)
  target_compile_definitions(maskpilot_linux_platform_tests PRIVATE QT_NO_KEYWORDS)
  target_link_libraries(maskpilot_linux_platform_tests PRIVATE maskpilot_infrastructure GTest::gtest PkgConfig::LIBSECRET)
  add_test(NAME linux_platform_product_path COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/linux/platform_fixture.py --exercise $<TARGET_FILE:maskpilot_linux_platform_tests>)
  set_tests_properties(linux_platform_product_path PROPERTIES TIMEOUT 90)
endif()
execute_process(COMMAND ${Python3_EXECUTABLE} -c "import paramiko"
  RESULT_VARIABLE sftp_fixture_result OUTPUT_QUIET ERROR_QUIET)
if(NOT sftp_fixture_result EQUAL 0)
  message(FATAL_ERROR "SFTP tests require: ${Python3_EXECUTABLE} -m pip install -r ${CMAKE_CURRENT_SOURCE_DIR}/requirements.txt")
endif()
target_sources(maskpilot_tests PRIVATE integration/sftp_test.cpp integration/live_test.cpp)
add_test(NAME live_recording_product_path COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/integration/live_test.py --exercise $<TARGET_FILE:maskpilot_cli>
  --scanner $<TARGET_FILE:maskpilot_scan_probe>)
set_tests_properties(live_recording_product_path PROPERTIES TIMEOUT 180)
target_link_libraries(maskpilot_tests PRIVATE CURL::libcurl)
set_source_files_properties(integration/sftp_test.cpp PROPERTIES COMPILE_DEFINITIONS
  "MASKPILOT_SFTP_TEST_PYTHON=\"${Python3_EXECUTABLE}\";MASKPILOT_SFTP_TEST_FIXTURE=\"${CMAKE_CURRENT_SOURCE_DIR}/support/sftp_fixture.py\"")
add_test(NAME sftp_product_path COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/integration/sftp_test.py --exercise $<TARGET_FILE:maskpilot_cli>
  --scanner $<TARGET_FILE:maskpilot_scan_probe>)
set_tests_properties(sftp_product_path PROPERTIES TIMEOUT 240)
add_test(NAME http_post_product_path COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/integration/http_post_test.py --exercise $<TARGET_FILE:maskpilot_cli>
  --scanner $<TARGET_FILE:maskpilot_scan_probe>)
set_tests_properties(http_post_product_path PROPERTIES TIMEOUT 180)
find_program(MASKPILOT_TEST_MEDIA_MUXER ffmpeg)
set(MASKPILOT_TEST_MEDIA_PARSER "${PROJECT_SOURCE_DIR}/out/tools/yt-dlp.exe" CACHE FILEPATH "Optional media test parser")
if(EXISTS "${MASKPILOT_TEST_MEDIA_PARSER}" AND MASKPILOT_TEST_MEDIA_MUXER)
  set_source_files_properties(integration/media_auth_test.cpp PROPERTIES COMPILE_DEFINITIONS
    "MASKPILOT_AUTH_TEST_PARSER=\"${MASKPILOT_TEST_MEDIA_PARSER}\";MASKPILOT_AUTH_TEST_MUXER=\"${MASKPILOT_TEST_MEDIA_MUXER}\"")
  add_test(NAME authenticated_media_product_path COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/integration/media_auth_test.py
    --exercise $<TARGET_FILE:maskpilot_cli> --parser ${MASKPILOT_TEST_MEDIA_PARSER}
    --muxer ${MASKPILOT_TEST_MEDIA_MUXER})
  set_tests_properties(authenticated_media_product_path PROPERTIES TIMEOUT 300)
endif()
add_test(NAME local_http_product_path COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/support/http_fixture.py --exercise $<TARGET_FILE:maskpilot_cli>)
set_tests_properties(local_http_product_path PROPERTIES TIMEOUT 120)
add_test(NAME browser_recovery_product_path COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/integration/browser_recovery_test.py
  --desktop $<TARGET_FILE:maskpilot> --host $<TARGET_FILE:maskpilot_native_host>)
set_tests_properties(browser_recovery_product_path PROPERTIES TIMEOUT 120)
add_test(NAME adaptive_http_product_path COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/adaptive_http_test.py --exercise $<TARGET_FILE:maskpilot_cli>)
set_tests_properties(adaptive_http_product_path PROPERTIES TIMEOUT 150)
add_test(NAME native_payload_budget_product_path COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/native_budget_test.py --exercise $<TARGET_FILE:maskpilot_cli>)
set_tests_properties(native_payload_budget_product_path PROPERTIES TIMEOUT 150)
add_test(NAME completion_scan_product_path COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_CURRENT_SOURCE_DIR}/integration/completion_scan_test.py
  --exercise $<TARGET_FILE:maskpilot_cli> --scanner $<TARGET_FILE:maskpilot_scan_probe>)
set_tests_properties(completion_scan_product_path PROPERTIES TIMEOUT 180)
add_test(NAME bounded_http_product_path COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/transfer_constraints_test.py --exercise $<TARGET_FILE:maskpilot_cli>)
set_tests_properties(bounded_http_product_path PROPERTIES TIMEOUT 180)
add_test(NAME http_response_metadata_product_path COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/response_metadata_test.py --exercise $<TARGET_FILE:maskpilot_cli>)
set_tests_properties(http_response_metadata_product_path PROPERTIES TIMEOUT 180)
add_test(NAME site_project_product_path COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration/site_project_test.py --exercise $<TARGET_FILE:maskpilot_cli>)
set_tests_properties(site_project_product_path PROPERTIES TIMEOUT 300)
add_test(NAME site_project_qt_product_path COMMAND $<TARGET_FILE:maskpilot_site_project_system_tests>)
set_tests_properties(site_project_qt_product_path PROPERTIES TIMEOUT 180)
if(WIN32)
  find_program(MASKPILOT_TEST_POWERSHELL powershell REQUIRED)
  add_test(NAME browser_registration_ownership COMMAND ${MASKPILOT_TEST_POWERSHELL}
    -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_CURRENT_SOURCE_DIR}/system/browser_registration_test.ps1
    -SourceRoot ${CMAKE_SOURCE_DIR})
  set_tests_properties(browser_registration_ownership PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()
