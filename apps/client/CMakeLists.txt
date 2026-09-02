find_package(Qt6 COMPONENTS Widgets Test REQUIRED)
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
  Qt6::Widgets Boost::program_options spdlog::spdlog lm_warnings)

copy_runtime_dependencies(lab_monitor_client)

lm_add_test(lab_monitor_client_tests
  NO_GTEST_MAIN
  # widget_main.cpp, not main.cpp: DetailWindow is a real widget, so this needs
  # a QApplication and a platform plugin (see libs/ui/tests/widget_main.cpp for
  # why offscreen is not an option on Windows).
  SOURCES ${CMAKE_SOURCE_DIR}/libs/ui/tests/widget_main.cpp
          tests/test_monitor_worker.cpp
          tests/test_detail_window.cpp
          tests/test_script_execution.cpp
          monitor_worker.cpp
          detail_window.cpp
  # spdlog::spdlog is required here, beyond the brief's original LINK list:
  # monitor_worker.cpp (compiled into this test binary too) calls
  # spdlog::error(), and spdlog's vcpkg build is not header-only -- its
  # fmt-backed formatting symbols only resolve once spdlog::spdlog is
  # actually linked, not merely reachable via vcpkg's global include path.
  # lm_ui joins the list because RuleDetail/describe() moved there, so both
  # apps can render rules the same way rather than the client owning it.
  # Qt6::Widgets for DetailWindow itself, Qt6::Test for QSignalSpy and QTest's
  # key simulation against the confirmation dialog.
  LINK lm_core lm_platform lm_transport lm_ui Qt6::Widgets Qt6::Test spdlog::spdlog)
target_include_directories(lab_monitor_client_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
