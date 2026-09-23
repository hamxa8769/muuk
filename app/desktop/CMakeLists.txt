add_executable(muuk-desktop
    src/main.cpp
    src/live_controller.cpp
    src/dashboard_window.cpp
    src/glass_ui.cpp
    src/config_discovery.cpp
)

target_link_libraries(muuk-desktop PRIVATE muuk_ctrader_fix imgui)
if(WIN32)
    # dwmapi: dark native title bar to match the app theme.
    target_link_libraries(muuk-desktop PRIVATE dwmapi)
endif()
target_compile_features(muuk-desktop PRIVATE cxx_std_20)
