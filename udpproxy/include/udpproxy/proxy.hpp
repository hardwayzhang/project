#pragma once

#include "udpproxy/metrics.hpp"
#include "udpproxy/port_pool.hpp"
#include "udpproxy/protocol.hpp"
#include "udpproxy/registry.hpp"
#include "udpproxy/session.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace udpproxy {

struct ProxyConfig {
  uint16_t public_port{0};  // 0 = ephemeral
  std::string public_bind_ip{"0.0.0.0"};
  PortPoolConfig port_pool{};
  std::chrono::milliseconds session_idle_timeout{60000};
  bool bind_token_required{false};
  // Optional token validator. Return true if token is acceptable for room_id.
  std::function<bool(uint64_t room_id, const std::string& token)> token_validator;
};

// Same-process UDP port-convergence proxy.
class Proxy {
 public:
  explicit Proxy(ProxyConfig cfg = {});
  ~Proxy();

  Proxy(const Proxy&) = delete;
  Proxy& operator=(const Proxy&) = delete;

  bool Start();
  void Stop();

  bool RegisterRoom(uint64_t room_id, uint16_t room_port);
  bool UnregisterRoom(uint64_t room_id);

  uint16_t PublicPort() const { return public_port_; }
  const Metrics& GetMetrics() const { return metrics_; }
  size_t SessionCount() const;

 private:
  void Loop();
  void HandlePublicPacket(const uint8_t* data, size_t len, const sockaddr_in& from);
  void HandleControl(const uint8_t* data, size_t len, const sockaddr_in& from);
  void HandleData(const uint8_t* data, size_t len, const sockaddr_in& from);
  void HandleLocalPacket(const std::shared_ptr<Session>& s, const uint8_t* data,
                         size_t len);
  void HandleBind(const sockaddr_in& from, uint64_t room_id, const std::string& token);
  void DestroySession(const std::shared_ptr<Session>& s, bool notify_client,
                      ErrorCode gone_code);
  void SendControlTo(const sockaddr_in& to, const ControlMessage& msg);
  void SweepIdle();
  int OpenLocalSocket(uint16_t port);
  void RebuildPollfds();

  ProxyConfig cfg_;
  Metrics metrics_;
  RoomRegistry registry_;
  PortPool port_pool_;
  SessionTable sessions_;

  int public_fd_{-1};
  uint16_t public_port_{0};
  std::atomic<bool> running_{false};
  std::thread thread_;
  mutable std::mutex poll_mu_;
  // local_fd -> session weak via shared_ptr kept in sessions_
  std::vector<std::shared_ptr<Session>> poll_sessions_;
};

}  // namespace udpproxy
