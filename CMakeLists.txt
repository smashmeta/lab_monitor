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
# Default ON, unlike the integration tests: these are meant to be *run by hand*
# against a live fleet, and one that has to be enabled first is one nobody
# remembers exists. They need Qt, so they follow LM_BUILD_GUI.
option(LM_BUILD_TOOLS "Build the hand-driven verification tools under tools/" ON)

# The four libraries are SHARED so that a test binary and an application load
# the *same* lm_core.dll, rather than each statically linking its own copy of
# the code and its own copy of every global inside it. What the tests exercise
# is then literally the file that ships.
#
# On Windows that needs symbols exported. Rather than annotate every public
# class with a dllexport macro, CMake generates a .def file listing what each
# DLL defines.
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
# Keep every DLL, import library and executable in one place per configuration.
# Without this each target builds into its own directory and no test binary can
# find the libraries it links, since Windows resolves DLLs from the executable's
# own directory rather than from anything recorded at link time.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

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
  add_subdirectory(apps/server)

  if(LM_BUILD_TOOLS)
    add_subdirectory(tools/shopping_cart)
  endif()
endif()
