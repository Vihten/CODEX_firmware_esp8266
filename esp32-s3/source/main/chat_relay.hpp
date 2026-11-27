#pragma once
#include "chat_board.hpp"
#include <cstddef>
#include <cstring>

namespace chat {
constexpr unsigned peers = 12;
constexpr uint16_t board = 1u << peers;
constexpr uint64_t retentionMs = 600000;
constexpr size_t textLimit = sizeof(ChatBoardMessage::text) - 1;

struct Window {
  uint64_t start = 0;
  unsigned count = 0;
  bool admit(uint64_t now, unsigned limit, uint64_t interval) {
    if (now - start >= interval) { start = now; count = 0; }
    if (count >= limit) return false;
    ++count;
    return true;
  }
};
struct Peer {
  uint32_t id = 0;
  char name[49]{};
  uint8_t key[32]{};
  uint64_t sequence = 0;
  Window requests, sends;
};
struct Record {
  ChatBoardMessage message{};
  uint64_t created = 0, order = 0;
  uint16_t targets = 0, pending = 0;
};
struct Relay {
  Peer agents[peers]{};
  Record messages[24]{};
  uint64_t order = 0;
  int lookup(uint32_t id) const {
    for (unsigned i = 0; i < peers; ++i) if (id && agents[i].id == id) return int(i);
    return -1;
  }
  int enroll(uint32_t id, const char *name, const uint8_t (&key)[32]) {
    if (!id || std::strlen(name) >= sizeof(Peer::name)) return 400;
    int index = lookup(id);
    if (index >= 0) return std::memcmp(agents[index].key, key, sizeof(key)) ? 409 : 200;
    for (auto &p : agents) if (!p.id) {
      p.id = id;
      std::strcpy(p.name, name);
      std::memcpy(p.key, key, sizeof(key));
      return 200;
    }
    return 429;
  }
  void expire(uint64_t now) {
    for (auto &m : messages) if (m.order && now - m.created >= retentionMs) m.order = 0;
  }
  int send(unsigned from, uint16_t targets, uint16_t online, const char *id, const char *body, uint64_t now) {
    if (from >= peers || !agents[from].id || !targets || targets & (1u << from) ||
        targets >= (board << 1) || std::strlen(id) != 36 || !*body || std::strlen(body) > textLimit) return 400;
    expire(now);
    Record *free = nullptr;
    unsigned counts[peers + 1]{};
    for (auto &m : messages) {
      if (!m.order) { free = &m; continue; }
      if (!std::strcmp(id, m.message.id))
        return m.message.sender == agents[from].id && m.targets == targets && !std::strcmp(m.message.text, body) ? 200 : 409;
      for (unsigned i = 0; i <= peers; ++i) if (m.pending & (1u << i)) ++counts[i];
    }
    if (targets & ~online) return 409;
    for (unsigned i = 0; i <= peers; ++i) if ((targets & (1u << i)) && counts[i] >= 8) return 429;
    if (!free || !agents[from].sends.admit(now, 6, 60000)) return 429;
    free->created = now;
    free->order = ++order;
    free->targets = free->pending = targets;
    free->message.sender = agents[from].id;
    std::strcpy(free->message.id, id);
    std::strcpy(free->message.name, agents[from].name);
    std::strcpy(free->message.text, body);
    return 200;
  }
  Record *first(uint16_t target, uint64_t now) {
    expire(now);
    Record *oldest = nullptr;
    for (auto &m : messages) if (m.order && (m.pending & target) && (!oldest || m.order < oldest->order)) oldest = &m;
    return oldest;
  }
  void ack(uint16_t target, const char *id) {
    for (auto &m : messages) if (m.order && !std::strcmp(id, m.message.id)) m.pending &= ~target;
  }
};
static_assert(sizeof(Relay) < 18000, "bounded shared PSRAM relay");
}
