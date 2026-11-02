// RAR5 password support is not wired on PS4 yet: libarchive 3.3 has no RAR5
// reader and the dedicated UnRAR worker is not packaged. Unencrypted ZIP/TAR/7z
// still work; encrypted RAR5 reports that the worker is unavailable.

#include "rar5_password.hpp"

namespace archives {
namespace {
struct State {};
std::string no_error;
} // namespace

bool Rar5PasswordReader::matches(const std::string&) { return false; }

Rar5PasswordReader::Rar5PasswordReader(const std::vector<std::string>&,
                                       const std::string&,
                                       const std::atomic<bool>&)
    : state(std::make_unique<State>()) {}

Rar5PasswordReader::~Rar5PasswordReader() = default;

la_ssize_t Rar5PasswordReader::read(archive*, void*, const void**) { return -1; }

const std::string& Rar5PasswordReader::error() const { return no_error; }

bool Rar5PasswordReader::verify(const std::string&, std::uint32_t) const {
    return false;
}

} // namespace archives
