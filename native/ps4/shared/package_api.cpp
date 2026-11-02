// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

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

enum class State : int { queued, installing, completed, failed, cancelled };

struct Task {
    std::uint32_t id = 0;
    std::string path;
    std::string title_id;
    std::atomic<State> state{State::queued};
    std::mutex error_mutex;
    std::string error;
};

std::mutex tasks_mutex;
std::vector<std::shared_ptr<Task>> tasks;
std::atomic<std::uint32_t> next_id{1};

const char* state_name(State state) {
    switch (state) {
        case State::queued: return "queued";
        case State::installing: return "installing";
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

void run_install(const std::shared_ptr<Task>& task) {
    task->state = State::installing;
    int result = sceAppInstUtilInitialize();
    if (result != 0 && result != static_cast<int>(0x80a10001u)) {
        fail(*task, "sceAppInstUtilInitialize failed", result);
        return;
    }

    char title_id[16] = {};
    int is_app = 0;
    result = sceAppInstUtilGetTitleIdFromPkg(task->path.c_str(), title_id, &is_app);
    if (result == 0 && title_id[0]) task->title_id = title_id;

    result = sceAppInstUtilAppInstallPkg(task->path.c_str(), nullptr);
    if (result != 0) {
        fail(*task, "sceAppInstUtilAppInstallPkg failed", result);
        return;
    }
    task->state = State::completed;
}

void* install_entry(void* value) {
    std::unique_ptr<std::shared_ptr<Task>> task(
        static_cast<std::shared_ptr<Task>*>(value));
    run_install(*task);
    return nullptr;
}

void start_install(const std::shared_ptr<Task>& task) {
    auto* argument = new std::shared_ptr<Task>(task);
    pthread_attr_t attributes;
    pthread_t thread{};
    bool started = false;
    if (pthread_attr_init(&attributes) == 0) {
        pthread_attr_setstacksize(&attributes, 8 * 1024 * 1024);
        started = pthread_create(&thread, &attributes, install_entry, argument) == 0;
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
    start_install(task);
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
        JSValue item = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, item, "id", JS_NewUint32(ctx, task->id));
        JS_SetPropertyStr(ctx, item, "state", JS_NewString(ctx, state_name(state)));
        {
            std::lock_guard<std::mutex> error_lock(task->error_mutex);
            JS_SetPropertyStr(ctx, item, "error", JS_NewString(ctx, task->error.c_str()));
        }
        JS_SetPropertyStr(ctx, item, "titleId", JS_NewString(ctx, task->title_id.c_str()));
        JS_SetPropertyStr(ctx, item, "contentId", JS_NewString(ctx, ""));
        JS_SetPropertyStr(ctx, item, "status", JS_NewInt32(ctx, static_cast<int>(state)));
        JS_SetPropertyStr(ctx, item, "written", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, item, "total", JS_NULL);
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
    JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
    JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 0));
    return api;
}
