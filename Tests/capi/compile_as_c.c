/* T-504 (Docs/design/50-c-abi.md 50.4 case 10): xt/capi/xtframe.h compiled by the C compiler as
   C99, linked against the shared library xtframe and run once. Exit 0 when the library's ABI
   version is the header's. */
#include <stdio.h>

#include "xt/capi/xtframe.h"

int main(void) {
    const uint32_t abi = xt_capi_abi_version();
    xt_status status = XT_STATUS_OK;
    xt_input_event event = {0};
    xt_damage damage = {0};
    event.struct_size = (uint32_t)sizeof(event);
    event.kind = XT_INPUT_POINTER_MOVE;
    (void)status;
    (void)damage;
    {
        /* T-709 (50 af): the world-layer struct and the camera helpers, which work in every build. */
        xt_world_layer layer = {0};
        float m[16];
        float origin[3], direction[3];
        layer.struct_size = (uint32_t)sizeof(layer);
        (void)layer;
        if (xt_world_layer_orbit_matrix(m, 1280.0f, 720.0f, 1280.0f, 720.0f, -30.0f, 14.0f, 0.0f) !=
                XT_STATUS_OK ||
            xt_world_layer_unproject(m, 1.0f, 640.0f, 360.0f, origin, direction) != XT_STATUS_OK ||
            !(direction[2] < 0.0f)) {
            fprintf(stderr, "xt_capi_c_compile: the world-layer camera helpers failed\n");
            return 1;
        }
        (void)xt_surface_world_layer_count(NULL);
    }
    if (abi != XT_CAPI_ABI_VERSION) {
        fprintf(stderr, "xt_capi_c_compile: ABI %u, header %u\n", (unsigned)abi,
                (unsigned)XT_CAPI_ABI_VERSION);
        return 1;
    }
    printf("xt_capi_c_compile: xtframe %s, ABI %u\n", xt_capi_version_string(), (unsigned)abi);
    return 0;
}
