// A decoded NV12 frame and the screen rectangle it belongs to.
//
// `data` points into the video module's staging copy and stays valid until the
// next pump that replaces the frame or until playback stops. The pointer is
// read on the render thread, which is also where the pump runs.
#pragma once

#include <cstdint>

namespace ps4 {

struct VideoFrameView {
    const std::uint8_t* data = nullptr;
    std::uint64_t serial = 0;
    int source_width = 0;
    int source_height = 0;
    int pitch = 0;
    int visible_width = 0;
    int visible_height = 0;
    int crop_left = 0;
    int crop_top = 0;
    int rect_x = 0;
    int rect_y = 0;
    int rect_width = 0;
    int rect_height = 0;
    bool full_range = false;
};

} // namespace ps4

// Fills `view` with the frame that is currently on screen. False when no frame
// is available (never played, stopped, or the decoder is between streams).
bool ps4_react_video_frame_view(ps4::VideoFrameView* view);

// When enabled, the video module stops converting NV12 into the software
// framebuffer; the host composites the frame on the GPU instead.
void ps4_react_video_set_gpu_present(bool enabled);
