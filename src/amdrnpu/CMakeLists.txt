# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2026, Advanced Micro Devices, Inc. All rights reserved.

cmake_minimum_required(VERSION 3.16)

project(amdrnpu_lib
  VERSION 0.1.0
  LANGUAGES C
  )

include(GNUInstallDirs)

option(AMDRNPU_LIB_BUILD_SHARED "Build shared library" ON)
option(AMDRNPU_LIB_BUILD_STATIC "Build static library" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED libdrm)

# UAPI header <drm/amdrnpu_accel.h> lives at the repo top level.
set(AMDRNPU_UAPI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include/uapi"
  CACHE PATH "amdrnpu UAPI")

set(AMDRNPU_LIB_SOURCES
  amdrnpu_lib.c
  )

set(AMDRNPU_LIB_HEADERS
  amdrnpu_lib.h
  )

function(_amdrnpu_lib_configure_target tgt)
  target_include_directories(${tgt}
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
      $<BUILD_INTERFACE:${AMDRNPU_UAPI_DIR}>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${LIBDRM_INCLUDE_DIRS}
    )
  target_link_libraries(${tgt} PRIVATE ${LIBDRM_LIBRARIES})
  target_compile_options(${tgt} PRIVATE ${LIBDRM_CFLAGS_OTHER} -Wall -Wextra)
endfunction()

set(_installed_targets)

if(AMDRNPU_LIB_BUILD_SHARED)
  add_library(amdrnpu SHARED ${AMDRNPU_LIB_SOURCES})
  add_library(amdrnpu::amdrnpu ALIAS amdrnpu)
  _amdrnpu_lib_configure_target(amdrnpu)
  set_target_properties(amdrnpu PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME amdrnpu
    )
  list(APPEND _installed_targets amdrnpu)
endif()

if(AMDRNPU_LIB_BUILD_STATIC)
  add_library(amdrnpu_static STATIC ${AMDRNPU_LIB_SOURCES})
  add_library(amdrnpu::amdrnpu_static ALIAS amdrnpu_static)
  _amdrnpu_lib_configure_target(amdrnpu_static)
  set_target_properties(amdrnpu_static PROPERTIES OUTPUT_NAME amdrnpu)
  list(APPEND _installed_targets amdrnpu_static)
endif()

install(TARGETS ${_installed_targets}
  DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )

install(FILES ${AMDRNPU_LIB_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/amdrnpu
  )

install(DIRECTORY ${AMDRNPU_UAPI_DIR}/drm
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "amdrnpu_accel.h"
  )
