// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include "app_config.generated.h"
#include "system.hpp"

#include <orbis/SaveData.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace {

struct Mounted {
    std::string path;
    OrbisSaveDataMountPoint point{};
    int user = -1;
    std::uint32_t blocks = ORBIS_SAVE_DATA_BLOCKS_MIN2;
    std::uint32_t mount_mode = 0;
    bool suspended = false;
};

std::mutex mount_mutex;
std::map<std::string, Mounted> mounted;
std::once_flag save_init;

bool initialize() {
    static int result = 0;
    std::call_once(save_init, [] { result = sceSaveDataInitialize3(0); });
    return result == 0;
}

bool user_id(int& id) {
    host::User user;
    if (!host::foreground_user(user) || user.id < 0)
        return false;
    id = user.id;
    return true;
}

constexpr int kBroken = static_cast<int>(0x809f000fu);

const char* error_name(int code) {
    switch (static_cast<unsigned>(code)) {
        case 0x809f0001u: return "not initialized";
        case 0x809f0007u: return "already exists";
        case 0x809f000au: return "no space";
        case 0x809f000du: return "bad mount";
        case 0x809f000fu: return "broken save data";
        case 0x809f0011u: return "invalid login user";
        default: return "save data error";
    }
}

int delete_save(int user, const std::string& directory) {
    OrbisSaveDataTitleId title{};
    std::snprintf(title.data, sizeof title.data, "%s", PS4_REACT_TITLE_ID);
    OrbisSaveDataDirName name{};
    std::snprintf(name.data, sizeof name.data, "%s", directory.c_str());
    OrbisSaveDataDelete request{};
    request.userId = user;
    request.titleId = &title;
    request.dirName = &name;
    return sceSaveDataDelete(&request);
}

JSValue failure(JSContext* ctx, const char* call, int code) {
    return JS_ThrowPlainError(ctx, "%s failed: %s (0x%08x)", call, error_name(code),
                              static_cast<unsigned>(code));
}

JSValue mount(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "SaveData.mount: expected a directory name");
    std::size_t length = 0;
    const char* name = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!name) return JS_EXCEPTION;
    const bool valid = length > 0 && length < ORBIS_SAVE_DATA_DIRNAME_DATA_MAXSIZE &&
                       !std::memchr(name, '/', length) && !std::memchr(name, 0, length);
    std::string directory(name, valid ? length : 0);
    JS_FreeCString(ctx, name);
    if (!valid)
        return JS_ThrowTypeError(ctx, "SaveData.mount: directory must be 1..31 bytes without '/'");

    std::uint32_t blocks = ORBIS_SAVE_DATA_BLOCKS_MIN2;
    if (argc > 1 && !JS_IsUndefined(argv[1])) {
        if (JS_ToUint32(ctx, &blocks, argv[1]))
            return JS_ThrowTypeError(ctx, "SaveData.mount: blocks must be a number");
    }
    if (blocks < ORBIS_SAVE_DATA_BLOCKS_MIN2 || blocks > ORBIS_SAVE_DATA_BLOCKS_MAX)
        return JS_ThrowTypeError(ctx, "SaveData.mount: blocks must be between 96 and 32768");

    int user = -1;
    if (!user_id(user))
        return JS_ThrowPlainError(ctx, "SaveData.mount: no foreground user");

    {
        std::lock_guard<std::mutex> lock(mount_mutex);
        const auto existing = mounted.find(directory);
        if (existing != mounted.end()) {
            JSValue out = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, out, "path", JS_NewString(ctx, existing->second.path.c_str()));
            return out;
        }
    }

    std::string title;
    std::string subtitle;
    std::string details;
    const auto string_arg = [&](int index, std::string& out) -> bool {
        if (argc <= index || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index])) return true;
        if (!JS_IsString(argv[index])) return false;
        const char* value = JS_ToCString(ctx, argv[index]);
        if (!value) return false;
        out = value;
        JS_FreeCString(ctx, value);
        return true;
    };
    if (!string_arg(2, title) || !string_arg(3, subtitle) || !string_arg(4, details))
        return JS_ThrowTypeError(ctx, "SaveData.mount: title, subtitle and details must be strings");
    const bool repair = argc > 5 && JS_ToBool(ctx, argv[5]) == 1;

    ps4::system::log("saveData.mount: initializing for user=%d dir=%s blocks=%u",
                     user, directory.c_str(), static_cast<unsigned>(blocks));
    if (!initialize())
        return JS_ThrowPlainError(ctx, "SaveData.mount: sceSaveDataInitialize3 failed");
    ps4::system::log("saveData.mount: initialize ok");

    OrbisSaveDataDirName dir_name{};
    std::snprintf(dir_name.data, sizeof dir_name.data, "%s", directory.c_str());
    OrbisSaveDataMount2 request{};
    request.userId = user;
    request.dirName = &dir_name;
    request.blocks = blocks;
    request.mountMode = ORBIS_SAVE_DATA_MOUNT_MODE_CREATE2 |
                       ORBIS_SAVE_DATA_MOUNT_MODE_RDWR |
                       ORBIS_SAVE_DATA_MOUNT_MODE_COPY_ICON;
    OrbisSaveDataMountResult result{};
    int code = sceSaveDataMount2(&request, &result);
    ps4::system::log("saveData.mount: sceSaveDataMount2=0x%08x path=%s",
                     static_cast<unsigned>(code), result.mountPathName);
    if (code == kBroken && repair) {
        ps4::system::log("saveData.mount: broken directory; deleting and recreating");
        const int deleted = delete_save(user, directory);
        ps4::system::log("saveData.mount: sceSaveDataDelete=0x%08x",
                         static_cast<unsigned>(deleted));
        if (deleted == 0) {
            result = {};
            code = sceSaveDataMount2(&request, &result);
            ps4::system::log("saveData.mount: retry=0x%08x path=%s",
                             static_cast<unsigned>(code), result.mountPathName);
        } else {
            // Deletion can fail if the system already considers the directory
            // unusable. The explicit repair path may clear the corrupt flag.
            request.mountMode |= ORBIS_SAVE_DATA_MOUNT_MODE_DESTRUCT_OFF;
            result = {};
            code = sceSaveDataMount2(&request, &result);
            ps4::system::log("saveData.mount: destruct-off retry=0x%08x path=%s",
                             static_cast<unsigned>(code), result.mountPathName);
        }
    }
    if (code != 0)
        return failure(ctx, "SaveData.mount", code);

    Mounted entry;
    std::size_t path_length = 0;
    while (path_length < sizeof result.mountPathName && result.mountPathName[path_length])
        ++path_length;
    entry.path.assign(result.mountPathName, path_length);
    std::memcpy(entry.point.data, result.mountPathName, sizeof entry.point.data);
    entry.user = user;
    entry.blocks = blocks;
    entry.mount_mode = request.mountMode;
    if (entry.path.empty()) {
        sceSaveDataUmount(&entry.point);
        return JS_ThrowPlainError(ctx, "SaveData.mount: the system returned an empty mount point");
    }

    if (!title.empty() || !subtitle.empty() || !details.empty()) {
        OrbisSaveDataParam param{};
        std::snprintf(param.title, sizeof param.title, "%s", title.c_str());
        std::snprintf(param.subtitle, sizeof param.subtitle, "%s", subtitle.c_str());
        std::snprintf(param.details, sizeof param.details, "%s", details.c_str());
        sceSaveDataSetParam(&entry.point, ORBIS_SAVE_DATA_PARAM_TYPE_ALL, &param, sizeof param);
    }

    {
        std::lock_guard<std::mutex> lock(mount_mutex);
        mounted[directory] = entry;
    }
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "path", JS_NewString(ctx, entry.path.c_str()));
    return out;
}

JSValue unmount(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string directory;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        if (!JS_IsString(argv[0]))
            return JS_ThrowTypeError(ctx, "SaveData.unmount: expected a directory name");
        const char* value = JS_ToCString(ctx, argv[0]);
        if (!value) return JS_EXCEPTION;
        directory = value;
        JS_FreeCString(ctx, value);
    }

    std::lock_guard<std::mutex> lock(mount_mutex);
    bool ok = true;
    if (directory.empty()) {
        for (auto& [name, entry] : mounted) {
            if (sceSaveDataUmount(&entry.point) != 0) ok = false;
            (void)name;
        }
        mounted.clear();
    } else {
        const auto found = mounted.find(directory);
        if (found == mounted.end())
            return JS_ThrowPlainError(ctx, "SaveData.unmount: '%s' is not mounted", directory.c_str());
        if (sceSaveDataUmount(&found->second.point) != 0) ok = false;
        else mounted.erase(found);
    }
    return JS_NewBool(ctx, ok);
}

JSValue remove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "SaveData.remove: expected a directory name");
    const char* value = JS_ToCString(ctx, argv[0]);
    if (!value) return JS_EXCEPTION;
    std::string directory = value;
    JS_FreeCString(ctx, value);

    int user = -1;
    if (!user_id(user))
        return JS_ThrowPlainError(ctx, "SaveData.remove: no foreground user");
    if (!initialize())
        return JS_ThrowPlainError(ctx, "SaveData.remove: sceSaveDataInitialize3 failed");

    {
        std::lock_guard<std::mutex> lock(mount_mutex);
        const auto found = mounted.find(directory);
        if (found != mounted.end()) {
            sceSaveDataUmount(&found->second.point);
            mounted.erase(found);
        }
    }

    const int code = delete_save(user, directory);
    if (code != 0)
        return failure(ctx, "SaveData.remove", code);
    return JS_TRUE;
}

} // namespace

JSValue ps4_react_save_data_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "mount", JS_NewCFunction(ctx, mount, "mount", 5));
    JS_SetPropertyStr(ctx, api, "unmount", JS_NewCFunction(ctx, unmount, "unmount", 1));
    JS_SetPropertyStr(ctx, api, "remove", JS_NewCFunction(ctx, remove, "remove", 1));
    return api;
}

// Releases every mount before the title is closed. A mounted save data
// directory holds a SceSaveData app-suspend block; the system's KillApp
// suspends the title first, and that block makes the suspend time out.
extern "C" void ps4_react_save_data_shutdown() {
    std::lock_guard<std::mutex> lock(mount_mutex);
    for (auto& [name, entry] : mounted) {
        if (!entry.suspended)
            (void)sceSaveDataUmount(&entry.point);
        (void)name;
    }
    mounted.clear();
}

// The system suspends the title when it goes to the background. Unmount every
// directory first so SceSaveData does not hold an app-suspend block.
extern "C" void ps4_react_save_data_suspend() {
    std::lock_guard<std::mutex> lock(mount_mutex);
    for (auto& [name, entry] : mounted) {
        if (entry.suspended)
            continue;
        const int code = sceSaveDataUmount(&entry.point);
        if (code == 0) {
            entry.suspended = true;
            ps4::system::log("saveData: suspended %s", name.c_str());
        } else {
            ps4::system::log("saveData: suspend umount %s=0x%08x", name.c_str(),
                             static_cast<unsigned>(code));
        }
    }
}

// Reopens the directories that were mounted when the title went to the
// background. The mount point normally stays /savedata0.
extern "C" void ps4_react_save_data_resume() {
    std::lock_guard<std::mutex> lock(mount_mutex);
    for (auto& [name, entry] : mounted) {
        if (!entry.suspended)
            continue;
        OrbisSaveDataDirName dir_name{};
        std::snprintf(dir_name.data, sizeof dir_name.data, "%s", name.c_str());
        OrbisSaveDataMount2 request{};
        request.userId = entry.user;
        request.dirName = &dir_name;
        request.blocks = entry.blocks;
        request.mountMode = entry.mount_mode;
        OrbisSaveDataMountResult result{};
        int code = sceSaveDataMount2(&request, &result);
        if (code == static_cast<int>(0x809f0007u)) {
            request.mountMode &= ~ORBIS_SAVE_DATA_MOUNT_MODE_CREATE2;
            result = {};
            code = sceSaveDataMount2(&request, &result);
        }
        if (code != 0) {
            ps4::system::log("saveData: resume %s=0x%08x", name.c_str(),
                             static_cast<unsigned>(code));
            continue;
        }
        std::size_t path_length = 0;
        while (path_length < sizeof result.mountPathName && result.mountPathName[path_length])
            ++path_length;
        entry.path.assign(result.mountPathName, path_length);
        std::memcpy(entry.point.data, result.mountPathName, sizeof entry.point.data);
        entry.suspended = false;
        ps4::system::log("saveData: resumed %s at %s", name.c_str(), entry.path.c_str());
    }
}
