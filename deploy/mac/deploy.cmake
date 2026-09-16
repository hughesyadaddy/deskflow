# SPDX-FileCopyrightText: (C) 2024 Chris Rizzitello <sithlord48@gmail.com>
# SPDX-License-Identifier: MIT

# HACK This is set when the files is included so its the real path
# calling CMAKE_CURRENT_LIST_DIR after include would return the wrong scope var
set(MY_DIR ${CMAKE_CURRENT_LIST_DIR})
set(OSX_BUNDLE ${BUILD_OSX_BUNDLE})

set(OS_STRING "macos-${BUILD_ARCHITECTURE}")

if (OSX_BUNDLE)
  # Sign the deployed bundle with the developer identity when one is
  # configured (stable TCC identity across installs). Ad-hoc fallback only
  # when FLEET_STRICT_SIGNING=OFF; with it ON an empty/"-" identity is fatal.
  if("${APPLE_CODESIGN_DEV}" STREQUAL "" OR "${APPLE_CODESIGN_DEV}" STREQUAL "-")
    if(FLEET_STRICT_SIGNING)
      message(FATAL_ERROR
        "FLEET_STRICT_SIGNING=ON but APPLE_CODESIGN_DEV is empty or '-' (ad-hoc); "
        "refusing to deploy an ad-hoc signed bundle.")
    endif()
    message(WARNING "ad-hoc signing (FLEET_STRICT_SIGNING=OFF): macdeployqt -codesign uses '-'")
    set(MAC_DEPLOY_CODESIGN_ID "-")
  else()
    set(MAC_DEPLOY_CODESIGN_ID "${APPLE_CODESIGN_DEV}")
  endif()
  # -executable: macdeployqt only rewrites the bundle's main executable by
  # default; deskflow-core would keep absolute Qt paths and load a second
  # Qt at runtime (fatal cocoa-plugin clash on machines that have one).
  install(CODE "
    execute_process(COMMAND
      ${DEPLOYQT}
      \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app\"
      \"-executable=\${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app/Contents/MacOS/deskflow-core\"
      -timestamp \"-codesign=${MAC_DEPLOY_CODESIGN_ID}\"
      RESULT_VARIABLE _deployqt_rc
    )
    if(NOT _deployqt_rc EQUAL 0)
      message(FATAL_ERROR \"macdeployqt/codesign of \${CMAKE_INSTALL_PREFIX}/${CMAKE_PROJECT_PROPER_NAME}.app failed (exit \${_deployqt_rc})\")
    endif()
  ")
  set(CPACK_PACKAGE_ICON "${MY_DIR}/dmg-volume.icns")
  set(CPACK_DMG_BACKGROUND_IMAGE "${MY_DIR}/dmg-background.tiff")
  set(CPACK_DMG_DS_STORE_SETUP_SCRIPT "${MY_DIR}/generate_ds_store.applescript")
  set(CPACK_DMG_VOLUME_NAME "${CMAKE_PROJECT_PROPER_NAME}")
  set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE ON)
  set(CPACK_GENERATOR "DragNDrop")
endif()
