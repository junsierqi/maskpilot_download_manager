add_library(maskpilot_ui src/windows/main_window.cpp
    src/dialogs/site_project_dialog.cpp
    src/dialogs/media_selection_widget.cpp src/dialogs/media_selection_widget.h
    src/dialogs/settings_dialog.cpp
    src/dialogs/completion_scan_settings.cpp src/dialogs/completion_scan_dialog.cpp
    src/dialogs/media_support_dialog.cpp
    src/dialogs/task_management_dialogs.cpp src/dialogs/add_download_dialog.cpp
    src/dialogs/context_dialog.cpp src/dialogs/queue_dialog.cpp src/models/task_list_codec.cpp
    src/dialogs/torrent_dialog.cpp src/style/appearance.cpp)
target_include_directories(maskpilot_ui PUBLIC include)
target_link_libraries(maskpilot_ui PUBLIC maskpilot_application Qt6::Widgets PRIVATE Qt6::Network)
