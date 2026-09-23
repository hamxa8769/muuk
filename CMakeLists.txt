cmake_minimum_required(VERSION 3.24)

project(muuk
    VERSION 0.1.0
    DESCRIPTION "Muuk XAUUSD pending-order strategy engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MUUK_BUILD_TESTS "Build unit tests" ON)
option(MUUK_BUILD_BENCHMARKS "Build micro-benchmarks" ON)
option(MUUK_BUILD_DESKTOP_APP "Build the native desktop status/control app" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /EHsc)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(FetchContent)

if(MUUK_BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(CTest)
    enable_testing()
endif()

if(MUUK_BUILD_BENCHMARKS)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googlebenchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.1
    )
    FetchContent_MakeAvailable(googlebenchmark)
endif()

# nlohmann/json: used by the config_json module (shared JSON schema for
# StrategyConfig) and the ctrader_fix live config loader.
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Standalone (non-Boost) Asio: header-only, used by engine/ctrader_fix for
# TCP sockets. Its own CMakeLists.txt targets building its examples/tests,
# not clean consumption, so it's wired up here as a plain INTERFACE target
# over the fetched headers instead of add_subdirectory()/MakeAvailable's
# default target discovery.
FetchContent_Declare(
    asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-30-2
)
FetchContent_MakeAvailable(asio)
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
find_package(Threads REQUIRED)
target_link_libraries(asio INTERFACE Threads::Threads)
if(WIN32)
    target_compile_definitions(asio INTERFACE _WIN32_WINNT=0x0A00)
    target_link_libraries(asio INTERFACE ws2_32 mswsock)
endif()

# Dear ImGui + GLFW + OpenGL3: the native desktop status/control app
# (app/desktop). GLFW ships its own proper CMake support; ImGui doesn't
# (it's meant to be dropped straight into a consumer's own build), so a
# small static-library target wraps its fetched sources here, the same
# pattern already used for standalone Asio above.
if(MUUK_BUILD_DESKTOP_APP)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        3.4
    )
    FetchContent_MakeAvailable(glfw)

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5
    )
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    find_package(OpenGL REQUIRED)
    target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
endif()

add_subdirectory(engine/core)
add_subdirectory(engine/config_json)
add_subdirectory(engine/backtester)
add_subdirectory(engine/ctrader_fix)

if(MUUK_BUILD_DESKTOP_APP)
    add_subdirectory(app/desktop)
endif()
