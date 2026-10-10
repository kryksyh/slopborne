/* Explicit CPU bring-up backend. Never reports graphics/network success and
 * exports no fake guest services. Unimplemented imports retain loader traps. */
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
void bbgpu_register_kernel(void) {}
void bbnet_configure(const char *a, const char *b, const char *c, const char *d) {
    (void)a; (void)b; (void)c; (void)d;
}
int bbgpu_init(const BbGpuConfig *config) {
    (void)config;
    fputs("This is the CPU bring-up build. Use --cpu-only; the game renderer is unavailable.\n",stderr);
    exit(1);
}
uintptr_t bbgpu_resolve(const char *nid) { (void)nid; return 0; }
int bbgpu_handle_fault(void *context, void *address) { (void)context; (void)address; return 0; }
void bbgpu_dump_guest_writes(void *context) { (void)context; }
int bbgpu_text_input_begin(const char *initial, const char *prompt) { (void)initial; (void)prompt; return 0; }
int bbgpu_text_input_poll(char *text, uint64_t size) { (void)text; (void)size; return 2; }
int bbgpu_overlay_captures_input(void) { return 0; }
unsigned bbgpu_symbol_count(void) { return 0; }
int vulkan_smoke(void) {
    fputs("Use the separate bb-vulkan-smoke executable to test Metal/Vulkan.\n",stderr);
    return 1;
}
