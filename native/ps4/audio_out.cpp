// PS4 audio output thread through sceAudioOut. See audio_out.hpp.

#include "audio_out.hpp"

#include "system.hpp"

#include <orbis/AudioOut.h>
#include <orbis/_types/audio_out.h>
#include <orbis/_types/user.h>

namespace ps4 {
namespace {

constexpr int kAlreadyInitialized = static_cast<int>(0x8026000e);

} // namespace

AudioOut::~AudioOut() {
    stop();
}

bool AudioOut::start(hui::audio::Mixer& mixer) {
    const int init = sceAudioOutInit();
    if (init != 0 && init != kAlreadyInitialized) {
        system::log("audio: sceAudioOutInit=0x%08x", static_cast<unsigned>(init));
        return false;
    }

    handle_ = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM,
                              ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, kGrainFrames,
                              hui::audio::kSampleRate,
                              ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (handle_ < 0) {
        system::log("audio: sceAudioOutOpen=0x%08x", static_cast<unsigned>(handle_));
        handle_ = -1;
        return false;
    }

    mixer_ = &mixer;
    stop_.store(false);
    // Never name threads: pthread_setname_np hangs on PS4.
    if (pthread_create(&thread_, nullptr, &AudioOut::thread_main, this) != 0) {
        system::log("audio: thread creation failed");
        sceAudioOutClose(handle_);
        handle_ = -1;
        return false;
    }
    running_ = true;
    system::log("audio: open handle=%d grain=%d rate=%d", handle_, kGrainFrames,
                hui::audio::kSampleRate);
    return true;
}

void* AudioOut::thread_main(void* self) {
    static_cast<AudioOut*>(self)->run();
    return nullptr;
}

void AudioOut::run() {
    alignas(64) std::int16_t grain[kGrainFrames * 2];
    while (!stop_.load(std::memory_order_relaxed)) {
        mixer_->render(grain, kGrainFrames);
        if (sceAudioOutOutput(handle_, grain) < 0) {
            errors_.fetch_add(1, std::memory_order_relaxed);
            system::sleep_us(5000);
            continue;
        }
        grains_.fetch_add(1, std::memory_order_relaxed);
    }
    sceAudioOutOutput(handle_, nullptr); // drain the queued grain
}

void AudioOut::stop() {
    if (running_) {
        stop_.store(true);
        pthread_join(thread_, nullptr);
        running_ = false;
    }
    if (handle_ >= 0)
        sceAudioOutClose(handle_);
    handle_ = -1;
}

} // namespace ps4
