// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include "system.hpp"

#include <curl/curl.h>
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

enum class State : int { connecting, open, closed, failed };

struct Event {
    std::string type;
    std::string data;
    std::string id;
};

struct Task {
    std::uint32_t id = 0;
    std::string url;
    std::vector<std::string> headers;
    std::atomic<State> state{State::connecting};
    State reported = State::connecting;
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::deque<Event> events;
    std::string error;
    std::string pending;
    std::string event_type;
    std::string event_data;
    std::string event_id;
};

std::mutex tasks_mutex;
std::vector<std::shared_ptr<Task>> tasks;
std::atomic<std::uint32_t> next_id{1};

const char* state_name(State state) {
    switch (state) {
        case State::connecting: return "connecting";
        case State::open: return "open";
        case State::closed: return "closed";
        default: return "failed";
    }
}

void fail(Task& task, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        task.error = message;
    }
    task.state = State::failed;
    ps4::system::log("sse: %s failed: %s", task.url.c_str(), message.c_str());
}

void dispatch(Task& task) {
    if (task.event_data.empty()) {
        task.event_type.clear();
        return;
    }
    if (task.event_data.back() == '\n')
        task.event_data.pop_back();
    Event event;
    event.type = task.event_type.empty() ? "message" : task.event_type;
    event.data = std::move(task.event_data);
    event.id = task.event_id;
    {
        std::lock_guard<std::mutex> lock(task.mutex);
        if (task.events.size() >= 256)
            task.events.pop_front();
        task.events.push_back(std::move(event));
    }
    task.event_type.clear();
    task.event_data.clear();
}

void line(Task& task, const std::string& value) {
    if (value.empty()) {
        dispatch(task);
        return;
    }
    if (value[0] == ':')
        return;
    const std::size_t colon = value.find(':');
    std::string field = colon == std::string::npos ? value : value.substr(0, colon);
    std::string content = colon == std::string::npos ? std::string() : value.substr(colon + 1);
    if (!content.empty() && content[0] == ' ')
        content.erase(0, 1);
    if (field == "data") {
        task.event_data += content;
        task.event_data += '\n';
    } else if (field == "event") {
        task.event_type = content;
    } else if (field == "id" && content.find('\0') == std::string::npos) {
        task.event_id = content;
    }
}

size_t write_callback(char* data, size_t size, size_t count, void* user) {
    auto& task = *static_cast<Task*>(user);
    if (task.cancelled) return 0;
    const size_t total = size * count;
    task.pending.append(data, total);
    size_t start = 0;
    for (;;) {
        const std::size_t newline = task.pending.find('\n', start);
        if (newline == std::string::npos) break;
        std::string value = task.pending.substr(start, newline - start);
        if (!value.empty() && value.back() == '\r')
            value.pop_back();
        line(task, value);
        start = newline + 1;
    }
    task.pending.erase(0, start);
    task.state = State::open;
    return total;
}

size_t header_callback(char* data, size_t size, size_t count, void* user) {
    auto& task = *static_cast<Task*>(user);
    const size_t total = size * count;
    std::string value(data, total);
    if (value.rfind("HTTP/", 0) == 0) {
        const std::size_t space = value.find(' ');
        if (space != std::string::npos) {
            const int status = std::atoi(value.c_str() + space + 1);
            if (status == 200)
                task.state = State::open;
            else if (status >= 400)
                fail(task, "HTTP " + std::to_string(status));
        }
    }
    return total;
}

int progress_callback(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Task*>(user)->cancelled ? 1 : 0;
}

void run(Task& task) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        fail(task, "curl_easy_init failed");
        return;
    }

    struct curl_slist* list = nullptr;
    for (const std::string& header : task.headers)
        list = curl_slist_append(list, header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, task.url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/app0/ca-bundle.crt");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &task);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &task);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &task);
    if (list)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);

    ps4::system::log("sse: connecting %s", task.url.c_str());
    const CURLcode result = curl_easy_perform(curl);
    if (task.cancelled) {
        task.state = State::closed;
    } else if (result != CURLE_OK) {
        fail(task, curl_easy_strerror(result));
    } else {
        task.state = State::closed;
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    ps4::system::log("sse: closed %s", task.url.c_str());
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
        fail(*task, "cannot start the SSE thread");
    }
}

JSValue connect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Sse.connect: expected a URL");
    std::size_t length = 0;
    const char* url = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!url) return JS_EXCEPTION;
    std::string address(url, length);
    JS_FreeCString(ctx, url);
    if (length > 2048 ||
        (address.rfind("http://", 0) != 0 && address.rfind("https://", 0) != 0))
        return JS_ThrowTypeError(ctx, "Sse.connect: URL must be http(s) and at most 2048 bytes");

    auto task = std::make_shared<Task>();
    task->id = next_id++;
    task->url = std::move(address);

    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (!JS_IsArray(argv[1]))
            return JS_ThrowTypeError(ctx, "Sse.connect: headers must be an array of strings");
        const JSValue size_value = JS_GetPropertyStr(ctx, argv[1], "length");
        std::uint32_t count = 0;
        const int converted = JS_ToUint32(ctx, &count, size_value);
        JS_FreeValue(ctx, size_value);
        if (converted || count > 64)
            return JS_ThrowTypeError(ctx, "Sse.connect: at most 64 headers");
        for (std::uint32_t i = 0; i < count; ++i) {
            const JSValue value = JS_GetPropertyUint32(ctx, argv[1], i);
            if (!JS_IsString(value)) {
                JS_FreeValue(ctx, value);
                return JS_ThrowTypeError(ctx, "Sse.connect: headers must be strings");
            }
            std::size_t header_length = 0;
            const char* header = JS_ToCStringLen(ctx, &header_length, value);
            JS_FreeValue(ctx, value);
            if (!header) return JS_EXCEPTION;
            if (header_length > 4096 || std::memchr(header, '\0', header_length) ||
                std::memchr(header, '\r', header_length) || std::memchr(header, '\n', header_length)) {
                JS_FreeCString(ctx, header);
                return JS_ThrowTypeError(ctx, "Sse.connect: invalid header");
            }
            task->headers.emplace_back(header, header_length);
            JS_FreeCString(ctx, header);
        }
    }

    {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        tasks.push_back(task);
    }
    start(task);
    return JS_NewUint32(ctx, task->id);
}

JSValue poll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Sse.poll: expected a task id");

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, "closed"));
    JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, "unknown SSE task"));
    JS_SetPropertyStr(ctx, out, "events", JS_NewArray(ctx));
    std::lock_guard<std::mutex> lock(tasks_mutex);
    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
        const auto& task = *it;
        if (task->id != id) continue;
        const State state = task->state;
        if (task->reported != state) {
            task->reported = state;
            ps4::system::log("sse: poll %u state=%s", task->id, state_name(state));
        }
        JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, state_name(state)));
        {
            std::lock_guard<std::mutex> task_lock(task->mutex);
            JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, task->error.c_str()));
            JSValue events = JS_NewArray(ctx);
            std::uint32_t index = 0;
            for (const Event& event : task->events) {
                JSValue item = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, item, "type", JS_NewString(ctx, event.type.c_str()));
                JS_SetPropertyStr(ctx, item, "data", JS_NewString(ctx, event.data.c_str()));
                JS_SetPropertyStr(ctx, item, "id", JS_NewString(ctx, event.id.c_str()));
                JS_SetPropertyUint32(ctx, events, index++, item);
            }
            task->events.clear();
            JS_SetPropertyStr(ctx, out, "events", events);
        }
        if (state == State::closed || state == State::failed)
            it = tasks.erase(it);
        break;
    }
    return out;
}

JSValue close(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Sse.close: expected a task id");
    std::lock_guard<std::mutex> lock(tasks_mutex);
    for (const auto& task : tasks) {
        if (task->id == id) {
            task->cancelled = true;
            break;
        }
    }
    return JS_UNDEFINED;
}

} // namespace

JSValue ps4_react_sse_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "connect", JS_NewCFunction(ctx, connect, "connect", 2));
    JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 1));
    JS_SetPropertyStr(ctx, api, "close", JS_NewCFunction(ctx, close, "close", 1));
    return api;
}
