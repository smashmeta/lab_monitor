find_package(Qt6 COMPONENTS Widgets Test REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(spdlog CONFIG REQUIRED)

set(CMAKE_AUTOMOC ON)

add_executable(lab_monitor_client WIN32
  main.cpp
  monitor_worker.cpp
  tray_controller.cpp
  detail_window.cpp)

# autostart is the Scheduled Task mechanism that obtains elevation without a
# UAC prompt -- see autostart.hpp. It is genuinely Windows-only (schtasks.exe,
# /RL HIGHEST); the stub keeps the target linking on the Linux leg, which has
# never been compiled (see CLAUDE.md, Known gaps) but is still configured.
if(WIN32)
  target_sources(lab_monitor_client PRIVATE autostart_windows.cpp)
else()
  target_sources(lab_monitor_client PRIVATE autostart_stub.cpp)
endif()

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

if(LM_BUILD_INTEGRATION_TESTS)
  # The one case that runs the whole stack: a real DDS domain, the real
  # MonitorWorker on its own thread, and the real PowerShell runner. It belongs
  # here rather than beside the transport's loopback tests because the client
  # half of the loop is MonitorWorker -- this directory already sets AUTOMOC and
  # already links everything it needs. Gated with the rest because it opens a
  # real domain.
  lm_add_test(lab_monitor_client_dds_tests
    SOURCES tests/test_script_loopback.cpp monitor_worker.cpp
    LINK lm_core lm_platform lm_transport lm_ui Qt6::Widgets spdlog::spdlog)
  target_include_directories(lab_monitor_client_dds_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
