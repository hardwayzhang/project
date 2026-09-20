#include "udpproxy/protocol.hpp"
#include "udpproxy/proxy.hpp"
#include "udpproxy/room_sim.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

int MakeClientSocket() {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  return fd;
}

sockaddr_in MakeDest(uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  return a;
}

bool RecvOnce(int fd, std::vector<uint8_t>* out, int timeout_ms) {
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  out->assign(2048, 0);
  sockaddr_in from{};
  socklen_t flen = sizeof(from);
  const ssize_t n =
      ::recvfrom(fd, out->data(), out->size(), 0,
                 reinterpret_cast<sockaddr*>(&from), &flen);
  if (n < 0) return false;
  out->resize(static_cast<size_t>(n));
  return true;
}

}  // namespace

int main() {
  using namespace udpproxy;

  ProxyConfig cfg;
  cfg.public_bind_ip = "127.0.0.1";
  cfg.public_port = 0;
  cfg.port_pool.port_begin = 31000;
  cfg.port_pool.port_end = 31100;
  cfg.session_idle_timeout = std::chrono::milliseconds(60000);

  Proxy proxy(cfg);
  if (!proxy.Start()) {
    std::cerr << "proxy start failed\n";
    return 1;
  }

  RoomSim room1;
  RoomSim room2;
  if (!room1.Start(0) || !room2.Start(0)) {
    std::cerr << "room start failed\n";
    return 1;
  }

  const uint64_t id1 = 1001;
  const uint64_t id2 = 1002;
  proxy.RegisterRoom(id1, room1.Port());
  proxy.RegisterRoom(id2, room2.Port());

  std::cout << "proxy public port: " << proxy.PublicPort() << "\n"
            << "room1=" << room1.Port() << " room2=" << room2.Port() << "\n";

  int cfd = MakeClientSocket();
  auto dest = MakeDest(proxy.PublicPort());

  ControlMessage bind;
  bind.type = MsgType::kBind;
  bind.room_id = id1;
  auto bind_buf = EncodeControl(bind);
  ::sendto(cfd, bind_buf.data(), bind_buf.size(), 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

  std::vector<uint8_t> resp;
  if (!RecvOnce(cfd, &resp, 1000)) {
    std::cerr << "bind ack timeout\n";
    return 1;
  }
  ControlMessage ack;
  if (!DecodeControl(resp.data(), resp.size(), &ack) ||
      ack.code != ErrorCode::kOk) {
    std::cerr << "bind failed\n";
    return 1;
  }
  std::cout << "bind ok, expire_ms=" << ack.expire_ms << "\n";

  const char* payload = "hello-room-data";
  ::sendto(cfd, payload, std::strlen(payload), 0,
           reinterpret_cast<sockaddr*>(&dest), sizeof(dest));

  if (!RecvOnce(cfd, &resp, 1000)) {
    std::cerr << "echo timeout\n";
    return 1;
  }
  std::string got(resp.begin(), resp.end());
  std::cout << "echo: " << got << "\n";
  if (got != payload) {
    std::cerr << "payload mismatch\n";
    return 1;
  }

  std::cout << "metrics uplink=" << proxy.GetMetrics().uplink_packets.load()
            << " downlink=" << proxy.GetMetrics().downlink_packets.load()
            << " sessions=" << proxy.SessionCount() << "\n";

  ::close(cfd);
  proxy.Stop();
  room1.Stop();
  room2.Stop();
  std::cout << "demo ok\n";
  return 0;
}
