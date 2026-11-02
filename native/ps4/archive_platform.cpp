// PS4 host hooks for the shared archive extractor.
//
// ZIP/TAR/7z extraction runs in-process through libarchive. RAR sets need the
// dedicated UnRAR worker, which is not packaged yet, so start_rar_worker reports
// that clearly instead of failing halfway through an extraction.

#include "archives.hpp"

#include "host_api.hpp"
#include "image_loader.hpp"
#include "network.hpp"
#include "system.hpp"

#include <cstddef>
#include <string>

namespace archives {

void trace(const char* step, const char* detail) {
    ps4::system::log("[ARCH] %s%s%s", step ? step : "",
                     detail && *detail ? ": " : "", detail ? detail : "");
}

std::size_t pipeline_bytes() {
    // Inline writes: the extractor reads straight into the destination file and
    // does not reserve a write ring on the 512 MiB title heap.
    return 0;
}

bool hold_memory(bool hold) {
    images::hold_memory(hold);
    return true;
}

bool downloading() { return network::downloading(); }

bool list_directory(const char* path, void (*visit)(const char* name, void* user),
                    void* user) {
    return host::read_dir(path, visit, user);
}

int start_rar_worker(const std::string&, std::string& error) {
    error = "RAR extraction is not available on PS4 yet; use ZIP, TAR or 7z.";
    return -1;
}

long read_rar_worker(int, char*, std::size_t) { return -1; }
void cancel_rar_worker(int) {}
void close_rar_worker(int) {}

} // namespace archives
