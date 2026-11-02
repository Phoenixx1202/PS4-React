#pragma once

#include <cstdint>

#include <orbis/Pad.h>

namespace ps4 {

// DualShock 4 reader for the runtime.
class Pad {
public:
    Pad() = default;
    ~Pad();

    Pad(const Pad&) = delete;
    Pad& operator=(const Pad&) = delete;

    bool open();
    bool read(OrbisPadData& data);
    void close();

    bool is_open() const { return handle_ >= 0; }
    int handle() const { return handle_; }

    void set_light_bar(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    void reset_light_bar();
    void set_vibration(std::uint8_t large, std::uint8_t small);

private:
    int handle_ = -1;
    int user_ = -1;
};

} // namespace ps4
