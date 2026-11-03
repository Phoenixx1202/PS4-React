// PS4 heap: a dlmalloc mspace over memory mapped by us, installed as the libc
// malloc replacement. Based on DolphinPS4 by iHaiDeeZ and PS4SX2.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// OpenOrbis' libc builds its own heap on the first malloc by mapping about
// 2.5 GiB of system flexible memory. That mapping fails on retail consoles;
// libc then dereferences the null heap at address 0x38 and the title crashes
// before main(). ps4_crt1.S points _sceLibcParam's malloc table at these
// functions, so libc never reaches that path.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
int32_t sceKernelDebugOutText(int32_t channel, const char* format, ...);
int32_t sceKernelAvailableFlexibleMemorySize(size_t* size);
int32_t sceKernelReserveVirtualRange(void** address, size_t length, int32_t flags, size_t alignment);
int32_t sceKernelMapNamedFlexibleMemory(void** address, size_t length, int32_t prot,
                                        int32_t flags, const char* name);
int32_t sceKernelMapNamedSystemFlexibleMemory(void** address, size_t length, int32_t prot,
                                              int32_t flags, const char* name);
size_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t length, size_t alignment,
                                      int32_t type, off_t* physical);
int32_t sceKernelMapDirectMemory(void** address, size_t length, int32_t prot, int32_t flags,
                                 off_t physical, size_t alignment);

// dlmalloc mspace API (ps4_dlmalloc.c).
typedef void* mspace;
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
void* mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void* mem);
void* mspace_calloc(mspace msp, size_t count, size_t size);
void* mspace_realloc(mspace msp, void* mem, size_t size);
void* mspace_memalign(mspace msp, size_t alignment, size_t bytes);
size_t mspace_usable_size(const void* mem);
}

namespace {

constexpr size_t kMiB = 1024 * 1024;
constexpr size_t kPage = 16 * 1024;
constexpr int32_t kProtCpuRw = 0x3;
constexpr int32_t kMapFixed = 0x10;
constexpr size_t kDirectAlignment = 2 * kMiB;

mspace heap = nullptr;
bool creating_heap = false;

__attribute__((format(printf, 1, 2))) void heap_log(const char* format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);

    sceKernelDebugOutText(0, "%s", line);

    mkdir("/data/PS4React", 0777);
    const int fd = ::open("/data/PS4React/heap.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        (void)!::write(fd, line, std::strlen(line));
        (void)!::write(fd, "\n", 1);
        ::close(fd);
    }
}

mspace create_mspace(const char* pool, void* base, size_t size) {
    mspace result = create_mspace_with_base(base, size, 1);
    if (!result) {
        heap_log("heap: create_mspace_with_base on %zu MiB of %s failed", size / kMiB, pool);
        return nullptr;
    }
    void* probe = mspace_malloc(result, 4096);
    if (!probe) {
        heap_log("heap: %zu MiB of %s at %p failed its first allocation",
                 size / kMiB, pool, base);
        return nullptr;
    }
    mspace_free(result, probe);
    heap_log("heap: %zu MiB of %s at %p", size / kMiB, pool, base);
    return result;
}

// The kernel library puts its own objects around 0x200000000. A reservation
// there would overlap them and corrupt the heap control block, so ask for an
// address well above that range.
void* reserve_high(size_t size) {
    const uintptr_t hints[] = {0x1000000000ull, 0x800000000ull, 0x600000000ull};
    for (uintptr_t hint : hints) {
        void* address = reinterpret_cast<void*>(hint);
        if (sceKernelReserveVirtualRange(&address, size, 0, kPage) == 0)
            return address;
    }
    return nullptr;
}

mspace map_heap(bool system_pool, size_t size) {
    void* guard = reserve_high(size + kPage);
    if (!guard)
        return nullptr;
    void* base = static_cast<char*>(guard) + kPage;
    const int32_t result =
        system_pool
            ? sceKernelMapNamedSystemFlexibleMemory(&base, size, kProtCpuRw, kMapFixed,
                                                    "ps4-react heap")
            : sceKernelMapNamedFlexibleMemory(&base, size, kProtCpuRw, kMapFixed,
                                              "ps4-react heap");
    if (result != 0) {
        heap_log("heap: mapping %zu MiB of %s flexible memory failed (0x%08x)",
                 size / kMiB, system_pool ? "system" : "regular", static_cast<unsigned>(result));
        return nullptr;
    }
    return create_mspace(system_pool ? "system flexible memory" : "flexible memory", base, size);
}

mspace create_direct_heap(size_t size) {
    off_t physical = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), size,
                                      kDirectAlignment, 0, &physical) != 0) {
        heap_log("heap: allocating %zu MiB of direct memory failed", size / kMiB);
        return nullptr;
    }
    void* base = reinterpret_cast<void*>(0x1800000000ull);
    const int32_t result =
        sceKernelMapDirectMemory(&base, size, kProtCpuRw, 0, physical, kDirectAlignment);
    if (result != 0) {
        heap_log("heap: mapping %zu MiB of direct memory failed (0x%08x)",
                 size / kMiB, static_cast<unsigned>(result));
        return nullptr;
    }
    return create_mspace("direct memory", base, size);
}

mspace create_heap() {
    for (size_t size : {512 * kMiB, 384 * kMiB, 256 * kMiB, 192 * kMiB, 128 * kMiB}) {
        if (mspace result = map_heap(true, size))
            return result;
    }
    for (size_t size : {256 * kMiB, 128 * kMiB}) {
        if (mspace result = map_heap(false, size))
            return result;
    }
    for (size_t size : {256 * kMiB, 128 * kMiB}) {
        if (mspace result = create_direct_heap(size))
            return result;
    }
    heap_log("heap: no memory available");
    return nullptr;
}

mspace get_heap() {
    if (!heap && !creating_heap) {
        creating_heap = true;
        heap = create_heap();
        creating_heap = false;
    }
    return heap;
}

void* allocate(size_t size) {
    mspace msp = get_heap();
    return msp ? mspace_malloc(msp, size) : nullptr;
}

} // namespace

extern "C" {

void ps4_heap_error(void* msp, void* chunk, int corruption) {
    heap_log("heap: %s in mspace %p, chunk %p",
             corruption ? "corruption" : "invalid free", msp, chunk);
    __builtin_trap();
}

int ps4_replace_malloc_init(void) {
    get_heap();
    return 0;
}

int ps4_replace_malloc_finalize(void) { return 0; }

void* ps4_replace_malloc(size_t size) { return allocate(size); }

void ps4_replace_free(void* pointer) {
    if (pointer && get_heap())
        mspace_free(get_heap(), pointer);
}

void* ps4_replace_calloc(size_t count, size_t size) {
    mspace msp = get_heap();
    return msp ? mspace_calloc(msp, count, size) : nullptr;
}

void* ps4_replace_realloc(void* pointer, size_t size) {
    mspace msp = get_heap();
    return msp ? mspace_realloc(msp, pointer, size) : nullptr;
}

void* ps4_replace_memalign(size_t boundary, size_t size) {
    mspace msp = get_heap();
    return msp ? mspace_memalign(msp, boundary, size) : nullptr;
}

void* ps4_replace_reallocalign(void* pointer, size_t size, size_t boundary) {
    if (!pointer)
        return ps4_replace_memalign(boundary, size);
    if (size == 0) {
        ps4_replace_free(pointer);
        return nullptr;
    }
    void* replacement = ps4_replace_memalign(boundary, size);
    if (!replacement)
        return nullptr;
    const size_t old_size = mspace_usable_size(pointer);
    std::memcpy(replacement, pointer, old_size < size ? old_size : size);
    ps4_replace_free(pointer);
    return replacement;
}

int ps4_replace_posix_memalign(void** output, size_t boundary, size_t size) {
    void* pointer = ps4_replace_memalign(boundary, size);
    if (!pointer)
        return 12; // ENOMEM
    *output = pointer;
    return 0;
}

int ps4_replace_malloc_stats(void* output) {
    (void)output;
    return 0;
}

int ps4_replace_malloc_stats_fast(void* output) {
    (void)output;
    return 0;
}

size_t ps4_replace_malloc_usable_size(void* pointer) {
    return pointer ? mspace_usable_size(pointer) : 0;
}

// The app's own malloc family goes through these wrappers. libc's internal
// allocations use the replacement table above, but the app's first calloc is
// resolved at link time; without --wrap it still enters libc's broken heap
// setup before that table is consulted.
void* __wrap_malloc(size_t size) { return ps4_replace_malloc(size); }
void __wrap_free(void* pointer) { ps4_replace_free(pointer); }
void* __wrap_calloc(size_t count, size_t size) { return ps4_replace_calloc(count, size); }
void* __wrap_realloc(void* pointer, size_t size) { return ps4_replace_realloc(pointer, size); }
void* __wrap_memalign(size_t boundary, size_t size) {
    return ps4_replace_memalign(boundary, size);
}
int __wrap_posix_memalign(void** output, size_t boundary, size_t size) {
    return ps4_replace_posix_memalign(output, boundary, size);
}

} // extern "C"
