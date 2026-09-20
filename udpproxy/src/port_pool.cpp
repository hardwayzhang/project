#include "udpproxy/port_pool.hpp"

namespace udpproxy {

PortPool::PortPool(PortPoolConfig cfg) : cfg_(std::move(cfg)) {
  if (cfg_.port_end <= cfg_.port_begin) {
    cfg_.port_end = static_cast<uint16_t>(cfg_.port_begin + 1);
  }
  free_.reserve(cfg_.port_end - cfg_.port_begin);
  for (uint32_t p = cfg_.port_begin; p < cfg_.port_end; ++p) {
    free_.push_back(static_cast<uint16_t>(p));
  }
}

std::optional<uint16_t> PortPool::Acquire() {
  std::lock_guard<std::mutex> lock(mu_);
  const auto now = std::chrono::steady_clock::now();

  // Move cooled ports back to free list.
  for (auto it = cooling_.begin(); it != cooling_.end();) {
    if (now - it->second >= cfg_.reuse_cooldown) {
      free_.push_back(it->first);
      it = cooling_.erase(it);
    } else {
      ++it;
    }
  }

  if (free_.empty()) return std::nullopt;
  const uint16_t port = free_.back();
  free_.pop_back();
  return port;
}

void PortPool::Release(uint16_t port) {
  std::lock_guard<std::mutex> lock(mu_);
  cooling_[port] = std::chrono::steady_clock::now();
}

size_t PortPool::InUse() const {
  std::lock_guard<std::mutex> lock(mu_);
  const size_t total = cfg_.port_end - cfg_.port_begin;
  return total - free_.size() - cooling_.size();
}

size_t PortPool::AvailableApprox() const {
  std::lock_guard<std::mutex> lock(mu_);
  return free_.size();
}

}  // namespace udpproxy
