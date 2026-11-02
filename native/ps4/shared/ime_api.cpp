// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include <orbis/ImeDialog.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

enum class State { running, completed, cancelled, failed };

struct Task {
    std::uint32_t id = 0;
    State state = State::running;
    std::string text;
    std::string error;
    // The PS4 IME API takes wchar_t*, but its buffer is UTF-16. Keep it as
    // char16_t and cast at the boundary, like the system samples do.
    std::vector<char16_t> buffer;
    std::u16string title;
    std::u16string placeholder;
    OrbisImeDialogSetting setting{};
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

bool utf8_to_utf16(const char* text, std::size_t length, std::u16string& out) {
    out.clear();
    for (std::size_t i = 0; i < length;) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::uint32_t code = 0;
        std::size_t size = 0;
        if (c < 0x80) { code = c; size = 1; }
        else if ((c & 0xe0) == 0xc0) { code = c & 0x1f; size = 2; }
        else if ((c & 0xf0) == 0xe0) { code = c & 0x0f; size = 3; }
        else if ((c & 0xf8) == 0xf0) { code = c & 0x07; size = 4; }
        else return false;
        if (i + size > length) return false;
        for (std::size_t j = 1; j < size; ++j) {
            const auto next = static_cast<unsigned char>(text[i + j]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        if (code <= 0xffff) {
            out.push_back(static_cast<char16_t>(code));
        } else {
            code -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (code >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (code & 0x3ff)));
        }
        i += size;
    }
    return true;
}

std::string utf16_to_utf8(const char16_t* text, std::size_t length) {
    std::string out;
    for (std::size_t i = 0; i < length; ++i) {
        std::uint32_t code = text[i];
        if (code >= 0xd800 && code <= 0xdbff && i + 1 < length &&
            text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff) {
            code = 0x10000 + ((code - 0xd800) << 10) + (text[++i] - 0xdc00);
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else if (code < 0x10000) {
            out.push_back(static_cast<char>(0xe0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
        }
    }
    return out;
}

bool text_arg(JSContext* ctx, JSValueConst value, std::string& out) {
    if (JS_IsUndefined(value) || JS_IsNull(value)) {
        out.clear();
        return true;
    }
    if (!JS_IsString(value)) return false;
    std::size_t length = 0;
    const char* text = JS_ToCStringLen(ctx, &length, value);
    if (!text) return false;
    out.assign(text, length);
    JS_FreeCString(ctx, text);
    return true;
}

JSValue start(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string title;
    std::string placeholder;
    std::string value;
    if (!text_arg(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, title) ||
        !text_arg(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, placeholder) ||
        !text_arg(ctx, argc > 2 ? argv[2] : JS_UNDEFINED, value))
        return JS_ThrowTypeError(ctx, "Keyboard.prompt: title, placeholder and value must be strings");

    std::uint32_t max_length = 256;
    if (argc > 3 && !JS_IsUndefined(argv[3]) && JS_ToUint32(ctx, &max_length, argv[3]))
        return JS_ThrowTypeError(ctx, "Keyboard.prompt: maxLength must be a number");
    if (max_length < 1 || max_length > 1024)
        return JS_ThrowTypeError(ctx, "Keyboard.prompt: maxLength must be between 1 and 1024");

    OrbisImeType type = ORBIS_TYPE_DEFAULT;
    if (argc > 4 && !JS_IsUndefined(argv[4])) {
        std::string name;
        if (!text_arg(ctx, argv[4], name))
            return JS_ThrowTypeError(ctx, "Keyboard.prompt: type must be a string");
        if (name == "basic") type = ORBIS_TYPE_BASIC_LATIN;
        else if (name == "url") type = ORBIS_TYPE_TYPE_URL;
        else if (name == "email") type = ORBIS_TYPE_MAIL;
        else if (name == "number") type = ORBIS_TYPE_NUMBER;
        else if (!name.empty() && name != "default")
            return JS_ThrowTypeError(ctx, "Keyboard.prompt: type must be default, basic, url, email or number");
    }

    host::User user;
    if (!host::foreground_user(user) || user.id < 0)
        return JS_ThrowPlainError(ctx, "Keyboard.prompt: no foreground user");

    std::u16string wide_title;
    std::u16string wide_placeholder;
    std::u16string wide_value;
    if (!utf8_to_utf16(title.data(), title.size(), wide_title) ||
        !utf8_to_utf16(placeholder.data(), placeholder.size(), wide_placeholder) ||
        !utf8_to_utf16(value.data(), value.size(), wide_value))
        return JS_ThrowTypeError(ctx, "Keyboard.prompt: text is not valid UTF-8");

    auto task = std::make_shared<Task>();
    task->id = next_id++;
    task->title = std::move(wide_title);
    task->placeholder = std::move(wide_placeholder);
    task->buffer.assign(max_length + 1, u'\0');
    std::copy_n(wide_value.begin(), std::min(wide_value.size(), static_cast<std::size_t>(max_length)),
                task->buffer.begin());
    task->setting.userId = static_cast<std::uint32_t>(user.id);
    task->setting.type = type;
    task->setting.supportedLanguages = 0;
    task->setting.enterLabel = ORBIS_BUTTON_LABEL_DEFAULT;
    task->setting.inputMethod = ORBIS__DEFAULT;
    task->setting.filter = nullptr;
    task->setting.option = 0;
    task->setting.maxTextLength = max_length;
    task->setting.inputTextBuffer = reinterpret_cast<wchar_t*>(task->buffer.data());
    task->setting.posx = 960;
    task->setting.posy = 540;
    task->setting.horizontalAlignment = ORBIS_H_CENTER;
    task->setting.verticalAlignment = ORBIS_V_CENTER;
    task->setting.placeholder = reinterpret_cast<const wchar_t*>(task->placeholder.c_str());
    task->setting.title = reinterpret_cast<const wchar_t*>(task->title.c_str());

    std::lock_guard<std::mutex> lock(task_mutex);
    if (active)
        return JS_ThrowPlainError(ctx, "Keyboard.prompt: another dialog is already open");
    const int code = sceImeDialogInit(&task->setting, nullptr);
    if (code != 0)
        return JS_ThrowPlainError(ctx, "Keyboard.prompt: sceImeDialogInit failed (0x%08x)",
                                  static_cast<unsigned>(code));
    active = task;
    return JS_NewUint32(ctx, task->id);
}

JSValue cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Keyboard.cancel: expected a task id");
    std::lock_guard<std::mutex> lock(task_mutex);
    if (active && active->id == id) {
        sceImeDialogAbort();
        sceImeDialogTerm();
        active->state = State::cancelled;
    }
    return JS_UNDEFINED;
}

JSValue poll(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue out = JS_NewArray(ctx);
    std::lock_guard<std::mutex> lock(task_mutex);
    if (!active)
        return out;

    if (active->state == State::running) {
        const OrbisDialogStatus status = sceImeDialogGetStatus();
        if (status == ORBIS_DIALOG_STATUS_STOPPED) {
            OrbisDialogResult result{};
            const int code = sceImeDialogGetResult(&result);
            sceImeDialogTerm();
            if (code != 0) {
                active->state = State::failed;
                active->error = "sceImeDialogGetResult failed";
            } else if (result.endstatus == ORBIS_DIALOG_OK) {
                active->state = State::completed;
                active->text = utf16_to_utf8(active->buffer.data(), active->buffer.size());
                const auto nul = active->text.find('\0');
                if (nul != std::string::npos) active->text.resize(nul);
            } else {
                active->state = State::cancelled;
            }
        }
    }

    JSValue item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, item, "id", JS_NewUint32(ctx, active->id));
    JS_SetPropertyStr(ctx, item, "state", JS_NewString(ctx, state_name(active->state)));
    JS_SetPropertyStr(ctx, item, "text", JS_NewString(ctx, active->text.c_str()));
    JS_SetPropertyStr(ctx, item, "error", JS_NewString(ctx, active->error.c_str()));
    JS_SetPropertyUint32(ctx, out, 0, item);
    if (active->state != State::running)
        active.reset();
    return out;
}

} // namespace

JSValue ps4_react_ime_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "start", JS_NewCFunction(ctx, start, "start", 5));
    JS_SetPropertyStr(ctx, api, "cancel", JS_NewCFunction(ctx, cancel, "cancel", 1));
    JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 0));
    return api;
}
