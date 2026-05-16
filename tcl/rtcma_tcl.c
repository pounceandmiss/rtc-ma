/*
 * rtcma_tcl.c - ::rtcma::* commands wrapping include/rtcma.h.
 *
 * Handle scheme: rtc-ma's public API uses opaque struct pointers, not
 * int ids like libdatachannel. The binding mints small synthetic int
 * handles and keeps {kind, void *} entries in a process-global hash
 * table. Type-checking on lookup catches handle confusion at the Tcl
 * boundary. Mirrors the int-handle convention so Tcl scripts can pass
 * player / capturer handles around the same way they pass ::rtc::*
 * track ids.
 *
 * No callback trampolines: rtc-ma's data path is entirely C-side
 * (miniaudio thread -> libdatachannel worker thread). The Tcl layer is
 * just configuration + lifecycle, so there is no Tcl_ThreadQueueEvent
 * machinery the way libdatachannel-tcl needs.
 */

#include "rtcma_tcl.h"
#include "rtcma.h"

#include <stdint.h>
#include <string.h>

#ifndef RTCMA_TCL_PACKAGE_NAME
#define RTCMA_TCL_PACKAGE_NAME "rtcma"
#endif

#ifndef RTCMA_TCL_PACKAGE_VERSION
#define RTCMA_TCL_PACKAGE_VERSION "0.1.0"
#endif

/* -- handle registry ------------------------------------------------ */

typedef enum {
    H_PLAYER   = 1,
    H_CAPTURER = 2,
} HandleKind;

typedef struct {
    HandleKind kind;
    void      *ptr;
} HandleEntry;

static Tcl_Mutex     g_mutex = NULL;
static Tcl_HashTable g_handles;
static int           g_next_id = 1;
static int           g_inited  = 0;

static void EnsureRegistry(void) {
    Tcl_MutexLock(&g_mutex);
    if (!g_inited) {
        Tcl_InitHashTable(&g_handles, TCL_ONE_WORD_KEYS);
        g_inited = 1;
    }
    Tcl_MutexUnlock(&g_mutex);
}

static int InsertHandle(HandleKind kind, void *ptr) {
    Tcl_MutexLock(&g_mutex);
    int id = g_next_id++;
    int isNew;
    Tcl_HashEntry *he = Tcl_CreateHashEntry(&g_handles, (void *)(intptr_t)id, &isNew);
    HandleEntry *e = (HandleEntry *)ckalloc(sizeof(*e));
    e->kind = kind;
    e->ptr  = ptr;
    Tcl_SetHashValue(he, e);
    Tcl_MutexUnlock(&g_mutex);
    return id;
}

static void *LookupHandle(Tcl_Interp *interp, Tcl_Obj *o, HandleKind kind,
                          const char *kind_name) {
    int id;
    if (Tcl_GetIntFromObj(interp, o, &id) != TCL_OK) return NULL;
    Tcl_MutexLock(&g_mutex);
    Tcl_HashEntry *he = Tcl_FindHashEntry(&g_handles, (void *)(intptr_t)id);
    HandleEntry *e = he ? (HandleEntry *)Tcl_GetHashValue(he) : NULL;
    void *ptr = (e && e->kind == kind) ? e->ptr : NULL;
    Tcl_MutexUnlock(&g_mutex);
    if (!ptr) {
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("invalid %s handle: %d", kind_name, id));
        Tcl_SetErrorCode(interp, "RTCMA", "invalid-handle", (char *)NULL);
    }
    return ptr;
}

static int LookupAndRemove(Tcl_Interp *interp, Tcl_Obj *o, HandleKind kind,
                           const char *kind_name, void **out_ptr) {
    int id;
    if (Tcl_GetIntFromObj(interp, o, &id) != TCL_OK) return TCL_ERROR;
    Tcl_MutexLock(&g_mutex);
    Tcl_HashEntry *he = Tcl_FindHashEntry(&g_handles, (void *)(intptr_t)id);
    HandleEntry *e = he ? (HandleEntry *)Tcl_GetHashValue(he) : NULL;
    if (!e || e->kind != kind) {
        Tcl_MutexUnlock(&g_mutex);
        Tcl_SetObjResult(interp,
            Tcl_ObjPrintf("invalid %s handle: %d", kind_name, id));
        Tcl_SetErrorCode(interp, "RTCMA", "invalid-handle", (char *)NULL);
        return TCL_ERROR;
    }
    *out_ptr = e->ptr;
    ckfree((char *)e);
    Tcl_DeleteHashEntry(he);
    Tcl_MutexUnlock(&g_mutex);
    return TCL_OK;
}

/* rtc-ma functions return 0 on success and -1 on failure. There is no
 * negative-error namespace to translate, so a failure surfaces as a
 * single ::errorCode {RTCMA failure}. */
static int ReturnRc(Tcl_Interp *interp, int rc) {
    if (rc < 0) {
        Tcl_SetObjResult(interp, Tcl_NewStringObj("rtcma error", -1));
        Tcl_SetErrorCode(interp, "RTCMA", "failure", (char *)NULL);
        return TCL_ERROR;
    }
    Tcl_SetObjResult(interp, Tcl_NewIntObj(rc));
    return TCL_OK;
}

/* -- ::rtcma::enumerate-devices ------------------------------------- */

/* Returns a dict {playback {<list-of-dicts>} capture {<list-of-dicts>}}.
 * Each entry dict holds {name STRING default BOOL id STRING}. The `id`
 * is a persistable ASCII "<backend>:<payload>" identifier (see
 * include/rtcma.h RtcmaAudioDevice); pass it back as -device-id to
 * ::rtcma::player::new / ::rtcma::capturer::new / their reopen
 * commands to pin a specific endpoint. */
static Tcl_Obj *DeviceToDict(const RtcmaAudioDevice *d) {
    Tcl_Obj *dict = Tcl_NewDictObj();
    Tcl_DictObjPut(NULL, dict, Tcl_NewStringObj("name", -1),
                   Tcl_NewStringObj(d->name, -1));
    Tcl_DictObjPut(NULL, dict, Tcl_NewStringObj("default", -1),
                   Tcl_NewBooleanObj(d->is_default));
    Tcl_DictObjPut(NULL, dict, Tcl_NewStringObj("id", -1),
                   Tcl_NewStringObj(d->id ? d->id : "", -1));
    return dict;
}

static int Cmd_enumerate(void *cd, Tcl_Interp *interp, int objc,
                         Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 1) {
        Tcl_WrongNumArgs(interp, 1, objv, "");
        return TCL_ERROR;
    }
    RtcmaDeviceList list = {0};
    if (rtcma_enumerate_devices(&list) != 0) return ReturnRc(interp, -1);
    Tcl_Obj *pb = Tcl_NewListObj(0, NULL);
    for (int i = 0; i < list.playback_count; i++)
        Tcl_ListObjAppendElement(NULL, pb, DeviceToDict(&list.playback[i]));
    Tcl_Obj *cap = Tcl_NewListObj(0, NULL);
    for (int i = 0; i < list.capture_count; i++)
        Tcl_ListObjAppendElement(NULL, cap, DeviceToDict(&list.capture[i]));
    Tcl_Obj *out = Tcl_NewDictObj();
    Tcl_DictObjPut(NULL, out, Tcl_NewStringObj("playback", -1), pb);
    Tcl_DictObjPut(NULL, out, Tcl_NewStringObj("capture",  -1), cap);
    rtcma_device_list_free(&list);
    Tcl_SetObjResult(interp, out);
    return TCL_OK;
}

/* -- ::rtcma::player::new / start / reopen / attach / detach / destroy -- */

static int Cmd_player_new(void *cd, Tcl_Interp *interp, int objc,
                          Tcl_Obj *const objv[]) {
    (void)cd;
    RtcmaPlayerConfig cfg = {0};
    for (int i = 1; i < objc; i++) {
        const char *opt = Tcl_GetString(objv[i]);
        if (i + 1 >= objc) {
            Tcl_SetObjResult(interp,
                Tcl_ObjPrintf("option %s requires a value", opt));
            return TCL_ERROR;
        }
        Tcl_Obj *val = objv[++i];
        if (strcmp(opt, "-channels") == 0) {
            if (Tcl_GetIntFromObj(interp, val, &cfg.channels) != TCL_OK)
                return TCL_ERROR;
        } else if (strcmp(opt, "-payload-type") == 0) {
            if (Tcl_GetIntFromObj(interp, val, &cfg.payload_type) != TCL_OK)
                return TCL_ERROR;
        } else if (strcmp(opt, "-device-id") == 0) {
            const char *s = Tcl_GetString(val);
            cfg.device_id = (s && *s) ? s : NULL;
        } else {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s", opt));
            return TCL_ERROR;
        }
    }
    RtcmaPlayer *p = rtcma_player_new(&cfg);
    if (!p) return ReturnRc(interp, -1);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(InsertHandle(H_PLAYER, p)));
    return TCL_OK;
}

static int Cmd_player_start(void *cd, Tcl_Interp *interp, int objc,
                            Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 2) { Tcl_WrongNumArgs(interp, 1, objv, "player"); return TCL_ERROR; }
    RtcmaPlayer *p = LookupHandle(interp, objv[1], H_PLAYER, "player");
    if (!p) return TCL_ERROR;
    return ReturnRc(interp, rtcma_player_start(p));
}

/* ::rtcma::player::reopen $player ?-device-id STRING?
 *
 * Empty or omitted -device-id means "system default". */
static int Cmd_player_reopen(void *cd, Tcl_Interp *interp, int objc,
                             Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "player ?-device-id string?");
        return TCL_ERROR;
    }
    RtcmaPlayer *p = LookupHandle(interp, objv[1], H_PLAYER, "player");
    if (!p) return TCL_ERROR;
    const char *id = NULL;
    for (int i = 2; i < objc; i++) {
        const char *opt = Tcl_GetString(objv[i]);
        if (i + 1 >= objc) {
            Tcl_SetObjResult(interp,
                Tcl_ObjPrintf("option %s requires a value", opt));
            return TCL_ERROR;
        }
        Tcl_Obj *val = objv[++i];
        if (strcmp(opt, "-device-id") == 0) {
            const char *s = Tcl_GetString(val);
            id = (s && *s) ? s : NULL;
        } else {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s", opt));
            return TCL_ERROR;
        }
    }
    return ReturnRc(interp, rtcma_player_reopen(p, id));
}

static int Cmd_player_attach(void *cd, Tcl_Interp *interp, int objc,
                             Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "player rtc_track");
        return TCL_ERROR;
    }
    RtcmaPlayer *p = LookupHandle(interp, objv[1], H_PLAYER, "player");
    if (!p) return TCL_ERROR;
    int track;
    if (Tcl_GetIntFromObj(interp, objv[2], &track) != TCL_OK) return TCL_ERROR;
    return ReturnRc(interp, rtcma_player_attach(p, track));
}

static int Cmd_player_detach(void *cd, Tcl_Interp *interp, int objc,
                             Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 2) { Tcl_WrongNumArgs(interp, 1, objv, "player"); return TCL_ERROR; }
    RtcmaPlayer *p = LookupHandle(interp, objv[1], H_PLAYER, "player");
    if (!p) return TCL_ERROR;
    return ReturnRc(interp, rtcma_player_detach(p));
}

static int Cmd_player_set_volume(void *cd, Tcl_Interp *interp, int objc,
                                 Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "player volume");
        return TCL_ERROR;
    }
    RtcmaPlayer *p = LookupHandle(interp, objv[1], H_PLAYER, "player");
    if (!p) return TCL_ERROR;
    double v;
    if (Tcl_GetDoubleFromObj(interp, objv[2], &v) != TCL_OK) return TCL_ERROR;
    return ReturnRc(interp, rtcma_player_set_volume(p, (float)v));
}

static int Cmd_player_destroy(void *cd, Tcl_Interp *interp, int objc,
                              Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 2) { Tcl_WrongNumArgs(interp, 1, objv, "player"); return TCL_ERROR; }
    void *ptr;
    if (LookupAndRemove(interp, objv[1], H_PLAYER, "player", &ptr) != TCL_OK)
        return TCL_ERROR;
    rtcma_player_destroy((RtcmaPlayer *)ptr);
    return TCL_OK;
}

/* -- ::rtcma::capturer::new / start / reopen / attach / detach / destroy -- */

static int Cmd_capturer_new(void *cd, Tcl_Interp *interp, int objc,
                            Tcl_Obj *const objv[]) {
    (void)cd;
    RtcmaCapturerConfig cfg = {0};
    for (int i = 1; i < objc; i++) {
        const char *opt = Tcl_GetString(objv[i]);
        if (i + 1 >= objc) {
            Tcl_SetObjResult(interp,
                Tcl_ObjPrintf("option %s requires a value", opt));
            return TCL_ERROR;
        }
        Tcl_Obj *val = objv[++i];
        if (strcmp(opt, "-channels") == 0) {
            if (Tcl_GetIntFromObj(interp, val, &cfg.channels) != TCL_OK)
                return TCL_ERROR;
        } else if (strcmp(opt, "-payload-type") == 0) {
            if (Tcl_GetIntFromObj(interp, val, &cfg.payload_type) != TCL_OK)
                return TCL_ERROR;
        } else if (strcmp(opt, "-device-id") == 0) {
            const char *s = Tcl_GetString(val);
            cfg.device_id = (s && *s) ? s : NULL;
        } else {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s", opt));
            return TCL_ERROR;
        }
    }
    /* channels: 0 = default to mono device, 1/2 = explicit. The opus
     * encoder's channel count is auto-detected from SDP regardless. */
    if (cfg.channels != 0 && cfg.channels != 1 && cfg.channels != 2) {
        Tcl_SetObjResult(interp,
            Tcl_NewStringObj("-channels must be 0, 1, or 2", -1));
        return TCL_ERROR;
    }
    RtcmaCapturer *c = rtcma_capturer_new(&cfg);
    if (!c) return ReturnRc(interp, -1);
    Tcl_SetObjResult(interp, Tcl_NewIntObj(InsertHandle(H_CAPTURER, c)));
    return TCL_OK;
}

static int Cmd_capturer_start(void *cd, Tcl_Interp *interp, int objc,
                              Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 2) { Tcl_WrongNumArgs(interp, 1, objv, "capturer"); return TCL_ERROR; }
    RtcmaCapturer *c = LookupHandle(interp, objv[1], H_CAPTURER, "capturer");
    if (!c) return TCL_ERROR;
    return ReturnRc(interp, rtcma_capturer_start(c));
}

static int Cmd_capturer_reopen(void *cd, Tcl_Interp *interp, int objc,
                               Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc < 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "capturer ?-device-id string?");
        return TCL_ERROR;
    }
    RtcmaCapturer *c = LookupHandle(interp, objv[1], H_CAPTURER, "capturer");
    if (!c) return TCL_ERROR;
    const char *id = NULL;
    for (int i = 2; i < objc; i++) {
        const char *opt = Tcl_GetString(objv[i]);
        if (i + 1 >= objc) {
            Tcl_SetObjResult(interp,
                Tcl_ObjPrintf("option %s requires a value", opt));
            return TCL_ERROR;
        }
        Tcl_Obj *val = objv[++i];
        if (strcmp(opt, "-device-id") == 0) {
            const char *s = Tcl_GetString(val);
            id = (s && *s) ? s : NULL;
        } else {
            Tcl_SetObjResult(interp, Tcl_ObjPrintf("unknown option: %s", opt));
            return TCL_ERROR;
        }
    }
    return ReturnRc(interp, rtcma_capturer_reopen(c, id));
}

static int Cmd_capturer_attach(void *cd, Tcl_Interp *interp, int objc,
                               Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "capturer rtc_track");
        return TCL_ERROR;
    }
    RtcmaCapturer *c = LookupHandle(interp, objv[1], H_CAPTURER, "capturer");
    if (!c) return TCL_ERROR;
    int track;
    if (Tcl_GetIntFromObj(interp, objv[2], &track) != TCL_OK) return TCL_ERROR;
    return ReturnRc(interp, rtcma_capturer_attach(c, track));
}

static int Cmd_capturer_detach(void *cd, Tcl_Interp *interp, int objc,
                               Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 2) { Tcl_WrongNumArgs(interp, 1, objv, "capturer"); return TCL_ERROR; }
    RtcmaCapturer *c = LookupHandle(interp, objv[1], H_CAPTURER, "capturer");
    if (!c) return TCL_ERROR;
    return ReturnRc(interp, rtcma_capturer_detach(c));
}

static int Cmd_capturer_set_volume(void *cd, Tcl_Interp *interp, int objc,
                                   Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 3) {
        Tcl_WrongNumArgs(interp, 1, objv, "capturer volume");
        return TCL_ERROR;
    }
    RtcmaCapturer *c = LookupHandle(interp, objv[1], H_CAPTURER, "capturer");
    if (!c) return TCL_ERROR;
    double v;
    if (Tcl_GetDoubleFromObj(interp, objv[2], &v) != TCL_OK) return TCL_ERROR;
    return ReturnRc(interp, rtcma_capturer_set_volume(c, (float)v));
}

static int Cmd_capturer_destroy(void *cd, Tcl_Interp *interp, int objc,
                                Tcl_Obj *const objv[]) {
    (void)cd;
    if (objc != 2) { Tcl_WrongNumArgs(interp, 1, objv, "capturer"); return TCL_ERROR; }
    void *ptr;
    if (LookupAndRemove(interp, objv[1], H_CAPTURER, "capturer", &ptr) != TCL_OK)
        return TCL_ERROR;
    rtcma_capturer_destroy((RtcmaCapturer *)ptr);
    return TCL_OK;
}

/* -- module init ---------------------------------------------------- */

typedef struct {
    const char     *name;
    Tcl_ObjCmdProc *proc;
} RtcmaCommand;

static const RtcmaCommand kCommands[] = {
    { "::rtcma::enumerate-devices", Cmd_enumerate        },
    { "::rtcma::player::new",       Cmd_player_new       },
    { "::rtcma::player::start",     Cmd_player_start     },
    { "::rtcma::player::reopen",    Cmd_player_reopen    },
    { "::rtcma::player::attach",     Cmd_player_attach      },
    { "::rtcma::player::detach",     Cmd_player_detach      },
    { "::rtcma::player::set-volume", Cmd_player_set_volume  },
    { "::rtcma::player::destroy",    Cmd_player_destroy     },
    { "::rtcma::capturer::new",      Cmd_capturer_new       },
    { "::rtcma::capturer::start",    Cmd_capturer_start     },
    { "::rtcma::capturer::reopen",   Cmd_capturer_reopen    },
    { "::rtcma::capturer::attach",   Cmd_capturer_attach    },
    { "::rtcma::capturer::detach",   Cmd_capturer_detach    },
    { "::rtcma::capturer::set-volume", Cmd_capturer_set_volume },
    { "::rtcma::capturer::destroy",  Cmd_capturer_destroy   },
    { NULL, NULL }
};

int Rtcma_Init(Tcl_Interp *interp) {
#ifdef USE_TCL_STUBS
    if (Tcl_InitStubs(interp, "9.0", 0) == NULL) return TCL_ERROR;
#endif
    EnsureRegistry();
    static const char *namespaces[] = {
        "::rtcma", "::rtcma::player", "::rtcma::capturer", NULL
    };
    for (const char **ns = namespaces; *ns; ns++) {
        Tcl_CreateNamespace(interp, *ns, NULL, NULL);
    }
    Tcl_ResetResult(interp);
    for (const RtcmaCommand *c = kCommands; c->name; c++) {
        Tcl_CreateObjCommand(interp, c->name, c->proc, NULL, NULL);
    }
    if (Tcl_PkgProvide(interp, RTCMA_TCL_PACKAGE_NAME,
                       RTCMA_TCL_PACKAGE_VERSION) != TCL_OK) {
        return TCL_ERROR;
    }
    return TCL_OK;
}
