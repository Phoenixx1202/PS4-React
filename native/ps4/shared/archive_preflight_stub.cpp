// PS4 archive preflight.
//
// The full preflight reads 7z/RAR headers and validates LZMA decoder windows.
// OpenOrbis' libarchive 3.3 has no LZMA development headers, so the PS4 build
// lets libarchive reject archives it cannot decode instead of pre-scanning them.

#include "archive_preflight.hpp"

namespace archives {

std::chrono::milliseconds inspect_budget{100};

std::string preflight(const std::vector<std::string>&) { return {}; }

Inspection inspect(const std::vector<std::string>&) { return {}; }

} // namespace archives
