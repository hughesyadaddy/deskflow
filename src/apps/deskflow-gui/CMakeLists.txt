# SPDX-FileCopyrightText: (C) 2024 - 2025 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-FileCopyrightText: (C) 2024 Symless Ltd
# SPDX-License-Identifier: MIT

if(APPLE)
  set(target Deskflow)
else()
  set(target deskflow)
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(WIN32)
  set(EXE_DESCRIPTION "${CMAKE_PROJECT_PROPER_NAME} GUI configuration tool")
  set(EXE_ICON "IDI_ICON1 ICON DISCARDABLE \"${CMAKE_SOURCE_DIR}/src/apps/res/deskflow.ico\" ")
  configure_file(${CMAKE_SOURCE_DIR}/src/apps/res/windows.rc.in deskflow.rc)
  set(platform_extra deskflow.rc)
elseif(BUILD_OSX_BUNDLE)
  #setup our bundle plist file
  set(BUNDLE_EXECUTABLE_NAME "${target}")
  set(BUNDLE_BUNDLE_NAME "${target}")
  set(BUNDLE_DISPLAY_NAME "${target}")
  # Fork-specific macOS bundle identifier, deliberately NOT CMAKE_PROJECT_REV_FQDN.
  #
  # macOS 26.2 corrupts SystemUIServer's per-bundle-identifier menu-bar state when an
  # app runs: the status item gets a nominal slot but is laid out off the menu bar
  # (measured at y=1280-1310 on a 1296pt screen), and the damage persists for that
  # identifier across relaunches. Restarting the menu-bar services, deleting the
  # saved NSStatusItem Preferred Position, and relaunching all fail -- the app
  # re-poisons itself every start. Only a different identifier holds; the same fix
  # was verified on Mouser. macOS 26.5.1 is unaffected, so this works around an OS
  # bug rather than app behaviour.
  #
  # Scoped to the macOS bundle only: CMAKE_PROJECT_REV_FQDN also names the Linux
  # .desktop/.metainfo files and drives QIcon::fromTheme lookups via kRevFqdnName,
  # so changing it globally would break Linux packaging and icons.
  set(BUNDLE_GUI_IDENTIFIER "io.github.hughesyadaddy.deskflow")
  set(BUNDLE_ICON_FILE ${target}.icns)
  set(BUNDLE_INFO_STRING "${CMAKE_PROJECT_DESCRIPTION}")
  set(BUNDLE_COPYRIGHT "${CMAKE_PROJECT_COPYRIGHT}")

  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/../res/deskflow.plist.in
    ${CMAKE_CURRENT_BINARY_DIR}/deskflow.plist
    @ONLY
  )

  set(platform_extra ../res/Deskflow.icns)
  set_source_files_properties(${platform_extra} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

  set(login_bridge_install_script "${CMAKE_SOURCE_DIR}/scripts/install-login-bridge-macos.sh")
  set_source_files_properties(${login_bridge_install_script} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
  list(APPEND platform_extra "${login_bridge_install_script}")
endif()

add_executable(${target} WIN32 MACOSX_BUNDLE
  ${platform_extra}
  ../res/deskflow.qrc
  deskflow-gui.cpp
)

set(deskflow_gui_qt_libs Qt6::Core Qt6::Widgets Qt6::Network)
if(APPLE)
  list(APPEND deskflow_gui_qt_libs Qt6::Svg)
endif()

target_link_libraries(
  ${target}
  gui
  common
  ${deskflow_gui_qt_libs}
)

install(
  TARGETS ${target}
  RUNTIME_DEPENDENCY_SET guiDeps
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  BUNDLE DESTINATION .
)

if(WIN32)
  set_target_properties(${target} PROPERTIES LINK_FLAGS "/NODEFAULTLIB:LIBCMT")

  install(RUNTIME_DEPENDENCY_SET guiDeps
    ${WIN32_RUNTIME_DEPENDENCY_SET_ARGS}
    PRE_EXCLUDE_REGEXES ${WIN32_PRE_EXCLUDE_REGEXES}
    POST_EXCLUDE_REGEXES ${WIN32_POST_EXCLUDE_REGEXES}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )

  set(QT_DEPENDS_DIR ${CMAKE_BINARY_DIR}/qt-depends)

  add_custom_command(
    TARGET ${target} POST_BUILD
    COMMAND ${DEPLOYQT}
      --no-compiler-runtime
      --no-system-d3d-compiler
      --no-opengl-sw
      --no-quick-import
      --dir "${QT_DEPENDS_DIR}"
      --plugindir "${QT_DEPENDS_DIR}/plugins"
      $<TARGET_FILE:${target}>
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
      ${QT_DEPENDS_DIR}
      $<TARGET_FILE_DIR:${target}>
  )

  install(
    DIRECTORY ${QT_DEPENDS_DIR}/
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
    PATTERN "dx*.dll" EXCLUDE
    PATTERN "icu*.dll" EXCLUDE
  )

elseif(APPLE)
  if (BUILD_OSX_BUNDLE)
    set_target_properties(${target} PROPERTIES
      INSTALL_RPATH "@loader_path/../Libraries;@loader_path/../Frameworks"
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/deskflow.plist"
    )
    if (APPLE_CODESIGN_DEV)
      configure_mac_codesign(${target})
    endif()
  else()
    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE FALSE)
  endif()
else()
  generate_app_man(${target} "${CMAKE_PROJECT_DESCRIPTION} \\(GUI\\)")
endif()
