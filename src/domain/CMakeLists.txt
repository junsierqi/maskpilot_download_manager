add_library(maskpilot_domain src/task.cpp src/torrent.cpp src/proxy.cpp src/download_context.cpp src/queue.cpp src/media.cpp)
target_sources(maskpilot_domain PRIVATE src/site_project.cpp src/http_request.cpp)
configure_file(include/maskpilot/domain/version.h.in
    ${PROJECT_BINARY_DIR}/generated/maskpilot/domain/version.h @ONLY)
target_include_directories(maskpilot_domain PUBLIC include ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(maskpilot_domain PUBLIC maskpilot_options)
