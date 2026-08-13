add_library(lm_platform STATIC
  src/host_probes.cpp)
add_library(lm::platform ALIAS lm_platform)

if(WIN32)
  target_sources(lm_platform PRIVATE
    src/windows/platform_windows.cpp
    src/windows/resource_probe_windows.cpp)
else()
  target_sources(lm_platform PRIVATE
    src/linux/platform_linux.cpp
    src/linux/resource_probe_linux.cpp)
endif()

target_include_directories(lm_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_platform PUBLIC lm_core PRIVATE lm_warnings)

lm_add_test(lm_platform_tests
  SOURCES tests/test_host_probes.cpp tests/test_resource_probe.cpp
  LINK lm_platform)
