# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

if(WIN32)
  set(extra_libs version)
endif()

create_test(
  NAME StringTests
  DEPENDS base
  LIBS arch
  SOURCE StringTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)

create_test(
  NAME UnicodeTests
  DEPENDS base
  LIBS arch ${extra_libs}
  SOURCE UnicodeTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)

create_test(
  NAME LogTests
  DEPENDS base
  LIBS arch ${extra_libs}
  SOURCE LogTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)

create_test(
  NAME BaseExceptionTests
  DEPENDS base
  LIBS arch
  SOURCE BaseExceptionTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)


# K8: waitForReady() honours the ready flag (no thread deaths across epochs).
create_test(
  NAME EventQueueReadyTests
  DEPENDS base
  LIBS arch mt
  SOURCE EventQueueReadyTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/base"
)
