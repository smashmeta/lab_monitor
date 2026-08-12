find_package(nlohmann_json CONFIG REQUIRED)

add_library(lm_core STATIC
  src/types.cpp)
add_library(lm::core ALIAS lm_core)

target_include_directories(lm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_core PUBLIC nlohmann_json::nlohmann_json PRIVATE lm_warnings)

lm_add_test(lm_core_tests SOURCES tests/test_smoke.cpp LINK lm_core)
