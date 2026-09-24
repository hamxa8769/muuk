add_library(muuk_ctrader_fix STATIC
    src/fix_message.cpp
    src/fix_session.cpp
    src/messages.cpp
    src/live_config.cpp
    src/live_runner.cpp
    src/basket_history.cpp
    src/live_state.cpp
    src/volume_stats.cpp
    src/pdf_writer.cpp
    src/session_report.cpp
)

target_include_directories(muuk_ctrader_fix PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(muuk_ctrader_fix PUBLIC muuk_core muuk_config_json nlohmann_json::nlohmann_json asio)
target_compile_features(muuk_ctrader_fix PUBLIC cxx_std_20)

add_executable(muuk-live cli/main.cpp)
target_link_libraries(muuk-live PRIVATE muuk_ctrader_fix)

if(MUUK_BUILD_TESTS)
    add_executable(muuk_ctrader_fix_tests
        tests/mock_fix_server.cpp
        tests/test_fix_message.cpp
        tests/test_fix_session.cpp
        tests/test_live_runner.cpp
        tests/test_live_resume.cpp
        tests/test_reporting.cpp
    )
    target_link_libraries(muuk_ctrader_fix_tests PRIVATE muuk_ctrader_fix Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(muuk_ctrader_fix_tests)
endif()
