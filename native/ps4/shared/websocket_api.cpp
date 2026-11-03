// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include "system.hpp"

#include <curl/curl.h>
#include <pthread.h>
#include <sys/select.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
    bool binary = false;
};

struct Task {
    std::uint32_t id = 0;
    std::string url;
    std::string origin;
    std::string host;
    std::string path;
    std::atomic<State> state{State::connecting};
    State reported = State::connecting;
    std::atomic<bool> cancelled{false};
    CURL* curl = nullptr;
    curl_socket_t socket = CURL_SOCKET_BAD;
    std::mutex io_mutex;
    std::mutex event_mutex;
    std::deque<Event> events;
    std::string error;
    std::vector<std::uint8_t> input;
    std::vector<std::uint8_t> message;
    std::uint8_t message_opcode = 0;
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
        std::lock_guard<std::mutex> lock(task.event_mutex);
        task.error = message;
    }
    task.state = State::failed;
    ps4::system::log("websocket: %s failed: %s", task.url.c_str(), message.c_str());
}

void event(Task& task, const char* type, std::string data = {}, bool binary = false) {
    std::lock_guard<std::mutex> lock(task.event_mutex);
    if (task.events.size() >= 256)
        task.events.pop_front();
    task.events.push_back({type, std::move(data), binary});
}

std::string base64(const std::uint8_t* data, std::size_t size) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t value =
            (static_cast<std::uint32_t>(data[i]) << 16) |
            (i + 1 < size ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0) |
            (i + 2 < size ? data[i + 2] : 0);
        out += alphabet[(value >> 18) & 63];
        out += alphabet[(value >> 12) & 63];
        out += i + 1 < size ? alphabet[(value >> 6) & 63] : '=';
        out += i + 2 < size ? alphabet[value & 63] : '=';
    }
    return out;
}

bool parse_url(const std::string& url, Task& task) {
    std::string rest;
    if (url.rfind("ws://", 0) == 0) {
        rest = url.substr(5);
        task.origin = "http://";
    } else if (url.rfind("wss://", 0) == 0) {
        rest = url.substr(6);
        task.origin = "https://";
    } else {
        return false;
    }
    const std::size_t slash = rest.find('/');
    task.host = slash == std::string::npos ? rest : rest.substr(0, slash);
    task.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (task.host.empty() || task.path.empty())
        return false;
    task.origin += task.host + "/";
    return true;
}

bool wait_writable(curl_socket_t socket, int timeout_ms) {
    fd_set writes;
    FD_ZERO(&writes);
    FD_SET(socket, &writes);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return select(socket + 1, nullptr, &writes, nullptr, &timeout) > 0;
}

bool send_all(Task& task, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::lock_guard<std::mutex> lock(task.io_mutex);
    while (size && !task.cancelled) {
        std::size_t sent = 0;
        const CURLcode result = curl_easy_send(task.curl, bytes, size, &sent);
        if (result == CURLE_AGAIN) {
            if (!wait_writable(task.socket, 1000)) continue;
            continue;
        }
        if (result != CURLE_OK)
            return false;
        bytes += sent;
        size -= sent;
    }
    return size == 0;
}

bool send_frame(Task& task, std::uint8_t opcode, const void* data, std::size_t size) {
    std::vector<std::uint8_t> frame;
    frame.reserve(size + 14);
    frame.push_back(static_cast<std::uint8_t>(0x80 | opcode));
    if (size < 126) {
        frame.push_back(static_cast<std::uint8_t>(0x80 | size));
    } else if (size <= 0xffff) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<std::uint8_t>(size >> 8));
        frame.push_back(static_cast<std::uint8_t>(size));
    } else {
        frame.push_back(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(size) >> shift));
    }
    std::uint64_t seed = static_cast<std::uint64_t>(time(nullptr)) ^ static_cast<std::uint64_t>(getpid());
    std::uint8_t mask[4];
    for (std::uint8_t& byte : mask) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        byte = static_cast<std::uint8_t>(seed);
    }
    frame.insert(frame.end(), mask, mask + 4);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i)
        frame.push_back(bytes[i] ^ mask[i & 3]);
    return send_all(task, frame.data(), frame.size());
}

void feed(Task& task, const std::uint8_t* data, std::size_t size) {
    task.input.insert(task.input.end(), data, data + size);
    std::size_t offset = 0;
    while (task.input.size() - offset >= 2) {
        const std::uint8_t first = task.input[offset];
        const std::uint8_t second = task.input[offset + 1];
        const bool fin = (first & 0x80) != 0;
        const std::uint8_t opcode = first & 0x0f;
        const bool masked = (second & 0x80) != 0;
        std::uint64_t length = second & 0x7f;
        std::size_t header = 2;
        if (length == 126) {
            if (task.input.size() - offset < 4) return;
            length = (static_cast<std::uint64_t>(task.input[offset + 2]) << 8) |
                     task.input[offset + 3];
            header = 4;
        } else if (length == 127) {
            if (task.input.size() - offset < 10) return;
            length = 0;
            for (int i = 0; i < 8; ++i)
                length = (length << 8) | task.input[offset + 2 + i];
            header = 10;
        }
        if (length > 4 * 1024 * 1024) {
            fail(task, "WebSocket frame is larger than 4 MiB");
            task.state = State::failed;
            return;
        }
        const std::size_t mask_bytes = masked ? 4 : 0;
        if (task.input.size() - offset < header + mask_bytes + length) return;
        std::vector<std::uint8_t> payload(length);
        const std::uint8_t* source = task.input.data() + offset + header + mask_bytes;
        if (masked) {
            for (std::size_t i = 0; i < length; ++i)
                payload[i] = source[i] ^ task.input[offset + header + (i & 3)];
        } else {
            std::memcpy(payload.data(), source, length);
        }
        offset += header + mask_bytes + length;

        if (opcode == 0x8) {
            (void)send_frame(task, 0x8, payload.data(), payload.size());
            task.state = State::closed;
            event(task, "close");
            task.cancelled = true;
            return;
        }
        if (opcode == 0x9) {
            (void)send_frame(task, 0xA, payload.data(), payload.size());
            continue;
        }
        if (opcode == 0xA) continue;
        if (opcode == 0x1 || opcode == 0x2) {
            task.message_opcode = opcode;
            task.message = std::move(payload);
        } else if (opcode == 0x0) {
            task.message.insert(task.message.end(), payload.begin(), payload.end());
        }
        if (fin && (task.message_opcode == 0x1 || task.message_opcode == 0x2)) {
            if (task.message_opcode == 0x1) {
                event(task, "message",
                      std::string(reinterpret_cast<const char*>(task.message.data()),
                                  task.message.size()));
            } else {
                event(task, "message", base64(task.message.data(), task.message.size()), true);
            }
            task.message.clear();
            task.message_opcode = 0;
        }
    }
    if (offset) task.input.erase(task.input.begin(), task.input.begin() + offset);
}

bool wait_readable(curl_socket_t socket, int timeout_ms, std::atomic<bool>& cancelled) {
    if (cancelled) return false;
    fd_set reads;
    FD_ZERO(&reads);
    FD_SET(socket, &reads);
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int result = select(socket + 1, &reads, nullptr, nullptr, &timeout);
    return result > 0;
}

void run(Task& task) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        fail(task, "curl_easy_init failed");
        return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, task.origin.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/app0/ca-bundle.crt");
    const CURLcode connected = curl_easy_perform(curl);
    if (connected != CURLE_OK) {
        fail(task, curl_easy_strerror(connected));
        curl_easy_cleanup(curl);
        return;
    }
    curl_socket_t socket = CURL_SOCKET_BAD;
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket);
    if (socket == CURL_SOCKET_BAD) {
        fail(task, "no active socket");
        curl_easy_cleanup(curl);
        return;
    }
    task.curl = curl;
    task.socket = socket;

    std::uint8_t random[16];
    std::uint64_t seed = static_cast<std::uint64_t>(time(nullptr)) ^ static_cast<std::uint64_t>(getpid());
    for (std::uint8_t& byte : random) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        byte = static_cast<std::uint8_t>(seed);
    }
    const std::string key = base64(random, sizeof random);
    const std::string request =
        "GET " + task.path + " HTTP/1.1\r\n" +
        "Host: " + task.host + "\r\n" +
        "Upgrade: websocket\r\n" +
        "Connection: Upgrade\r\n" +
        "Sec-WebSocket-Key: " + key + "\r\n" +
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!send_all(task, request.data(), request.size())) {
        fail(task, "cannot send the WebSocket handshake");
        curl_easy_cleanup(curl);
        return;
    }

    std::string response;
    std::uint8_t buffer[16 * 1024];
    while (!task.cancelled && response.find("\r\n\r\n") == std::string::npos) {
        if (response.size() > 16 * 1024) {
            fail(task, "WebSocket handshake is too large");
            curl_easy_cleanup(curl);
            return;
        }
        if (!wait_readable(socket, 1000, task.cancelled)) {
            if (task.cancelled) break;
            continue;
        }
        std::size_t received = 0;
        CURLcode result;
        {
            std::lock_guard<std::mutex> io(task.io_mutex);
            result = curl_easy_recv(curl, buffer, sizeof buffer, &received);
        }
        if (result == CURLE_AGAIN) continue;
        if (result != CURLE_OK || received == 0) {
            if (!task.cancelled) fail(task, "WebSocket handshake closed");
            break;
        }
        response.append(reinterpret_cast<const char*>(buffer), received);
    }
    if (task.cancelled || response.find(" 101 ") == std::string::npos) {
        if (!task.cancelled && task.state != State::failed)
            fail(task, "WebSocket upgrade was rejected");
        curl_easy_cleanup(curl);
        return;
    }
    const std::size_t header_end = response.find("\r\n\r\n") + 4;
    if (header_end < response.size())
        feed(task, reinterpret_cast<const std::uint8_t*>(response.data()) + header_end,
             response.size() - header_end);

    task.state = State::open;
    event(task, "open");
    ps4::system::log("websocket: open %s", task.url.c_str());

    while (!task.cancelled && task.state == State::open) {
        if (!wait_readable(socket, 1000, task.cancelled))
            continue;
        std::size_t received = 0;
        CURLcode result;
        {
            std::lock_guard<std::mutex> io(task.io_mutex);
            result = curl_easy_recv(curl, buffer, sizeof buffer, &received);
        }
        if (result == CURLE_AGAIN) continue;
        if (result != CURLE_OK) {
            fail(task, curl_easy_strerror(result));
            break;
        }
        if (received == 0) {
            task.state = State::closed;
            event(task, "close");
            break;
        }
        feed(task, buffer, received);
    }
    if (task.state == State::open && task.cancelled) {
        task.state = State::closed;
        event(task, "close");
    }
    task.cancelled = true;
    curl_easy_cleanup(curl);
    task.curl = nullptr;
    ps4::system::log("websocket: closed %s", task.url.c_str());
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
        fail(*task, "cannot start the WebSocket thread");
    }
}

JSValue connect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "WebSocket.connect: expected a ws(s) URL");
    std::size_t length = 0;
    const char* url = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!url) return JS_EXCEPTION;
    std::string address(url, length);
    JS_FreeCString(ctx, url);
    auto task = std::make_shared<Task>();
    task->id = next_id++;
    if (length > 2048 || !parse_url(address, *task))
        return JS_ThrowTypeError(ctx, "WebSocket.connect: URL must start with ws:// or wss://");
    task->url = std::move(address);
    {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        tasks.push_back(task);
    }
    start(task);
    return JS_NewUint32(ctx, task->id);
}

JSValue send(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (argc < 2 || JS_ToUint32(ctx, &id, argv[0]) || !JS_IsString(argv[1]))
        return JS_ThrowTypeError(ctx, "WebSocket.send: expected id and text");
    std::size_t length = 0;
    const char* text = JS_ToCStringLen(ctx, &length, argv[1]);
    if (!text) return JS_EXCEPTION;
    std::shared_ptr<Task> target;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        for (const auto& task : tasks) {
            if (task->id == id) {
                target = task;
                break;
            }
        }
    }
    bool ok = false;
    if (target && target->state == State::open)
        ok = send_frame(*target, 0x1, text, length);
    JS_FreeCString(ctx, text);
    return JS_NewBool(ctx, ok);
}

JSValue poll(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "WebSocket.poll: expected a task id");
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, "closed"));
    JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, "unknown WebSocket task"));
    JS_SetPropertyStr(ctx, out, "events", JS_NewArray(ctx));
    std::lock_guard<std::mutex> lock(tasks_mutex);
    for (auto it = tasks.begin(); it != tasks.end(); ++it) {
        const auto& task = *it;
        if (task->id != id) continue;
        const State state = task->state;
        if (task->reported != state) {
            task->reported = state;
            ps4::system::log("websocket: poll %u state=%s", task->id, state_name(state));
        }
        JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, state_name(state)));
        {
            std::lock_guard<std::mutex> event_lock(task->event_mutex);
            JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, task->error.c_str()));
            JSValue events = JS_NewArray(ctx);
            std::uint32_t index = 0;
            for (const Event& item : task->events) {
                JSValue value = JS_NewObject(ctx);
                JS_SetPropertyStr(ctx, value, "type", JS_NewString(ctx, item.type.c_str()));
                JS_SetPropertyStr(ctx, value, "data", JS_NewString(ctx, item.data.c_str()));
                JS_SetPropertyStr(ctx, value, "binary", JS_NewBool(ctx, item.binary));
                JS_SetPropertyUint32(ctx, events, index++, value);
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
        return JS_ThrowTypeError(ctx, "WebSocket.close: expected a task id");
    std::shared_ptr<Task> target;
    {
        std::lock_guard<std::mutex> lock(tasks_mutex);
        for (const auto& task : tasks) {
            if (task->id == id) {
                target = task;
                break;
            }
        }
    }
    if (target) {
        target->cancelled = true;
        if (target->socket != CURL_SOCKET_BAD)
            shutdown(target->socket, SHUT_RDWR);
    }
    return JS_UNDEFINED;
}

} // namespace

JSValue ps4_react_websocket_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "connect", JS_NewCFunction(ctx, connect, "connect", 1));
    JS_SetPropertyStr(ctx, api, "send", JS_NewCFunction(ctx, send, "send", 2));
    JS_SetPropertyStr(ctx, api, "poll", JS_NewCFunction(ctx, poll, "poll", 1));
    JS_SetPropertyStr(ctx, api, "close", JS_NewCFunction(ctx, close, "close", 1));
    return api;
}
