# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
#
# Windows secure-desktop bridge: submits HID reports to deskflow-vhid.sys.

if(NOT WIN32)
  return()
endif()

set(target ${CMAKE_PROJECT_NAME}-vhid-bridge)

# SingleInstanceLock is Qt-free; compile it in directly rather than linking
# `common`, which would pull Qt into the secure-desktop bridge.
add_executable(${target}
  deskflow-vhid-bridge-win.cpp
  ${CMAKE_SOURCE_DIR}/src/lib/common/SingleInstanceLock.cpp
)

target_include_directories(${target} PRIVATE
  ${CMAKE_SOURCE_DIR}/src/driver/deskflow-vhid/public
  ${CMAKE_SOURCE_DIR}/src/lib
)

target_link_libraries(${target} PRIVATE setupapi advapi32)

install(
  TARGETS ${target}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
