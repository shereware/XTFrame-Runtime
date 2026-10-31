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
    if (abi != XT_CAPI_ABI_VERSION) {
        fprintf(stderr, "xt_capi_c_compile: ABI %u, header %u\n", (unsigned)abi,
                (unsigned)XT_CAPI_ABI_VERSION);
        return 1;
    }
    printf("xt_capi_c_compile: xtframe %s, ABI %u\n", xt_capi_version_string(), (unsigned)abi);
    return 0;
}
