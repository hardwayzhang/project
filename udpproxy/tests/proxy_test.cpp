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

int g_failed = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << " " << #cond      \
                << "\n";                                                       \
      ++g_failed;                                                              \
    }                                                                          \
  } while (0)

int MakeUdp() { return ::socket(AF_INET, SOCK_DGRAM, 0); }

sockaddr_in Dest(uint16_t port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  return a;
}

bool Recv(int fd, std::vector<uint8_t>* out, int timeout_ms) {
  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  out->assign(4096, 0);
  sockaddr_in from{};
  socklen_t flen = sizeof(from);
  const ssize_t n =
      ::recvfrom(fd, out->data(), out->size(), 0,
                 reinterpret_cast<sockaddr*>(&from), &flen);
  if (n < 0) return false;
  out->resize(static_cast<size_t>(n));
  return true;
}

void SendTo(int fd, const sockaddr_in& dest, const void* data, size_t len) {
  ::sendto(fd, data, len, 0, reinterpret_cast<const sockaddr*>(&dest),
           sizeof(dest));
}

udpproxy::ProxyConfig TestCfg() {
  udpproxy::ProxyConfig cfg;
  cfg.public_bind_ip = "127.0.0.1";
  cfg.public_port = 0;
  cfg.port_pool.port_begin = 32000;
  cfg.port_pool.port_end = 32020;
  cfg.port_pool.reuse_cooldown = std::chrono::milliseconds(50);
  cfg.session_idle_timeout = std::chrono::milliseconds(300);
  return cfg;
}

udpproxy::ErrorCode BindRoom(int fd, const sockaddr_in& dest, uint64_t room_id,
                             const std::string& token = {}) {
  using namespace udpproxy;
  ControlMessage bind;
  bind.type = MsgType::kBind;
  bind.room_id = room_id;
  bind.token = token;
  auto buf = EncodeControl(bind);
  SendTo(fd, dest, buf.data(), buf.size());
  std::vector<uint8_t> resp;
  if (!Recv(fd, &resp, 1000)) return ErrorCode::kInternal;
  ControlMessage ack;
  if (!DecodeControl(resp.data(), resp.size(), &ack)) return ErrorCode::kInternal;
  return ack.code;
}

}  // namespace

int main() {
  using namespace udpproxy;

  // A1: Bind legal roomId
  {
    auto cfg = TestCfg();
    Proxy proxy(cfg);
    CHECK(proxy.Start());
    RoomSim room;
    CHECK(room.Start(0));
    CHECK(proxy.RegisterRoom(1, room.Port()));
    int fd = MakeUdp();
    auto dest = Dest(proxy.PublicPort());
    CHECK(BindRoom(fd, dest, 1) == ErrorCode::kOk);
    CHECK(proxy.SessionCount() == 1);
    ::close(fd);
    proxy.Stop();
    room.Stop();
  }

  // A2: Bind missing room
  {
    auto cfg = TestCfg();
    Proxy proxy(cfg);
    CHECK(proxy.Start());
    int fd = MakeUdp();
    auto dest = Dest(proxy.PublicPort());
    CHECK(BindRoom(fd, dest, 99) == ErrorCode::kRoomNotFound);
    CHECK(proxy.SessionCount() == 0);
    ::close(fd);
    proxy.Stop();
  }

  // A3/A4/A11: data payload unchanged echo
  {
    auto cfg = TestCfg();
    Proxy proxy(cfg);
    CHECK(proxy.Start());
    RoomSim room;
    CHECK(room.Start(0));
    CHECK(proxy.RegisterRoom(2, room.Port()));
    int fd = MakeUdp();
    auto dest = Dest(proxy.PublicPort());
    CHECK(BindRoom(fd, dest, 2) == ErrorCode::kOk);
    const std::string payload = "biz-payload-bytes-\x01\x02\xff";
    SendTo(fd, dest, payload.data(), payload.size());
    std::vector<uint8_t> resp;
    CHECK(Recv(fd, &resp, 1000));
    CHECK(std::string(resp.begin(), resp.end()) == payload);
    CHECK(proxy.GetMetrics().uplink_packets.load() >= 1);
    CHECK(proxy.GetMetrics().downlink_packets.load() >= 1);
    ::close(fd);
    proxy.Stop();
    room.Stop();
  }

  // A5: unbound data dropped
  {
    auto cfg = TestCfg();
    Proxy proxy(cfg);
    CHECK(proxy.Start());
    int fd = MakeUdp();
    auto dest = Dest(proxy.PublicPort());
    const char* p = "no-bind";
    SendTo(fd, dest, p, std::strlen(p));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(proxy.GetMetrics().drop_unbound_uplink.load() >= 1);
    std::vector<uint8_t> resp;
    CHECK(!Recv(fd, &resp, 100));
    ::close(fd);
    proxy.Stop();
  }

  // A6: two clients different rooms
  {
    auto cfg = TestCfg();
    Proxy proxy(cfg);
    CHECK(proxy.Start());
    RoomSim r1, r2;
    CHECK(r1.Start(0));
    CHECK(r2.Start(0));
    CHECK(proxy.RegisterRoom(10, r1.Port()));
    CHECK(proxy.RegisterRoom(20, r2.Port()));
    int a = MakeUdp();
    int b = MakeUdp();
    auto dest = Dest(proxy.PublicPort());
    CHECK(BindRoom(a, dest, 10) == ErrorCode::kOk);
    CHECK(BindRoom(b, dest, 20) == ErrorCode::kOk);
    SendTo(a, dest, "A", 1);
    SendTo(b, dest, "B", 1);
    std::vector<uint8_t> ra, rb;
    CHECK(Recv(a, &ra, 1000));
    CHECK(Recv(b, &rb, 1000));
    CHECK(ra.size() == 1 && ra[0] == 'A');
    CHECK(rb.size() == 1 && rb[0] == 'B');
    ::close(a);
    ::close(b);
    proxy.Stop();
    r1.Stop();
    r2.Stop();
  }

  // A7: two clients same room
  {
    auto cfg = TestCfg();
    Proxy proxy(cfg);
    CHECK(proxy.Start());
    RoomSim room;
    CHECK(room.Start(0));
    CHECK(proxy.RegisterRoom(7, room.Port()));
    int a = MakeUdp();
    int b = MakeUdp();
    auto dest = Dest(proxy.PublicPort());
    CHECK(BindRoom(a, dest, 7) == ErrorCode::kOk);
    CHECK(BindRoom(b, dest, 7) == ErrorCode::kOk);
    CHECK(proxy.SessionCount() == 2);
    SendTo(a, dest, "X", 1);
    SendTo(b, dest, "Y", 1);
    std::vector<uint8_t> ra, rb;
    CHECK(Recv(a, &ra, 1000));
    CHECK(Recv(b, &rb, 1000));
    CHECK(ra.size() == 1 && ra[0] == 'X');
    CHECK(rb.size() == 1 && rb[0] == 'Y');
    ::close(a);
    ::close(b);
    proxy.Stop();
    room.Stop();
  }

  // A8: idle timeout
  {
    auto cfg = TestCfg();
    cfg.session_idle_timeout = std::chrono::milliseconds(200);
    Proxy proxy(cfg);
    CHECK(proxy.Start());
    RoomSim room;
    CHECK(room.Start(0));
    CHECK(proxy.RegisterRoom(8, room.Port()));
    int fd = MakeUdp();
    auto dest = Dest(proxy.PublicPort());
    CHECK(BindRoom(fd, dest, 8) == ErrorCode::kOk);
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    CHECK(proxy.SessionCount() == 0);
    CHECK(proxy.GetMetrics().session_expire_total.load() >= 1);
    // After expire, data should drop until rebind
    SendTo(fd, dest, "z", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    CHECK(proxy.GetMetrics().drop_unbound_uplink.load() >= 1);
    ::close(fd);
    proxy.Stop();
    room.Stop();
  }

  // A9: room destroy kicks sessions
  {
    auto cfg = TestCfg();
    Proxy proxy(cfg);
    CHECK(proxy.Start());
    RoomSim room;
    CHECK(room.Start(0));
    CHECK(proxy.RegisterRoom(9, room.Port()));
    int fd = MakeUdp();
    auto dest = Dest(proxy.PublicPort());
    CHECK(BindRoom(fd, dest, 9) == ErrorCode::kOk);
    CHECK(proxy.UnregisterRoom(9));
    CHECK(proxy.SessionCount() == 0);
    CHECK(BindRoom(fd, dest, 9) == ErrorCode::kRoomNotFound);
    ::close(fd);
    proxy.Stop();
    room.Stop();
  }

  // A10: port pool exhausted
  {
    auto cfg = TestCfg();
    cfg.port_pool.port_begin = 33000;
    cfg.port_pool.port_end = 33002;  // only 2 ports
    Proxy proxy(cfg);
    CHECK(proxy.Start());
    RoomSim room;
    CHECK(room.Start(0));
    CHECK(proxy.RegisterRoom(11, room.Port()));
    int a = MakeUdp();
    int b = MakeUdp();
    int c = MakeUdp();
    auto dest = Dest(proxy.PublicPort());
    CHECK(BindRoom(a, dest, 11) == ErrorCode::kOk);
    CHECK(BindRoom(b, dest, 11) == ErrorCode::kOk);
    CHECK(BindRoom(c, dest, 11) == ErrorCode::kNoResource);
    // Existing sessions still work
    SendTo(a, dest, "1", 1);
    std::vector<uint8_t> ra;
    CHECK(Recv(a, &ra, 1000));
    CHECK(ra.size() == 1 && ra[0] == '1');
    ::close(a);
    ::close(b);
    ::close(c);
    proxy.Stop();
    room.Stop();
  }

  // Protocol roundtrip
  {
    ControlMessage m;
    m.type = MsgType::kBind;
    m.room_id = 0x1122334455667788ull;
    m.token = "tok";
    auto enc = EncodeControl(m);
    CHECK(IsControlPacket(enc.data(), enc.size()));
    ControlMessage out;
    CHECK(DecodeControl(enc.data(), enc.size(), &out));
    CHECK(out.room_id == m.room_id);
    CHECK(out.token == m.token);
  }

  if (g_failed == 0) {
    std::cout << "ALL TESTS PASSED\n";
    return 0;
  }
  std::cerr << g_failed << " checks failed\n";
  return 1;
}
