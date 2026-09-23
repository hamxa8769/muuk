add_library(muuk_backtester STATIC
    src/tick_data_source.cpp
    src/stats_collector.cpp
    src/backtest_runner.cpp
)

target_include_directories(muuk_backtester PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(muuk_backtester PUBLIC muuk_core)
target_compile_features(muuk_backtester PUBLIC cxx_std_20)

add_executable(muuk-backtest cli/main.cpp)
target_link_libraries(muuk-backtest PRIVATE muuk_backtester muuk_config_json nlohmann_json::nlohmann_json)

if(MUUK_BUILD_TESTS)
    add_executable(muuk_backtester_tests
        tests/test_tick_data_source.cpp
        tests/test_backtest_runner.cpp
    )
    target_link_libraries(muuk_backtester_tests PRIVATE muuk_backtester Catch2::Catch2WithMain)
    target_compile_definitions(muuk_backtester_tests PRIVATE
        MUUK_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
    )
    include(Catch)
    catch_discover_tests(muuk_backtester_tests)
endif()
