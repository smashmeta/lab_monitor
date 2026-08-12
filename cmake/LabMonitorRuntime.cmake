function(copy_runtime_dependencies target)
  # $<TARGET_RUNTIME_DLLS:target> expands to nothing when a target has no
  # shared-library runtime dependencies (e.g. an all-static link closure).
  # With COMMAND_EXPAND_LISTS that empty list vanishes entirely, collapsing
  # the command line to "copy_if_different <dest-dir>" with no source
  # arguments, which cmake -E rejects. Swap the verb to the no-op "true"
  # in that case so the command stays valid either way.
  add_custom_command(
    TARGET "${target}" POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E $<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>,copy_if_different,true>
      $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
    COMMAND_EXPAND_LISTS
    COMMENT "Copying runtime dependencies for ${target}...")
endfunction()
