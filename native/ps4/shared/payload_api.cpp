// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include "system.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <orbis/NetCtl.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

constexpr int kPort = 9090;
constexpr std::uint64_t kMaxPayload = 64ull * 1024 * 1024;

enum class State : int { queued, sending, completed, failed, cancelled };

struct Task {
    std::uint32_t id = 0;
    std::string path;
    std::atomic<State> state{State::queued};
    std::atomic<std::uint64_t> written{0};
    std::uint64_t total = 0;
    std::atomic<bool> cancelled{false};
    std::atomic<int> socket{-1};
    std::mutex error_mutex;
    std::string error;
};

std::mutex tasks_mutex;
std::vector<std::shared_ptr<Task>> tasks;
std::atomic<std::uint32_t> next_id{1};

const char* state_name(State state) {
    switch (state) {
        case State::queued: return "queued";
        case State::sending: return "sending";
        case State::completed: return "completed";
        case State::cancelled: return "cancelled";
        default: return "failed";
    }
}

void fail(Task& task, const char* message, int code = 0) {
    {
        std::lock_guard<std::mutex> lock(task.error_mutex);
        task.error = message;
        if (code) {
            char detail[64];
            std::snprintf(detail, sizeof detail, " (0x%08x)", static_cast<unsigned>(code));
            task.error += detail;
        }
    }
    task.state = State::failed;
    ps4::system::log("payloads: %s failed: %s", task.path.c_str(), task.error.c_str());
}

bool resolve(JSContext* ctx, JSValueConst value, std::string& out) {
    if (!JS_IsString(value)) return false;
    std::size_t length = 0;
    const char* input = JS_ToCStringLen(ctx, &length, value);
    if (!input) return false;
    char path[PATH_MAX];
    const bool ok = length && length < PATH_MAX && !std::memchr(input, 0, length) &&
                    host::resolve_path(input, path, sizeof path);
    if (ok) out = path;
    JS_FreeCString(ctx, input);
    return ok;
}

int connect_address(std::uint32_t address, std::uint16_t port, int timeout_ms) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(port);
    destination.sin_addr.s_addr = address;

    int result = connect(fd, reinterpret_cast<sockaddr*>(&destination), sizeof destination);
    if (result != 0 && errno == EINPROGRESS) {
        fd_set writes;
        FD_ZERO(&writes);
        FD_SET(fd, &writes);
        timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        result = select(fd + 1, nullptr, &writes, nullptr, &timeout);
        if (result > 0) {
            int error = 0;
            socklen_t length = sizeof error;
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
                result = -1;
                errno = error;
            } else {
                result = 0;
            }
        } else {
            result = -1;
            errno = ETIMEDOUT;
        }
    }
    if (result != 0) {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }

    if (flags >= 0) fcntl(fd, F_SETFL, flags);
    timeval send_timeout{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof send_timeout);
    return fd;
}

std::uint32_t local_address() {
    OrbisNetCtlInfo info{};
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) != 0)
        return 0;
    struct in_addr address {};
    if (inet_pton(AF_INET, info.ip_address, &address) != 1)
        return 0;
    return address.s_addr;
}

struct Probe {
    bool loopback = false;
    bool lan = false;
    bool ftp_loopback = false;
    bool ftp_lan = false;
    int loopback_error = 0;
    int lan_error = 0;
    int ftp_loopback_error = 0;
    int ftp_lan_error = 0;
    std::string ip;
};

int probe_port(std::uint32_t address, std::uint16_t port, int timeout_ms, bool& open) {
    const int fd = connect_address(address, port, timeout_ms);
    if (fd >= 0) {
        close(fd);
        open = true;
        return 0;
    }
    open = false;
    return errno;
}

Probe probe_binloader(int timeout_ms) {
    Probe probe;

    const std::uint32_t loopback = htonl(INADDR_LOOPBACK);
    probe.loopback_error = probe_port(loopback, kPort, timeout_ms, probe.loopback);

    OrbisNetCtlInfo info{};
    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) == 0)
        probe.ip = info.ip_address;
    const std::uint32_t address = local_address();
    if (address)
        probe.lan_error = probe_port(address, kPort, timeout_ms, probe.lan);
    else
        probe.lan_error = ENETUNREACH;

    // The FTP server on 2121 is always enabled. Probing it tells a closed port
    // from a network namespace that cannot reach the console's own services.
    probe.ftp_loopback_error = probe_port(loopback, 2121, timeout_ms, probe.ftp_loopback);
    if (address)
        probe.ftp_lan_error = probe_port(address, 2121, timeout_ms, probe.ftp_lan);
    else
        probe.ftp_lan_error = ENETUNREACH;

    return probe;
}

bool send_all(int fd, const char* data, std::size_t size) {
    while (size) {
        const ssize_t written = send(fd, data, size, 0);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
    return true;
}

void run(Task& task) {
    const int fd = open(task.path.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        fail(task, "cannot open the payload", errno);
        return;
    }

    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        close(fd);
        fail(task, "the payload is empty", errno);
        return;
    }
    if (static_cast<std::uint64_t>(info.st_size) > kMaxPayload) {
        close(fd);
        fail(task, "the payload is larger than 64 MiB");
        return;
    }
    task.total = static_cast<std::uint64_t>(info.st_size);

    char magic[4] = {};
    const ssize_t magic_size = read(fd, magic, sizeof magic);
    lseek(fd, 0, SEEK_SET);
    if (magic_size != 4 || std::memcmp(magic, "\x7f" "ELF", 4) != 0) {
        close(fd);
        fail(task, "the payload is not an ELF file");
        return;
    }

    const int socket = connect_address(htonl(INADDR_LOOPBACK), kPort, 3000);
    if (socket < 0) {
        const std::uint32_t address = local_address();
        const int probe_lan = address ? connect_address(address, kPort, 3000) : -1;
        if (probe_lan < 0) {
            close(fd);
            fail(task, "BinLoader is not reachable at 127.0.0.1:9090", errno);
            return;
        }
        task.socket = probe_lan;
    } else {
        task.socket = socket;
    }
    const int connection = task.socket;
    task.state = State::sending;
    ps4::system::log("payloads: sending %s (%llu bytes)", task.path.c_str(),
                     static_cast<unsigned long long>(task.total));

    char buffer[64 * 1024];
    bool ok = true;
    while (!task.cancelled) {
        const ssize_t read_size = read(fd, buffer, sizeof buffer);
        if (read_size < 0) {
            if (errno == EINTR) continue;
            fail(task, "cannot read the payload", errno);
            ok = false;
            break;
        }
        if (read_size == 0) break;
        if (!send_all(connection, buffer, static_cast<std::size_t>(read_size))) {
            fail(task, "cannot send the payload", errno);
            ok = false;
            break;
        }
        task.written += static_cast<std::uint64_t>(read_size);
    }

    close(fd);
    shutdown(connection, SHUT_WR);
    close(connection);
    task.socket = -1;

    if (ok && !task.cancelled) {
        task.state = State::completed;
        ps4::system::log("payloads: sent %s", task.path.c_str());
    } else if (task.cancelled) {
        task.state = State::cancelled;
    }
}

void* entry(void* value) {
    std::unique_ptr<std::shared_ptr<Task>> task(
        static_cast<std::shared_ptr<Task>*>(value));
    run(**task);
    return nullptr;
}

void start(const std::shared_ptr<Task>& task) {
    auto* argument = new std::shared_ptr<Task>(task);
    pthread_attr_t attributes;
    pthread_t thread{};
    bool started = false;
    if (pthread_attr_init(&attributes) == 0) {
        pthread_attr_setstacksize(&attributes, 1024 * 1024);
        started = pthread_create(&thread, &attributes, entry, argument) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (started) {
        pthread_detach(thread);
    } else {
        delete argument;
        fail(*task, "cannot start the payload thread");
    }
}

JSValue available(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const Probe probe = probe_binloader(500);
    ps4::system::log("payloads: probe loopback=0x%08x lan=%s=0x%08x ftp-loopback=0x%08x ftp-lan=0x%08x",
                     static_cast<unsigned>(probe.loopback_error),
                     probe.ip.empty() ? "?" : probe.ip.c_str(),
                     static_cast<unsigned>(probe.lan_error),
                     static_cast<unsigned>(probe.ftp_loopback_error),
                     static_cast<unsigned>(probe.ftp_lan_error));
    return JS_NewBool(ctx, probe.loopback || probe.lan);
}

JSValue diagnose(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const Probe probe = probe_binloader(500);
    ps4::system::log("payloads: probe loopback=0x%08x lan=%s=0x%08x ftp-loopback=0x%08x ftp-lan=0x%08x",
                     static_cast<unsigned>(probe.loopback_error),
                     probe.ip.empty() ? "?" : probe.ip.c_str(),
                     static_cast<unsigned>(probe.lan_error),
                     static_cast<unsigned>(probe.ftp_loopback_error),
                     static_cast<unsigned>(probe.ftp_lan_error));
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "available", JS_NewBool(ctx, probe.loopback || probe.lan));
    JS_SetPropertyStr(ctx, out, "loopback", JS_NewBool(ctx, probe.loopback));
    JS_SetPropertyStr(ctx, out, "lan", JS_NewBool(ctx, probe.lan));
    JS_SetPropertyStr(ctx, out, "ftpLoopback", JS_NewBool(ctx, probe.ftp_loopback));
    JS_SetPropertyStr(ctx, out, "ftpTcp", JS_NewBool(ctx, probe.ftp_lan));
    JS_SetPropertyStr(ctx, out, "ip", JS_NewString(ctx, probe.ip.c_str()));
    JS_SetPropertyStr(ctx, out, "port", JS_NewInt32(ctx, kPort));
    JS_SetPropertyStr(ctx, out, "loopbackError", JS_NewInt32(ctx, probe.loopback_error));
    JS_SetPropertyStr(ctx, out, "lanError", JS_NewInt32(ctx, probe.lan_error));
    JS_SetPropertyStr(ctx, out, "ftpLoopbackError", JS_NewInt32(ctx, probe.ftp_loopback_error));
    JS_SetPropertyStr(ctx, out, "ftpLanError", JS_NewInt32(ctx, probe.ftp_lan_error));
    return out;
}

JSValue send_payload(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Payloads.send: expected a path");
    auto task = std::make_shared<Task>();
    task->id = next_id++;
    if (!resolve(ctx, argv[0], task->path)) {
        return JS_ThrowTypeError(ctx, "Payloads.send: path is not allowed");
    }
    {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        tasks.push_back(task);
    }
    start(task);
    return JS_NewUint32(ctx, task->id);
}

JSValue cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Payloads.cancel: expected a task id");
    std::lock_guard<std::mutex> lock(tasks_mutex);
    for (const auto& task : tasks) {
        if (task->id == id) {
            task->cancelled = true;
            const int socket = task->socket;
            if (socket >= 0) shutdown(socket, SHUT_RDWR);
            break;
        }
    }
    return JS_UNDEFINED;
}

JSValue poll(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue out = JS_NewArray(ctx);
    std::uint32_t index = 0;
    std::lock_guard<std::mutex> lock(tasks_mutex);
    for (auto it = tasks.begin(); it != tasks.end();) {
        const auto& task = *it;
        const State state = task->state;
        JSValue item = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, item, "id", JS_NewUint32(ctx, task->id));
        JS_SetPropertyStr(ctx, item, "state", JS_NewString(ctx, state_name(state)));
        {
            std::lock_guard<std::mutex> error_lock(task->error_mutex);
            JS_SetPropertyStr(ctx, item, "error", JS_NewString(ctx, task->error.c_str()));
        }
        JS_SetPropertyStr(ctx, item, "written", JS_NewFloat64(ctx, task->written));
        JS_SetPropertyStr(ctx, item, "total", JS_NewFloat64(ctx, task->total));
        JS_SetPropertyUint32(ctx, out, index++, item);
        if (state == State::completed || state == State::failed || state == State::cancelled)
            it = tasks.erase(it);
        else
            ++it;
    }
    return out;
}

} // namespace

JSValue ps4_react_payload_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "isAvailable", JS_NewCFunction(ctx, available, "isAvailable", 0));
    JS_SetPropertyStr(ctx, api, "diagnose", JS_NewCFunction(ctx, diagnose, "diagnose", 0));
    JS_SetPropertyStr(ctx, api, "send", JS_NewCFunction(ctx, send_payload, "send", 1));
    JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
    JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 0));
    return api;
}
