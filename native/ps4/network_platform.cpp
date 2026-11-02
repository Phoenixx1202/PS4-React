// PS4 network lifecycle for the shared libcurl transport.
//
// sceNetInit is process-wide. Some titles start with the network stack already
// initialised by the system, so a non-zero result is logged but does not abort
// the transport: libcurl can still create sockets. The CA bundle shipped in
// /app0 is preferred; the system store is the fallback.

#include "network.hpp"

#include "system.hpp"

#include <orbis/Net.h>
#include <orbis/NetCtl.h>

#include <unistd.h>

namespace network {
namespace {

bool net_initialized = false;
bool ctl_initialized = false;

} // namespace

bool platform_start(std::string& error) {
    const int result = sceNetInit();
    ps4::system::log("network: sceNetInit=0x%08x", static_cast<unsigned>(result));
    if (result == 0)
        net_initialized = true;

    const int ctl = sceNetCtlInit();
    ps4::system::log("network: sceNetCtlInit=0x%08x", static_cast<unsigned>(ctl));
    if (ctl == 0)
        ctl_initialized = true;

    OrbisNetCtlInfo info{};
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) == 0)
        ps4::system::log("network: ip=%s", info.ip_address);
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_PRIMARY_DNS, &info) == 0)
        ps4::system::log("network: dns=%s", info.primary_dns);

    (void)error;
    return true;
}

void platform_stop() {
    if (ctl_initialized) {
        sceNetCtlTerm();
        ctl_initialized = false;
    }
    if (net_initialized) {
        sceNetTerm();
        net_initialized = false;
    }
}

const char* ca_path() {
    static const char* const candidates[] = {
        "/app0/ca-bundle.crt",
        "/system/common/cert/CA_LIST.cer",
    };
    for (const char* path : candidates)
        if (access(path, R_OK) == 0) return path;
    return nullptr;
}

void platform_log(const char* line) {
    ps4::system::log("[NET] %s", line ? line : "");
}

} // namespace network
