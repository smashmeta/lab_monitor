find_package(fastcdr CONFIG REQUIRED)
# fastdds's exported CMake config links nlohmann_json::nlohmann_json; that
# target is only visible in the directory scope that resolved it, so it must
# be found here too even though lm_core already depends on it.
find_package(nlohmann_json CONFIG REQUIRED)
find_package(fastdds CONFIG REQUIRED)

add_library(lm_transport SHARED
  src/in_memory_transport.cpp
  src/codec.cpp
  src/fastdds/fast_dds_transport.cpp
  src/fastdds/dds_probe.cpp)
add_library(lm::transport ALIAS lm_transport)

target_include_directories(lm_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# lm_platform is PUBLIC because dds_probe.hpp returns a lm::platform::IDdsProbe:
# the interface is declared there (DDS-free, so lm_platform still depends only
# on lm_core) while the implementation lives here, where Fast DDS is linked.
target_link_libraries(lm_transport PUBLIC lm_core lm_platform PRIVATE fastdds fastcdr lm_warnings)

lm_add_test(lm_transport_tests
  SOURCES tests/test_in_memory_transport.cpp tests/test_codec.cpp
  LINK lm_transport)

if(LM_BUILD_INTEGRATION_TESTS)
  # Qt and spdlog are here only for the end-to-end script case below, which
  # drives the client's real MonitorWorker. Nothing in lm_transport itself
  # touches either.
  find_package(Qt6 COMPONENTS Core REQUIRED)
  find_package(spdlog CONFIG REQUIRED)

  # The malformed-payload test talks to Fast DDS directly (a hand-rolled
  # TopicDataType/participant/writer), not just through lm_transport, so it
  # needs fastdds linked itself -- lm_transport only links it PRIVATE.
  #
  # It also carries the one case that runs the whole stack: a real domain, the
  # real MonitorWorker, the real PowerShell runner. That is why an
  # apps/client source is compiled into a libs/ test target here, which is
  # otherwise backwards -- the worker is the *client half* of this loopback,
  # and the alternative was a second integration target whose only job was to
  # hold one test. lm_ui comes with it because MonitorWorker's report signal
  # carries lm::ui::RuleDetail.
  lm_add_test(lm_transport_dds_tests
    SOURCES tests/test_fastdds_loopback.cpp
            ${CMAKE_SOURCE_DIR}/apps/client/monitor_worker.cpp
    LINK lm_transport lm_ui fastdds Qt6::Core spdlog::spdlog)
  target_include_directories(lm_transport_dds_tests PRIVATE ${CMAKE_SOURCE_DIR}/apps/client)
  # MonitorWorker is a Q_OBJECT, and this directory does not set CMAKE_AUTOMOC
  # globally the way the app directories do.
  set_target_properties(lm_transport_dds_tests PROPERTIES AUTOMOC ON)

  # The one test that proves the no-IDL claim end to end: it publishes a type
  # built at runtime and reads it back through the probe, which has no
  # compile-time knowledge of it either. Gated with the rest because it opens a
  # real domain.
  lm_add_test(lm_dds_probe_tests
    SOURCES tests/test_dds_probe_loopback.cpp
    LINK lm_transport fastdds)
endif()
