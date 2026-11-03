// System-service event pump and app lifecycle state.
//
// sceSystemServiceReceiveEvent is non-blocking: it returns 0 with an event or
// 0x80a10004 when the queue is empty. The render loop drains a few events per
// frame and JavaScript reads them through AppState.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host_api.hpp"

#include "system.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {

// The OpenOrbis header declares this without arguments; keep the real shape
// local so the event payload can be read.
struct SystemServiceEvent {
    int32_t type;
    union {
        char param[8192];
        struct {
            char source[1024];
            char url[4096];
        } url_open;
    };
};

int sceSystemServiceReceiveEvent(SystemServiceEvent* event);

struct SystemServiceStatus {
    int32_t event_num;
    bool is_system_ui_overlaid;
    bool is_in_background_execution;
    bool is_cpu_mode7_cpu_normal;
    bool is_game_live_streaming_on_air;
    bool is_out_of_vr_play_area;
    std::uint8_t reserved[240];
};

int sceSystemServiceGetStatus(SystemServiceStatus* status);

} // extern "C"

namespace {

constexpr int32_t kNoEvent = static_cast<int32_t>(0x80a10004u);
constexpr int32_t kOnResume = 0x10000000;
constexpr int32_t kDisplaySafeAreaUpdate = 0x10000005;
constexpr int32_t kUrlOpen = 0x10000006;
constexpr int32_t kLaunchApp = 0x10000007;
constexpr int32_t kAppLaunchLink = 0x10000008;
constexpr int32_t kWebBrowserClosed = 0x10000012;
constexpr int32_t kControllerSettingsClosed = 0x10000013;
constexpr int32_t kOpenShareMenu = 0x30000000;

struct Event {
    int32_t type = 0;
    std::string name;
    std::string url;
};

std::mutex state_mutex;
std::vector<Event> pending;
std::string state = "active";
std::string last_event;
std::string last_url;
unsigned pump_count = 0;
bool status_logged = false;

const char* known_name(int32_t type) {
    switch (type) {
        case kOnResume: return "resume";
        case kDisplaySafeAreaUpdate: return "displaySafeArea";
        case kUrlOpen: return "urlOpen";
        case kLaunchApp: return "launchApp";
        case kAppLaunchLink: return "appLaunchLink";
        case kWebBrowserClosed: return "webBrowserClosed";
        case kControllerSettingsClosed: return "controllerSettingsClosed";
        case kOpenShareMenu: return "shareMenu";
        default: return nullptr;
    }
}

void record(int32_t type, const char* name, std::string url = {}) {
    if (!url.empty())
        ps4::system::log("app: event %s url=%s", name, url.c_str());
    else
        ps4::system::log("app: event %s (0x%08x)", name, static_cast<unsigned>(type));

    std::lock_guard<std::mutex> lock(state_mutex);
    if (pending.size() >= 64)
        pending.erase(pending.begin());
    Event event;
    event.type = type;
    event.name = name;
    event.url = std::move(url);
    last_event = event.name;
    if (!event.url.empty())
        last_url = event.url;
    pending.push_back(std::move(event));
}

void handle(const SystemServiceEvent& event) {
    if (const char* name = known_name(event.type)) {
        if (event.type == kUrlOpen) {
            std::size_t length = 0;
            while (length < sizeof event.url_open.url && event.url_open.url[length])
                ++length;
            record(event.type, name, std::string(event.url_open.url, length));
        } else {
            record(event.type, name);
        }
        if (event.type == kOnResume) {
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                state = "active";
            }
            ps4_react_save_data_resume();
        }
        return;
    }

    char name[32];
    std::snprintf(name, sizeof name, "event-0x%08x", static_cast<unsigned>(event.type));
    record(event.type, name);
}

} // namespace

void ps4_react_app_state_pump() {
    for (int i = 0; i < 4; ++i) {
        SystemServiceEvent event{};
        const int result = sceSystemServiceReceiveEvent(&event);
        if (result == 0) {
            handle(event);
            continue;
        }
        if (result != kNoEvent && result != static_cast<int32_t>(0x80a10003u))
            ps4::system::log("app: ReceiveEvent=0x%08x", static_cast<unsigned>(result));
        break;
    }

    // Background/focus changes are not part of the event payload on this
    // firmware; poll the status at a low rate instead.
    if (++pump_count % 30 != 0)
        return;
    SystemServiceStatus status{};
    const int result = sceSystemServiceGetStatus(&status);
    if (result != 0) {
        if (!status_logged) {
            status_logged = true;
            ps4::system::log("app: GetStatus=0x%08x", static_cast<unsigned>(result));
        }
        return;
    }
    const char* next = status.is_in_background_execution ? "background" : "active";
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (state != next) {
            state = next;
            changed = true;
        }
    }
    if (changed) {
        record(-1, next);
        if (std::strcmp(next, "background") == 0)
            ps4_react_save_data_suspend();
        else
            ps4_react_save_data_resume();
    }
}

JSValue ps4_react_app_state_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);

    JSValue get = JS_NewCFunction(ctx, +[](JSContext* context, JSValueConst, int, JSValueConst*) -> JSValue {
        std::lock_guard<std::mutex> lock(state_mutex);
        JSValue object = JS_NewObject(context);
        JS_SetPropertyStr(context, object, "state", JS_NewString(context, state.c_str()));
        JS_SetPropertyStr(context, object, "lastEvent", JS_NewString(context, last_event.c_str()));
        JS_SetPropertyStr(context, object, "lastUrl", JS_NewString(context, last_url.c_str()));
        return object;
    }, "get", 0);

    JSValue poll = JS_NewCFunction(ctx, +[](JSContext* context, JSValueConst, int, JSValueConst*) -> JSValue {
        JSValue array = JS_NewArray(context);
        std::lock_guard<std::mutex> lock(state_mutex);
        std::uint32_t index = 0;
        for (const Event& event : pending) {
            JSValue item = JS_NewObject(context);
            JS_SetPropertyStr(context, item, "type", JS_NewInt32(context, event.type));
            JS_SetPropertyStr(context, item, "name", JS_NewString(context, event.name.c_str()));
            JS_SetPropertyStr(context, item, "url", JS_NewString(context, event.url.c_str()));
            JS_SetPropertyUint32(context, array, index++, item);
        }
        pending.clear();
        return array;
    }, "poll", 0);

    JS_SetPropertyStr(ctx, api, "get", get);
    JS_SetPropertyStr(ctx, api, "poll", poll);
    return api;
}
