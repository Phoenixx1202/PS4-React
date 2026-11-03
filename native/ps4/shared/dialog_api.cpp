// System message dialogs for the React host.
//
// The dialog runs in the console's common-dialog process. The render thread
// keeps running; JavaScript polls this API like the network tasks. While a
// dialog is open the host stops dispatching pad input to the app.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "host_api.hpp"

#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/libkernel.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>

namespace {

enum class State { running, completed, cancelled, failed };

struct Task {
    std::uint32_t id = 0;
    State state = State::running;
    bool confirm = false;
    std::string message;
    OrbisMsgDialogParam param{};
    OrbisMsgDialogUserMessageParam user{};
    int button = 0;
    std::string error;
};

std::mutex task_mutex;
std::shared_ptr<Task> active;
std::uint32_t next_id = 1;

const char* state_name(State state) {
    switch (state) {
        case State::running: return "running";
        case State::completed: return "completed";
        case State::cancelled: return "cancelled";
        default: return "failed";
    }
}

bool read_string(JSContext* ctx, JSValueConst value, std::string& out) {
    if (JS_IsUndefined(value) || JS_IsNull(value))
        return true;
    if (!JS_IsString(value))
        return false;
    std::size_t length = 0;
    const char* text = JS_ToCStringLen(ctx, &length, value);
    if (!text)
        return false;
    out.assign(text, length);
    JS_FreeCString(ctx, text);
    return true;
}

void init_base_param(OrbisCommonDialogBaseParam& base) {
    std::memset(&base, 0, sizeof base);
    base.size = sizeof base;
    // The system's common-dialog cookie mixes the magic with the address of
    // the base param; Apollo's helper does the same.
    base.magic = static_cast<std::uint32_t>(
        ORBIS_COMMON_DIALOG_MAGIC_NUMBER + static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(&base)));
}

JSValue throw_error(JSContext* ctx, const char* call, int code) {
    return JS_ThrowPlainError(ctx, "%s: sceMsgDialogOpen failed (0x%08x)",
                              call, static_cast<unsigned>(code));
}

JSValue start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    const char* call = argc > 2 && JS_ToBool(ctx, argv[2]) == 1 ? "Dialog.confirm" : "Dialog.alert";
    std::string title;
    std::string message;
    bool confirm = false;
    if (!read_string(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, title) ||
        !read_string(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, message))
        return JS_ThrowTypeError(ctx, "%s: title and message must be strings", call);
    confirm = argc > 2 && JS_ToBool(ctx, argv[2]) == 1;

    std::lock_guard<std::mutex> lock(task_mutex);
    if (active && active->state == State::running)
        return JS_ThrowPlainError(ctx, "%s: another dialog is already open", call);

    auto task = std::make_shared<Task>();
    task->id = next_id++;
    task->confirm = confirm;
    if (!title.empty()) {
        task->message = title;
        task->message += "\n\n";
    }
    task->message += message;

    // Initialize is idempotent on this firmware. The common dialog itself was
    // initialized during host startup.
    (void)sceMsgDialogInitialize();

    init_base_param(task->param.baseParam);
    task->param.size = sizeof(OrbisMsgDialogParam);
    task->param.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
    task->user.buttonType = confirm ? ORBIS_MSG_DIALOG_BUTTON_TYPE_YESNO
                                    : ORBIS_MSG_DIALOG_BUTTON_TYPE_OK;
    task->user.msg = task->message.c_str();
    task->param.userMsgParam = &task->user;

    const int opened = sceMsgDialogOpen(&task->param);
    if (opened < 0) {
        task->state = State::failed;
        task->error = "sceMsgDialogOpen failed";
        return throw_error(ctx, call, opened);
    }

    active = task;
    return JS_NewUint32(ctx, task->id);
}

JSValue poll(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue out = JS_NewArray(ctx);
    std::lock_guard<std::mutex> lock(task_mutex);
    if (!active)
        return out;

    if (active->state == State::running) {
        const OrbisCommonDialogStatus status = sceMsgDialogUpdateStatus();
        if (status == ORBIS_COMMON_DIALOG_STATUS_FINISHED) {
            (void)sceMsgDialogClose();
            OrbisMsgDialogResult result{};
            const int got = sceMsgDialogGetResult(&result);
            (void)sceMsgDialogTerminate();
            if (got < 0) {
                active->state = State::failed;
                active->error = "sceMsgDialogGetResult failed";
            } else {
                active->state = State::completed;
                active->button = result.buttonId;
            }
        } else if (status == ORBIS_COMMON_DIALOG_STATUS_NONE) {
            (void)sceMsgDialogTerminate();
            active->state = State::failed;
            active->error = "the dialog closed unexpectedly";
        }
    }

    JSValue item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, item, "id", JS_NewUint32(ctx, active->id));
    JS_SetPropertyStr(ctx, item, "state", JS_NewString(ctx, state_name(active->state)));
    JS_SetPropertyStr(ctx, item, "buttonId", JS_NewInt32(ctx, active->button));
    JS_SetPropertyStr(ctx, item, "confirmed",
                      JS_NewBool(ctx, active->state == State::completed && active->button == 1));
    JS_SetPropertyStr(ctx, item, "error", JS_NewString(ctx, active->error.c_str()));
    JS_SetPropertyUint32(ctx, out, 0, item);

    if (active->state != State::running)
        active.reset();
    return out;
}

JSValue cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Dialog.cancel: expected a task id");
    std::lock_guard<std::mutex> lock(task_mutex);
    if (active && active->id == id) {
        (void)sceMsgDialogClose();
        (void)sceMsgDialogTerminate();
        active->state = State::cancelled;
        active.reset();
    }
    return JS_UNDEFINED;
}

} // namespace

bool ps4_react_dialog_active() {
    std::lock_guard<std::mutex> lock(task_mutex);
    return active && active->state == State::running;
}

JSValue ps4_react_dialog_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "start", JS_NewCFunction(ctx, start, "start", 3));
    JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 0));
    JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
    return api;
}
