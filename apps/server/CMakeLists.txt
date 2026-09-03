find_package(Qt6 COMPONENTS Widgets Test REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(CMAKE_AUTOMOC ON)

add_executable(lab_monitor_server WIN32
  main.cpp
  server_controller.cpp
  status_ribbon.cpp
  add_rule_dialog.cpp
  fleet_window.cpp
  scripts_tab.cpp
  script_run.cpp
  script_library.cpp)

target_include_directories(lab_monitor_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lab_monitor_server PRIVATE
  lm_core lm_platform lm_transport lm_ui
  Qt6::Widgets Boost::program_options spdlog::spdlog nlohmann_json::nlohmann_json lm_warnings)

copy_runtime_dependencies(lab_monitor_server)

# ServerController's config load/save and publish bookkeeping are plain logic,
# and so is a surprising amount of what FleetWindow does — which templates may
# be created, what an assignment edit means. Both are built here, so this needs
# a QApplication and a platform plugin (see libs/ui/tests/widget_main.cpp for
# why offscreen is not an option on Windows).
lm_add_test(lab_monitor_server_tests
  NO_GTEST_MAIN
  SOURCES ${CMAKE_SOURCE_DIR}/libs/ui/tests/widget_main.cpp
          tests/test_server_controller.cpp
          tests/test_fleet_window.cpp
          tests/test_add_rule_dialog.cpp
          tests/test_script_run.cpp
          tests/test_script_dispatch.cpp
          tests/test_scripts_tab.cpp
          tests/test_script_library.cpp
          server_controller.cpp
          status_ribbon.cpp
          add_rule_dialog.cpp
          fleet_window.cpp
          scripts_tab.cpp
          script_run.cpp
          script_library.cpp
  LINK lm_core lm_platform lm_transport lm_ui
       Qt6::Widgets Qt6::Test spdlog::spdlog nlohmann_json::nlohmann_json)
# libs/ui/tests for pixel_probe.hpp: the run view's outcome colours survive
# selection only because of KeepForegroundDelegate, and a stylesheet overriding
# a delegate is visible to nothing but a painted image.
target_include_directories(lab_monitor_server_tests
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libs/ui/tests)
