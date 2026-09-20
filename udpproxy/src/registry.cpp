#include "udpproxy/registry.hpp"

namespace udpproxy {

bool RoomRegistry::Register(uint64_t room_id, uint16_t room_port) {
  std::lock_guard<std::mutex> lock(mu_);
  if (rooms_.count(room_id) != 0) return false;
  rooms_.emplace(room_id, room_port);
  return true;
}

bool RoomRegistry::Unregister(uint64_t room_id) {
  std::lock_guard<std::mutex> lock(mu_);
  return rooms_.erase(room_id) > 0;
}

std::optional<uint16_t> RoomRegistry::Find(uint64_t room_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = rooms_.find(room_id);
  if (it == rooms_.end()) return std::nullopt;
  return it->second;
}

size_t RoomRegistry::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return rooms_.size();
}

}  // namespace udpproxy
