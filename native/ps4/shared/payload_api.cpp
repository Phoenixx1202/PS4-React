// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include "system.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
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

int connect_binloader(int timeout_ms) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int result = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address);
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

    const int socket = connect_binloader(3000);
    if (socket < 0) {
        close(fd);
        fail(task, "BinLoader is not reachable at 127.0.0.1:9090", errno);
        return;
    }
    task.socket = socket;
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
        if (!send_all(socket, buffer, static_cast<std::size_t>(read_size))) {
            fail(task, "cannot send the payload", errno);
            ok = false;
            break;
        }
        task.written += static_cast<std::uint64_t>(read_size);
    }

    close(fd);
    shutdown(socket, SHUT_WR);
    close(socket);
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
    const int socket = connect_binloader(500);
    if (socket < 0) return JS_FALSE;
    close(socket);
    return JS_TRUE;
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
    JS_SetPropertyStr(ctx, api, "send", JS_NewCFunction(ctx, send_payload, "send", 1));
    JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
    JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 0));
    return api;
}
