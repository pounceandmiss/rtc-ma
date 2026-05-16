/* rtcma_tcl.h - Tcl 9 bindings for librtcma.
 *
 * Wraps the C API in include/rtcma.h with ::rtcma::* commands. Coexists
 * with the libdatachannel-tcl runtime: rtc-ma never installs Tcl-level
 * callbacks, so its commands take and return the same int track ids
 * that ::rtc::* hands out.
 *
 * Two binary forms are produced when -DRTCMA_BUILD_TCL=ON:
 *   - rtcma.so          loadable extension; `package require rtcma`.
 *   - librtcma_tcl.a    static archive; embedders link it into a custom
 *                       tclsh and call Rtcma_Init (typically via
 *                       Tcl_StaticPackage).
 */

#ifndef RTCMA_TCL_H
#define RTCMA_TCL_H

#include <tcl.h>

#ifdef __cplusplus
extern "C" {
#endif

DLLEXPORT int Rtcma_Init(Tcl_Interp *interp);

#ifdef __cplusplus
}
#endif

#endif /* RTCMA_TCL_H */
