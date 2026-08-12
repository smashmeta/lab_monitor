cmake_minimum_required(VERSION 3.28)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

project(lab_monitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

option(LM_BUILD_GUI "Build the Qt GUI applications" ON)
option(LM_BUILD_INTEGRATION_TESTS "Build tests that open a real DDS domain" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")
include(LabMonitorWarnings)
include(LabMonitorRuntime)
include(LabMonitorTesting)

if(WIN32)
  add_compile_definitions(_WIN32_WINNT=0x0601 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

enable_testing()

add_subdirectory(libs/core)
add_subdirectory(libs/platform)
add_subdirectory(libs/transport)

if(LM_BUILD_GUI)
  add_subdirectory(libs/ui)
  add_subdirectory(apps/client)
endif()
