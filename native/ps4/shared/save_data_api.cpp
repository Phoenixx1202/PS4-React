// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include "app_config.generated.h"

#include <orbis/SaveData.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

namespace {

struct Mounted {
    std::string path;
    OrbisSaveDataMountPoint point{};
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

JSValue failure(JSContext* ctx, const char* call, int code) {
    return JS_ThrowPlainError(ctx, "%s failed (0x%08x)", call, static_cast<unsigned>(code));
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

    if (!initialize())
        return JS_ThrowPlainError(ctx, "SaveData.mount: sceSaveDataInitialize3 failed");

    OrbisSaveDataMount request{};
    request.userId = user;
    request.titleId = PS4_REACT_TITLE_ID;
    request.dirName = directory.c_str();
    request.fingerprint = "";
    request.blocks = blocks;
    request.mountMode = ORBIS_SAVE_DATA_MOUNT_MODE_CREATE2 | ORBIS_SAVE_DATA_MOUNT_MODE_RDWR;
    OrbisSaveDataMountResult result{};
    const int code = sceSaveDataMount(&request, &result);
    if (code != 0)
        return failure(ctx, "SaveData.mount", code);

    Mounted entry;
    std::size_t path_length = 0;
    while (path_length < sizeof result.mountPathName && result.mountPathName[path_length])
        ++path_length;
    entry.path.assign(result.mountPathName, path_length);
    std::memcpy(entry.point.data, result.mountPathName, sizeof entry.point.data);
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

    OrbisSaveDataTitleId title{};
    std::snprintf(title.data, sizeof title.data, "%s", PS4_REACT_TITLE_ID);
    OrbisSaveDataDirName name{};
    std::snprintf(name.data, sizeof name.data, "%s", directory.c_str());
    OrbisSaveDataDelete request{};
    request.userId = user;
    request.titleId = &title;
    request.dirName = &name;
    const int code = sceSaveDataDelete(&request);
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
