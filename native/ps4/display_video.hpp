#pragma once

#include <cstddef>
#include <cstdint>

#include <orbis/libkernel.h>

namespace ps4 {

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// Presents an opaque 0xAARRGGBB framebuffer through the PS4 video-out API.
//
// Two linear 1920x1080 buffers live in direct memory. Each present copies the
// frame into the back buffer, submits a vsync flip and waits for that flip.
// The copy masks the alpha field to 0x80, the value used by the OpenOrbis
// graphics sample; if hardware accepts 0xFF this can be removed later.
class Display {
public:
    Display() = default;
    ~Display();

    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    bool open(int width, int height);
    void close();

    bool present(const std::uint32_t* argb, int width, int height,
                 const Rect* damage = nullptr, int damage_count = 0);

    int width() const { return width_; }
    int height() const { return height_; }
    bool is_open() const { return handle_ >= 0; }

private:
    bool wait_for_flip(std::int64_t frame_id);
    void copy_frame(const std::uint32_t* src, int src_width, int src_height,
                    std::uint32_t* dst);

    int handle_ = -1;
    int width_ = 0;
    int height_ = 0;

    std::uint32_t* buffers_[2] = {nullptr, nullptr};
    void* mapped_ = nullptr;
    std::size_t mapped_size_ = 0;
    off_t direct_offset_ = 0;
    bool direct_allocated_ = false;

    OrbisKernelEqueue equeue_ = 0;
    bool has_equeue_ = false;

    int current_ = 0;
    std::int64_t frame_id_ = 0;
};

} // namespace ps4
