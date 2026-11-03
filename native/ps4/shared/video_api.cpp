// sceAvPlayer video playback for the PS4 runtime.
//
// The player is loaded dynamically so the app has no link-time import from a
// sysmodule that may not be loaded yet. The base AvPlayer API delivers NV12
// frames; this file first brings up the lifecycle and frame polling, and blits
// decoded frames into the engine framebuffer in the next stage.

#include "host_api.hpp"

#include "audio/mixer.hpp"
#include "system.hpp"

#include <orbis/Sysmodule.h>
#include <orbis/libkernel.h>

#include <sys/types.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "software_backend.h"
}

void start_audio_thread();
void stop_audio_thread();

namespace {

struct AvPlayerVideo {
    std::uint32_t width;
    std::uint32_t height;
    float aspect_ratio;
    char language_code[4];
};

struct AvPlayerAudio {
    std::uint16_t channel_count;
    std::uint8_t reserved[2];
    std::uint32_t sample_rate;
    std::uint32_t size;
    char language_code[4];
};

struct AvPlayerStreamDetails {
    union {
        std::uint8_t reserved[16];
        AvPlayerVideo video;
        AvPlayerAudio audio;
    };
};

struct AvPlayerFrameInfo {
    std::uint8_t* p_data;
    std::uint8_t reserved[4];
    std::uint64_t timestamp;
    AvPlayerStreamDetails details;
};

struct AvPlayerStreamInfo {
    std::uint32_t type;
    std::uint8_t reserved[4];
    AvPlayerStreamDetails details;
    std::uint64_t duration;
    std::uint64_t start_time;
};

struct AvPlayerVideoEx {
    std::uint32_t width;
    std::uint32_t height;
    float aspect_ratio;
    char language_code[4];
    std::uint32_t framerate;
    std::uint32_t crop_left_offset;
    std::uint32_t crop_right_offset;
    std::uint32_t crop_top_offset;
    std::uint32_t crop_bottom_offset;
    std::uint32_t pitch;
    std::uint8_t luma_bit_depth;
    std::uint8_t chroma_bit_depth;
    bool video_full_range_flag;
    std::uint8_t reserved[37];
};

struct AvPlayerStreamDetailsEx {
    union {
        AvPlayerVideoEx video;
        std::uint8_t reserved[80];
    };
};

struct AvPlayerFrameInfoEx {
    void* p_data;
    std::uint8_t reserved[4];
    std::uint64_t timestamp;
    AvPlayerStreamDetailsEx details;
};

static_assert(sizeof(AvPlayerVideoEx) == 80, "AvPlayerVideoEx ABI");
static_assert(sizeof(AvPlayerFrameInfoEx) == 104, "AvPlayerFrameInfoEx ABI");

struct AvPlayerDecoderInit {
    union {
        std::uint32_t video_type;
        std::uint32_t audio_type;
        std::uint8_t reserved[4];
    } decoder_type;
    union {
        struct {
            std::int32_t cpu_affinity_mask;
            std::int32_t cpu_thread_priority;
            std::uint8_t decode_pipeline_depth;
            std::uint8_t compute_pipe_id;
            std::uint8_t compute_queue_id;
            std::uint8_t enable_interlaced;
            std::uint8_t reserved[16];
        } avc_sw2;
        struct {
            std::uint8_t audio_channel_order;
            std::uint8_t reserved[27];
        } aac;
        std::uint8_t reserved[28];
    } decoder_params;
};

struct AvPlayerHttpContext {
    std::uint32_t http_context_id;
    std::uint32_t ssl_context_id;
};

struct AvPlayerPostInitData {
    std::uint32_t demux_video_buffer_size;
    AvPlayerDecoderInit video_decoder_init;
    AvPlayerDecoderInit audio_decoder_init;
    AvPlayerHttpContext http_context;
    std::uint8_t reserved[56];
};

static_assert(sizeof(AvPlayerPostInitData) == 132, "AvPlayerPostInitData ABI");

using AvPlayerAllocate = void* (*)(void* object, std::uint32_t align, std::uint32_t size);
using AvPlayerDeallocate = void (*)(void* object, void* memory);
using AvPlayerOpenFile = int (*)(void* object, const char* name);
using AvPlayerCloseFile = int (*)(void* object);
using AvPlayerReadOffsetFile = int (*)(void* object, std::uint8_t* buffer, std::uint64_t position,
                                        std::uint32_t length);
using AvPlayerSizeFile = std::uint64_t (*)(void* object);
using AvPlayerEventCallback = void (*)(void* object, int event, int source_id, void* data);

struct AvPlayerMemAllocator {
    void* object_ptr;
    AvPlayerAllocate allocate;
    AvPlayerDeallocate deallocate;
    AvPlayerAllocate allocate_texture;
    AvPlayerDeallocate deallocate_texture;
};

struct AvPlayerFileReplacement {
    void* object_ptr;
    AvPlayerOpenFile open;
    AvPlayerCloseFile close;
    AvPlayerReadOffsetFile read_offset;
    AvPlayerSizeFile size;
};

struct AvPlayerEventReplacement {
    void* object_ptr;
    AvPlayerEventCallback event_callback;
};

struct AvPlayerInitData {
    AvPlayerMemAllocator memory_replacement;
    AvPlayerFileReplacement file_replacement;
    AvPlayerEventReplacement event_replacement;
    std::uint32_t debug_level;
    std::uint32_t base_priority;
    std::int32_t num_output_video_framebuffers;
    bool auto_start;
    std::uint8_t reserved[3];
    const char* default_language;
};

static_assert(sizeof(AvPlayerMemAllocator) == 40, "AvPlayerMemAllocator ABI");
static_assert(sizeof(AvPlayerInitData) == 120, "AvPlayerInitData ABI");
static_assert(offsetof(AvPlayerInitData, debug_level) == 96, "AvPlayerInitData debug offset");
static_assert(offsetof(AvPlayerInitData, num_output_video_framebuffers) == 104,
              "AvPlayerInitData framebuffer offset");
static_assert(offsetof(AvPlayerInitData, auto_start) == 108, "AvPlayerInitData auto_start offset");
static_assert(offsetof(AvPlayerInitData, default_language) == 112,
              "AvPlayerInitData language offset");

using InitFn = void* (*)(AvPlayerInitData*);
using PostInitFn = int (*)(void*, AvPlayerPostInitData*);
using AddSourceFn = int (*)(void*, const char*);
using StartFn = int (*)(void*);
using StopFn = int (*)(void*);
using CloseFn = int (*)(void*);
using GetVideoDataFn = bool (*)(void*, AvPlayerFrameInfo*);
using GetVideoDataExFn = bool (*)(void*, AvPlayerFrameInfoEx*);
using GetAudioDataFn = bool (*)(void*, AvPlayerFrameInfo*);
using IsActiveFn = bool (*)(void*);
using PauseFn = int (*)(void*);
using ResumeFn = int (*)(void*);
using JumpFn = int (*)(void*, std::uint64_t);
using CurrentTimeFn = std::uint64_t (*)(void*);
using StreamCountFn = int (*)(void*);
using GetStreamInfoFn = int (*)(void*, std::uint32_t, AvPlayerStreamInfo*);
using EnableStreamFn = int (*)(void*, std::uint32_t);
using LogCallback = int (*)(void*, const char*, va_list);
using SetLogCallbackFn = int (*)(LogCallback, void*);

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

int module_ = -1;
int gnm_module_ = -1;
void* player_ = nullptr;
InitFn init_ = nullptr;
PostInitFn post_init_ = nullptr;
AddSourceFn add_source_ = nullptr;
StartFn start_ = nullptr;
StopFn stop_ = nullptr;
CloseFn close_ = nullptr;
GetVideoDataFn get_video_ = nullptr;
GetVideoDataExFn get_video_ex_ = nullptr;
GetAudioDataFn get_audio_ = nullptr;
IsActiveFn is_active_ = nullptr;
PauseFn pause_ = nullptr;
ResumeFn resume_ = nullptr;
JumpFn jump_ = nullptr;
CurrentTimeFn current_time_ = nullptr;
StreamCountFn stream_count_ = nullptr;
GetStreamInfoFn get_stream_info_ = nullptr;
EnableStreamFn enable_stream_ = nullptr;
SetLogCallbackFn set_log_callback_ = nullptr;

int avplayer_log(void*, const char* format, va_list args) {
    char buffer[1024] = {};
    std::vsnprintf(buffer, sizeof buffer, format, args);
    ps4::system::log("avplayer: %s", buffer);
    return 0;
}

std::string path_;
std::string error_;
bool paused_ = false;
std::uint64_t frames_ = 0;
std::uint64_t last_timestamp_ = 0;
std::uint64_t duration_ms_ = 0;
std::uint32_t last_width_ = 0;
std::uint32_t last_height_ = 0;
bool start_attempted_ = false;
std::uint64_t start_wait_us_ = 0;
bool video_used_ = false;
Rect rect_{0, 0, 1920, 1080};
std::atomic<int> event_code_{-1};
std::atomic<bool> ready_event_{false};
std::atomic<int> allocation_logs_{0};
std::atomic<int> audio_logs_{0};

hui::audio::StreamRing video_audio_deck(1u << 15);
hui::audio::Mixer* video_mixer = nullptr;
std::vector<std::uint8_t> audio_staging;
std::vector<float> audio_resampled;
std::atomic<bool> audio_thread_running_{false};
pthread_t audio_thread_{};
bool audio_thread_started_ = false;

void event_callback(void*, int event, int, void*) {
    // Called by AvPlayer's controller thread. Only publish state here; the
    // render thread does the logging and calls Start/EnableStream.
    event_code_.store(event);
    if (event == 0x02) ready_event_.store(true);
}

struct DirectBlock {
    void* address;
    off_t physical;
    std::size_t length;
};

std::mutex direct_mutex;
std::vector<DirectBlock> direct_blocks;

std::size_t next_power_of_two(std::size_t value) {
    std::size_t result = 1;
    while (result < value) result <<= 1;
    return result;
}

void* direct_allocate(std::uint32_t align, std::uint32_t size, int memory_type,
                      const char* kind) {
    constexpr std::size_t page = 0x4000;
    const std::size_t length = (static_cast<std::size_t>(size ? size : 1) + page - 1) & ~(page - 1);
    std::size_t alignment = next_power_of_two(std::max<std::size_t>(align, page));
    if (alignment < page) alignment = page;

    off_t physical = 0;
    int result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), length,
                                               alignment, memory_type, &physical);
    void* address = nullptr;
    if (result >= 0) {
        result = sceKernelMapDirectMemory(&address, length, 0x33, 0, physical, alignment);
        if (result < 0) {
            sceKernelReleaseDirectMemory(physical, length);
            address = nullptr;
        }
    }
    const int index = allocation_logs_.fetch_add(1);
    if (!address || index < 12) {
        ps4::system::log("video: alloc %s align=%u size=%u type=%d rc=0x%08x -> %p",
                         kind, static_cast<unsigned>(alignment), static_cast<unsigned>(length),
                         memory_type, static_cast<unsigned>(result), address);
    }
    if (!address) return nullptr;

    {
        std::lock_guard<std::mutex> lock(direct_mutex);
        direct_blocks.push_back({address, physical, length});
    }
    return address;
}

std::size_t release_all_direct_blocks() {
    std::lock_guard<std::mutex> lock(direct_mutex);
    const std::size_t count = direct_blocks.size();
    for (const DirectBlock& block : direct_blocks) {
        sceKernelMunmap(block.address, block.length);
        sceKernelReleaseDirectMemory(block.physical, block.length);
    }
    direct_blocks.clear();
    return count;
}

void direct_release(void* address) {
    if (!address) return;
    std::lock_guard<std::mutex> lock(direct_mutex);
    for (auto it = direct_blocks.begin(); it != direct_blocks.end(); ++it) {
        if (it->address == address) {
            sceKernelMunmap(it->address, it->length);
            sceKernelReleaseDirectMemory(it->physical, it->length);
            direct_blocks.erase(it);
            return;
        }
    }
    ps4::system::log("video: dealloc of unknown direct block %p", address);
}

void* allocate(void*, std::uint32_t align, std::uint32_t size) {
    return direct_allocate(align, size, ORBIS_KERNEL_WB_ONION, "cpu");
}

void deallocate(void*, void* memory) {
    direct_release(memory);
}

void* allocate_texture(void*, std::uint32_t align, std::uint32_t size) {
    // The AVC decoder asks the texture allocator for two different pools:
    // small frame surfaces (WC_GARLIC) and a large pCpuGpuMemory workspace that
    // it validates as WB_ONION. The callback has no usage tag, so separate them
    // by size. 1080p frame sets stay well below 16 MiB; the workspace is ~32 MiB.
    constexpr std::uint32_t kWorkspaceThreshold = 16u * 1024u * 1024u;
    const int type = size >= kWorkspaceThreshold ? ORBIS_KERNEL_WB_ONION : ORBIS_KERNEL_WC_GARLIC;
    return direct_allocate(align, size, type, size >= kWorkspaceThreshold ? "gpu-cpu" : "texture");
}

void deallocate_texture(void*, void* memory) {
    direct_release(memory);
}

bool load_player() {
    if (player_) return true;
    if (module_ < 0) {
        char prefix[256] = {};
        const char* random = sceKernelGetFsSandboxRandomWord();
        if (!random || !*random) {
            error_ = "sandbox path unavailable";
            return false;
        }
        std::snprintf(prefix, sizeof prefix, "/%s/common/lib/", random);

        // libSceAvPlayer's decoder Start path resolves against libSceGnmDriver
        // even when PostInit selects the software decoder. Load it before
        // AvPlayer so GoldHEN does not trap the unresolved import.
        {
            const std::string gnm = std::string(prefix) + "libSceGnmDriver.sprx";
            int gnm_start = 0;
            gnm_module_ = static_cast<int>(
                sceKernelLoadStartModule(gnm.c_str(), 0, nullptr, 0, nullptr, &gnm_start));
            ps4::system::log("video: preload libSceGnmDriver handle=%d start=0x%08x",
                             gnm_module_, static_cast<unsigned>(gnm_start));
        }

        const std::string path = std::string(prefix) + "libSceAvPlayer.sprx";
        int start = 0;
        module_ = static_cast<int>(
            sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &start));
        ps4::system::log("video: module=%d start=0x%08x", module_,
                         static_cast<unsigned>(start));
        if (module_ < 0) {
            error_ = "cannot load libSceAvPlayer";
            return false;
        }
    }

    struct Symbol {
        const char* name;
        void** target;
    };
    const Symbol symbols[] = {
        {"sceAvPlayerInit", reinterpret_cast<void**>(&init_)},
        {"sceAvPlayerPostInit", reinterpret_cast<void**>(&post_init_)},
        {"sceAvPlayerAddSource", reinterpret_cast<void**>(&add_source_)},
        {"sceAvPlayerStart", reinterpret_cast<void**>(&start_)},
        {"sceAvPlayerStop", reinterpret_cast<void**>(&stop_)},
        {"sceAvPlayerClose", reinterpret_cast<void**>(&close_)},
        {"sceAvPlayerGetVideoData", reinterpret_cast<void**>(&get_video_)},
        {"sceAvPlayerGetVideoDataEx", reinterpret_cast<void**>(&get_video_ex_)},
        {"sceAvPlayerGetAudioData", reinterpret_cast<void**>(&get_audio_)},
        {"sceAvPlayerIsActive", reinterpret_cast<void**>(&is_active_)},
        {"sceAvPlayerPause", reinterpret_cast<void**>(&pause_)},
        {"sceAvPlayerResume", reinterpret_cast<void**>(&resume_)},
        {"sceAvPlayerJumpToTime", reinterpret_cast<void**>(&jump_)},
        {"sceAvPlayerCurrentTime", reinterpret_cast<void**>(&current_time_)},
        {"sceAvPlayerStreamCount", reinterpret_cast<void**>(&stream_count_)},
        {"sceAvPlayerGetStreamInfo", reinterpret_cast<void**>(&get_stream_info_)},
        {"sceAvPlayerEnableStream", reinterpret_cast<void**>(&enable_stream_)},
        {"sceAvPlayerSetLogCallback", reinterpret_cast<void**>(&set_log_callback_)},
    };
    for (const Symbol& symbol : symbols) {
        if (sceKernelDlsym(module_, symbol.name, symbol.target) != 0 || !*symbol.target) {
            ps4::system::log("video: missing symbol %s", symbol.name);
            error_ = std::string("missing symbol ") + symbol.name;
            return false;
        }
    }
    return true;
}

void reset_state() {
    player_ = nullptr;
    paused_ = false;
    frames_ = 0;
    last_timestamp_ = 0;
    duration_ms_ = 0;
    last_width_ = 0;
    last_height_ = 0;
    start_attempted_ = false;
    start_wait_us_ = 0;
    event_code_.store(-1);
    ready_event_.store(false);
    path_.clear();
    error_.clear();
}

void close_player() {
    if (player_) {
        stop_audio_thread();
        if (stop_) stop_(player_);
        // The decoder and demuxer run on AvPlayer threads. Give Stop time to
        // shut them down before Close frees the player they reference.
        for (int i = 0; i < 30 && is_active_ && is_active_(player_); ++i)
            ps4::system::sleep_us(10000);
        if (close_) close_(player_);
        player_ = nullptr;
        video_audio_deck.drain();
        if (video_mixer) video_mixer->set_stream_gain(1, 0, 0.05f);
        ps4::system::log("video: closed");
    }
    reset_state();
}

} // namespace

bool ps4_react_video_play(const char* path, int x, int y, int width, int height) {
    close_player();
    if (!path || !*path) {
        error_ = "empty video path";
        return false;
    }
    if (!load_player()) return false;

    if (set_log_callback_) {
        const int logged = set_log_callback_(avplayer_log, nullptr);
        ps4::system::log("video: log callback rc=0x%08x", static_cast<unsigned>(logged));
    }

    AvPlayerInitData data{};
    data.memory_replacement = {
        nullptr, allocate, deallocate, allocate_texture, deallocate_texture,
    };
    data.event_replacement = {nullptr, event_callback};
    data.debug_level = 2;
    data.base_priority = 0;
    data.num_output_video_framebuffers = 3;
    data.auto_start = false;
    data.default_language = "en";

    player_ = init_(&data);
    ps4::system::log("video: init=%p", player_);
    if (!player_) {
        error_ = "sceAvPlayerInit failed";
        return false;
    }

    if (post_init_) {
        AvPlayerPostInitData post{};
        // The hardware AVC decoder leaves a GPU submit-done pending that makes
        // the system suspend time out on app close. Use the CPU decoder for now;
        // it keeps suspend/close clean and is fast enough for SD content.
        post.video_decoder_init.decoder_type.video_type = 2;  // Software
        const int configured = post_init_(player_, &post);
        ps4::system::log("video: post_init software rc=0x%08x",
                         static_cast<unsigned>(configured));
    }

    const int added = add_source_(player_, path);
    ps4::system::log("video: add %s rc=0x%08x", path, static_cast<unsigned>(added));
    if (added < 0) {
        close_player();
        error_ = "sceAvPlayerAddSource failed";
        return false;
    }

    // AddSource is asynchronous; with auto_start the player begins on its own
    // once the demuxer is ready. If no frame arrives shortly, try Start once.
    start_attempted_ = false;
    start_wait_us_ = ps4::system::monotonic_us();
    start_audio_thread();
    video_used_ = true;
    video_audio_deck.drain();
    if (video_mixer) video_mixer->set_stream_gain(1, 1, 0.05f);
    path_ = path;
    rect_ = {x, y, width, height};
    paused_ = false;
    error_.clear();
    ps4::system::log("video: playing %s at %d,%d %dx%d", path_.c_str(), rect_.x, rect_.y,
                     rect_.width, rect_.height);
    return true;
}

std::vector<std::uint8_t> frame_copy;

const std::uint8_t* stage_frame(const void* data, int pitch, int height) {
    if (!data || pitch <= 0 || height <= 0) return nullptr;
    const std::size_t bytes = static_cast<std::size_t>(pitch) * height * 3 / 2;
    frame_copy.resize(bytes);
    std::memcpy(frame_copy.data(), data, bytes);
    return frame_copy.data();
}

void blit_nv12(const std::uint8_t* data, int source_width, int source_height, int pitch,
               int visible_width, int visible_height, int crop_left, int crop_top) {
    if (!data || source_width <= 0 || source_height <= 0 || pitch <= 0) return;
    if (visible_width <= 0 || visible_height <= 0) return;
    const int fb_width = er_software_fb_width();
    const int fb_height = er_software_fb_height();
    std::uint32_t* framebuffer = er_software_framebuffer();
    if (!framebuffer || fb_width <= 0 || fb_height <= 0) return;

    const int rect_x = std::clamp(rect_.x, 0, fb_width);
    const int rect_y = std::clamp(rect_.y, 0, fb_height);
    const int rect_w = std::clamp(rect_.width, 0, fb_width - rect_x);
    const int rect_h = std::clamp(rect_.height, 0, fb_height - rect_y);
    if (rect_w <= 0 || rect_h <= 0) return;

    const double aspect = static_cast<double>(visible_width) / visible_height;
    int draw_width = rect_w;
    int draw_height = static_cast<int>(draw_width / aspect + 0.5);
    if (draw_height > rect_h) {
        draw_height = rect_h;
        draw_width = static_cast<int>(draw_height * aspect + 0.5);
    }
    draw_width = std::clamp(draw_width, 1, rect_w);
    draw_height = std::clamp(draw_height, 1, rect_h);
    const int draw_x = rect_x + (rect_w - draw_width) / 2;
    const int draw_y = rect_y + (rect_h - draw_height) / 2;

    for (int row = 0; row < rect_h; ++row) {
        std::fill_n(framebuffer + static_cast<std::size_t>(rect_y + row) * fb_width + rect_x,
                    rect_w, 0xFF000000u);
    }

    static std::vector<int> xmap;
    static std::vector<int> ymap;
    xmap.resize(static_cast<std::size_t>(draw_width));
    ymap.resize(static_cast<std::size_t>(draw_height));
    for (int i = 0; i < draw_width; ++i)
        xmap[i] = static_cast<int>(static_cast<std::int64_t>(i) * visible_width / draw_width);
    for (int i = 0; i < draw_height; ++i)
        ymap[i] = static_cast<int>(static_cast<std::int64_t>(i) * visible_height / draw_height);

    const std::uint8_t* luma = data + static_cast<std::size_t>(crop_top) * pitch + crop_left;
    const std::uint8_t* chroma = data + static_cast<std::size_t>(pitch) * source_height +
                                 static_cast<std::size_t>(crop_top / 2) * pitch +
                                 (crop_left & ~1);
    for (int row = 0; row < draw_height; ++row) {
        const int source_y = ymap[row];
        const std::uint8_t* luma_row = luma + static_cast<std::size_t>(source_y) * pitch;
        const std::uint8_t* chroma_row = chroma + static_cast<std::size_t>(source_y / 2) * pitch;
        std::uint32_t* out = framebuffer + static_cast<std::size_t>(draw_y + row) * fb_width + draw_x;
        for (int column = 0; column < draw_width; ++column) {
            const int source_x = xmap[column];
            const int y = luma_row[source_x] - 16;
            const int u = chroma_row[source_x & ~1] - 128;
            const int v = chroma_row[(source_x & ~1) + 1] - 128;
            const int r = std::clamp((298 * y + 409 * v + 128) >> 8, 0, 255);
            const int g = std::clamp((298 * y - 100 * u - 208 * v + 128) >> 8, 0, 255);
            const int b = std::clamp((298 * y + 516 * u + 128) >> 8, 0, 255);
            out[column] = 0xFF000000u | (static_cast<std::uint32_t>(r) << 16) |
                          (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
        }
    }

    static bool logged = false;
    if (!logged) {
        ps4::system::log("video: blit %dx%d pitch=%d crop=%d,%d -> %dx%d at %d,%d", visible_width,
                         visible_height, pitch, crop_left, crop_top, draw_width, draw_height,
                         draw_x, draw_y);
        logged = true;
    }
}

void pump_audio(const AvPlayerFrameInfo& frame) {
    if (!frame.p_data) return;
    const AvPlayerAudio& audio = frame.details.audio;
    const int channels = audio.channel_count ? static_cast<int>(audio.channel_count) : 2;
    const int rate = audio.sample_rate ? static_cast<int>(audio.sample_rate) : 48000;
    const std::size_t bytes = audio.size;
    if (bytes == 0) return;

    const int index = audio_logs_.fetch_add(1);
    if (index < 6) {
        const auto* raw = static_cast<const std::uint8_t*>(frame.p_data);
        ps4::system::log("video: audio %d ch=%d rate=%d size=%zu ts=%llu bytes=%02x%02x%02x%02x",
                         index, channels, rate, bytes,
                         static_cast<unsigned long long>(frame.timestamp), raw[0], raw[1],
                         raw[2], raw[3]);
    }

    audio_staging.resize(bytes);
    std::memcpy(audio_staging.data(), frame.p_data, bytes);
    const auto* pcm = reinterpret_cast<const std::int16_t*>(audio_staging.data());
    const std::size_t input_frames = bytes / (sizeof(std::int16_t) * channels);
    if (input_frames == 0) return;
    const std::size_t output_frames =
        rate == 48000 ? input_frames
                      : static_cast<std::size_t>(static_cast<std::uint64_t>(input_frames) * 48000 /
                                                 static_cast<std::uint64_t>(rate));
    if (output_frames == 0) return;

    audio_resampled.resize(output_frames * 2);
    for (std::size_t i = 0; i < output_frames; ++i) {
        std::size_t source = rate == 48000
                                 ? i
                                 : static_cast<std::size_t>(static_cast<std::uint64_t>(i) * rate /
                                                            static_cast<std::uint64_t>(48000));
        if (source >= input_frames) source = input_frames - 1;
        const float left = static_cast<float>(pcm[source * channels]) / 32768.0f;
        const float right = channels > 1 ? static_cast<float>(pcm[source * channels + 1]) / 32768.0f
                                         : left;
        audio_resampled[i * 2] = left;
        audio_resampled[i * 2 + 1] = right;
    }

    std::size_t offset = 0;
    while (offset < output_frames) {
        const std::size_t written =
            video_audio_deck.write(audio_resampled.data() + offset * 2, output_frames - offset);
        if (written == 0) break;
        offset += written;
    }
}

void* audio_thread_main(void*) {
    while (audio_thread_running_.load(std::memory_order_relaxed)) {
        bool received = false;
        for (int i = 0; i < 8; ++i) {
            AvPlayerFrameInfo frame{};
            if (!player_ || !get_audio_ || !get_audio_(player_, &frame) || !frame.p_data) break;
            pump_audio(frame);
            received = true;
        }
        ps4::system::sleep_us(received ? 1000 : 5000);
    }
    return nullptr;
}

void start_audio_thread() {
    if (audio_thread_started_) return;
    audio_thread_running_.store(true);
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        audio_thread_running_.store(false);
        return;
    }
    pthread_attr_setstacksize(&attributes, 1024 * 1024);
    audio_thread_started_ =
        pthread_create(&audio_thread_, &attributes, audio_thread_main, nullptr) == 0;
    pthread_attr_destroy(&attributes);
    if (!audio_thread_started_) {
        audio_thread_running_.store(false);
        ps4::system::log("video: audio thread creation failed");
    }
}

void stop_audio_thread() {
    if (!audio_thread_started_) return;
    audio_thread_running_.store(false);
    pthread_join(audio_thread_, nullptr);
    audio_thread_started_ = false;
}

void ps4_react_video_pump() {
    if (!player_ || !get_video_) return;
    const int event = event_code_.exchange(-1);
    if (event >= 0) {
        ps4::system::log("video: event=0x%02x", static_cast<unsigned>(event));
    }
    if (!start_attempted_ && ready_event_.exchange(false)) {
        start_attempted_ = true;
        const int count = stream_count_ ? stream_count_(player_) : 0;
        ps4::system::log("video: ready streams=%d", count);
        for (int index = 0; index < count; ++index) {
            AvPlayerStreamInfo info{};
            const int rc =
                get_stream_info_ ? get_stream_info_(player_, static_cast<std::uint32_t>(index), &info)
                                 : -1;
            ps4::system::log("video: stream %d type=%u rc=0x%08x duration=%llu",
                             index, static_cast<unsigned>(info.type), static_cast<unsigned>(rc),
                             static_cast<unsigned long long>(info.duration));
            if (rc == 0 && info.duration > duration_ms_) duration_ms_ = info.duration;
            if (rc == 0 && enable_stream_) {
                const int enabled = enable_stream_(player_, static_cast<std::uint32_t>(index));
                ps4::system::log("video: enable %d rc=0x%08x", index,
                                 static_cast<unsigned>(enabled));
            }
        }
        if (start_) {
            const int started = start_(player_);
            ps4::system::log("video: start rc=0x%08x", static_cast<unsigned>(started));
        }
    }
    if (get_video_ex_) {
        AvPlayerFrameInfoEx frame{};
        if (!get_video_ex_(player_, &frame) || !frame.p_data) return;
        const AvPlayerVideoEx& video = frame.details.video;
        const int pitch = video.pitch ? static_cast<int>(video.pitch) : static_cast<int>(video.width);
        const int visible_width = static_cast<int>(video.width) -
                                  static_cast<int>(video.crop_left_offset) -
                                  static_cast<int>(video.crop_right_offset);
        const int visible_height = static_cast<int>(video.height) -
                                   static_cast<int>(video.crop_top_offset) -
                                   static_cast<int>(video.crop_bottom_offset);
        last_timestamp_ = frame.timestamp;
        if (frames_ < 8) {
            ps4::system::log("video: frame %llu %ux%u pitch=%u crop=%u,%u,%u,%u bd=%u/%u full=%d ts=%llu data=%p",
                             static_cast<unsigned long long>(frames_), video.width, video.height,
                             video.pitch, video.crop_left_offset, video.crop_right_offset,
                             video.crop_top_offset, video.crop_bottom_offset,
                             video.luma_bit_depth, video.chroma_bit_depth,
                             video.video_full_range_flag ? 1 : 0,
                             static_cast<unsigned long long>(frame.timestamp), frame.p_data);
        }
        ++frames_;
        if (visible_width > 0 && visible_height > 0 && video.luma_bit_depth == 8) {
            last_width_ = static_cast<std::uint32_t>(visible_width);
            last_height_ = static_cast<std::uint32_t>(visible_height);
            // The decoder writes into WC_GARLIC memory; per-pixel CPU reads from
            // it are extremely slow. Copy the surface once into cacheable memory
            // and convert from there.
            const std::uint8_t* staged =
                stage_frame(frame.p_data, pitch, static_cast<int>(video.height));
            if (staged) {
                blit_nv12(staged, static_cast<int>(video.width), static_cast<int>(video.height),
                          pitch, visible_width, visible_height,
                          static_cast<int>(video.crop_left_offset),
                          static_cast<int>(video.crop_top_offset));
            }
        }
        return;
    }

    AvPlayerFrameInfo frame{};
    if (!get_video_(player_, &frame) || !frame.p_data) return;
    const std::uint32_t width = frame.details.video.width;
    const std::uint32_t height = frame.details.video.height;
    last_timestamp_ = frame.timestamp;
    if (frames_ < 5) {
        ps4::system::log("video: frame %llu %ux%u ts=%llu data=%p",
                         static_cast<unsigned long long>(frames_), width, height,
                         static_cast<unsigned long long>(frame.timestamp), frame.p_data);
    }
    ++frames_;
    if (width && height) {
        last_width_ = width;
        last_height_ = height;
        const std::uint8_t* staged = stage_frame(frame.p_data, static_cast<int>(width),
                                                 static_cast<int>(height));
        if (staged) {
            blit_nv12(staged, static_cast<int>(width), static_cast<int>(height),
                      static_cast<int>(width), static_cast<int>(width),
                      static_cast<int>(height), 0, 0);
        }
    }
}

void ps4_react_video_stop() {
    close_player();
}

bool ps4_react_video_used() {
    return video_used_;
}

void ps4_react_video_attach(hui::audio::Mixer& target) {
    video_mixer = &target;
    target.attach_stream(1, &video_audio_deck);
    target.set_stream_gain(1, 1, 0);
}

void ps4_react_video_shutdown() {
    close_player();
    // Unload the modules while the process is still ours. The decoder's GPU
    // resources and suspend blocks otherwise survive into KillApp's suspend,
    // which then times out with CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC.
    const std::size_t remaining = release_all_direct_blocks();
    ps4::system::log("video: released %zu direct blocks after close", remaining);
    ps4::system::sleep_us(100000);
    if (module_ >= 0) {
        const int rc = sceKernelStopUnloadModule(module_, 0, nullptr, 0, nullptr, nullptr);
        ps4::system::log("video: unload AvPlayer rc=0x%08x", static_cast<unsigned>(rc));
        module_ = -1;
    }
    if (gnm_module_ >= 0) {
        const int rc = sceKernelStopUnloadModule(gnm_module_, 0, nullptr, 0, nullptr, nullptr);
        ps4::system::log("video: unload GNM rc=0x%08x", static_cast<unsigned>(rc));
        gnm_module_ = -1;
    }
}

namespace {

const char* state_name() {
    if (!error_.empty()) return "failed";
    if (!player_) return "stopped";
    if (paused_) return "paused";
    if (is_active_ && frames_ > 0 && !is_active_(player_)) return "completed";
    return "playing";
}

std::uint64_t position_raw() {
    return player_ && current_time_ ? current_time_(player_) : 0;
}

JSValue play(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "Video.play: expected a path");
    const char* path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;
    int x = 0;
    int y = 0;
    int width = 1920;
    int height = 1080;
    if (argc > 1) JS_ToInt32(ctx, &x, argv[1]);
    if (argc > 2) JS_ToInt32(ctx, &y, argv[2]);
    if (argc > 3) JS_ToInt32(ctx, &width, argv[3]);
    if (argc > 4) JS_ToInt32(ctx, &height, argv[4]);
    const bool ok = ps4_react_video_play(path, x, y, width, height);
    JS_FreeCString(ctx, path);
    return JS_NewBool(ctx, ok);
}

JSValue pause(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!player_ || !pause_) return JS_NewBool(ctx, false);
    const int rc = pause_(player_);
    ps4::system::log("video: pause rc=0x%08x", static_cast<unsigned>(rc));
    if (rc >= 0) paused_ = true;
    return JS_NewBool(ctx, rc >= 0);
}

JSValue resume(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    if (!player_ || !resume_) return JS_NewBool(ctx, false);
    const int rc = resume_(player_);
    ps4::system::log("video: resume rc=0x%08x", static_cast<unsigned>(rc));
    if (rc >= 0) paused_ = false;
    return JS_NewBool(ctx, rc >= 0);
}

JSValue seek(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    double seconds = 0;
    if (!argc || JS_ToFloat64(ctx, &seconds, argv[0]) || !player_ || !jump_)
        return JS_NewBool(ctx, false);
    // sceAvPlayerJumpToTime takes milliseconds (the library converts them to
    // its internal 90 kHz clock).
    const std::uint64_t milliseconds =
        static_cast<std::uint64_t>(std::max(0.0, seconds) * 1000.0);
    const int rc = jump_(player_, milliseconds);
    ps4::system::log("video: seek %.3fs (%llu ms) rc=0x%08x", seconds,
                     static_cast<unsigned long long>(milliseconds), static_cast<unsigned>(rc));
    return JS_NewBool(ctx, rc >= 0);
}

JSValue stop(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    close_player();
    return JS_NewBool(ctx, true);
}

JSValue set_rect(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 4) return JS_ThrowTypeError(ctx, "Video.setRect: expected x, y, width, height");
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    JS_ToInt32(ctx, &width, argv[2]);
    JS_ToInt32(ctx, &height, argv[3]);
    rect_ = {x, y, width, height};
    return JS_UNDEFINED;
}

JSValue status(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    JSValue out = JS_NewObject(ctx);
    const std::uint64_t raw = position_raw();
    JS_SetPropertyStr(ctx, out, "state", JS_NewString(ctx, state_name()));
    JS_SetPropertyStr(ctx, out, "position", JS_NewFloat64(ctx, static_cast<double>(raw) / 1000.0));
    JS_SetPropertyStr(ctx, out, "positionRaw", JS_NewFloat64(ctx, static_cast<double>(raw)));
    JS_SetPropertyStr(ctx, out, "duration",
                      JS_NewFloat64(ctx, static_cast<double>(duration_ms_) / 1000.0));
    JS_SetPropertyStr(ctx, out, "width", JS_NewUint32(ctx, last_width_));
    JS_SetPropertyStr(ctx, out, "height", JS_NewUint32(ctx, last_height_));
    JS_SetPropertyStr(ctx, out, "frames", JS_NewFloat64(ctx, static_cast<double>(frames_)));
    JS_SetPropertyStr(ctx, out, "path", JS_NewString(ctx, path_.c_str()));
    JS_SetPropertyStr(ctx, out, "error", JS_NewString(ctx, error_.c_str()));
    return out;
}

} // namespace

JSValue ps4_react_video_api(JSContext* ctx) {
    JSValue api = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, api, "play", JS_NewCFunction(ctx, play, "play", 5));
    JS_SetPropertyStr(ctx, api, "pause", JS_NewCFunction(ctx, pause, "pause", 0));
    JS_SetPropertyStr(ctx, api, "resume", JS_NewCFunction(ctx, resume, "resume", 0));
    JS_SetPropertyStr(ctx, api, "seek", JS_NewCFunction(ctx, seek, "seek", 1));
    JS_SetPropertyStr(ctx, api, "stop", JS_NewCFunction(ctx, stop, "stop", 0));
    JS_SetPropertyStr(ctx, api, "setRect", JS_NewCFunction(ctx, set_rect, "setRect", 4));
    JS_SetPropertyStr(ctx, api, "status", JS_NewCFunction(ctx, status, "status", 0));
    return api;
}
