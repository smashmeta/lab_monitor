find_package(GTest CONFIG REQUIRED)

function(lm_add_test target)
  cmake_parse_arguments(ARG "" "" "SOURCES;LINK" ${ARGN})
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE
    ${ARG_LINK} GTest::gtest GTest::gmock GTest::gtest_main lm_warnings)
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES FAIL_REGULAR_EXPRESSION "Running 0 tests from 0 test suites")
  copy_runtime_dependencies(${target})
endfunction()
