#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

namespace udpproxy {

// Minimal same-process room: binds 127.0.0.1:port and echoes payload back
// to the peer address (which will be proxy local port L).
class RoomSim {
 public:
  using Handler = std::function<void(const uint8_t* data, size_t len,
                                     const sockaddr_in& from)>;

  RoomSim() = default;
  ~RoomSim();

  RoomSim(const RoomSim&) = delete;
  RoomSim& operator=(const RoomSim&) = delete;

  // If port==0, OS assigns an ephemeral port on 127.0.0.1.
  bool Start(uint16_t port = 0, Handler handler = nullptr);
  void Stop();

  uint16_t Port() const { return port_; }
  uint64_t RecvCount() const { return recv_count_.load(); }

 private:
  void Loop();

  int fd_{-1};
  uint16_t port_{0};
  std::atomic<bool> running_{false};
  std::thread thread_;
  Handler handler_;
  std::atomic<uint64_t> recv_count_{0};
};

}  // namespace udpproxy
