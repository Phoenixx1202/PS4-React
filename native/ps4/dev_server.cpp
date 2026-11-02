#include "dev_server.hpp"

#include "system.hpp"

#include <orbis/Net.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>

namespace ps4::dev {
namespace {

constexpr std::size_t kMaxPayload = 8 * 1024 * 1024;

// PS4's BSD socket address starts with the length byte, like FreeBSD.
struct SockaddrIn {
    std::uint8_t len;
    std::uint8_t family;
    std::uint16_t port;
    std::uint32_t address;
    char zero[8];
};
static_assert(sizeof(SockaddrIn) == 16, "PS4 sockaddr_in must be 16 bytes");

bool nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

} // namespace

Server::~Server() {
    stop();
}

bool Server::start(std::uint16_t port) {
    if (listen_ >= 0)
        return true;

    listen_ = sceNetSocket("ps4-react-dev", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
    if (listen_ < 0) {
        ps4::system::log("dev server: socket failed 0x%08x", static_cast<unsigned>(listen_));
        listen_ = -1;
        return false;
    }

    const int yes = 1;
    sceNetSetsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    SockaddrIn address{};
    address.len = sizeof address;
    address.family = ORBIS_NET_AF_INET;
    address.port = sceNetHtons(port);
    address.address = 0;
    if (sceNetBind(listen_, reinterpret_cast<const OrbisNetSockaddr*>(&address),
                   sizeof address) < 0 ||
        sceNetListen(listen_, 1) < 0) {
        ps4::system::log("dev server: bind/listen on port %u failed", static_cast<unsigned>(port));
        stop();
        return false;
    }
    if (!nonblocking(listen_)) {
        ps4::system::log("dev server: nonblocking listener failed");
        stop();
        return false;
    }

    buffer_.resize(kMaxPayload);
    er_hotreload_init(&parser_, buffer_.data(), buffer_.size());
    ps4::system::log("dev server: listening on tcp/%u", static_cast<unsigned>(port));
    return true;
}

void Server::stop() {
    if (client_ >= 0) {
        sceNetSocketClose(client_);
        client_ = -1;
    }
    if (listen_ >= 0) {
        sceNetSocketClose(listen_);
        listen_ = -1;
    }
    frame_ready_ = false;
    frame_size_ = 0;
}

bool Server::accept_client() {
    OrbisNetSockaddr address{};
    OrbisNetSocklen_t length = sizeof address;
    client_ = sceNetAccept(listen_, &address, &length);
    if (client_ < 0)
        return false;
    if (!nonblocking(client_)) {
        ps4::system::log("dev server: nonblocking client failed");
        drop_client();
        return false;
    }
    frame_ready_ = false;
    frame_size_ = 0;
    return true;
}

void Server::drop_client() {
    if (client_ >= 0)
        sceNetSocketClose(client_);
    client_ = -1;
    frame_ready_ = false;
    frame_size_ = 0;
    er_hotreload_init(&parser_, buffer_.data(), buffer_.size());
}

bool Server::poll(const std::uint8_t*& data, std::size_t& size) {
    if (listen_ < 0 || buffer_.empty())
        return false;
    if (client_ < 0 && !accept_client())
        return false;
    if (frame_ready_) {
        data = buffer_.data();
        size = frame_size_;
        return true;
    }

    for (;;) {
        std::uint8_t chunk[8192];
        const int received = sceNetRecv(client_, chunk, sizeof chunk, 0);
        if (received == 0) {
            drop_client();
            return false;
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return false;
            ps4::system::log("dev server: recv failed: %s", std::strerror(errno));
            drop_client();
            return false;
        }

        std::size_t offset = 0;
        while (offset < static_cast<std::size_t>(received)) {
            std::size_t consumed = 0;
            std::size_t frame = 0;
            const ErHotReloadStatus status = er_hotreload_feed(
                &parser_, chunk + offset, static_cast<std::size_t>(received) - offset,
                &consumed, &frame);
            offset += consumed;
            if (status == ER_HR_FRAME_READY) {
                frame_ready_ = true;
                frame_size_ = frame;
                data = buffer_.data();
                size = frame;
                return true;
            }
            if (status == ER_HR_NEED_MORE)
                break;
            ps4::system::log("dev server: dropped frame: %s",
                             er_hotreload_status_str(status));
        }
    }
}

void Server::ack(bool ok) {
    if (client_ >= 0) {
        const std::uint8_t status = ok ? 0 : 1;
        sceNetSend(client_, &status, sizeof status, 0);
    }
    drop_client();
}

} // namespace ps4::dev
