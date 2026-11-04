/* The C ABI host of Docs/intro/for-programmers.md ("Drive it through the C ABI"), compiled as C99.
   The block between the snippet markers is copied into the intro; intro_snippets checks it.
   Usage: intro_capi_host <bundle dir>  (the directory xtframe-pack wrote) */

#include <stdio.h>
#include <stdlib.h>
#include <xt/capi/xtframe.h>

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: intro_capi_host <bundle dir>\n");
        return 2;
    }
    const char* bundleDir = argv[1];
    int result = 1;

    /* snippet:begin */
    xt_bundle* bundle = xt_bundle_open(bundleDir);
    if (!bundle) {
        fprintf(stderr, "open failed: %s\n", xt_capi_last_error());
        return 1;
    }
    xt_surface_desc desc = {0};
    desc.struct_size = sizeof desc;
    desc.width = 640;
    desc.height = 360;
    desc.system_fonts = 1; /* or bring fonts in the bundle; without any, no text is drawn */
    xt_surface* hud = xt_surface_create(bundle, "hud", &desc); /* UI id = the .xt file's stem */
    if (!hud) {
        fprintf(stderr, "surface failed: %s\n", xt_capi_last_error());
        return 1;
    }

    uint32_t hp = xt_surface_port_lookup(hud, "hp");
    unsigned char* pixels = malloc((size_t)desc.width * desc.height * 4);
    for (int frame = 0; frame < 3; ++frame) {
        xt_surface_port_set_number(hud, hp, 42.0);
        /* xt_surface_push_input(hud, &event);  for each input event */
        xt_surface_update(hud, 1.0f / 60.0f);
        xt_damage damage;
        xt_surface_render(hud, pixels, desc.width * 4, &damage); /* RGBA8 -> upload to your texture */
    }
    /* snippet:end */

    printf("intro_capi_host: hp=%g\n", xt_surface_port_get_number(hud, hp));
    if (xt_surface_port_get_number(hud, hp) == 42.0) {
        result = 0;
    }
    free(pixels);
    xt_surface_destroy(hud);
    xt_bundle_close(bundle);
    return result;
}
