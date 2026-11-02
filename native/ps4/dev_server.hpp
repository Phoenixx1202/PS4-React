#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" {
#include "er_hotreload.h"
}

namespace ps4::dev {

// TCP receiver for the development bundle uploader. It only exists in builds
// configured with -DPS4_REACT_DEV_SERVER=ON; the release runtime never opens a
// listening socket.
class Server {
public:
    Server() = default;
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(std::uint16_t port = 9001);
    void stop();

    // Returns a complete, CRC-checked payload. The caller applies it and then
    // calls ack(); the connection is closed after the ack.
    bool poll(const std::uint8_t*& data, std::size_t& size);
    void ack(bool ok);

private:
    bool accept_client();
    void drop_client();

    int listen_ = -1;
    int client_ = -1;
    bool frame_ready_ = false;
    std::size_t frame_size_ = 0;
    std::vector<std::uint8_t> buffer_;
    ErHotReload parser_{};
};

} // namespace ps4::dev
