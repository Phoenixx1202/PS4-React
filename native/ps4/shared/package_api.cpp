// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include "app_config.generated.h"
#include "system.hpp"

#include <orbis/Bgft.h>
#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
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
    bool overwrite = false;
    std::atomic<State> state{State::queued};
    std::atomic<int> percent{0};
    std::atomic<bool> progress_logged{false};
    std::atomic<int> bgft_task{-1};
    std::mutex error_mutex;
    std::string error;
};

std::mutex tasks_mutex;
std::vector<std::shared_ptr<Task>> tasks;
std::atomic<std::uint32_t> next_id{1};

// The installer modules are privileged system libraries. Linking them creates
// import stubs that GoldHEN treats as unpatched calls (and crashes the app), so
// load the modules explicitly and resolve every entry point at run time.
struct AppInstApi {
    std::int32_t module = -1;
    int (*initialize)() = nullptr;
    int (*get_title_id)(const char*, char*, int*) = nullptr;
    int (*exists)(const char*, int*) = nullptr;
    int (*uninstall)(const char*) = nullptr;
};

struct BgftApi {
    std::int32_t module = -1;
    int (*init)(OrbisBgftInitParams*) = nullptr;
    int (*register_storage)(OrbisBgftDownloadParamEx*, OrbisBgftTaskId*) = nullptr;
    int (*start)(OrbisBgftTaskId) = nullptr;
    int (*stop)(OrbisBgftTaskId) = nullptr;
    int (*unregister)(OrbisBgftTaskId) = nullptr;
    int (*progress)(OrbisBgftTaskId, OrbisBgftTaskProgress*) = nullptr;
};

AppInstApi appinst;
BgftApi bgft;
std::once_flag api_init;
int api_init_error = 0;

// OpenOrbis' OrbisBgftTaskProgress uses 32-bit sizes, but the firmware fills
// 64-bit size_t fields (flatz's and Itemzflow's layouts). Read the real 64-byte
// layout and pass it through the header type only for the declaration.
struct BgftTaskProgress {
    std::uint32_t bits;
    std::int32_t error_result;
    std::uint64_t length;
    std::uint64_t transferred;
    std::uint64_t length_total;
    std::uint64_t transferred_total;
    std::uint32_t num_index;
    std::uint32_t num_total;
    std::uint32_t rest_sec;
    std::uint32_t rest_sec_total;
    std::int32_t preparing_percent;
    std::int32_t local_copy_percent;
};
static_assert(sizeof(BgftTaskProgress) == 64, "unexpected BGFT progress layout");

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

// The outer PKG header stores the 36-byte content id at 0x40.
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

std::int32_t find_module(const char* needle) {
    OrbisKernelModule modules[256] = {};
    std::size_t count = 0;
    const int result = sceKernelGetModuleList(modules, 256, &count);
    if (result != 0) {
        ps4::system::log("packages: GetModuleList=0x%08x", static_cast<unsigned>(result));
        return -1;
    }
    for (std::size_t i = 0; i < count && i < 256; ++i) {
        OrbisKernelModuleInfo info{};
        info.size = sizeof info;
        if (sceKernelGetModuleInfo(modules[i], &info) != 0) continue;
        if (std::strstr(info.name, needle))
            return static_cast<std::int32_t>(modules[i]);
    }
    ps4::system::log("packages: %s not in the module list (%u modules)", needle,
                     static_cast<unsigned>(count));
    return -1;
}

std::int32_t load_sysmodule(OrbisSysModuleInternal id, const char* needle) {
    const std::uint32_t raw = sceSysmoduleLoadModuleInternal(id);
    ps4::system::log("packages: sysmodule 0x%08x module=0x%08x",
                     static_cast<unsigned>(id), static_cast<unsigned>(raw));
    const std::int32_t result = static_cast<std::int32_t>(raw);
    if (result < 0) return result;
    return find_module(needle);
}

template <typename Function>
bool resolve_symbol(std::int32_t module, const char* name, Function& function) {
    void* address = nullptr;
    const int result = sceKernelDlsym(module, name, &address);
    if (result != 0 || !address) {
        ps4::system::log("packages: dlsym %s=0x%08x", name, static_cast<unsigned>(result));
        return false;
    }
    function = reinterpret_cast<Function>(address);
    return true;
}

int initialize_services() {
    std::call_once(api_init, [] {
        appinst.module = load_sysmodule(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL,
                                        "libSceAppInstUtil");
        if (appinst.module < 0) {
            api_init_error = appinst.module;
            return;
        }
        if (!resolve_symbol(appinst.module, "sceAppInstUtilInitialize", appinst.initialize) ||
            !resolve_symbol(appinst.module, "sceAppInstUtilGetTitleIdFromPkg", appinst.get_title_id) ||
            !resolve_symbol(appinst.module, "sceAppInstUtilAppExists", appinst.exists) ||
            !resolve_symbol(appinst.module, "sceAppInstUtilAppUnInstall", appinst.uninstall)) {
            api_init_error = -1;
            return;
        }

        bgft.module = load_sysmodule(ORBIS_SYSMODULE_INTERNAL_BGFT, "libSceBgft");
        if (bgft.module < 0) {
            api_init_error = bgft.module;
            return;
        }
        if (!resolve_symbol(bgft.module, "sceBgftServiceIntInit", bgft.init) ||
            !resolve_symbol(bgft.module, "sceBgftServiceIntDownloadRegisterTaskByStorageEx",
                            bgft.register_storage) ||
            !resolve_symbol(bgft.module, "sceBgftServiceDownloadStartTask", bgft.start) ||
            !resolve_symbol(bgft.module, "sceBgftServiceDownloadStopTask", bgft.stop) ||
            !resolve_symbol(bgft.module, "sceBgftServiceIntDownloadUnregisterTask",
                            bgft.unregister) ||
            !resolve_symbol(bgft.module, "sceBgftServiceDownloadGetProgress", bgft.progress)) {
            api_init_error = -1;
            return;
        }

        OrbisBgftInitParams params{};
        params.heap = std::malloc(1024 * 1024);
        params.heapSize = 1024 * 1024;
        if (!params.heap) {
            api_init_error = -1;
            return;
        }
        const int result = bgft.init(&params);
        if (result != 0) {
            api_init_error = result;
            return;
        }
        ps4::system::log("packages: installer services ready");
    });
    return api_init_error;
}

int foreground_user_id() {
    host::User user;
    return host::foreground_user(user) ? user.id : -1;
}

int register_install(Task& task, int& error) {
    // The install service runs in ShellCore's namespace, where the console's
    // /data tree is mounted as /user/data. Apps usually pass /data/pkg.
    std::string content_url = task.path;
    if (content_url.rfind("/data/", 0) == 0)
        content_url = "/user" + content_url;

    OrbisBgftDownloadParamEx params{};
    params.params.userId = foreground_user_id();
    params.params.entitlementType = 5;
    params.params.id = "";
    params.params.contentUrl = content_url.c_str();
    params.params.contentName = task.title_id.c_str();
    params.params.iconPath = "";
    params.params.playgoScenarioId = "0";
    params.params.option = ORBIS_BGFT_TASK_OPT_DISABLE_CDN_QUERY_PARAM;
    params.slot = 0;

    OrbisBgftTaskId id = -1;
    int result = bgft.register_storage(&params, &id);
    if (result == 0) {
        error = 0;
        return id;
    }

    // 0x80990088/0x80990015: the title is already installed or needs a
    // reinstall. When overwrite was requested, remove it once and retry.
    if ((result == static_cast<int>(0x80990088u) ||
         result == static_cast<int>(0x80990015u)) && task.overwrite) {
        ps4::system::log("packages: replacing %s before install", task.title_id.c_str());
        const int removed = appinst.uninstall(task.title_id.c_str());
        if (removed != 0)
            ps4::system::log("packages: replace uninstall %s=0x%08x",
                             task.title_id.c_str(), static_cast<unsigned>(removed));
        result = bgft.register_storage(&params, &id);
        if (result == 0) {
            error = 0;
            return id;
        }
    }
    error = result;
    return -1;
}

void run_install(const std::shared_ptr<Task>& task) {
    task->state = State::installing;
    const int services = initialize_services();
    if (services != 0) {
        fail(*task, "installer services are unavailable", services);
        return;
    }

    char title_id[16] = {};
    int is_app = 0;
    std::string pkg_path = task->path;
    int result = appinst.get_title_id(pkg_path.c_str(), title_id, &is_app);
    if (result != 0 && pkg_path.rfind("/data/", 0) == 0) {
        // ShellCore sees the console's data tree as /user/data.
        pkg_path = "/user" + pkg_path;
        result = appinst.get_title_id(pkg_path.c_str(), title_id, &is_app);
    }
    if (result != 0) {
        fail(*task, "sceAppInstUtilGetTitleIdFromPkg failed", result);
        return;
    }
    if (!title_id[0]) {
        fail(*task, "the PKG has no title id");
        return;
    }
    task->title_id = title_id;
    task->path = pkg_path;
    read_content_id(task->path, task->content_id);

    int exists = 0;
    result = appinst.exists(task->title_id.c_str(), &exists);
    if (result == 0 && exists) {
        if (!task->overwrite) {
            fail(*task, "the title is already installed; uninstall it first or pass overwrite: true");
            return;
        }
        result = appinst.uninstall(task->title_id.c_str());
        if (result != 0) {
            fail(*task, "sceAppInstUtilAppUnInstall failed", result);
            return;
        }
        ps4::system::log("packages: removed %s for reinstall", task->title_id.c_str());
    }

    int register_error = 0;
    int id = register_install(*task, register_error);
    if (id < 0) {
        fail(*task, "sceBgftServiceIntDownloadRegisterTaskByStorageEx failed", register_error);
        return;
    }
    task->bgft_task = id;
    result = bgft.start(id);
    if (result != 0) {
        (void)bgft.unregister(id);
        task->bgft_task = -1;
        fail(*task, "sceBgftServiceDownloadStartTask failed", result);
        return;
    }
    ps4::system::log("packages: bgft task %d for %s (%s)", id,
                     task->title_id.c_str(), task->content_id.c_str());
}

void run_uninstall(const std::shared_ptr<Task>& task) {
    task->state = State::uninstalling;
    const int services = initialize_services();
    if (services != 0) {
        fail(*task, "installer services are unavailable", services);
        return;
    }

    int exists = 0;
    int result = appinst.exists(task->title_id.c_str(), &exists);
    if (result != 0) {
        fail(*task, "sceAppInstUtilAppExists failed", result);
        return;
    }
    if (!exists) {
        fail(*task, "the title is not installed");
        return;
    }

    result = appinst.uninstall(task->title_id.c_str());
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
    if (!PS4_REACT_PRIVILEGED) {
        return JS_ThrowTypeError(ctx,
            "Packages.install requires a privileged app; set \"privileged\": true in app.json");
    }
    if (argc < 1) return JS_ThrowTypeError(ctx, "Packages.install: expected a path");
    auto task = std::make_shared<Task>();
    task->id = next_id++;
    if (!resolve(ctx, argv[0], task->path)) {
        return JS_ThrowTypeError(ctx, "Packages.install: path is not allowed");
    }
    if (argc > 2) {
        const int overwrite = JS_ToBool(ctx, argv[2]);
        if (overwrite < 0) return JS_EXCEPTION;
        task->overwrite = overwrite == 1;
    }
    {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        tasks.push_back(task);
    }
    start_task(task);
    return JS_NewUint32(ctx, task->id);
}

JSValue uninstall(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!PS4_REACT_PRIVILEGED) {
        return JS_ThrowTypeError(ctx,
            "Packages.uninstall requires a privileged app; set \"privileged\": true in app.json");
    }
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
            const int bgft_task = task->bgft_task.exchange(-1);
            if (bgft_task >= 0 && bgft.stop && bgft.unregister) {
                (void)bgft.stop(bgft_task);
                (void)bgft.unregister(bgft_task);
            }
            task->state = State::cancelled;
            break;
        }
    }
    return JS_UNDEFINED;
}

void update_install_progress(Task& task) {
    const int bgft_task = task.bgft_task;
    if (bgft_task < 0 || !bgft.progress) return;

    BgftTaskProgress progress{};
    const int result = bgft.progress(
        bgft_task, reinterpret_cast<OrbisBgftTaskProgress*>(&progress));
    if (result != 0) {
        fail(task, "sceBgftServiceDownloadGetProgress failed", result);
        if (bgft.unregister) (void)bgft.unregister(bgft_task);
        task.bgft_task = -1;
        return;
    }
    if (progress.error_result != 0) {
        fail(task, "BGFT install failed", progress.error_result);
        if (bgft.unregister) (void)bgft.unregister(bgft_task);
        task.bgft_task = -1;
        return;
    }

    int percent = 0;
    if (progress.length)
        percent = static_cast<int>(std::min<std::uint64_t>(
            100, progress.transferred * 100 / progress.length));
    percent = std::max(percent, static_cast<int>(progress.preparing_percent));
    percent = std::max(percent, static_cast<int>(progress.local_copy_percent));
    if (percent > 100) percent = 100;
    task.percent = percent;
    if (percent > 0 && !task.progress_logged.exchange(true)) {
        ps4::system::log("packages: install progress %s %d%% (%llu/%llu copy=%d prepare=%d)",
                         task.content_id.c_str(), percent,
                         static_cast<unsigned long long>(progress.transferred),
                         static_cast<unsigned long long>(progress.length),
                         progress.local_copy_percent, progress.preparing_percent);
    }

    if (percent < 100) return;

    // The copy is complete; wait for the install service to register the
    // title in app.db before reporting completion.
    int exists = 0;
    const int check = appinst.exists ? appinst.exists(task.title_id.c_str(), &exists) : 0;
    if (check == 0 && exists) {
        task.percent = 100;
        task.state = State::completed;
        task.bgft_task = -1;
        ps4::system::log("packages: installed %s from %s", task.title_id.c_str(),
                         task.path.c_str());
    }
}

JSValue poll(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue out = JS_NewArray(ctx);
    std::uint32_t index = 0;
    std::lock_guard<std::mutex> lock(tasks_mutex);
    for (auto it = tasks.begin(); it != tasks.end();) {
        const auto& task = *it;
        if (task->state == State::installing)
            update_install_progress(*task);
        const State state = task->state;
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
        JS_SetPropertyStr(ctx, item, "percent", JS_NewInt32(ctx, task->percent));
        JS_SetPropertyStr(ctx, item, "written", JS_NewFloat64(ctx, 0));
        JS_SetPropertyStr(ctx, item, "total", JS_NULL);
        JS_SetPropertyStr(ctx, item, "bgftTask", JS_NewInt32(ctx, task->bgft_task));
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
    JS_SetPropertyStr(ctx, api, "install", JS_NewCFunction(ctx, install, "install", 3));
    JS_SetPropertyStr(ctx, api, "uninstall", JS_NewCFunction(ctx, uninstall, "uninstall", 1));
    JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
    JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 0));
    return api;
}
