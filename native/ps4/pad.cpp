#include "pad.hpp"

#include "system.hpp"

#include <orbis/UserService.h>
#include <orbis/libkernel.h>

namespace ps4 {
namespace {
constexpr int kAlreadyInitialized = static_cast<int>(0x80960003u);
}

Pad::~Pad() {
    close();
}

bool Pad::open() {
    if (handle_ >= 0)
        return true;

    OrbisUserServiceInitializeParams parameters{};
    parameters.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    const int user_init = sceUserServiceInitialize(&parameters);
    if (user_init != 0 && user_init != kAlreadyInitialized)
        system::log("pad: UserServiceInitialize=0x%08x", static_cast<unsigned>(user_init));

    if (sceUserServiceGetInitialUser(&user_) != 0) {
        system::log("pad: no initial user");
        return false;
    }

    const int pad_init = scePadInit();
    if (pad_init != 0)
        system::log("pad: PadInit=0x%08x", static_cast<unsigned>(pad_init));

    // The pad service can lag behind title start; retry briefly.
    for (int attempt = 0; attempt < 20; ++attempt) {
        handle_ = scePadOpen(user_, 0, 0, nullptr);
        if (handle_ >= 0) {
            system::log("pad: open user=0x%08x handle=%d attempts=%d",
                        static_cast<unsigned>(user_), handle_, attempt + 1);
            return true;
        }
        sceKernelUsleep(50000);
    }

    system::log("pad: PadOpen failed=0x%08x", static_cast<unsigned>(handle_));
    handle_ = -1;
    return false;
}

bool Pad::read(OrbisPadData& data) {
    if (handle_ < 0)
        return false;
    const int count = scePadRead(handle_, &data, 1);
    return count > 0;
}

void Pad::close() {
    if (handle_ >= 0)
        scePadClose(handle_);
    handle_ = -1;
}

} // namespace ps4
