#include "udpproxy/session.hpp"

namespace udpproxy {

void SessionTable::Insert(std::shared_ptr<Session> s) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::string key = ClientKey(s->client_addr);
  by_client_[key] = s;
  by_local_[s->local_port] = s;
  by_room_[s->room_id].insert(key);
}

std::shared_ptr<Session> SessionTable::FindByClient(const sockaddr_in& addr) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = by_client_.find(ClientKey(addr));
  if (it == by_client_.end()) return nullptr;
  return it->second;
}

std::shared_ptr<Session> SessionTable::FindByLocalPort(uint16_t port) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = by_local_.find(port);
  if (it == by_local_.end()) return nullptr;
  return it->second;
}

std::vector<std::shared_ptr<Session>> SessionTable::FindByRoom(uint64_t room_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::shared_ptr<Session>> out;
  auto it = by_room_.find(room_id);
  if (it == by_room_.end()) return out;
  out.reserve(it->second.size());
  for (const auto& key : it->second) {
    auto cit = by_client_.find(key);
    if (cit != by_client_.end()) out.push_back(cit->second);
  }
  return out;
}

bool SessionTable::EraseByClient(const sockaddr_in& addr) {
  std::lock_guard<std::mutex> lock(mu_);
  const std::string key = ClientKey(addr);
  auto it = by_client_.find(key);
  if (it == by_client_.end()) return false;
  auto s = it->second;
  by_client_.erase(it);
  by_local_.erase(s->local_port);
  auto rit = by_room_.find(s->room_id);
  if (rit != by_room_.end()) {
    rit->second.erase(key);
    if (rit->second.empty()) by_room_.erase(rit);
  }
  return true;
}

bool SessionTable::EraseSession(const std::shared_ptr<Session>& s) {
  if (!s) return false;
  return EraseByClient(s->client_addr);
}

std::vector<std::shared_ptr<Session>> SessionTable::ExpireIdle(
    std::chrono::milliseconds idle_timeout,
    std::chrono::steady_clock::time_point now) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::shared_ptr<Session>> expired;
  for (auto& kv : by_client_) {
    if (now - kv.second->last_active >= idle_timeout) {
      expired.push_back(kv.second);
    }
  }
  for (auto& s : expired) {
    const std::string key = ClientKey(s->client_addr);
    by_client_.erase(key);
    by_local_.erase(s->local_port);
    auto rit = by_room_.find(s->room_id);
    if (rit != by_room_.end()) {
      rit->second.erase(key);
      if (rit->second.empty()) by_room_.erase(rit);
    }
  }
  return expired;
}

std::vector<std::shared_ptr<Session>> SessionTable::All() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::shared_ptr<Session>> out;
  out.reserve(by_client_.size());
  for (const auto& kv : by_client_) {
    out.push_back(kv.second);
  }
  return out;
}

size_t SessionTable::Size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return by_client_.size();
}

}  // namespace udpproxy
