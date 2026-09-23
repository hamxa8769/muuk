add_executable(muuk-desktop
    src/main.cpp
    src/live_controller.cpp
    src/dashboard_window.cpp
)

target_link_libraries(muuk-desktop PRIVATE muuk_ctrader_fix imgui)
target_compile_features(muuk-desktop PRIVATE cxx_std_20)
