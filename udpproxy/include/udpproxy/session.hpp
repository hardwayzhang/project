#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace udpproxy {

inline std::string ClientKey(const sockaddr_in& addr) {
  char ip[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

struct Session {
  sockaddr_in client_addr{};
  uint16_t local_port{0};
  int local_fd{-1};
  uint64_t room_id{0};
  uint16_t room_port{0};
  std::chrono::steady_clock::time_point created_at{};
  std::chrono::steady_clock::time_point last_active{};
};

class SessionTable {
 public:
  void Insert(std::shared_ptr<Session> s);
  std::shared_ptr<Session> FindByClient(const sockaddr_in& addr) const;
  std::shared_ptr<Session> FindByLocalPort(uint16_t port) const;
  std::vector<std::shared_ptr<Session>> FindByRoom(uint64_t room_id) const;
  bool EraseByClient(const sockaddr_in& addr);
  bool EraseSession(const std::shared_ptr<Session>& s);
  std::vector<std::shared_ptr<Session>> ExpireIdle(
      std::chrono::milliseconds idle_timeout,
      std::chrono::steady_clock::time_point now);
  std::vector<std::shared_ptr<Session>> All() const;
  size_t Size() const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<Session>> by_client_;
  std::unordered_map<uint16_t, std::shared_ptr<Session>> by_local_;
  std::unordered_map<uint64_t, std::unordered_set<std::string>> by_room_;
};

}  // namespace udpproxy
