# SPDX-FileCopyrightText: (C) 2025-2026 Deskflow Contributors
# SPDX-License-Identifier: MIT

# Warning: Do not use for CI/production, as the `entitlements-dev.plist` file adds special
# entitlements that are only appropriate for local development.
#
# macOS made TCC stricter so that if you don't sign your local dev builds properly, macOS will
# nag you to remove and re-approve the app every time you make a change to the binary which is
# extremely annoying during development.
#
# If you were to use ad-hoc signing (i.e. not specify a certificate), TCC would still nag you
# because the binary identity is anchored not on the app ID, but on the CD hash (which changes
# based on the binary contents).
#
# To use, simply generate a personal certificate for free with Xcode and pass the ID to CMake.
# Full instructions are in the docs.

# Note: "-" is a truthy string to CMake's if(), so the include gate in the
# top-level CMakeLists.txt does not filter an explicit ad-hoc identity.
if("${APPLE_CODESIGN_DEV}" STREQUAL "" OR "${APPLE_CODESIGN_DEV}" STREQUAL "-")
  if(FLEET_STRICT_SIGNING)
    message(FATAL_ERROR
      "FLEET_STRICT_SIGNING=ON but APPLE_CODESIGN_DEV is empty or '-' (ad-hoc); "
      "MacCodesign.cmake requires a real developer identity.")
  endif()
  message(WARNING "ad-hoc signing (FLEET_STRICT_SIGNING=OFF): codesign-dev target signs with '-'")
endif()

# The codesign invocations below run as add_custom_command steps, so a
# non-zero codesign exit already fails the build; no execute_process here.

function(configure_mac_codesign target)
  set_property(GLOBAL APPEND PROPERTY _MAC_CODESIGN_DEPENDS $<TARGET_FILE:${target}>)
  # Plain target name (== the installed Contents/MacOS/<name> filename for
  # these targets), for consumers that need it outside a build-time COMMAND
  # context, where the $<TARGET_FILE:...> genex above can't be resolved
  # with configure-time string commands like get_filename_component.
  set_property(GLOBAL APPEND PROPERTY _MAC_CODESIGN_TARGET_NAMES "${target}")
  # Stable code identifier per target. Without --identifier codesign derives
  # one from the file name (plus an LC_UUID-style suffix for bare Mach-Os,
  # e.g. deskflow-core-5555), which changes per build and breaks both TCC
  # grants and `tools/fleet-health --check identifiers`. Keep in sync with
  # tools/fleet-health.identifiers.
  string(TOLOWER "${target}" _mac_codesign_id_tail)
  set_property(GLOBAL APPEND PROPERTY _MAC_CODESIGN_IDENTIFIERS "org.deskflow.${_mac_codesign_id_tail}")

  get_property(deferred GLOBAL PROPERTY _MAC_CODESIGN_DEFERRED)

  if(NOT deferred)
    set_property(GLOBAL PROPERTY _MAC_CODESIGN_DEFERRED TRUE)
    message(STATUS "Apple codesign ID for development only: ${APPLE_CODESIGN_DEV}")
    cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL _finalize_mac_codesign)
  endif()
endfunction()

function(_finalize_mac_codesign)
  get_property(depends GLOBAL PROPERTY _MAC_CODESIGN_DEPENDS)
  get_property(identifiers GLOBAL PROPERTY _MAC_CODESIGN_IDENTIFIERS)

  set(stamp_file "${CMAKE_BINARY_DIR}/CMakeFiles/codesign-dev.stamp")

  # Use a stamp file because codesign modifies the binaries it signs.
  # Nested executables are signed before the bundle: the x86_64 linker
  # does not ad-hoc sign its output (unlike arm64), and signing a bundle
  # fails on unsigned subcomponents. Each nested binary gets its explicit
  # org.deskflow.<target> identifier; the bundle itself keeps the
  # CFBundleIdentifier from its Info.plist (no --identifier on that call).
  set(_codesign_cmds)
  set(_i 0)
  foreach(_bin IN LISTS depends)
    list(GET identifiers ${_i} _ident)
    math(EXPR _i "${_i} + 1")
    list(APPEND _codesign_cmds
      COMMAND /usr/bin/codesign
              --force
              --options runtime
              --identifier "${_ident}"
              --entitlements "${CMAKE_SOURCE_DIR}/src/apps/res/entitlements-dev.plist"
              --sign "${APPLE_CODESIGN_DEV}"
              "${_bin}"
    )
  endforeach()

  add_custom_command(
    OUTPUT ${stamp_file}
    ${_codesign_cmds}
    COMMAND /usr/bin/codesign
            --force
            --options runtime
            --entitlements "${CMAKE_SOURCE_DIR}/src/apps/res/entitlements-dev.plist"
            --sign "${APPLE_CODESIGN_DEV}"
            "$<TARGET_BUNDLE_DIR:${CMAKE_PROJECT_PROPER_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E touch ${stamp_file}
    DEPENDS ${depends}
    COMMENT "Codesigning ${CMAKE_PROJECT_PROPER_NAME}"
    VERBATIM
  )

  add_custom_target(codesign-dev ALL DEPENDS ${stamp_file})
endfunction()
