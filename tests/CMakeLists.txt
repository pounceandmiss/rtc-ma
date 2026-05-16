# Each test transitively links libdatachannel (a C++ static archive in
# bundled mode). LINKER_LANGUAGE CXX makes g++ the link driver so the
# -static-libstdc++ option from Rtcma::libdatachannel actually folds
# libstdc++ into the binary in bundled builds.

function(_rtcma_add_test name)
  set(_options AUDIBLE)
  set(_one TIMEOUT)
  cmake_parse_arguments(T "${_options}" "${_one}" "" ${ARGN})

  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE rtcma m)
  # Tests reach into src/ for the internal API (jitter, sdp parser,
  # internal track adapter). The library installs include/ publicly.
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
  # Keep assert() live in Release: tests are built with the project,
  # which defaults to -DNDEBUG in Release. -UNDEBUG re-enables assert()
  # after Release adds it, regardless of build type. Without this the
  # tests' state-changing calls embedded in assert() would compile out.
  target_compile_options(${name} PRIVATE -UNDEBUG)
  set_target_properties(${name} PROPERTIES LINKER_LANGUAGE CXX)

  add_test(NAME ${name} COMMAND ${name})
  if(T_AUDIBLE)
    set_tests_properties(${name} PROPERTIES LABELS "audible")
  endif()
  if(T_TIMEOUT)
    set_tests_properties(${name} PROPERTIES TIMEOUT ${T_TIMEOUT})
  endif()
endfunction()

_rtcma_add_test(test_jitter      TIMEOUT 10)
_rtcma_add_test(test_sdp_parse   TIMEOUT 10)
_rtcma_add_test(test_rtp_demux   TIMEOUT 10)
_rtcma_add_test(test_recv_decode TIMEOUT 10)
_rtcma_add_test(test_device_id_serialize TIMEOUT 10)
_rtcma_add_test(test_enumerate_dump      AUDIBLE TIMEOUT 10)

# Lifecycle / leak test: hits libdatachannel but not miniaudio, so it's
# safe to run in headless CI.
_rtcma_add_test(test_attach_detach TIMEOUT 30)

_rtcma_add_test(test_audible_loopback     AUDIBLE TIMEOUT 30)
_rtcma_add_test(test_audible_mic_loopback AUDIBLE TIMEOUT 30)
_rtcma_add_test(test_device_reopen        AUDIBLE TIMEOUT 15)

# Tcl-side device-id round-trip - only when the bindings are built.
# tclsh runs the .tcl script directly and loads the freshly-built
# rtcma.so by absolute path (bypasses package require / auto_path so a
# stale system install can't shadow the build artifact).
if(RTCMA_BUILD_TCL)
  find_program(RTCMA_TCLSH NAMES tclsh9.0 tclsh)
  if(RTCMA_TCLSH)
    add_test(NAME test_tcl_device_id
      COMMAND ${RTCMA_TCLSH}
              ${CMAKE_CURRENT_SOURCE_DIR}/test_tcl_device_id.tcl
              $<TARGET_FILE:rtcma_tcl>)
    set_tests_properties(test_tcl_device_id PROPERTIES
      LABELS  "audible"
      TIMEOUT 15)
  else()
    message(WARNING "rtcma/tests: tclsh not found - skipping test_tcl_device_id")
  endif()
endif()
