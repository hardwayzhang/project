#pragma once

#include <atomic>
#include <cstdint>

namespace udpproxy {

struct Metrics {
  std::atomic<uint64_t> sessions_active{0};
  std::atomic<uint64_t> ports_in_use{0};
  std::atomic<uint64_t> bind_total{0};
  std::atomic<uint64_t> bind_fail{0};
  std::atomic<uint64_t> uplink_packets{0};
  std::atomic<uint64_t> downlink_packets{0};
  std::atomic<uint64_t> uplink_bytes{0};
  std::atomic<uint64_t> downlink_bytes{0};
  std::atomic<uint64_t> drop_unbound_uplink{0};
  std::atomic<uint64_t> session_expire_total{0};
  std::atomic<uint64_t> room_register_total{0};
  std::atomic<uint64_t> room_unregister_total{0};
};

}  // namespace udpproxy
