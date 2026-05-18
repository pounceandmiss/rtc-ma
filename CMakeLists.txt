cmake_minimum_required(VERSION 3.21)
project(rtcma
  VERSION 0.1.0
  LANGUAGES C CXX)

option(RTCMA_BUNDLE_LIBDATACHANNEL
  "Build libdatachannel + mbedtls from source as static archives, folding \
them into binaries with no runtime .so dependency. When OFF, libdatachannel \
is consumed via find_package (system / CMAKE_PREFIX_PATH)."
  OFF)

option(RTCMA_BUNDLE_OPUS
  "Build opus from source as a static archive, folding it into binaries \
with no runtime .so dependency. When OFF, opus is consumed via \
pkg_check_modules from the system."
  OFF)

# Backwards-compat: RTCMA_BUNDLE_DEPS was the single coarse switch. If a
# caller still sets it ON and hasn't touched the finer flags, force both
# finer flags ON so the old invocation keeps working.
option(RTCMA_BUNDLE_DEPS
  "Deprecated alias: turns on both RTCMA_BUNDLE_LIBDATACHANNEL and \
RTCMA_BUNDLE_OPUS when neither has been set explicitly."
  OFF)
if(RTCMA_BUNDLE_DEPS AND NOT RTCMA_BUNDLE_LIBDATACHANNEL AND NOT RTCMA_BUNDLE_OPUS)
  set(RTCMA_BUNDLE_LIBDATACHANNEL ON CACHE BOOL "" FORCE)
  set(RTCMA_BUNDLE_OPUS          ON CACHE BOOL "" FORCE)
endif()

option(RTCMA_BUILD_TCL
  "Build the ::rtcma:: Tcl 9 bindings - produces a loadable rtcma.so \
(`package require rtcma`) plus a librtcma_tcl.a static archive for \
embedders linking the package into a custom tclsh."
  OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

find_package(Threads REQUIRED)

if(RTCMA_BUNDLE_LIBDATACHANNEL OR RTCMA_BUNDLE_OPUS)
  include(BundleDeps)
endif()

if(NOT RTCMA_BUNDLE_LIBDATACHANNEL)
  # libdatachannel installs that link mbedtls statically (e.g. zippy's
  # vendored prefix) export `MbedTLS::MbedTLS` in LibDataChannelTargets'
  # INTERFACE_LINK_LIBRARIES - an aggregate that libdatachannel's
  # build-time cmake/Modules/FindMbedTLS.cmake creates but that the
  # installed LibDataChannelConfig.cmake doesn't find_dependency() or
  # recreate. Re-assemble it from the official MbedTLSConfig exports
  # (lowercase: MbedTLS::mbedtls / mbedx509 / mbedcrypto) so
  # find_package(LibDataChannel) succeeds against those installs.
  #
  # Distro packages of libdatachannel are typically shared and don't
  # reference MbedTLS::MbedTLS in their export, so this lookup stays
  # QUIET / non-REQUIRED. If MbedTLS really is needed and missing,
  # find_package(LibDataChannel REQUIRED) below produces the real error.
  find_package(MbedTLS CONFIG QUIET)
  if(MbedTLS_FOUND AND NOT TARGET MbedTLS::MbedTLS)
    add_library(MbedTLS::MbedTLS INTERFACE IMPORTED)
    target_link_libraries(MbedTLS::MbedTLS INTERFACE
      MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
  endif()
  find_package(LibDataChannel REQUIRED)
  add_library(Rtcma::libdatachannel INTERFACE IMPORTED)
  target_link_libraries(Rtcma::libdatachannel INTERFACE LibDataChannel::LibDataChannel)
endif()

if(NOT RTCMA_BUNDLE_OPUS)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(opus REQUIRED IMPORTED_TARGET opus)
  add_library(Rtcma::opus INTERFACE IMPORTED)
  target_link_libraries(Rtcma::opus INTERFACE PkgConfig::opus)
endif()

# -- The library --------------------------------------------------------
# Single static archive: track adapter + jitter + SDP parser + miniaudio
# glue. miniaudio.h is the only large single-header amalgamation in here;
# the audio source file silences its noisy warnings privately so the rest
# of the project still compiles under -Wall -Wextra.

add_library(rtcma STATIC
  src/rtcma_jitter.c
  src/rtcma_sdp.c
  src/rtcma_track.c
  src/rtcma_audio.c
  src/rtcma_device_id.c
  src/rtcma_log.c)

target_include_directories(rtcma
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(rtcma
  PUBLIC  Rtcma::libdatachannel Rtcma::opus Threads::Threads
  PRIVATE ${CMAKE_DL_LIBS} m)

# miniaudio is dlopen-based - PipeWire/PulseAudio/ALSA/JACK are picked at
# runtime. No link-time audio backend deps; just libdl + libm + pthread.
set_source_files_properties(src/rtcma_audio.c PROPERTIES
  COMPILE_OPTIONS
    "-Wno-unused-function;-Wno-unused-parameter;\
-Wno-unused-but-set-variable;-Wno-unused-variable;\
-Wno-unused-result;-Wno-pedantic;\
-Wno-cast-function-type;-Wno-implicit-fallthrough")

add_library(Rtcma::rtcma ALIAS rtcma)

if(RTCMA_BUILD_TCL)
  add_subdirectory(tcl)
endif()

enable_testing()
add_subdirectory(tests)
