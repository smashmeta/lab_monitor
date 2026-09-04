find_package(nlohmann_json CONFIG REQUIRED)

add_library(lm_core SHARED
  src/types.cpp
  src/version.cpp
  src/rule.cpp
  src/template_bundle.cpp
  src/json.cpp
  src/json_path.cpp
  src/compliance.cpp
  src/fleet.cpp
  src/client_registry.cpp
  src/script.cpp)
add_library(lm::core ALIAS lm_core)

target_include_directories(lm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_core PUBLIC nlohmann_json::nlohmann_json PRIVATE lm_warnings)

lm_add_test(lm_core_tests
  SOURCES
    tests/test_smoke.cpp
    tests/test_version.cpp
    tests/test_capabilities.cpp
    tests/test_template_bundle.cpp
    tests/test_rule_id.cpp
    tests/test_json.cpp
    tests/test_json_path.cpp
    tests/test_evaluate.cpp
    tests/test_evaluate_dds.cpp
    tests/test_fleet.cpp
    tests/test_client_registry.cpp
    tests/test_script_result.cpp
  LINK lm_core)
