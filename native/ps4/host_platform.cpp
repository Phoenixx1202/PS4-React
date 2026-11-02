// PS4 host services for the React runtime.
//
// This is the small platform surface host_api.hpp needs. Networking, archives,
// package installs and audio are deliberately stubbed for the first React
// build; the UI, filesystem, users, notifications and controller work.

#include "audio_out.hpp"
#include "host_api.hpp"
#include "directory_records.hpp"
#include "storage_stats.hpp"
#include "system.hpp"

#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/_types/kernel.h>
#include <orbis/libkernel.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace {
constexpr const char* kDataRoot = "/data/PS4React";
constexpr const char* kTempRoot = "/data/PS4React/tmp";
constexpr const char* kCacheRoot = "/data/PS4React/cache";

ps4::AudioOut audio_out;

bool failed(int result) {
    if (result >= 0) return false;
    errno = static_cast<int>(static_cast<unsigned>(result) & 0xffffu);
    return true;
}

bool user_name(int id, char* name, std::size_t size) {
    if (sceUserServiceGetUserName(id, name, size) != 0) {
        name[0] = '\0';
        return false;
    }
    return true;
}
}

namespace host {

const char* platform_name() { return "ps4"; }

bool resolve_path(const char* path, char* out, std::size_t size) {
    if (!path || path[0] != '/') return false;
    for (const char* part = path; part; part = std::strchr(part + 1, '/'))
        if (part[1] == '.' && part[2] == '.' && (part[3] == '/' || part[3] == '\0'))
            return false;

    struct Mapping { const char* logical; const char* real; };
    const Mapping mappings[] = {
        {"/download0", kDataRoot},
        {"/temp0", kTempRoot},
        {"/cache0", kCacheRoot},
        {"/app0", "/app0"},
    };
    for (const Mapping& mapping : mappings) {
        const std::size_t length = std::strlen(mapping.logical);
        if (!std::strncmp(path, mapping.logical, length) &&
            (path[length] == '\0' || path[length] == '/')) {
            const int written = std::snprintf(out, size, "%s%s", mapping.real, path + length);
            return written >= 0 && static_cast<std::size_t>(written) < size;
        }
    }
    const int written = std::snprintf(out, size, "%s", path);
    return written >= 0 && static_cast<std::size_t>(written) < size;
}

int list_mounts(MountEntry*, int) {
    // The first React build does not expose mount discovery. The filesystem
    // APIs that the starter uses do not call this.
    return 0;
}

bool read_dir(const char* real_path, void (*visit)(const char* name, void* user), void* user) {
    const int fd = sceKernelOpen(real_path, O_RDONLY | O_DIRECTORY, 0);
    if (failed(fd)) return false;
    std::vector<char> buffer(64 * 1024);
    int read = 0;
    while (!failed(read = sceKernelGetdents(fd, buffer.data(), buffer.size())) && read > 0) {
        if (read > static_cast<int>(buffer.size()) ||
            !visit_directory_records(buffer.data(), static_cast<std::size_t>(read), visit, user)) {
            errno = EIO;
            read = -1;
            break;
        }
    }
    const int saved = errno;
    sceKernelClose(fd);
    errno = saved;
    return read == 0;
}

bool disk_usage(const char* real_path, double& total, double& free) {
    struct statvfs info {};
    if (statvfs(real_path, &info) != 0) return false;
    if (!storage::disk_bytes(info.f_frsize, info.f_blocks,
                             static_cast<std::int64_t>(info.f_bavail), total, free)) {
        errno = EIO;
        return false;
    }
    return true;
}

void trace(const char* call) {
    ps4::system::log("native: %s", call ? call : "");
}

bool arrange_relaunch(char* error, std::size_t size) {
    if (error && size)
        std::snprintf(error, size, "relaunch is not implemented on PS4 yet");
    return false;
}

void device_info(DeviceInfo& info) {
    std::snprintf(info.model, sizeof info.model, "PS4");
    OrbisKernelSwVersion version{};
    if (sceKernelGetSystemSwVersion(&version) == 0)
        std::snprintf(info.firmware, sizeof info.firmware, "0x%08x", version.Version);
    const int frequency = sceKernelGetCpuFrequency();
    if (frequency > 0) info.cpu_frequency = frequency;
    info.process_time = static_cast<std::int64_t>(sceKernelGetProcessTime());
    std::snprintf(info.language, sizeof info.language, "en-US");
}

bool foreground_user(User& user) {
    int id = -1;
    if (sceUserServiceGetForegroundUser(&id) != 0 && sceUserServiceGetInitialUser(&id) != 0)
        return false;
    user.id = id;
    user_name(id, user.name, sizeof user.name);
    return true;
}

int logged_in_users(User* users, int max) {
    if (!users || max <= 0) return 0;
    OrbisUserServiceLoginUserIdList list{};
    if (sceUserServiceGetLoginUserIdList(&list) != 0) return 0;
    int count = 0;
    for (int id : list.userId) {
        if (id < 0 || count == max) continue;
        users[count].id = id;
        user_name(id, users[count].name, sizeof users[count].name);
        ++count;
    }
    return count;
}

bool notify(const char* message, const char* sub_message) {
    OrbisNotificationRequest request{};
    request.type = NotificationRequest;
    std::snprintf(request.message, sizeof request.message, *sub_message ? "%s\n%s" : "%s",
                  message, sub_message);
    return sceKernelSendNotificationRequest(0, &request, sizeof request, 0) == 0;
}

bool open_url(const char*) {
    // Not used by the starter; the PS4 browser launcher has a different ABI.
    return false;
}

void set_light_bar(std::uint8_t, std::uint8_t, std::uint8_t) {}
void reset_light_bar() {}
void vibrate(float, float) {}
void keep_awake(bool) {}

bool start_audio(hui::audio::Mixer& mixer) { return audio_out.start(mixer); }
void stop_audio() { audio_out.stop(); }

} // namespace host
