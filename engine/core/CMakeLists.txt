add_library(muuk_core STATIC
    src/lot_progression.cpp
    src/pnl.cpp
    src/risk.cpp
    src/strategy_engine.cpp
)

target_include_directories(muuk_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(muuk_core PUBLIC cxx_std_20)

if(MUUK_BUILD_TESTS)
    add_executable(muuk_core_tests
        tests/test_types.cpp
        tests/test_lot_progression.cpp
        tests/test_pnl.cpp
        tests/test_risk.cpp
        tests/test_strategy_engine.cpp
    )
    target_link_libraries(muuk_core_tests PRIVATE muuk_core Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(muuk_core_tests)
endif()

if(MUUK_BUILD_BENCHMARKS)
    add_executable(muuk_core_bench
        bench/bench_strategy_engine.cpp
    )
    target_link_libraries(muuk_core_bench PRIVATE muuk_core benchmark::benchmark)
endif()
