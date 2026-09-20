#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace udpproxy {

// roomId -> room UDP port (Rn on 127.0.0.1)
class RoomRegistry {
 public:
  // Rejects duplicate room_id. Returns false if already registered.
  bool Register(uint64_t room_id, uint16_t room_port);

  // Returns true if room existed.
  bool Unregister(uint64_t room_id);

  std::optional<uint16_t> Find(uint64_t room_id) const;

  size_t Size() const;

 private:
  mutable std::mutex mu_;
  std::unordered_map<uint64_t, uint16_t> rooms_;
};

}  // namespace udpproxy
