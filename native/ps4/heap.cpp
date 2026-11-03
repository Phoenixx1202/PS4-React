#include <cstdint>

// OpenOrbis' crt1.o carries the local sceLibcHeapSize the loader reads, and its
// default (-1) gives a heap of only about 12.7 MiB on hardware. tools/patch_crt1.py
// rewrites that value at build time; this definition keeps the global symbol in
// sync for code that resolves it by name.
extern "C" std::uint64_t sceLibcHeapSize = 512ull * 1024 * 1024;
