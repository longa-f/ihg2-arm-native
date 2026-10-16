#include "ihg2_arm/gateway_socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ihg2::arm {

GatewaySocket::~GatewaySocket() {
    close();
}

bool GatewaySocket::open(const char* gateway_ipv4, std::uint16_t gateway_port) noexcept {
    close();
    if (gateway_ipv4 == nullptr || gateway_port == 0) {
        error_number_ = EINVAL;
        return false;
    }

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons(gateway_port);
    if (inet_pton(AF_INET, gateway_ipv4, &peer.sin_addr) != 1) {
        error_number_ = EINVAL;
        return false;
    }

    const int candidate = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (candidate < 0) {
        error_number_ = errno;
        return false;
    }

    const int receive_buffer_bytes = 8192;
    if (setsockopt(candidate, SOL_SOCKET, SO_RCVBUF, &receive_buffer_bytes, sizeof receive_buffer_bytes) != 0) {
        error_number_ = errno;
        ::close(candidate);
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(0);
    if (::bind(candidate, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0 ||
        ::connect(candidate, reinterpret_cast<const sockaddr*>(&peer), sizeof peer) != 0) {
        error_number_ = errno;
        ::close(candidate);
        return false;
    }

    fd_ = candidate;
    error_number_ = 0;
    return true;
}

void GatewaySocket::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool GatewaySocket::is_open() const noexcept {
    return fd_ >= 0;
}

int GatewaySocket::error_number() const noexcept {
    return error_number_;
}

std::uint16_t GatewaySocket::local_port() const noexcept {
    if (fd_ < 0) {
        return 0;
    }
    sockaddr_in local{};
    socklen_t size = sizeof local;
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &size) != 0) {
        return 0;
    }
    return ntohs(local.sin_port);
}

bool GatewaySocket::send_packet(const std::uint8_t* bytes, std::size_t size) noexcept {
    if (fd_ < 0) {
        error_number_ = EBADF;
        return false;
    }
    if (bytes == nullptr || size == 0 || size > kGatewayDatagramLimit) {
        error_number_ = size > kGatewayDatagramLimit ? EMSGSIZE : EINVAL;
        return false;
    }
    const ssize_t sent = ::send(fd_, bytes, size, 0);
    if (sent < 0) {
        error_number_ = errno;
        return false;
    }
    if (static_cast<std::size_t>(sent) != size) {
        error_number_ = EIO;
        return false;
    }
    error_number_ = 0;
    return true;
}

ReceiveStatus GatewaySocket::receive(std::uint32_t timeout_ms, GatewayDatagramView& datagram) noexcept {
    datagram = {};
    if (fd_ < 0) {
        error_number_ = EBADF;
        return ReceiveStatus::socket_error;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    pollfd descriptor{fd_, POLLIN, 0};
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = now >= deadline
                ? 0L
                : std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const auto bounded_timeout = remaining > 0x7fffffffL ? 0x7fffffff : static_cast<int>(remaining);
        const int ready = ::poll(&descriptor, 1, bounded_timeout);
        if (ready == 0) {
            return ReceiveStatus::timeout;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                if (std::chrono::steady_clock::now() < deadline) {
                    continue;
                }
                return ReceiveStatus::timeout;
            }
            error_number_ = errno;
            return ReceiveStatus::socket_error;
        }
        if ((descriptor.revents & POLLNVAL) != 0) {
            error_number_ = EBADF;
            return ReceiveStatus::socket_error;
        }
        if ((descriptor.revents & POLLHUP) != 0) {
            error_number_ = EIO;
            return ReceiveStatus::socket_error;
        }
        if ((descriptor.revents & POLLERR) != 0) {
            // Connected UDP sockets report asynchronous ICMP errors through
            // POLLERR. Reading SO_ERROR also clears the queued error; leaving
            // it pending makes every later poll wake immediately and prevents
            // the bridge from receiving any more gateway reports.
            int socket_error = 0;
            socklen_t size = sizeof socket_error;
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socket_error, &size) != 0) {
                error_number_ = errno;
            } else {
                error_number_ = socket_error != 0 ? socket_error : EIO;
            }
            return ReceiveStatus::socket_error;
        }
        if ((descriptor.revents & POLLIN) == 0) {
            error_number_ = EIO;
            return ReceiveStatus::socket_error;
        }

        iovec iov{buffer_.data(), buffer_.size()};
        msghdr message{};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        const ssize_t size = ::recvmsg(fd_, &message, MSG_TRUNC);
        if (size < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                descriptor.revents = 0;
                if (std::chrono::steady_clock::now() < deadline) {
                    continue;
                }
                return ReceiveStatus::timeout;
            }
            error_number_ = errno;
            return ReceiveStatus::socket_error;
        }
        if ((message.msg_flags & MSG_TRUNC) != 0 ||
            static_cast<std::size_t>(size) > buffer_.size() || size == 0) {
            return ReceiveStatus::malformed;
        }
        datagram = {buffer_.data(), static_cast<std::size_t>(size)};
        return ReceiveStatus::datagram;
    }
}

} // namespace ihg2::arm
