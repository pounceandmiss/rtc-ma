# Injected into libdatachannel's CMake configure via CMAKE_PROJECT_INCLUDE_BEFORE
# (set by ExternalProject_Add in cmake/BundleDeps.cmake).
#
# Background: libdatachannel ships its own cmake/Modules/FindMbedTLS.cmake
# that takes precedence over mbedtls's installed MbedTLSConfig.cmake (because
# it's installed first on CMAKE_MODULE_PATH). Unlike the CONFIG-mode targets,
# the bundled Find module does NOT propagate the PUBLIC compile definition
# that mbedtls's build sets for MBEDTLS_USER_CONFIG_FILE - so libdatachannel's
# compile would see mbedtls's default config (no DTLS-SRTP) and fail to find
# mbedtls_ssl_conf_dtls_srtp_protection_profiles and friends.
#
# Re-define MBEDTLS_USER_CONFIG_FILE here so libdatachannel (and its bundled
# libsrtp) compile against the same mbedtls config that mbedtls itself was
# built with. RTCMA_MBEDTLS_USER_CONFIG is set by the parent project
# via -D in ExternalProject_Add's CMAKE_CACHE_ARGS.
add_compile_definitions(MBEDTLS_USER_CONFIG_FILE="${RTCMA_MBEDTLS_USER_CONFIG}")
