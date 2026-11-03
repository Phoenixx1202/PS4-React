// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include "system.hpp"

#include <orbis/AppInstUtil.h>

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <vector>

namespace {

enum class State : int { queued, installing, uninstalling, completed, failed, cancelled };

struct Task {
    std::uint32_t id = 0;
    std::string path;
    std::string title_id;
    std::string content_id;
    bool uninstall = false;
    std::atomic<State> state{State::queued};
    std::atomic<int> percent{0};
    std::atomic<bool> progress_logged{false};
    std::mutex error_mutex;
    std::string error;
};

std::mutex tasks_mutex;
std::vector<std::shared_ptr<Task>> tasks;
std::atomic<std::uint32_t> next_id{1};
std::once_flag install_init;
int install_init_result = 0;

const char* state_name(State state) {
    switch (state) {
        case State::queued: return "queued";
        case State::installing: return "installing";
        case State::uninstalling: return "uninstalling";
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
    ps4::system::log("packages: %s failed: %s", task.title_id.empty() ? "task" : task.title_id.c_str(),
                     task.error.c_str());
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

bool valid_title_id(const std::string& id) {
    if (id.size() != 9) return false;
    for (char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

// The outer PKG header stores the 36-byte content id at 0x40. The install
// service reports progress by content id, so read it from the file before the
// blocking install call.
bool read_content_id(const std::string& path, std::string& out) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    char id[36] = {};
    const bool read = std::fseek(file, 0x40, SEEK_SET) == 0 &&
                      std::fread(id, 1, sizeof id, file) == sizeof id;
    std::fclose(file);
    if (!read) return false;
    for (char c : id) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return false;
    }
    out.assign(id, sizeof id);
    return true;
}

bool initialize(Task& task) {
    std::call_once(install_init, [] { install_init_result = sceAppInstUtilInitialize(); });
    if (install_init_result != 0 &&
        install_init_result != static_cast<int>(0x80a10001u)) {
        fail(task, "sceAppInstUtilInitialize failed", install_init_result);
        return false;
    }
    return true;
}

void run_install(const std::shared_ptr<Task>& task) {
    task->state = State::installing;
    if (!initialize(*task)) return;

    int result = 0;
    char title_id[16] = {};
    int is_app = 0;
    result = sceAppInstUtilGetTitleIdFromPkg(task->path.c_str(), title_id, &is_app);
    if (result == 0 && title_id[0]) task->title_id = title_id;
    read_content_id(task->path, task->content_id);

    if (!task->title_id.empty()) {
        int exists = 0;
        if (sceAppInstUtilAppExists(task->title_id.c_str(), &exists) == 0 && exists)
            (void)sceAppInstUtilAppPrepareOverwritePkg(task->path.c_str());
    }

    result = sceAppInstUtilAppInstallPkg(task->path.c_str(), nullptr);
    if (result != 0) {
        fail(*task, "sceAppInstUtilAppInstallPkg failed", result);
        return;
    }
    task->percent = 100;
    task->state = State::completed;
    ps4::system::log("packages: installed %s from %s",
                     task->title_id.empty() ? "?" : task->title_id.c_str(),
                     task->path.c_str());
}

void run_uninstall(const std::shared_ptr<Task>& task) {
    task->state = State::uninstalling;
    if (!initialize(*task)) return;

    int exists = 0;
    int result = sceAppInstUtilAppExists(task->title_id.c_str(), &exists);
    if (result != 0) {
        fail(*task, "sceAppInstUtilAppExists failed", result);
        return;
    }
    if (!exists) {
        fail(*task, "the title is not installed");
        return;
    }

    result = sceAppInstUtilAppUnInstall(task->title_id.c_str());
    if (result != 0) {
        fail(*task, "sceAppInstUtilAppUnInstall failed", result);
        return;
    }
    task->state = State::completed;
    ps4::system::log("packages: uninstalled %s", task->title_id.c_str());
}

void* task_entry(void* value) {
    std::unique_ptr<std::shared_ptr<Task>> task(
        static_cast<std::shared_ptr<Task>*>(value));
    if ((*task)->uninstall)
        run_uninstall(*task);
    else
        run_install(*task);
    return nullptr;
}

void start_task(const std::shared_ptr<Task>& task) {
    auto* argument = new std::shared_ptr<Task>(task);
    pthread_attr_t attributes;
    pthread_t thread{};
    bool started = false;
    if (pthread_attr_init(&attributes) == 0) {
        pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
        started = pthread_create(&thread, &attributes, task_entry, argument) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (started) {
        pthread_detach(thread);
    } else {
        delete argument;
        fail(*task, "cannot start the package installer thread");
    }
}

JSValue install(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Packages.install: expected a path");
    auto task = std::make_shared<Task>();
    task->id = next_id++;
    if (!resolve(ctx, argv[0], task->path)) {
        return JS_ThrowTypeError(ctx, "Packages.install: path is not allowed");
    }
    {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        tasks.push_back(task);
    }
    start_task(task);
    return JS_NewUint32(ctx, task->id);
}

JSValue uninstall(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Packages.uninstall: expected {titleId}");
    JSValue value = JS_GetPropertyStr(ctx, argv[0], "titleId");
    if (JS_IsException(value)) return value;
    std::size_t length = 0;
    const char* input = JS_ToCStringLen(ctx, &length, value);
    JS_FreeValue(ctx, value);
    if (!input)
        return JS_ThrowTypeError(ctx, "Packages.uninstall: titleId must be a string");
    std::string title_id(input, length);
    JS_FreeCString(ctx, input);
    if (!valid_title_id(title_id))
        return JS_ThrowTypeError(ctx, "Packages.uninstall: invalid titleId");

    auto task = std::make_shared<Task>();
    task->id = next_id++;
    task->title_id = title_id;
    task->uninstall = true;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        tasks.push_back(task);
    }
    start_task(task);
    return JS_NewUint32(ctx, task->id);
}

JSValue cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Packages.cancel: expected a task id");
    std::lock_guard<std::mutex> lock(tasks_mutex);
    for (const auto& task : tasks) {
        if (task->id == id) {
            task->state = State::cancelled;
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
        int percent = task->percent;
        JSValue item = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, item, "id", JS_NewUint32(ctx, task->id));
        JS_SetPropertyStr(ctx, item, "state", JS_NewString(ctx, state_name(state)));
        {
            std::lock_guard<std::mutex> error_lock(task->error_mutex);
            JS_SetPropertyStr(ctx, item, "error", JS_NewString(ctx, task->error.c_str()));
        }
        JS_SetPropertyStr(ctx, item, "titleId", JS_NewString(ctx, task->title_id.c_str()));
        JS_SetPropertyStr(ctx, item, "contentId", JS_NewString(ctx, task->content_id.c_str()));
        JS_SetPropertyStr(ctx, item, "status", JS_NewInt32(ctx, static_cast<int>(state)));
        JS_SetPropertyStr(ctx, item, "written", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, item, "total", JS_NULL);
        if (state == State::installing && !task->content_id.empty()) {
            std::uint32_t install_state = 0;
            std::uint32_t progress = 0;
            std::uint32_t progress_size = 0;
            std::uint32_t total_size = 0;
            std::uint32_t rest_seconds = 0;
            if (sceAppInstUtilGetInstallProgressInfo(
                    task->content_id.c_str(), &install_state, &progress,
                    &progress_size, &total_size, &rest_seconds) == 0) {
                if (progress > 100) progress = 100;
                percent = static_cast<int>(progress);
                task->percent = percent;
                if (progress > 0 && !task->progress_logged.exchange(true)) {
                    ps4::system::log(
                        "packages: install progress %s %u%% state=%u size=%u/%u",
                        task->content_id.c_str(), progress, install_state,
                        progress_size, total_size);
                }
                JS_SetPropertyStr(ctx, item, "installState", JS_NewUint32(ctx, install_state));
                JS_SetPropertyStr(ctx, item, "written", JS_NewFloat64(ctx, progress_size));
                JS_SetPropertyStr(ctx, item, "total", JS_NewFloat64(ctx, total_size));
                JS_SetPropertyStr(ctx, item, "secondsRemaining",
                                  JS_NewUint32(ctx, rest_seconds));
            }
        }
        JS_SetPropertyStr(ctx, item, "percent", JS_NewInt32(ctx, percent));
        JS_SetPropertyUint32(ctx, out, index++, item);
        if (state == State::completed || state == State::failed || state == State::cancelled)
            it = tasks.erase(it);
        else
            ++it;
    }
    return out;
}

} // namespace

JSValue ps4_react_package_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "install", JS_NewCFunction(ctx, install, "install", 2));
    JS_SetPropertyStr(ctx, api, "uninstall", JS_NewCFunction(ctx, uninstall, "uninstall", 1));
    JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
    JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 0));
    return api;
}
