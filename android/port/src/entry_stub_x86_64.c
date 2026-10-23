/* x86_64 builds have no AArch64 gate entry (guest code cannot run on that ABI).
 * The symbols still exist so the shared host sources link; reaching them is a
 * programming error, so they fail loudly. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

uint64_t unimpl_stubs[4096];

static void unreachable(const char *what) {
    fprintf(stderr, "[biko3] %s called on a non-AArch64 build\n", what);
    abort();
}

void unimpl_entry(void) { unreachable("unimpl_entry"); }
void dm_gate(void) { unreachable("dm_gate"); }
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp) {
    (void)entry; (void)arg; (void)sp;
    unreachable("dm_thread_enter");
}
