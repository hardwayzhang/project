#include "udpproxy/proxy.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace udpproxy {
namespace {

bool SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return false;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

sockaddr_in MakeAddr(const std::string& ip, uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  }
  return addr;
}

}  // namespace

Proxy::Proxy(ProxyConfig cfg)
    : cfg_(std::move(cfg)), port_pool_(cfg_.port_pool) {}

Proxy::~Proxy() { Stop(); }

bool Proxy::Start() {
  if (running_.load()) return true;

  public_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (public_fd_ < 0) return false;
  int yes = 1;
  setsockopt(public_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (!SetNonBlocking(public_fd_)) {
    ::close(public_fd_);
    public_fd_ = -1;
    return false;
  }

  sockaddr_in addr = MakeAddr(cfg_.public_bind_ip, cfg_.public_port);
  if (::bind(public_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(public_fd_);
    public_fd_ = -1;
    return false;
  }

  sockaddr_in bound{};
  socklen_t blen = sizeof(bound);
  if (getsockname(public_fd_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0) {
    public_port_ = ntohs(bound.sin_port);
  } else {
    public_port_ = cfg_.public_port;
  }

  running_.store(true);
  thread_ = std::thread([this] { Loop(); });
  return true;
}

void Proxy::Stop() {
  if (!running_.exchange(false)) {
    if (public_fd_ >= 0) {
      ::close(public_fd_);
      public_fd_ = -1;
    }
    return;
  }
  if (thread_.joinable()) thread_.join();

  auto left = sessions_.All();
  for (auto& s : left) {
    sessions_.EraseSession(s);
    if (s->local_fd >= 0) {
      ::close(s->local_fd);
      s->local_fd = -1;
    }
    port_pool_.Release(s->local_port);
  }
  {
    std::lock_guard<std::mutex> lock(poll_mu_);
    poll_sessions_.clear();
  }

  if (public_fd_ >= 0) {
    ::close(public_fd_);
    public_fd_ = -1;
  }
}

bool Proxy::RegisterRoom(uint64_t room_id, uint16_t room_port) {
  if (!registry_.Register(room_id, room_port)) return false;
  metrics_.room_register_total.fetch_add(1);
  return true;
}

bool Proxy::UnregisterRoom(uint64_t room_id) {
  if (!registry_.Unregister(room_id)) return false;
  metrics_.room_unregister_total.fetch_add(1);
  auto list = sessions_.FindByRoom(room_id);
  for (auto& s : list) {
    DestroySession(s, true, ErrorCode::kRoomNotFound);
  }
  return true;
}

size_t Proxy::SessionCount() const { return sessions_.Size(); }

int Proxy::OpenLocalSocket(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return -1;
  if (!SetNonBlocking(fd)) {
    ::close(fd);
    return -1;
  }
  sockaddr_in addr = MakeAddr("127.0.0.1", port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

void Proxy::SendControlTo(const sockaddr_in& to, const ControlMessage& msg) {
  auto buf = EncodeControl(msg);
  ::sendto(public_fd_, buf.data(), buf.size(), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

void Proxy::RebuildPollfds() {
  auto all = sessions_.All();
  std::lock_guard<std::mutex> lock(poll_mu_);
  poll_sessions_ = std::move(all);
}

void Proxy::DestroySession(const std::shared_ptr<Session>& s, bool notify_client,
                           ErrorCode gone_code) {
  if (!s) return;
  if (!sessions_.EraseSession(s)) {
    return;
  }
  if (s->local_fd >= 0) {
    ::close(s->local_fd);
    s->local_fd = -1;
  }
  port_pool_.Release(s->local_port);
  RebuildPollfds();
  metrics_.sessions_active.store(sessions_.Size());
  metrics_.ports_in_use.store(port_pool_.InUse());

  if (notify_client) {
    ControlMessage gone;
    gone.type = MsgType::kSessionGone;
    gone.code = gone_code;
    gone.expire_ms = 0;
    SendControlTo(s->client_addr, gone);
  }
}

void Proxy::HandleBind(const sockaddr_in& from, uint64_t room_id,
                       const std::string& token) {
  metrics_.bind_total.fetch_add(1);

  ControlMessage ack;
  ack.type = MsgType::kBindAck;
  ack.expire_ms =
      static_cast<uint32_t>(cfg_.session_idle_timeout.count());

  if (room_id == 0) {
    ack.code = ErrorCode::kBadParam;
    metrics_.bind_fail.fetch_add(1);
    SendControlTo(from, ack);
    return;
  }

  auto room_port = registry_.Find(room_id);
  if (!room_port) {
    ack.code = ErrorCode::kRoomNotFound;
    metrics_.bind_fail.fetch_add(1);
    SendControlTo(from, ack);
    return;
  }

  if (cfg_.bind_token_required) {
    bool ok = false;
    if (cfg_.token_validator) {
      ok = cfg_.token_validator(room_id, token);
    }
    if (!ok) {
      ack.code = ErrorCode::kAuthFailed;
      metrics_.bind_fail.fetch_add(1);
      SendControlTo(from, ack);
      return;
    }
  }

  auto existing = sessions_.FindByClient(from);
  if (existing) {
    if (existing->room_id == room_id) {
      existing->last_active = std::chrono::steady_clock::now();
      ack.code = ErrorCode::kOk;
      SendControlTo(from, ack);
      return;
    }
    DestroySession(existing, false, ErrorCode::kOk);
  }

  auto port = port_pool_.Acquire();
  if (!port) {
    ack.code = ErrorCode::kNoResource;
    metrics_.bind_fail.fetch_add(1);
    SendControlTo(from, ack);
    return;
  }

  int lfd = OpenLocalSocket(*port);
  if (lfd < 0) {
    port_pool_.Release(*port);
    ack.code = ErrorCode::kNoResource;
    metrics_.bind_fail.fetch_add(1);
    SendControlTo(from, ack);
    return;
  }

  auto s = std::make_shared<Session>();
  s->client_addr = from;
  s->local_port = *port;
  s->local_fd = lfd;
  s->room_id = room_id;
  s->room_port = *room_port;
  s->created_at = std::chrono::steady_clock::now();
  s->last_active = s->created_at;
  sessions_.Insert(s);
  RebuildPollfds();
  metrics_.sessions_active.store(sessions_.Size());
  metrics_.ports_in_use.store(port_pool_.InUse());

  ack.code = ErrorCode::kOk;
  SendControlTo(from, ack);
}

void Proxy::HandleControl(const uint8_t* data, size_t len,
                          const sockaddr_in& from) {
  ControlMessage msg;
  if (!DecodeControl(data, len, &msg)) {
    ControlMessage ack;
    ack.type = MsgType::kBindAck;
    ack.code = ErrorCode::kBadParam;
    SendControlTo(from, ack);
    return;
  }

  switch (msg.type) {
    case MsgType::kBind:
      HandleBind(from, msg.room_id, msg.token);
      break;
    case MsgType::kUnbind: {
      auto s = sessions_.FindByClient(from);
      if (!s) {
        ControlMessage ack;
        ack.type = MsgType::kBindAck;
        ack.code = ErrorCode::kNoSession;
        SendControlTo(from, ack);
        break;
      }
      DestroySession(s, false, ErrorCode::kOk);
      break;
    }
    case MsgType::kHeartbeat: {
      auto s = sessions_.FindByClient(from);
      ControlMessage ack;
      ack.type = MsgType::kHeartbeatAck;
      ack.expire_ms =
          static_cast<uint32_t>(cfg_.session_idle_timeout.count());
      if (!s) {
        ack.code = ErrorCode::kNoSession;
        SendControlTo(from, ack);
        break;
      }
      s->last_active = std::chrono::steady_clock::now();
      ack.code = ErrorCode::kOk;
      SendControlTo(from, ack);
      break;
    }
    default:
      break;
  }
}

void Proxy::HandleData(const uint8_t* data, size_t len, const sockaddr_in& from) {
  auto s = sessions_.FindByClient(from);
  if (!s) {
    metrics_.drop_unbound_uplink.fetch_add(1);
    return;
  }

  sockaddr_in room = MakeAddr("127.0.0.1", s->room_port);
  const ssize_t n =
      ::sendto(s->local_fd, data, len, 0, reinterpret_cast<sockaddr*>(&room),
               sizeof(room));
  if (n > 0) {
    s->last_active = std::chrono::steady_clock::now();
    metrics_.uplink_packets.fetch_add(1);
    metrics_.uplink_bytes.fetch_add(static_cast<uint64_t>(n));
  }
}

void Proxy::HandlePublicPacket(const uint8_t* data, size_t len,
                               const sockaddr_in& from) {
  if (IsControlPacket(data, len)) {
    HandleControl(data, len, from);
  } else {
    HandleData(data, len, from);
  }
}

void Proxy::HandleLocalPacket(const std::shared_ptr<Session>& s,
                              const uint8_t* data, size_t len) {
  if (!s) return;
  const ssize_t n =
      ::sendto(public_fd_, data, len, 0,
               reinterpret_cast<const sockaddr*>(&s->client_addr),
               sizeof(s->client_addr));
  if (n > 0) {
    s->last_active = std::chrono::steady_clock::now();
    metrics_.downlink_packets.fetch_add(1);
    metrics_.downlink_bytes.fetch_add(static_cast<uint64_t>(n));
  }
}

void Proxy::SweepIdle() {
  auto expired =
      sessions_.ExpireIdle(cfg_.session_idle_timeout, std::chrono::steady_clock::now());
  for (auto& s : expired) {
    metrics_.session_expire_total.fetch_add(1);
    if (s->local_fd >= 0) {
      ::close(s->local_fd);
      s->local_fd = -1;
    }
    port_pool_.Release(s->local_port);
    ControlMessage gone;
    gone.type = MsgType::kSessionGone;
    gone.code = ErrorCode::kNoSession;
    SendControlTo(s->client_addr, gone);
  }
  if (!expired.empty()) {
    RebuildPollfds();
    metrics_.sessions_active.store(sessions_.Size());
    metrics_.ports_in_use.store(port_pool_.InUse());
  }
}

void Proxy::Loop() {
  std::vector<uint8_t> buf(65535);
  auto last_sweep = std::chrono::steady_clock::now();

  while (running_.load()) {
    std::vector<pollfd> pfds;
    std::vector<std::shared_ptr<Session>> local_sessions;
    {
      std::lock_guard<std::mutex> lock(poll_mu_);
      local_sessions = poll_sessions_;
    }

    pfds.push_back(pollfd{public_fd_, POLLIN, 0});
    for (auto& s : local_sessions) {
      if (s && s->local_fd >= 0) {
        pfds.push_back(pollfd{s->local_fd, POLLIN, 0});
      } else {
        pfds.push_back(pollfd{-1, 0, 0});
      }
    }

    const int pr = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 50);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (!pfds.empty() && (pfds[0].revents & POLLIN)) {
      for (;;) {
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        const ssize_t n =
            ::recvfrom(public_fd_, buf.data(), buf.size(), 0,
                       reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          break;
        }
        HandlePublicPacket(buf.data(), static_cast<size_t>(n), from);
      }
    }

    for (size_t i = 0; i < local_sessions.size(); ++i) {
      if (!(pfds[i + 1].revents & POLLIN)) continue;
      auto& s = local_sessions[i];
      if (!s || s->local_fd < 0) continue;
      for (;;) {
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        const ssize_t n =
            ::recvfrom(s->local_fd, buf.data(), buf.size(), 0,
                       reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          break;
        }
        HandleLocalPacket(s, buf.data(), static_cast<size_t>(n));
      }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - last_sweep >= std::chrono::milliseconds(50)) {
      SweepIdle();
      last_sweep = now;
    }
  }
}

}  // namespace udpproxy
