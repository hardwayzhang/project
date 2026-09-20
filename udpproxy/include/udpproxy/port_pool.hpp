#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace udpproxy {

struct PortPoolConfig {
  uint16_t port_begin{20000};
  uint16_t port_end{20100};  // exclusive upper bound in practice: [begin, end)
  std::chrono::milliseconds reuse_cooldown{3000};
};

// Allocates exclusive local UDP ports with cooldown after release.
class PortPool {
 public:
  explicit PortPool(PortPoolConfig cfg = {});

  // Returns a free port, or nullopt if exhausted.
  std::optional<uint16_t> Acquire();

  void Release(uint16_t port);

  size_t InUse() const;
  size_t AvailableApprox() const;

 private:
  PortPoolConfig cfg_;
  mutable std::mutex mu_;
  std::vector<uint16_t> free_;
  std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> cooling_;
};

}  // namespace udpproxy
