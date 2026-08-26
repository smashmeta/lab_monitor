find_package(Qt6 COMPONENTS Widgets REQUIRED)
find_package(Boost REQUIRED COMPONENTS program_options)
# fastdds's exported config links nlohmann_json::nlohmann_json, and that target
# is only visible in the directory scope that resolved it -- so it has to be
# found here too, exactly as libs/transport already documents. Without it the
# link fails looking for a literal "nlohmann_json::nlohmann_json.lib".
find_package(nlohmann_json CONFIG REQUIRED)
find_package(fastdds CONFIG REQUIRED)

set(CMAKE_AUTOMOC ON)

add_executable(shopping_cart WIN32
  main.cpp
  cart_state.cpp
  cart_publisher.cpp
  cart_window.cpp
  # AUTOMOC finds this one on its own -- it sits beside its .cpp rather than
  # under an include/ directory, which is the arrangement lm_ui has to work
  # around by listing its Q_OBJECT headers explicitly.
  cart_window.hpp)

target_include_directories(shopping_cart PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# fastdds directly, and deliberately no lm_transport: this fixture stands in
# for a third-party publisher, and one built on the product's own transport
# code would prove considerably less. lm_ui is here for the theme alone.
target_link_libraries(shopping_cart PRIVATE
  lm_ui Qt6::Widgets Boost::program_options fastdds lm_warnings)

copy_runtime_dependencies(shopping_cart)

lm_add_test(shopping_cart_tests
  SOURCES tests/test_cart_state.cpp cart_state.cpp
  LINK lm_warnings)
target_include_directories(shopping_cart_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(LM_BUILD_INTEGRATION_TESTS)
  # The end-to-end case the whole DDS rule feature exists for: this fixture
  # publishes, the real probe reads a type it was never compiled against, and
  # the real evaluate() judges it. Gated because it opens a domain -- and the
  # sources are compiled in directly rather than linked from the GUI target,
  # the same arrangement apps/client uses for monitor_worker.cpp.
  lm_add_test(shopping_cart_dds_tests
    SOURCES tests/test_cart_rules.cpp cart_state.cpp cart_publisher.cpp
    LINK lm_core lm_transport fastdds)
  target_include_directories(shopping_cart_dds_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
