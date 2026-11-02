// PS4 SHA-1/SHA-256 adapter over mbedTLS (the OpenOrbis libcurl is built
// against mbedTLS; there is no OpenSSL development package).

#pragma once

#include <mbedtls/md.h>

#include <cstddef>
#include <string>

namespace integrity {

class Hash {
  public:
    explicit Hash(bool sha1 = false)
        : type_(sha1 ? MBEDTLS_MD_SHA1 : MBEDTLS_MD_SHA256),
          wanted_(sha1 ? 20 : 32) {
        mbedtls_md_init(&context_);
        const mbedtls_md_info_t* info = mbedtls_md_info_from_type(type_);
        valid_ = info && mbedtls_md_setup(&context_, info, 0) == 0 &&
                 mbedtls_md_starts(&context_) == 0;
    }

    ~Hash() { mbedtls_md_free(&context_); }

    Hash(const Hash&) = delete;
    Hash& operator=(const Hash&) = delete;

    bool update(const void* bytes, std::size_t size) {
        valid_ = valid_ &&
                 mbedtls_md_update(&context_, static_cast<const unsigned char*>(bytes),
                                   size) == 0;
        return valid_;
    }

    std::string finish() {
        unsigned char digest[32];
        if (!valid_ || mbedtls_md_finish(&context_, digest) != 0)
            return {};
        if (mbedtls_md_get_size(mbedtls_md_info_from_type(type_)) != wanted_)
            return {};
        constexpr char digits[] = "0123456789abcdef";
        std::string out(wanted_ * 2, '0');
        for (unsigned i = 0; i < wanted_; ++i) {
            out[2 * i] = digits[digest[i] >> 4];
            out[2 * i + 1] = digits[digest[i] & 15];
        }
        return out;
    }

  private:
    mbedtls_md_context_t context_{};
    mbedtls_md_type_t type_;
    unsigned wanted_ = 32;
    bool valid_ = false;
};

} // namespace integrity
