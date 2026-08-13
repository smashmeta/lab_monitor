find_package(Qt5 COMPONENTS Widgets Test REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(CMAKE_AUTOMOC ON)

add_executable(lab_monitor_server WIN32
  main.cpp
  server_controller.cpp
  status_ribbon.cpp
  fleet_window.cpp)

target_include_directories(lab_monitor_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lab_monitor_server PRIVATE
  lm_core lm_platform lm_transport lm_ui
  Qt5::Widgets Boost::program_options spdlog::spdlog nlohmann_json::nlohmann_json lm_warnings)

copy_runtime_dependencies(lab_monitor_server)

# ServerController's config load/save and publish bookkeeping are plain logic
# and worth testing; the widgets are not built here, so no display is needed.
lm_add_test(lab_monitor_server_tests
  NO_GTEST_MAIN
  SOURCES ${CMAKE_SOURCE_DIR}/libs/ui/tests/main.cpp
          tests/test_server_controller.cpp
          server_controller.cpp
  LINK lm_core lm_platform lm_transport lm_ui
       Qt5::Widgets Qt5::Test spdlog::spdlog nlohmann_json::nlohmann_json)
target_include_directories(lab_monitor_server_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
