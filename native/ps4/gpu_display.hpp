// Piglet/OpenGL ES presentation for the PS4 runtime.
//
// The UI still composites through the software backend; this layer uploads the
// finished ARGB8888 frame as a texture and presents it with a full-screen quad
// through EGL. It is the GPU path's first stage and falls back to VideoOut when
// Piglet cannot start.

#pragma once

#include <cstdint>
#include <vector>

namespace ps4 {

class GpuDisplay {
  public:
    GpuDisplay() = default;
    GpuDisplay(const GpuDisplay&) = delete;
    GpuDisplay& operator=(const GpuDisplay&) = delete;
    ~GpuDisplay();

    bool open(int width, int height);
    void close();
    bool present(const std::uint32_t* pixels);

  private:
    bool load_modules();
    bool create_context(int width, int height);
    bool create_program();
    bool create_texture();

    int width_ = 0;
    int height_ = 0;
    int piglet_ = -1;
    int shaders_ = -1;
    void* display_ = nullptr;
    void* surface_ = nullptr;
    void* context_ = nullptr;
    unsigned program_ = 0;
    unsigned texture_ = 0;
    std::vector<std::uint8_t> staging_;
    int vertex_ = -1;
    int model_ = -1;
    int projection_ = -1;
    int texture_space_ = -1;
    int opacity_ = -1;
    int sampler_ = -1;
};

} // namespace ps4
