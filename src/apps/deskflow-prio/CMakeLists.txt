# SPDX-FileCopyrightText: (C) 2026 Deskflow Contributors
# SPDX-License-Identifier: MIT
#
# macOS scheduling helper: promotes deskflow-core to the foreground task role
# and raises its threads' precedence (see deskflow-prio.c). Runs as a root
# LaunchDaemon from tools/launchd/io.github.hughesyadaddy.deskflow-prio.plist.
#
# Ships inside Deskflow.app/Contents/MacOS and is signed by
# configure_mac_codesign with the stable org.deskflow.deskflow-prio identifier
# (allowlisted in tools/fleet-health.identifiers), so it is covered by the
# same signing gates as every other first-party binary. Installing the
# LaunchDaemon plist itself is left to scripts/install-macos.sh.

if(NOT APPLE)
  return()
endif()

set(target ${CMAKE_PROJECT_NAME}-prio)

add_executable(${target} deskflow-prio.c)

# Plain libc + Mach + libproc; no Qt, no frameworks, no C++ runtime.
set_target_properties(${target} PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  LINKER_LANGUAGE C
  AUTOMOC OFF
  AUTOUIC OFF
  AUTORCC OFF
)

install(
  TARGETS ${target}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(BUILD_OSX_BUNDLE)
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_PROPER_NAME}>/MacOS
  )
  if(APPLE_CODESIGN_DEV)
    configure_mac_codesign(${target})
  endif()
endif()
