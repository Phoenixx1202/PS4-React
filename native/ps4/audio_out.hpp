// PS4 audio output thread through sceAudioOut.
//
// The mixer posts commands from the render thread and this thread renders
// 256-frame grains. sceAudioOutOutput blocks until the grain is consumed, which
// paces the loop at 48 kHz.

#pragma once

#include "audio/mixer.hpp"

#include <atomic>
#include <cstdint>
#include <pthread.h>

namespace ps4 {

class AudioOut {
  public:
    static constexpr int kGrainFrames = 256;

    AudioOut() = default;
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;
    ~AudioOut();

    bool start(hui::audio::Mixer& mixer);
    void stop();

    std::uint64_t grains() const {
        return grains_.load(std::memory_order_relaxed);
    }
    std::uint64_t errors() const {
        return errors_.load(std::memory_order_relaxed);
    }

  private:
    static void* thread_main(void* self);
    void run();

    hui::audio::Mixer* mixer_ = nullptr;
    int handle_ = -1;
    pthread_t thread_{};
    bool running_ = false;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> grains_{0};
    std::atomic<std::uint64_t> errors_{0};
};

} // namespace ps4
