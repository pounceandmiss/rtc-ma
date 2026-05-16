/*
 * Diagnostic: call rtcma_enumerate_devices and dump each entry's id
 * as both a C string (printf %s) and a hex dump of the bytes up to
 * strlen + a few extra. If the id strings look like "pulse:..." here
 * but show up mangled in the Tcl layer, the bug is in the binding.
 */

#include "rtcma.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump_id(const char *id)
{
    if (!id) { printf("    id=(null)\n"); return; }
    size_t n    = strlen(id);
    size_t show = n < 64 ? n : 64;
    printf("    id=%s\n", id);
    printf("    strlen=%zu\n", n);
    printf("    first %zu bytes (hex):", show);
    for (size_t i = 0; i < show; i++) {
        printf(" %02x", (unsigned char)id[i]);
    }
    printf("\n");
}

int main(void)
{
    RtcmaDeviceList list = {0};
    if (rtcma_enumerate_devices(&list) != 0) {
        fprintf(stderr, "enumerate failed\n");
        return 1;
    }
    printf("playback: %d device(s)\n", list.playback_count);
    for (int i = 0; i < list.playback_count; i++) {
        printf("  [%d] name=%s default=%d\n",
               i, list.playback[i].name, list.playback[i].is_default);
        dump_id(list.playback[i].id);
    }
    printf("capture: %d device(s)\n", list.capture_count);
    for (int i = 0; i < list.capture_count; i++) {
        printf("  [%d] name=%s default=%d\n",
               i, list.capture[i].name, list.capture[i].is_default);
        dump_id(list.capture[i].id);
    }
    rtcma_device_list_free(&list);
    return 0;
}
