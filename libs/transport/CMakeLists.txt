find_package(fastcdr CONFIG REQUIRED)
# fastdds's exported CMake config links nlohmann_json::nlohmann_json; that
# target is only visible in the directory scope that resolved it, so it must
# be found here too even though lm_core already depends on it.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(fastdds CONFIG REQUIRED)

add_library(lm_transport STATIC
  src/in_memory_transport.cpp
  src/codec.cpp
  src/fastdds/fast_dds_transport.cpp)
add_library(lm::transport ALIAS lm_transport)

target_include_directories(lm_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_transport PUBLIC lm_core PRIVATE fastdds fastcdr lm_warnings)

lm_add_test(lm_transport_tests
  SOURCES tests/test_in_memory_transport.cpp tests/test_codec.cpp
  LINK lm_transport)

if(LM_BUILD_INTEGRATION_TESTS)
  # The malformed-payload test talks to Fast DDS directly (a hand-rolled
  # TopicDataType/participant/writer), not just through lm_transport, so it
  # needs fastdds linked itself -- lm_transport only links it PRIVATE.
  lm_add_test(lm_transport_dds_tests
    SOURCES tests/test_fastdds_loopback.cpp
    LINK lm_transport fastdds)
endif()
