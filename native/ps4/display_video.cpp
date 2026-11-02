#include "display_video.hpp"

#include "system.hpp"

#include <orbis/VideoOut.h>

#include <cerrno>
#include <cstring>

namespace ps4 {
namespace {

// A8R8G8B8, linear, no extra flags. This is the format used by the OpenOrbis
// graphics sample (samples/_common/graphics.cpp).
constexpr std::uint32_t kPixelFormat = 0x80000000u;
// The sample writes 0x80000000 | RGB. The engine framebuffer uses 0xFF for
// alpha; normalize it to the value the sample uses until hardware proves the
// alpha field is ignored.
constexpr std::uint32_t kOpaque = 0x80000000u;
constexpr std::size_t kAlignment = 0x200000;
constexpr int kBufferCount = 2;

} // namespace

Display::~Display() {
    close();
}

bool Display::open(int width, int height) {
    if (width <= 0 || height <= 0 || width > 3840 || height > 2160) return false;
    if (handle_ >= 0) return true;

    system::log("videoout: opening %dx%d", width, height);
    handle_ = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
    if (handle_ < 0) {
        system::log("videoout: open failed 0x%08x", static_cast<unsigned>(handle_));
        return false;
    }
    system::log("videoout: handle=%d", handle_);

    // Keep the requested 60 Hz cadence; the sample uses the same call.
    const int flip_rate = sceVideoOutSetFlipRate(handle_, 0);
    system::log("videoout: flip rate=0 result=0x%08x", static_cast<unsigned>(flip_rate));

    const std::size_t frame_bytes = static_cast<std::size_t>(width) * height * 4;
    // Direct-memory sizes are aligned by the allocator; align here so the map
    // covers every byte the two buffers use.
    mapped_size_ = (frame_bytes * kBufferCount + kAlignment - 1) & ~(kAlignment - 1);
    system::log("videoout: allocating %zu bytes of direct memory", mapped_size_);
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), mapped_size_,
                                      kAlignment, 3, &direct_offset_) < 0) {
        system::log("videoout: direct memory allocation failed (%zu bytes)", mapped_size_);
        close();
        return false;
    }
    direct_allocated_ = true;
    system::log("videoout: direct memory offset=%lld", static_cast<long long>(direct_offset_));

    void* mapped = nullptr;
    if (sceKernelMapDirectMemory(&mapped, mapped_size_, 0x33, 0, direct_offset_,
                                 kAlignment) < 0) {
        system::log("videoout: direct memory map failed");
        close();
        return false;
    }
    mapped_ = mapped;
    buffers_[0] = static_cast<std::uint32_t*>(mapped);
    buffers_[1] = buffers_[0] + static_cast<std::size_t>(width) * height;
    system::log("videoout: mapped at %p", mapped);

    OrbisVideoOutBufferAttribute attribute{};
    sceVideoOutSetBufferAttribute(&attribute, kPixelFormat, 1, 0,
                                  static_cast<unsigned>(width),
                                  static_cast<unsigned>(height),
                                  static_cast<unsigned>(width));
    if (sceVideoOutRegisterBuffers(handle_, 0, reinterpret_cast<void**>(buffers_),
                                   kBufferCount, &attribute) < 0) {
        system::log("videoout: register buffers failed");
        close();
        return false;
    }
    system::log("videoout: buffers registered");

    // Optional flip queue. If it cannot be created, present() falls back to
    // polling sceVideoOutGetFlipStatus.
    if (sceKernelCreateEqueue(&equeue_, "ps4-react-flip") == 0) {
        has_equeue_ = true;
        if (sceVideoOutAddFlipEvent(equeue_, handle_, nullptr) < 0)
            system::log("videoout: add flip event failed; polling instead");
        else
            system::log("videoout: flip equeue ready");
    } else {
        has_equeue_ = false;
        system::log("videoout: equeue unavailable; polling flip status");
    }

    width_ = width;
    height_ = height;
    current_ = 0;
    frame_id_ = 0;
    system::log("videoout: %dx%d, 2 linear buffers, %zu MiB direct memory",
                width, height, mapped_size_ >> 20);
    return true;
}

void Display::close() {
    if (handle_ >= 0)
        sceVideoOutUnregisterBuffers(handle_, 0);
    if (has_equeue_) {
        sceKernelDeleteEqueue(equeue_);
        equeue_ = 0;
        has_equeue_ = false;
    }
    if (direct_allocated_) {
        sceKernelReleaseDirectMemory(direct_offset_, mapped_size_);
        direct_allocated_ = false;
    }
    if (handle_ >= 0)
        sceVideoOutClose(handle_);

    handle_ = -1;
    mapped_ = nullptr;
    mapped_size_ = 0;
    direct_offset_ = 0;
    direct_allocated_ = false;
    buffers_[0] = buffers_[1] = nullptr;
    width_ = height_ = 0;
    current_ = 0;
    frame_id_ = 0;
}

void Display::copy_frame(const std::uint32_t* src, int src_width, int src_height,
                         std::uint32_t* dst) {
    const std::size_t count = static_cast<std::size_t>(src_width) * src_height;
    for (std::size_t i = 0; i < count; ++i)
        dst[i] = (src[i] & 0x00FFFFFFu) | kOpaque;
}

bool Display::present(const std::uint32_t* argb, int width, int height,
                      const Rect* damage, int damage_count) {
    if (handle_ < 0 || !argb || width != width_ || height != height_)
        return false;

    // The first version copies the whole frame. Damage-aware updates need to
    // touch both buffers (one may be the front buffer); that is the next
    // optimization once the presenter is proven on hardware.
    (void)damage;
    (void)damage_count;
    copy_frame(argb, width, height, buffers_[current_]);

    const std::int64_t submitted = frame_id_;
    if (frame_id_ == 0)
        system::log("videoout: submitting first flip");
    if (sceVideoOutSubmitFlip(handle_, current_, ORBIS_VIDEO_OUT_FLIP_VSYNC,
                              submitted) < 0) {
        system::log("videoout: submit flip failed");
        return false;
    }
    ++frame_id_;
    current_ ^= 1;
    const bool flipped = wait_for_flip(submitted);
    if (submitted == 0)
        system::log("videoout: first flip completed=%d", flipped);
    return flipped;
}

bool Display::wait_for_flip(std::int64_t frame_id) {
    for (;;) {
        OrbisVideoOutFlipStatus status{};
        if (sceVideoOutGetFlipStatus(handle_, &status) < 0)
            return false;
        if (status.flipArg == frame_id)
            return true;

        if (!has_equeue_) {
            sceKernelUsleep(1000);
            continue;
        }

        OrbisKernelEvent event{};
        int count = 0;
        if (sceKernelWaitEqueue(equeue_, &event, 1, &count, nullptr) != 0)
            return false;
    }
}

} // namespace ps4
