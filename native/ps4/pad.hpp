#pragma once

#include <orbis/Pad.h>

namespace ps4 {

// Minimal DualShock 4 reader for the PoC.
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

private:
    int handle_ = -1;
    int user_ = -1;
};

} // namespace ps4
