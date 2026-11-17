add_library(maskpilot_ui src/windows/main_window.cpp
    src/dialogs/settings_dialog.cpp
    src/dialogs/media_support_dialog.cpp
    src/dialogs/task_management_dialogs.cpp src/dialogs/add_download_dialog.cpp)
target_include_directories(maskpilot_ui PUBLIC include)
target_link_libraries(maskpilot_ui PUBLIC maskpilot_application Qt6::Widgets PRIVATE Qt6::Network)
