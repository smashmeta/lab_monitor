find_package(Qt5 COMPONENTS Widgets Svg Test REQUIRED)
find_package(spdlog CONFIG REQUIRED)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

add_library(lm_ui STATIC
  src/theme.cpp
  src/status_pill.cpp
  src/sparkline.cpp
  src/meter_bar.cpp
  src/fleet_model.cpp
  src/keep_foreground_delegate.cpp
  src/rule_detail.cpp
  src/sample_coalescer.cpp
  src/token_edit.cpp
  # Q_OBJECT headers must be listed explicitly: they live under include/, not
  # next to their .cpp under src/, so AUTOMOC's directory-based header
  # discovery would otherwise miss them and silently skip moc generation.
  include/lm/ui/status_pill.hpp
  include/lm/ui/sparkline.hpp
  include/lm/ui/meter_bar.hpp
  include/lm/ui/fleet_model.hpp
  include/lm/ui/keep_foreground_delegate.hpp
  include/lm/ui/sample_coalescer.hpp
  include/lm/ui/token_edit.hpp
  resources/lm_ui.qrc)
add_library(lm::ui ALIAS lm_ui)

target_include_directories(lm_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lm_ui
  PUBLIC lm_core lm_transport Qt5::Widgets Qt5::Svg
  # spdlog is private: only theme.cpp's failed-open log call needs it, and
  # it is not part of any lm_ui public header.
  PRIVATE lm_warnings spdlog::spdlog)

lm_add_test(lm_ui_tests
  NO_GTEST_MAIN
  SOURCES tests/main.cpp tests/test_fleet_model.cpp tests/test_sample_coalescer.cpp tests/test_rule_detail.cpp
  # Qt5::Test brings in QSignalSpy, used throughout these tests; it is not
  # part of lm_ui's own public interface (Widgets/Svg only).
  LINK lm_ui Qt5::Test)

# A separate binary rather than more cases in lm_ui_tests: these construct and
# paint real widgets, so they need a QApplication and a platform plugin,
# whereas every other lm_ui test runs under a QCoreApplication and needs
# neither. Qt5::Test supplies QTest's key/mouse simulation and QSignalSpy.
lm_add_test(lm_ui_widget_tests
  NO_GTEST_MAIN
  SOURCES tests/widget_main.cpp tests/test_selection_colours.cpp tests/test_token_edit.cpp
          tests/test_sparkline.cpp
  LINK lm_ui Qt5::Test)
# offscreen would keep this headless, but vcpkg's applocal deployment copies
# only the plugins it can see the binary needs -- which is qwindows alone, so
# forcing offscreen on Windows leaves the test with no platform plugin at all
# and it aborts before reaching main(). The windows plugin paints through the
# same raster engine, so the pixels under test are identical either way.
if(NOT WIN32)
  set_tests_properties(lm_ui_widget_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()




