/*
 * Additive overrides on top of mbedtls's default mbedtls_config.h.
 * Consumed via -DMBEDTLS_USER_CONFIG_FILE=<this file> at mbedtls's CMake
 * configure time. The define is PUBLIC, so libdatachannel (which finds
 * mbedtls via find_package) inherits it automatically.
 */

/* DTLS-SRTP (RFC 5764) - libdatachannel's WebRTC media transport relies on
 * mbedtls_ssl_conf_dtls_srtp_protection_profiles &c., which are gated here. */
#define MBEDTLS_SSL_DTLS_SRTP
