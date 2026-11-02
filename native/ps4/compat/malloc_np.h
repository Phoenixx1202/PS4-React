#pragma once
#include <stddef.h>

/* OpenOrbis's FreeBSD-shaped libc does not ship malloc_np.h. QuickJS only
   needs this for js__malloc_usable_size(); the React bridge supplies its own
   accounting allocator, so a zero is safe here. */
static inline size_t malloc_usable_size(void *pointer) {
    (void)pointer;
    return 0;
}
