add_library(muuk_config_json STATIC
    src/strategy_config_json.cpp
)

target_include_directories(muuk_config_json PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(muuk_config_json PUBLIC muuk_core nlohmann_json::nlohmann_json)
target_compile_features(muuk_config_json PUBLIC cxx_std_20)
