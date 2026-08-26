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

  copy_qt_plugins("${target}")
endfunction()

# A Qt plugin is loaded by name at runtime, so it is not a link-time dependency
# and $<TARGET_RUNTIME_DLLS> above cannot see it. Under Qt 5 that did not
# matter here: vcpkg's qt5-base port installed a plugins/qtdeploy.ps1 which
# vcpkg's own applocal deployment sourced, and platforms/qwindows.dll appeared
# beside every executable for free. The Qt 6 qtbase port ships no such script
# -- Qt 6 expects windeployqt or qt_generate_deploy_app_script, both of which
# are install-time tools, where everything here runs out of the build tree. So
# the plugins are copied explicitly, into the plugin-type subdirectory Qt looks
# for them in ("platforms", "styles", ...), which each plugin target names in
# its own QT_PLUGIN_TYPE property rather than us hard-coding the mapping.
#
# Without this a freshly built app aborts before main() with "could not find or
# load the Qt platform plugin windows" -- and so does every widget test.
function(copy_qt_plugins target)
  foreach(plugin IN ITEMS
      # The platform plugin. Nothing with a QGuiApplication starts without it.
      Qt6::QWindowsIntegrationPlugin
      # Theme::apply() installs Fusion, which is compiled into QtWidgets and
      # needs no plugin -- but a widget shown before that, and every native
      # dialog, falls back to the platform style, which is a plugin in Qt 6.
      # Named QWindowsVistaStylePlugin before Qt 6.7, so try both.
      Qt6::QModernWindowsStylePlugin
      Qt6::QWindowsVistaStylePlugin)
    if(NOT TARGET ${plugin})
      continue()
    endif()
    # make_directory first: copy_if_different will not create the destination,
    # it fails with "Invalid argument" on a path that is not already a
    # directory, and nothing else here creates bin/<config>/platforms.
    add_custom_command(
      TARGET "${target}" POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${target}>/$<TARGET_PROPERTY:${plugin},QT_PLUGIN_TYPE>"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:${plugin}>"
        "$<TARGET_FILE_DIR:${target}>/$<TARGET_PROPERTY:${plugin},QT_PLUGIN_TYPE>/$<TARGET_FILE_NAME:${plugin}>"
      COMMENT "Deploying ${plugin} for ${target}...")
  endforeach()
endfunction()
