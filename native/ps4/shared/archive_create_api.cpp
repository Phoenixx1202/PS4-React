// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include "archives.hpp"
#include "system.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <pthread.h>
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

// libarchive's ZIP writer references this FreeBSD helper, which OpenOrbis'
// libc does not provide. It is only used for temporary names/extra fields, not
// for encryption, so a deterministic xorshift generator is sufficient.
extern "C" void arc4random_buf(void* buffer, std::size_t size) {
    static std::atomic<std::uint64_t> state{0x9e3779b97f4a7c15ull};
    auto* bytes = static_cast<unsigned char*>(buffer);
    std::uint64_t value = state.fetch_add(0x9e3779b97f4a7c15ull);
    for (std::size_t i = 0; i < size; ++i) {
        if ((i & 7) == 0) {
            value ^= value << 13;
            value ^= value >> 7;
            value ^= value << 17;
        }
        bytes[i] = static_cast<unsigned char>(value >> ((i & 7) * 8));
    }
}

namespace {

constexpr std::uint64_t kMaxBytes = 4ull * 1024 * 1024 * 1024;
constexpr std::uint64_t kMaxEntries = 100000;

enum class State : int { queued, writing, completed, failed, cancelled };

struct Task {
    std::uint32_t id = 0;
    std::string destination;
    std::string format;
    std::atomic<State> state{State::queued};
    std::atomic<std::uint64_t> written{0};
    std::atomic<std::uint64_t> entries{0};
    std::atomic<bool> cancelled{false};
    bool overwrite = false;
    std::vector<std::string> sources;
    std::mutex error_mutex;
    std::string error;
};

std::mutex tasks_mutex;
std::vector<std::shared_ptr<Task>> tasks;
std::atomic<std::uint32_t> next_id{1};

const char* state_name(State state) {
    switch (state) {
        case State::queued: return "queued";
        case State::writing: return "writing";
        case State::completed: return "completed";
        case State::cancelled: return "cancelled";
        default: return "failed";
    }
}

void fail(Task& task, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(task.error_mutex);
        task.error = message;
    }
    task.state = State::failed;
    ps4::system::log("archives.create: %s", message.c_str());
}

std::string archive_error(archive* handle, const char* fallback) {
    const char* text = archive_error_string(handle);
    return text && *text ? text : fallback;
}

bool entry_name_ok(const std::string& name) {
    if (name.empty() || name[0] == '/') return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t end = name.find('/', start);
        const std::string part = name.substr(start, end == std::string::npos
                                                        ? std::string::npos
                                                        : end - start);
        if (part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool add_file(archive* handle, Task& task, const std::string& path,
              const std::string& name) {
    if (!entry_name_ok(name)) return true;
    if (task.entries >= kMaxEntries) {
        fail(task, "Archive has more than 100000 entries.");
        return false;
    }

    const int fd = open(path.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        fail(task, "Cannot open " + path + ": " + std::strerror(errno));
        return false;
    }
    struct stat info {};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        fail(task, "Not a regular file: " + path);
        return false;
    }
    if (task.written + static_cast<std::uint64_t>(info.st_size) > kMaxBytes) {
        close(fd);
        fail(task, "Archive exceeds the 4 GiB creation limit.");
        return false;
    }

    archive_entry* entry = archive_entry_new();
    if (!entry) {
        close(fd);
        fail(task, "Cannot allocate an archive entry.");
        return false;
    }
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_size(entry, info.st_size);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_mtime(entry, info.st_mtime, 0);
    if (archive_write_header(handle, entry) != ARCHIVE_OK) {
        fail(task, archive_error(handle, "Cannot write the archive header."));
        archive_entry_free(entry);
        close(fd);
        return false;
    }
    archive_entry_free(entry);

    char buffer[64 * 1024];
    while (!task.cancelled) {
        const ssize_t read_size = read(fd, buffer, sizeof buffer);
        if (read_size < 0) {
            if (errno == EINTR) continue;
            fail(task, "Cannot read " + path + ": " + std::strerror(errno));
            close(fd);
            return false;
        }
        if (read_size == 0) break;
        if (archive_write_data(handle, buffer, static_cast<std::size_t>(read_size)) < 0) {
            fail(task, archive_error(handle, "Cannot write archive data."));
            close(fd);
            return false;
        }
        task.written += static_cast<std::uint64_t>(read_size);
    }
    close(fd);
    if (task.cancelled) return false;
    task.entries += 1;
    return true;
}

struct Walk {
    archive* handle = nullptr;
    Task* task = nullptr;
    bool ok = true;
};

bool walk_path(Walk& walk, const std::string& path, const std::string& name);

// Walk keeps the current directory separately; the callback below cannot see
// it, so this struct is used by the recursive function below.
struct DirectoryWalk {
    Walk* walk = nullptr;
    std::string directory;
    std::string prefix;
};

void visit_directory(const char* entry, void* user) {
    auto& state = *static_cast<DirectoryWalk*>(user);
    if (!state.walk->ok || !entry || !*entry) return;
    if (!std::strcmp(entry, ".") || !std::strcmp(entry, "..")) return;
    const std::string path = state.directory + "/" + entry;
    const std::string name = state.prefix + entry;
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) {
        fail(*state.walk->task, "Cannot inspect " + path + ": " + std::strerror(errno));
        state.walk->ok = false;
        return;
    }
    if (S_ISDIR(info.st_mode)) {
        DirectoryWalk child{state.walk, path, name + "/"};
        state.walk->ok = archives::list_directory(path.c_str(), visit_directory, &child);
        if (!state.walk->ok)
            fail(*state.walk->task, "Cannot list " + path);
    } else if (S_ISREG(info.st_mode)) {
        state.walk->ok = add_file(state.walk->handle, *state.walk->task, path, name);
    }
}

bool walk_path(Walk& walk, const std::string& path, const std::string& name) {
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) {
        fail(*walk.task, "Cannot inspect " + path + ": " + std::strerror(errno));
        return false;
    }
    if (S_ISREG(info.st_mode))
        return add_file(walk.handle, *walk.task, path, name);
    if (!S_ISDIR(info.st_mode)) {
        fail(*walk.task, "Only regular files and directories can be archived: " + path);
        return false;
    }
    DirectoryWalk child{&walk, path, name + "/"};
    if (!archives::list_directory(path.c_str(), visit_directory, &child))
        return false;
    return walk.ok;
}

std::string base_name(const std::string& path) {
    if (path.empty()) return {};
    std::size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    const std::size_t slash = path.rfind('/', end - 1);
    return path.substr(slash == std::string::npos ? 0 : slash + 1, end - (slash == std::string::npos ? 0 : slash + 1));
}

void run(Task& task) {
    task.state = State::writing;
    if (access(task.destination.c_str(), F_OK) == 0) {
        if (!task.overwrite) {
            fail(task, "Destination already exists; pass overwrite: true.");
            return;
        }
        if (unlink(task.destination.c_str()) != 0) {
            fail(task, "Cannot replace the destination: " + std::string(std::strerror(errno)));
            return;
        }
    }

    archive* handle = archive_write_new();
    if (!handle) {
        fail(task, "Cannot create the archive writer.");
        return;
    }
    int result = 0;
    if (task.format == "tar")
        result = archive_write_set_format_pax_restricted(handle);
    else
        result = archive_write_set_format_zip(handle);
    if (result != ARCHIVE_OK) {
        fail(task, archive_error(handle, "Unsupported archive format."));
        archive_write_free(handle);
        return;
    }
    if (archive_write_open_filename(handle, task.destination.c_str()) != ARCHIVE_OK) {
        fail(task, archive_error(handle, "Cannot open the destination."));
        archive_write_free(handle);
        return;
    }

    Walk walk{handle, &task, true};
    for (const std::string& source : task.sources) {
        if (task.cancelled) break;
        if (!walk_path(walk, source, base_name(source))) {
            break;
        }
    }

    const int closed = archive_write_close(handle);
    if (closed != ARCHIVE_OK && !task.cancelled)
        fail(task, archive_error(handle, "Cannot finish the archive."));
    archive_write_free(handle);

    if (task.cancelled) {
        unlink(task.destination.c_str());
        task.state = State::cancelled;
    } else if (task.state != State::failed) {
        task.state = State::completed;
        ps4::system::log("archives.create: wrote %s (%llu entries, %llu bytes)",
                         task.destination.c_str(),
                         static_cast<unsigned long long>(task.entries),
                         static_cast<unsigned long long>(task.written));
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
        pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
        started = pthread_create(&thread, &attributes, entry, argument) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (started) {
        pthread_detach(thread);
    } else {
        delete argument;
        fail(*task, "Cannot start the archive thread.");
    }
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

JSValue create(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3 || !JS_IsArray(argv[0]))
        return JS_ThrowTypeError(ctx, "Archives.create: expected sources, destination and format");
    auto task = std::make_shared<Task>();
    task->id = next_id++;
    if (!resolve(ctx, argv[1], task->destination))
        return JS_ThrowTypeError(ctx, "Archives.create: destination is not allowed");
    {
        const char* destination = JS_ToCString(ctx, argv[1]);
        const bool readonly = destination && (!std::strcmp(destination, "/app0") || !std::strncmp(destination, "/app0/", 6));
        if (destination) JS_FreeCString(ctx, destination);
        if (readonly) return JS_ThrowTypeError(ctx, "Archives.create: /app0 is read-only");
    }
    if (!JS_IsString(argv[2]))
        return JS_ThrowTypeError(ctx, "Archives.create: format must be zip or tar");
    {
        const char* format = JS_ToCString(ctx, argv[2]);
        if (!format) return JS_EXCEPTION;
        task->format = format;
        JS_FreeCString(ctx, format);
    }
    if (task->format != "zip" && task->format != "tar")
        return JS_ThrowTypeError(ctx, "Archives.create: format must be zip or tar");
    if (argc > 3) task->overwrite = JS_ToBool(ctx, argv[3]) == 1;

    const JSValue length_value = JS_GetPropertyStr(ctx, argv[0], "length");
    std::uint32_t length = 0;
    const int converted = JS_ToUint32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    if (converted || !length || length > 1024)
        return JS_ThrowTypeError(ctx, "Archives.create: expected 1 to 1024 sources");
    for (std::uint32_t i = 0; i < length; ++i) {
        const JSValue value = JS_GetPropertyUint32(ctx, argv[0], i);
        std::string source;
        const bool ok = resolve(ctx, value, source);
        JS_FreeValue(ctx, value);
        if (!ok) return JS_ThrowTypeError(ctx, "Archives.create: source path is not allowed");
        task->sources.push_back(std::move(source));
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
        return JS_ThrowTypeError(ctx, "Archives.create cancel: expected a task id");
    std::lock_guard<std::mutex> lock(tasks_mutex);
    for (const auto& task : tasks) {
        if (task->id == id) {
            task->cancelled = true;
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
        JS_SetPropertyStr(ctx, item, "entries", JS_NewFloat64(ctx, task->entries));
        JS_SetPropertyStr(ctx, item, "destination", JS_NewString(ctx, task->destination.c_str()));
        JS_SetPropertyStr(ctx, item, "format", JS_NewString(ctx, task->format.c_str()));
        JS_SetPropertyUint32(ctx, out, index++, item);
        if (state == State::completed || state == State::failed || state == State::cancelled)
            it = tasks.erase(it);
        else
            ++it;
    }
    return out;
}

} // namespace

JSValue ps4_react_archive_create_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "create", JS_NewCFunction(ctx, create, "create", 4));
    JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
    JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 0));
    return api;
}
