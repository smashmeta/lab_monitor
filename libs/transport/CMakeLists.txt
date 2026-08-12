add_library(lm_transport STATIC
  src/in_memory_transport.cpp)
add_library(lm::transport ALIAS lm_transport)

target_include_directories(lm_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_transport PUBLIC lm_core PRIVATE lm_warnings)

lm_add_test(lm_transport_tests
  SOURCES tests/test_in_memory_transport.cpp
  LINK lm_transport)
