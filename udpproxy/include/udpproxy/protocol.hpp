#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace udpproxy {

// Control magic: "UPDC"
constexpr uint32_t kControlMagic = 0x55504443u;

enum class MsgType : uint8_t {
  kBind = 1,
  kBindAck = 2,
  kUnbind = 3,
  kHeartbeat = 4,
  kHeartbeatAck = 5,
  kSessionGone = 6,
};

enum class ErrorCode : uint16_t {
  kOk = 0,
  kBadParam = 1,
  kRoomNotFound = 2,
  kAuthFailed = 3,
  kNoResource = 4,
  kNoSession = 5,
  kInternal = 6,
};

struct ControlMessage {
  MsgType type{MsgType::kBind};
  uint64_t room_id{0};
  std::string token;
  ErrorCode code{ErrorCode::kOk};
  uint32_t expire_ms{0};
};

bool IsControlPacket(const uint8_t* data, size_t len);

// Returns empty on failure.
std::vector<uint8_t> EncodeControl(const ControlMessage& msg);

// Returns false if not a valid control packet.
bool DecodeControl(const uint8_t* data, size_t len, ControlMessage* out);

}  // namespace udpproxy
