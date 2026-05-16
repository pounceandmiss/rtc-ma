# Tcl 9 binding for librtcma.
#
# Two targets share rtcma_tcl.c:
#
#   - rtcma_tcl  (MODULE)  -> rtcma.so for `load` / `package require rtcma`.
#       Links libtclstub so the .so resolves Tcl symbols against whatever
#       runtime loads it (single binary works against any Tcl 9.x).
#
#   - rtcma_tcl_static (STATIC) -> for embedders that want to link the
#       extension into a custom tclsh and register it via
#       Tcl_StaticPackage + Rtcma_Init. Compiled without USE_TCL_STUBS;
#       the host tclsh provides the Tcl symbols.
#
# LINKER_LANGUAGE CXX: librtcma's transitive deps include libdatachannel
# (a C++ static archive in bundled mode), so the linker has to be the
# C++ driver.

# Tcl 9 discovery - relies on CMake's standard prefix search
# (/usr, /usr/local, and anything passed via -DCMAKE_PREFIX_PATH=...).
# For a Tcl install in a non-standard location, pass:
#   -DCMAKE_PREFIX_PATH=/path/to/tcl-install
# or override the resolved vars directly:
#   -DRTCMA_TCL_INCLUDE_DIR=...  -DRTCMA_TCL_STUB_LIBRARY=...
find_path(RTCMA_TCL_INCLUDE_DIR tcl.h
  PATH_SUFFIXES tcl9.0 tcl
  REQUIRED)

find_library(RTCMA_TCL_STUB_LIBRARY tclstub
  REQUIRED)

message(STATUS "rtcma/tcl: tcl.h        = ${RTCMA_TCL_INCLUDE_DIR}")
message(STATUS "rtcma/tcl: tclstub lib  = ${RTCMA_TCL_STUB_LIBRARY}")

set(_rtcma_tcl_sources rtcma_tcl.c)

# -- Loadable module (the .so) ------------------------------------------
add_library(rtcma_tcl MODULE ${_rtcma_tcl_sources})

set_target_properties(rtcma_tcl PROPERTIES
  PREFIX ""                 # `load` wants `rtcma.so`, not `librtcma.so`
  OUTPUT_NAME "rtcma"
  LINKER_LANGUAGE CXX)

target_compile_definitions(rtcma_tcl PRIVATE
  USE_TCL_STUBS
  RTCMA_TCL_PACKAGE_NAME="rtcma"
  RTCMA_TCL_PACKAGE_VERSION="${PROJECT_VERSION}")

target_include_directories(rtcma_tcl PRIVATE
  ${RTCMA_TCL_INCLUDE_DIR})

target_link_libraries(rtcma_tcl PRIVATE
  rtcma
  ${RTCMA_TCL_STUB_LIBRARY})

# Drop pkgIndex.tcl next to the .so so TCLLIBPATH=<build>/tcl resolves
# `package require rtcma`.
configure_file(pkgIndex.tcl.in
  ${CMAKE_CURRENT_BINARY_DIR}/pkgIndex.tcl @ONLY)

# -- Static archive (for embedding in a custom tclsh) -------------------
add_library(rtcma_tcl_static STATIC ${_rtcma_tcl_sources})

set_target_properties(rtcma_tcl_static PROPERTIES
  OUTPUT_NAME "rtcma_tcl"
  LINKER_LANGUAGE CXX)

target_compile_definitions(rtcma_tcl_static PRIVATE
  RTCMA_TCL_PACKAGE_NAME="rtcma"
  RTCMA_TCL_PACKAGE_VERSION="${PROJECT_VERSION}")
  # No USE_TCL_STUBS: the host tclsh provides Tcl symbols directly.

target_include_directories(rtcma_tcl_static PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${RTCMA_TCL_INCLUDE_DIR})

target_link_libraries(rtcma_tcl_static PUBLIC
  rtcma)

add_library(Rtcma::tcl ALIAS rtcma_tcl_static)
