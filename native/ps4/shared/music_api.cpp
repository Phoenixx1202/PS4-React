// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"

#include "audio/mixer.hpp"
#include "system.hpp"

#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

enum class State : int { playing, paused, completed, failed, stopped };

struct Task {
    std::uint32_t id = 0;
    std::string path;
    std::atomic<State> state{State::playing};
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    std::atomic<std::uint64_t> position{0};
    std::uint64_t duration = 0;
    std::mutex error_mutex;
    std::string error;
};

hui::audio::StreamRing deck(1u << 15);
hui::audio::Mixer* mixer = nullptr;
std::mutex task_mutex;
std::shared_ptr<Task> current;
std::atomic<std::uint32_t> next_id{1};

const char* state_name(State state) {
    switch (state) {
        case State::playing: return "playing";
        case State::paused: return "paused";
        case State::completed: return "completed";
        case State::failed: return "failed";
        default: return "stopped";
    }
}

void fail(Task& task, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(task.error_mutex);
        task.error = message;
    }
    task.state = State::failed;
    ps4::system::log("music: %s failed: %s", task.path.c_str(), message.c_str());
}

void run(Task& task) {
    mp3dec_t decoder{};
    mp3dec_file_info_t info{};
    const int opened = mp3dec_load(&decoder, task.path.c_str(), &info, nullptr, nullptr);
    if (opened != 0 || !info.buffer || info.channels < 1 || info.hz <= 0) {
        if (info.buffer) std::free(info.buffer);
        fail(task, "cannot decode the MP3");
        return;
    }
    const std::uint64_t frames = info.samples / static_cast<std::uint64_t>(info.channels);
    task.duration = info.hz > 0 ? frames / static_cast<std::uint64_t>(info.hz) : 0;
    const std::uint64_t output_frames =
        static_cast<std::uint64_t>(frames * 48000ull / static_cast<std::uint64_t>(info.hz));
    ps4::system::log("music: %s %d Hz %d ch, %llu frames", task.path.c_str(), info.hz,
                     info.channels, static_cast<unsigned long long>(frames));

    std::vector<float> buffer(2048 * 2);
    std::uint64_t written = 0;
    const double ratio = static_cast<double>(info.hz) / 48000.0;
    while (!task.stop && written < output_frames) {
        if (task.paused) {
            task.state = State::paused;
            ps4::system::sleep_us(20000);
            continue;
        }
        task.state = State::playing;
        const std::size_t frames_to_mix = std::min<std::uint64_t>(1024, output_frames - written);
        for (std::size_t i = 0; i < frames_to_mix; ++i) {
            const double source = static_cast<double>(written + i) * ratio;
            std::uint64_t index = static_cast<std::uint64_t>(source);
            if (index + 1 >= frames) index = frames > 0 ? frames - 1 : 0;
            const double fraction = source - static_cast<double>(index);
            const std::uint64_t next = index + 1 < frames ? index + 1 : index;
            const float left = static_cast<float>(info.buffer[index * info.channels]) / 32768.0f;
            const float right = info.channels > 1
                                    ? static_cast<float>(info.buffer[index * info.channels + 1]) / 32768.0f
                                    : left;
            const float next_left = static_cast<float>(info.buffer[next * info.channels]) / 32768.0f;
            const float next_right = info.channels > 1
                                         ? static_cast<float>(info.buffer[next * info.channels + 1]) / 32768.0f
                                         : next_left;
            buffer[i * 2] = left + (next_left - left) * static_cast<float>(fraction);
            buffer[i * 2 + 1] = right + (next_right - right) * static_cast<float>(fraction);
        }
        std::size_t offset = 0;
        while (offset < frames_to_mix && !task.stop) {
            const std::size_t count = deck.write(buffer.data() + offset * 2, frames_to_mix - offset);
            offset += count;
            written += count;
            task.position = written;
            if (count == 0) {
                if (task.paused) break;
                ps4::system::sleep_us(10000);
            }
        }
    }
    std::free(info.buffer);
    if (task.stop) {
        task.state = State::stopped;
        return;
    }
    task.state = State::completed;
    ps4::system::log("music: completed %s", task.path.c_str());
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
        pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
        started = pthread_create(&thread, &attributes, entry, argument) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (started) {
        pthread_detach(thread);
    } else {
        delete argument;
        fail(*task, "cannot start the music thread");
    }
}

JSValue play(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "Music.play: expected a path");
    std::size_t length = 0;
    const char* input = JS_ToCStringLen(ctx, &length, argv[0]);
    if (!input) return JS_EXCEPTION;
    char path[PATH_MAX];
    const bool allowed = length && length < PATH_MAX && !std::memchr(input, 0, length) &&
                         host::resolve_path(input, path, sizeof path);
    JS_FreeCString(ctx, input);
    if (!allowed) return JS_ThrowTypeError(ctx, "Music.play: path is not allowed");
    double volume = 1;
    if (argc > 1 && JS_ToFloat64(ctx, &volume, argv[1])) return JS_EXCEPTION;
    volume = std::clamp(volume, 0.0, 1.0);

    auto task = std::make_shared<Task>();
    task->id = next_id++;
    task->path = path;
    {
        std::lock_guard<std::mutex> lock(task_mutex);
        if (current) {
            current->stop = true;
            current->state = State::stopped;
        }
        current = task;
    }
    deck.drain();
    if (mixer) {
        mixer->set_stream_gain(0, 0, 0);
        mixer->set_stream_gain(0, static_cast<float>(volume), 0.2f);
    }
    start(task);
    return JS_NewUint32(ctx, task->id);
}

JSValue set_volume(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (!argc) return JS_ThrowTypeError(ctx, "Music.setVolume: expected a volume");
    double volume = 1;
    if (JS_ToFloat64(ctx, &volume, argv[0])) return JS_EXCEPTION;
    if (mixer) mixer->set_stream_gain(0, static_cast<float>(std::clamp(volume, 0.0, 1.0)), 0.1f);
    return JS_UNDEFINED;
}

JSValue pause(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Music.pause: expected a task id");
    std::lock_guard<std::mutex> lock(task_mutex);
    if (current && current->id == id) {
        current->paused = true;
        if (mixer) mixer->set_stream_gain(0, 0, 0.1f);
    }
    return JS_UNDEFINED;
}

JSValue resume(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Music.resume: expected a task id");
    std::lock_guard<std::mutex> lock(task_mutex);
    if (current && current->id == id) {
        current->paused = false;
        if (mixer) mixer->set_stream_gain(0, 1, 0.1f);
    }
    return JS_UNDEFINED;
}

JSValue stop(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Music.stop: expected a task id");
    std::lock_guard<std::mutex> lock(task_mutex);
    if (current && current->id == id) {
        current->stop = true;
        current->state = State::stopped;
        deck.drain();
        if (mixer) mixer->set_stream_gain(0, 0, 0);
    }
    return JS_UNDEFINED;
}

JSValue status(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::uint32_t id = 0;
    if (!argc || JS_ToUint32(ctx, &id, argv[0]))
        return JS_ThrowTypeError(ctx, "Music.status: expected a task id");
    JSValue out = JS_NewObject(ctx);
    std::lock_guard<std::mutex> lock(task_mutex);
    if (!current || current->id != id) {
        JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, "unknown"));
        JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, "unknown music task"));
        return out;
    }
    JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, state_name(current->state)));
    JS_SetPropertyStr(ctx, out, "position", JS_NewFloat64(ctx, current->position / 48000.0));
    JS_SetPropertyStr(ctx, out, "duration", JS_NewFloat64(ctx, current->duration));
    std::lock_guard<std::mutex> error_lock(current->error_mutex);
    JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, current->error.c_str()));
    return out;
}

} // namespace

void ps4_react_music_attach(hui::audio::Mixer& target) {
    mixer = &target;
    target.attach_stream(0, &deck);
    target.set_stream_gain(0, 0, 0);
}

JSValue ps4_react_music_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "play", JS_NewCFunction(ctx, play, "play", 2));
    JS_SetPropertyStr(ctx, api, "pause", JS_NewCFunction(ctx, pause, "pause", 1));
    JS_SetPropertyStr(ctx, api, "resume", JS_NewCFunction(ctx, resume, "resume", 1));
    JS_SetPropertyStr(ctx, api, "stop", JS_NewCFunction(ctx, stop, "stop", 1));
    JS_SetPropertyStr(ctx, api, "setVolume", JS_NewCFunction(ctx, set_volume, "setVolume", 1));
    JS_SetPropertyStr(ctx, api, "status", JS_NewCFunction(ctx, status, "status", 1));
    return api;
}
