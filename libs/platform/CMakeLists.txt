add_library(lm_platform SHARED
  src/host_probes.cpp)
add_library(lm::platform ALIAS lm_platform)

if(WIN32)
  target_sources(lm_platform PRIVATE
    src/windows/platform_windows.cpp
    src/windows/resource_probe_windows.cpp
    src/windows/registry_probe_windows.cpp
    src/windows/process_probe_windows.cpp
    src/windows/network_probe_windows.cpp)
else()
  target_sources(lm_platform PRIVATE
    src/linux/platform_linux.cpp
    src/linux/resource_probe_linux.cpp)
endif()

target_include_directories(lm_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_platform PUBLIC lm_core PRIVATE lm_warnings)

if(WIN32)
  # ole32 for INetConnectionManager, which is the API behind the Network
  # Connections folder and so decides which adapters are listed at all;
  # iphlpapi for the interface types it cannot distinguish; rasapi32 for the
  # phonebook, the only place a disconnected RAS entry exists in the fallback.
  target_link_libraries(lm_platform PRIVATE iphlpapi rasapi32 ole32)
endif()

set(lm_platform_test_sources
  tests/test_host_probes.cpp
  tests/test_resource_probe.cpp)

if(WIN32)
  # These exercise the real Windows registry and process list, so they only
  # build and run on Windows.
  list(APPEND lm_platform_test_sources
    tests/test_registry_probe_windows.cpp
    tests/test_process_probe_windows.cpp
    tests/test_network_probe_windows.cpp
    tests/test_ras_entries_windows.cpp
    tests/test_compliance_end_to_end_windows.cpp)
endif()

lm_add_test(lm_platform_tests
  SOURCES ${lm_platform_test_sources}
  LINK lm_platform)

if(WIN32)
  # test_ras_entries_windows.cpp reaches into src/windows for the phonebook
  # enumeration, and links RAS itself since lm_platform keeps that private.
  target_include_directories(lm_platform_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(lm_platform_tests PRIVATE rasapi32)
endif()
