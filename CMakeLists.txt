#******************************************************************************
# CMake configuration file for Surround View Application
#
# Copyright (c) 2015-2017 Cogent Embedded Inc. ALL RIGHTS RESERVED.
# 
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
# *******************************************************************************/

cmake_minimum_required(VERSION 2.8)

#------------------------------------------------------------------------------
# Constant defines
#------------------------------------------------------------------------------

project("sv-utest" C)

# search for additional CMake modules
SET(CMAKE_MODULE_PATH
        ${CMAKE_MODULE_PATH}
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
)

MESSAGE(STATUS "CMAKE_FIND_ROOT_PATH:" ${CMAKE_FIND_ROOT_PATH})

set(INSTALL_BIN_DIR bin CACHE PATH "Installation directory for executables")
set(INSTALL_LIB_DIR lib CACHE PATH "Installation directory for libraries")
set(INSTALL_INC_DIR include CACHE PATH "Installation directory for headers")
set(INSTALL_DATA_DIR share/${PROJECT_NAME} CACHE PATH "Installation directory for data files")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin")

#------------------------------------------------------------------------------
# Dependencies
#------------------------------------------------------------------------------
include(GNUInstallDirs)

set(CMAKE_THREAD_PREFER_PTHREAD true)
include(FindThreads)
if(!${CMAKE_USE_PTHREADS_INIT})
    message(FATAL_ERROR "pthreads library not found")
endif()

# search for additional CMake modules
list(APPEND CMAKE_MODULE_PATH
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake
)

find_package(Cairo REQUIRED)
find_package(Pango REQUIRED)
find_package(EGL REQUIRED)
find_package(GLIB REQUIRED COMPONENTS gobject gmodule gthread gio)
find_package(GStreamer REQUIRED COMPONENTS
    gstreamer-allocators
    gstreamer-app
    gstreamer-video
)
find_package(OpenGLES2 REQUIRED)
find_package(PCAP REQUIRED)
find_package(Wayland REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Spnav QUIET)

if (${PC_GSTREAMER_VERSION} VERSION_LESS "1.6.0")
    message(WARNING "Using old GStreamer!")
    add_definitions(-DGST_PAD_PROBE_HANDLED=GST_PAD_PROBE_DROP)
endif()

#find_package(libsv REQUIRED CONFIG)

#------------------------------------------------------------------------------
# Extract current version tag from git
#------------------------------------------------------------------------------
execute_process(
  COMMAND git describe --tags
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_HEAD_TAG
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

add_definitions(-DSV_VERSION_STRING="${GIT_HEAD_TAG}")

#------------------------------------------------------------------------------
# Build configuration variables
#------------------------------------------------------------------------------

add_definitions(
  -DOPENGL_ES
  -DEGL_HAS_IMG_EXTERNAL_EXT
)

option(WITH_SPACENAV "Enable Spacenav 3D joystick" OFF)

if (SPNAV_FOUND)
    add_definitions(
        -DSPACENAV_ENABLED
    )
endif()

include_directories(
    src
    ${CAIRO_INCLUDE_DIRS}
    ${PANGO_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIR}
    ${GLIBCONFIG_INCLUDE_DIR}
    ${GSTREAMER_INCLUDE_DIRS}
    ${OPENGLES2_INCLUDE_DIRS}
    ${PCAP_INCLUDE_DIR}
    ${WAYLAND_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (SPNAV_FOUND)
   list(APPEND ${PROJECT_NAME}_INCLUDE_DIRS ${SPNAV_INCLUDE_DIR})
endif()

set(
    ${PROJECT_NAME}_LIBS
    ${CMAKE_THREAD_LIBS_INIT}
    ${CAIRO_LIBRARIES}
    ${PANGO_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLIB_LIBRARIES}
    ${GLIB_GIO_LIBRARIES}
    ${GLIB_GOBJECT_LIBRARIES}
    ${GLIB_GTHREAD_LIBRARIES}
    ${GLIB_GMODULE_LIBRARIES}
    ${GSTREAMER_LIBRARIES}
    ${GSTREAMER_ALLOCATORS_LIBRARIES}
    ${GSTREAMER_APP_LIBRARIES}
    ${GSTREAMER_BASE_LIBRARIES}
    ${GSTREAMER_VIDEO_LIBRARIES}
    ${OPENGLES2_LIBRARIES}
    ${PCAP_LIBRARY}
    ${WAYLAND_LIBRARIES}
    ${ZLIB_LIBRARY}

    sv
    mmngr
    mmngrbuf
    m
)

if (SPNAV_FOUND)
   list(APPEND ${PROJECT_NAME}_LIBS ${SPNAV_LIBRARIES})
endif()

#------------------------------------------------------------------------------
# Sources
#------------------------------------------------------------------------------

set(
    ${PROJECT_NAME}_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/common.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/display.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/display.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/display-wayland.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/display-wayland.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/egl_renesas.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/event.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/netif.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/netif.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/video-decoder.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vin.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vpool.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vsink.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vsink.h
    )

list(APPEND ${PROJECT_NAME}_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/blf.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/camera.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/camera-mjpeg.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/camera-mjpeg.h
  ${CMAKE_CURRENT_SOURCE_DIR}/src/pcap.c
  )

if (SV_TARGET_PLATFORM STREQUAL GEN3)
  link_directories(${CMAKE_CURRENT_SOURCE_DIR}/libs/gen3)
  list(APPEND ${PROJECT_NAME}_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/mjpeg-decoder.c
    )
endif() 

# Note: jpu-decoder and mjpeg-decode are mutually exclusive
if (SV_TARGET_PLATFORM STREQUAL GEN2)
  link_directories(${CMAKE_CURRENT_SOURCE_DIR}/libs/gen2)
  add_definitions(
    -DJPU_SUPPORT
    )
  list(APPEND ${PROJECT_NAME}_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/jpu.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/jpu.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/jpu-decoder.c
    )
endif() 

#------------------------------------------------------------------------------
# Build options
#------------------------------------------------------------------------------
add_executable(${PROJECT_NAME} ${${PROJECT_NAME}_SOURCES})

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -g)

target_link_libraries(${PROJECT_NAME} ${${PROJECT_NAME}_LIBS})

#------------------------------------------------------------------------------
# Installation
#------------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

#------------------------------------------------------------------------------
# Tests
#------------------------------------------------------------------------------

#------------------------------------------------------------------------------
# Packaging
#------------------------------------------------------------------------------

