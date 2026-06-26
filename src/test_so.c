/* Minimal 32-bit .so to prove the toolchain + injection path before we
 * pull in ImGui. When LD_PRELOAD'd into any process it writes one line to
 * stderr from a library constructor, then gets out of the way. */
#include <stdio.h>

__attribute__((constructor))
static void x3ui_probe(void)
{
    fprintf(stderr, "[x3ui] injected: 32-bit .so constructor ran (sizeof(void*)=%zu)\n",
            sizeof(void *));
}
