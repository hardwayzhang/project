#include "udpproxy/protocol.hpp"

#include <cstring>

namespace udpproxy {
namespace {

void PutU16(std::vector<uint8_t>* out, uint16_t v) {
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out->push_back(static_cast<uint8_t>(v & 0xff));
}

void PutU32(std::vector<uint8_t>* out, uint32_t v) {
  out->push_back(static_cast<uint8_t>((v >> 24) & 0xff));
  out->push_back(static_cast<uint8_t>((v >> 16) & 0xff));
  out->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
  out->push_back(static_cast<uint8_t>(v & 0xff));
}

void PutU64(std::vector<uint8_t>* out, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
  }
}

bool ReadU16(const uint8_t* data, size_t len, size_t* off, uint16_t* v) {
  if (*off + 2 > len) return false;
  *v = static_cast<uint16_t>((data[*off] << 8) | data[*off + 1]);
  *off += 2;
  return true;
}

bool ReadU32(const uint8_t* data, size_t len, size_t* off, uint32_t* v) {
  if (*off + 4 > len) return false;
  *v = (static_cast<uint32_t>(data[*off]) << 24) |
       (static_cast<uint32_t>(data[*off + 1]) << 16) |
       (static_cast<uint32_t>(data[*off + 2]) << 8) |
       static_cast<uint32_t>(data[*off + 3]);
  *off += 4;
  return true;
}

bool ReadU64(const uint8_t* data, size_t len, size_t* off, uint64_t* v) {
  if (*off + 8 > len) return false;
  uint64_t x = 0;
  for (int i = 0; i < 8; ++i) {
    x = (x << 8) | data[*off + i];
  }
  *v = x;
  *off += 8;
  return true;
}

}  // namespace

bool IsControlPacket(const uint8_t* data, size_t len) {
  if (len < 5) return false;
  const uint32_t magic = (static_cast<uint32_t>(data[0]) << 24) |
                         (static_cast<uint32_t>(data[1]) << 16) |
                         (static_cast<uint32_t>(data[2]) << 8) |
                         static_cast<uint32_t>(data[3]);
  return magic == kControlMagic;
}

std::vector<uint8_t> EncodeControl(const ControlMessage& msg) {
  std::vector<uint8_t> out;
  out.reserve(32 + msg.token.size());
  PutU32(&out, kControlMagic);
  out.push_back(static_cast<uint8_t>(msg.type));

  switch (msg.type) {
    case MsgType::kBind:
      PutU64(&out, msg.room_id);
      PutU16(&out, static_cast<uint16_t>(msg.token.size()));
      out.insert(out.end(), msg.token.begin(), msg.token.end());
      break;
    case MsgType::kBindAck:
    case MsgType::kHeartbeatAck:
    case MsgType::kSessionGone:
      PutU16(&out, static_cast<uint16_t>(msg.code));
      PutU32(&out, msg.expire_ms);
      break;
    case MsgType::kUnbind:
    case MsgType::kHeartbeat:
      break;
  }
  return out;
}

bool DecodeControl(const uint8_t* data, size_t len, ControlMessage* out) {
  if (!IsControlPacket(data, len) || out == nullptr) return false;
  size_t off = 4;
  out->type = static_cast<MsgType>(data[off++]);
  out->room_id = 0;
  out->token.clear();
  out->code = ErrorCode::kOk;
  out->expire_ms = 0;

  switch (out->type) {
    case MsgType::kBind: {
      if (!ReadU64(data, len, &off, &out->room_id)) return false;
      uint16_t tlen = 0;
      if (!ReadU16(data, len, &off, &tlen)) return false;
      if (off + tlen > len) return false;
      out->token.assign(reinterpret_cast<const char*>(data + off), tlen);
      return true;
    }
    case MsgType::kBindAck:
    case MsgType::kHeartbeatAck:
    case MsgType::kSessionGone: {
      uint16_t code = 0;
      if (!ReadU16(data, len, &off, &code)) return false;
      out->code = static_cast<ErrorCode>(code);
      if (!ReadU32(data, len, &off, &out->expire_ms)) return false;
      return true;
    }
    case MsgType::kUnbind:
    case MsgType::kHeartbeat:
      return true;
    default:
      return false;
  }
}

}  // namespace udpproxy
