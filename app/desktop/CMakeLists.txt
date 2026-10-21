add_executable(muuk-desktop
    src/main.cpp
    src/live_controller.cpp
    src/dashboard_window.cpp
    src/glass_ui.cpp
    src/config_discovery.cpp
    src/open_path.cpp
    src/app_prefs.cpp
    src/system_monitor.cpp
    src/windows_startup.cpp
)

target_link_libraries(muuk-desktop PRIVATE muuk_ctrader_fix muuk_mt5_bridge imgui)
if(WIN32)
    # A Windows desktop app: no console window next to it, the Muuk icon and
    # version info. main() stays the entry point; started from a terminal it
    # still prints there (AttachConsole in main.cpp).
    target_sources(muuk-desktop PRIVATE res/muuk.rc)
    set_target_properties(muuk-desktop PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MSVC)
        target_link_options(muuk-desktop PRIVATE /ENTRY:mainCRTStartup)
    endif()
    # dwmapi: dark native title bar to match the app theme; shell32: open PDFs/folders.
    # ole32/uuid: the desktop / Start menu shortcuts (IShellLink).
    target_link_libraries(muuk-desktop PRIVATE dwmapi shell32 psapi wtsapi32 ole32 uuid)
endif()
target_compile_features(muuk-desktop PRIVATE cxx_std_20)
