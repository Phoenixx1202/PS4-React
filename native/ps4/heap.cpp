#include <cstdint>

// OpenOrbis libc reads this symbol at startup to size the process heap. The
// default is too small for two 1080p ARGB buffers plus C++ allocations on some
// SDK builds; 512 MiB is safe on a retail PS4.
extern "C" std::uint64_t sceLibcHeapSize = 512ull * 1024 * 1024;
