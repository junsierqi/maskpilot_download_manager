add_library(maskpilot_domain src/task.cpp src/torrent.cpp)
configure_file(include/maskpilot/domain/version.h.in
    ${PROJECT_BINARY_DIR}/generated/maskpilot/domain/version.h @ONLY)
target_include_directories(maskpilot_domain PUBLIC include ${PROJECT_BINARY_DIR}/generated)
target_link_libraries(maskpilot_domain PUBLIC maskpilot_options)
