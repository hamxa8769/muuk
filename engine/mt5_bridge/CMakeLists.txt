add_library(muuk_mt5_bridge STATIC
    src/mt5_gateway.cpp
)

target_include_directories(muuk_mt5_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(muuk_mt5_bridge PUBLIC muuk_ctrader_fix nlohmann_json::nlohmann_json asio)
target_compile_features(muuk_mt5_bridge PUBLIC cxx_std_20)

if(MUUK_BUILD_TESTS)
    add_executable(muuk_mt5_bridge_tests
        tests/test_mt5_gateway.cpp
    )
    target_include_directories(muuk_mt5_bridge_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../ctrader_fix/tests)
    target_link_libraries(muuk_mt5_bridge_tests PRIVATE muuk_mt5_bridge Catch2::Catch2WithMain)
    target_compile_definitions(muuk_mt5_bridge_tests PRIVATE MUUK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    include(Catch)
    catch_discover_tests(muuk_mt5_bridge_tests)
endif()

# The console bot lives with the FIX code but can also run the MT5 bridge.
add_executable(muuk-live ${CMAKE_CURRENT_SOURCE_DIR}/../ctrader_fix/cli/main.cpp)
target_link_libraries(muuk-live PRIVATE muuk_ctrader_fix muuk_mt5_bridge)
set_target_properties(muuk-live PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/engine/ctrader_fix)
