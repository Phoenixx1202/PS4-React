/* dlmalloc 2.8.6 configured as the app heap.
 * Based on DolphinPS4 by iHaiDeeZ and PS4SX2.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Only the mspace API is exported; no global malloc symbols, so nothing clashes
 * with the OpenOrbis libc. The memory block is mapped by ps4_heap.cpp. OpenOrbis
 * libc's own heap maps about 2.5 GiB of system flexible memory on the first
 * malloc; that fails on retail consoles and the next allocation faults at
 * address 0x38. The replacement table in ps4_crt1.S sends libc's malloc family
 * here before it reaches that path.
 */

#define ONLY_MSPACES 1
#define MSPACES 1
#define USE_LOCKS 1
#define HAVE_MMAP 0
#define HAVE_MREMAP 0
#define HAVE_MORECORE 0
#define FOOTERS 1
#define INSECURE 0
#define USE_DEV_RANDOM 0
#define NO_MALLINFO 1
#define NO_MALLOC_STATS 1
#define LACKS_SCHED_H 0
#define malloc_getpagesize ((size_t)16384U)
#define DEFAULT_GRANULARITY ((size_t)64U * (size_t)1024U)

void ps4_heap_error(void* mspace, void* chunk, int corruption);
#define CORRUPTION_ERROR_ACTION(m) ps4_heap_error((m), 0, 1)
#define USAGE_ERROR_ACTION(m, p) ps4_heap_error((m), (p), 0)

#include "third_party/dlmalloc/malloc.c"

int ps4_heap_ok(void* msp) {
    mstate ms = (mstate)msp;
    return ok_magic(ms);
}
