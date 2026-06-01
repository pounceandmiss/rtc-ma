# Bundled-deps build mode: compile selected upstream libraries from source
# into a private vendor prefix and expose Rtcma::libdatachannel and/or
# Rtcma::opus INTERFACE imported targets that fold the resulting static
# archives - plus libstdc++ for the libdatachannel half - into whoever
# links them.
#
# The two halves below are independent. The caller picks them via
# RTCMA_BUNDLE_LIBDATACHANNEL / RTCMA_BUNDLE_OPUS, so a downstream that
# already has libdatachannel built can still bundle opus on its own
# (and vice-versa).
#
# For the libdatachannel half, ExternalProject_Add expresses the
# install-first ordering libdatachannel's configure-time
# find_package(MbedTLS) requires (DEPENDS mbedtls_external), so this is
# still a single-command build with no separate shell script.

include(ExternalProject)

set(RTCMA_VENDOR_PREFIX ${CMAKE_BINARY_DIR}/vendor)
set(_lib ${RTCMA_VENDOR_PREFIX}/lib)
set(_inc ${RTCMA_VENDOR_PREFIX}/include)

# CMake validates IMPORTED targets' INTERFACE_INCLUDE_DIRECTORIES exist at
# configure time; pre-create the path since ExternalProject populates it later.
file(MAKE_DIRECTORY ${_inc})

set(_common_cache_args
  -DCMAKE_INSTALL_PREFIX:PATH=${RTCMA_VENDOR_PREFIX}
  -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
  -DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON
  -DBUILD_SHARED_LIBS:BOOL=OFF
  # ExternalProject sub-builds do not inherit the parent toolchain, so a
  # cross build would compile these vendored deps (opus, libdatachannel +
  # juice/srtp2/usrsctp, mbedtls) with the host compiler and emit host-format
  # objects the cross linker can't use. Forward it explicitly; empty on a
  # native build, where it's a harmless no-op.
  -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE})

# Each maps to the .a that the corresponding ExternalProject install will
# produce; add_dependencies wires the build order so cmake builds the
# external project before anything that links the IMPORTED target.
function(_rtcma_static_lib name external archive)
  add_library(${name} STATIC IMPORTED GLOBAL)
  set_target_properties(${name} PROPERTIES
    IMPORTED_LOCATION ${archive}
    INTERFACE_INCLUDE_DIRECTORIES ${_inc})
  add_dependencies(${name} ${external})
endfunction()

# -- libdatachannel + mbedtls -------------------------------------------
if(RTCMA_BUNDLE_LIBDATACHANNEL)
  set(_mbedtls_user_cfg ${CMAKE_SOURCE_DIR}/cmake/mbedtls-user-config.h)
  set(_libdc_overrides ${CMAKE_SOURCE_DIR}/cmake/libdatachannel-overrides.cmake)

  # --- mbedtls 3.6.6 (commit-pinned) -----------------------------------------
  ExternalProject_Add(mbedtls_external
    GIT_REPOSITORY    https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG           5b64a9fdb979c8971561ec78221b528e3cc4e00a
    GIT_SUBMODULES_RECURSE TRUE
    PREFIX            ${CMAKE_BINARY_DIR}/_mbedtls
    CMAKE_CACHE_ARGS
      ${_common_cache_args}
      -DENABLE_PROGRAMS:BOOL=OFF
      -DENABLE_TESTING:BOOL=OFF
      -DUSE_SHARED_MBEDTLS_LIBRARY:BOOL=OFF
      -DUSE_STATIC_MBEDTLS_LIBRARY:BOOL=ON
      -DMBEDTLS_FATAL_WARNINGS:BOOL=OFF
      -DMBEDTLS_USER_CONFIG_FILE:FILEPATH=${_mbedtls_user_cfg}
    BUILD_BYPRODUCTS
      ${_lib}/libmbedtls.a
      ${_lib}/libmbedx509.a
      ${_lib}/libmbedcrypto.a)

  # --- libdatachannel 0.24.3 (commit-pinned) ---------------------------------
  ExternalProject_Add(libdatachannel_external
    GIT_REPOSITORY    https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG           c47f5d77c124c35c31ac8378ad613295a124d354
    GIT_SUBMODULES_RECURSE TRUE
    PREFIX            ${CMAKE_BINARY_DIR}/_libdatachannel
    DEPENDS           mbedtls_external
    CMAKE_CACHE_ARGS
      ${_common_cache_args}
      -DCMAKE_PREFIX_PATH:PATH=${RTCMA_VENDOR_PREFIX}
      -DCMAKE_PROJECT_INCLUDE_BEFORE:FILEPATH=${_libdc_overrides}
      -DRTCMA_MBEDTLS_USER_CONFIG:FILEPATH=${_mbedtls_user_cfg}
      -DUSE_MBEDTLS:BOOL=ON
      -DNO_EXAMPLES:BOOL=ON
      -DNO_TESTS:BOOL=ON
      -DPREFER_SYSTEM_LIB:BOOL=OFF
      # libsrtp defaults ENABLE_WARNINGS_AS_ERRORS=ON for its srtp2 target. Its
      # debug_print("0x%08x", ntohl(ssrc)) then trips -Wformat under mingw, whose
      # ntohl returns u_long -- a distinct type from the uint32_t %x wants, even
      # though both are 32-bit. Doesn't fire on glibc (uint32_t == unsigned int).
      -DENABLE_WARNINGS_AS_ERRORS:BOOL=OFF
    BUILD_BYPRODUCTS
      ${_lib}/libdatachannel.a
      ${_lib}/libjuice.a
      ${_lib}/libsrtp2.a
      ${_lib}/libusrsctp.a)

  _rtcma_static_lib(Rtcma::_libdatachannel libdatachannel_external ${_lib}/libdatachannel.a)
  _rtcma_static_lib(Rtcma::_juice          libdatachannel_external ${_lib}/libjuice.a)
  _rtcma_static_lib(Rtcma::_srtp2          libdatachannel_external ${_lib}/libsrtp2.a)
  _rtcma_static_lib(Rtcma::_usrsctp        libdatachannel_external ${_lib}/libusrsctp.a)
  _rtcma_static_lib(Rtcma::_mbedtls        mbedtls_external        ${_lib}/libmbedtls.a)
  _rtcma_static_lib(Rtcma::_mbedx509       mbedtls_external        ${_lib}/libmbedx509.a)
  _rtcma_static_lib(Rtcma::_mbedcrypto     mbedtls_external        ${_lib}/libmbedcrypto.a)

  add_library(Rtcma::libdatachannel INTERFACE IMPORTED)
  target_link_libraries(Rtcma::libdatachannel INTERFACE
    Rtcma::_libdatachannel
    Rtcma::_juice
    Rtcma::_srtp2
    Rtcma::_usrsctp
    Rtcma::_mbedtls
    Rtcma::_mbedx509
    Rtcma::_mbedcrypto
    Threads::Threads)
  # Fold libstdc++ into anything that links Rtcma::libdatachannel. Requires
  # the linker driver to be g++ - see tests/CMakeLists.txt (LINKER_LANGUAGE CXX).
  # Scoped to the bundled-libdatachannel target: when libdatachannel comes
  # from find_package, the caller decides whether to static-link libstdc++.
  #
  # libgcc_s.so.1 is intentionally left dynamic. AppImage / linuxdeploy
  # convention treats it as host-provided alongside libc/libm - it's a hard
  # dep of glibc on every distro. -static-libgcc would only partially help
  # anyway (the versioned _Unwind_* symbols libstdc++.a expects don't fully
  # resolve from libgcc_eh.a), and mixing static libgcc.a helpers with a
  # dynamic libgcc_s.so for unwinding is incoherent.
  target_link_options(Rtcma::libdatachannel INTERFACE -static-libstdc++)
endif()

# -- opus ---------------------------------------------------------------
if(RTCMA_BUNDLE_OPUS)
  # --- opus 1.6.1 (sha256-pinned tarball) ------------------------------------
  ExternalProject_Add(opus_external
    URL               https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz
    URL_HASH          SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1
    PREFIX            ${CMAKE_BINARY_DIR}/_opus
    CMAKE_CACHE_ARGS
      ${_common_cache_args}
      -DOPUS_BUILD_PROGRAMS:BOOL=OFF
      -DOPUS_BUILD_TESTING:BOOL=OFF
      -DOPUS_INSTALL_PKG_CONFIG_MODULE:BOOL=ON
    BUILD_BYPRODUCTS
      ${_lib}/libopus.a)

  _rtcma_static_lib(Rtcma::_opus opus_external ${_lib}/libopus.a)

  add_library(Rtcma::opus INTERFACE IMPORTED)
  target_link_libraries(Rtcma::opus INTERFACE Rtcma::_opus)
endif()
