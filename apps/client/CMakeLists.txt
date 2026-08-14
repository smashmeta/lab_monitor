find_package(Qt5 COMPONENTS Widgets REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(spdlog CONFIG REQUIRED)

set(CMAKE_AUTOMOC ON)

add_executable(lab_monitor_client WIN32
  main.cpp
  monitor_worker.cpp
  tray_controller.cpp
  detail_window.cpp)

target_include_directories(lab_monitor_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lab_monitor_client PRIVATE
  lm_core lm_platform lm_transport lm_ui
  Qt5::Widgets Boost::program_options spdlog::spdlog lm_warnings)

copy_runtime_dependencies(lab_monitor_client)

lm_add_test(lab_monitor_client_tests
  NO_GTEST_MAIN
  SOURCES ${CMAKE_SOURCE_DIR}/libs/ui/tests/main.cpp
          tests/test_monitor_worker.cpp
          monitor_worker.cpp
  # spdlog::spdlog is required here, beyond the brief's original LINK list:
  # monitor_worker.cpp (compiled into this test binary too) calls
  # spdlog::error(), and spdlog's vcpkg build is not header-only -- its
  # fmt-backed formatting symbols only resolve once spdlog::spdlog is
  # actually linked, not merely reachable via vcpkg's global include path.
  # lm_ui joins the list because RuleDetail/describe() moved there, so both
  # apps can render rules the same way rather than the client owning it.
  LINK lm_core lm_platform lm_transport lm_ui Qt5::Core spdlog::spdlog)
target_include_directories(lab_monitor_client_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
