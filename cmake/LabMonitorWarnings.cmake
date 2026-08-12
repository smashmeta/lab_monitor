add_library(lm_warnings INTERFACE)
add_library(lm::warnings ALIAS lm_warnings)

if(MSVC)
  target_compile_options(lm_warnings INTERFACE /W4 /permissive- /EHsc /utf-8 /Zc:__cplusplus)
  target_compile_definitions(lm_warnings INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(lm_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
endif()
