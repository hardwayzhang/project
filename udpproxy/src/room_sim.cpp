#include "udpproxy/room_sim.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace udpproxy {
namespace {

bool SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

RoomSim::~RoomSim() { Stop(); }

bool RoomSim::Start(uint16_t port, Handler handler) {
  if (running_.load()) return true;
  handler_ = std::move(handler);

  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) return false;
  int yes = 1;
  setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (!SetNonBlocking(fd_)) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &blen);
  port_ = ntohs(bound.sin_port);

  running_.store(true);
  thread_ = std::thread([this] { Loop(); });
  return true;
}

void RoomSim::Stop() {
  if (!running_.exchange(false)) {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    return;
  }
  if (thread_.joinable()) thread_.join();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void RoomSim::Loop() {
  std::vector<uint8_t> buf(65535);
  while (running_.load()) {
    pollfd pfd{fd_, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, 50);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (!(pfd.revents & POLLIN)) continue;

    for (;;) {
      sockaddr_in from{};
      socklen_t flen = sizeof(from);
      const ssize_t n =
          ::recvfrom(fd_, buf.data(), buf.size(), 0,
                     reinterpret_cast<sockaddr*>(&from), &flen);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        break;
      }
      recv_count_.fetch_add(1);
      if (handler_) {
        handler_(buf.data(), static_cast<size_t>(n), from);
      } else {
        // Default: echo payload back to peer (proxy local port L).
        ::sendto(fd_, buf.data(), static_cast<size_t>(n), 0,
                 reinterpret_cast<sockaddr*>(&from), sizeof(from));
      }
    }
  }
}

}  // namespace udpproxy
