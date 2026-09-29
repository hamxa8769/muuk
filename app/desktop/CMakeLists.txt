add_executable(muuk-desktop
    src/main.cpp
    src/live_controller.cpp
    src/dashboard_window.cpp
    src/glass_ui.cpp
    src/config_discovery.cpp
    src/open_path.cpp
    src/app_prefs.cpp
    src/system_monitor.cpp
)

target_link_libraries(muuk-desktop PRIVATE muuk_ctrader_fix muuk_mt5_bridge imgui)
if(WIN32)
    # dwmapi: dark native title bar to match the app theme; shell32: open PDFs/folders.
    target_link_libraries(muuk-desktop PRIVATE dwmapi shell32 psapi wtsapi32)
endif()
target_compile_features(muuk-desktop PRIVATE cxx_std_20)
