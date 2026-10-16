#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace agent_update {
constexpr uint32_t freshMs = 60000, requestMs = 600000;
enum class Action : uint8_t { None, Rename, Uninstall };
struct Entry {
  uint64_t version = 0, requested = 0, lastCounter = 0, failed = 0;
  uint32_t clientId = 0, polledAt = 0, requestedAt = 0;
  uint64_t actionId = 0;
  uint32_t actionAt = 0;
  Action action = Action::None;
  uint8_t actionState = 0;
  bool management = false;
  char actionName[49] = {};
};
struct Table {
  Entry entries[12] = {};
  const Entry *find(uint32_t id) const {
    for (const auto &entry : entries) if (id && entry.clientId == id) return &entry;
    return nullptr;
  }
  void poll(uint32_t id, uint64_t version, uint32_t now, uint64_t counter = 0, uint64_t failed = 0,
      bool management = false, uint64_t ack = 0, uint8_t result = 0) {
    if (!id) return;
    Entry *slot = nullptr;
    for (auto &entry : entries) if (entry.clientId == id) { slot = &entry; break; }
    if (!slot) for (auto &entry : entries) if (!entry.clientId) { slot = &entry; break; }
    if (!slot) return;
    if (slot->clientId && version < slot->version) return;
    const bool confirming = ack && ack == slot->actionId && slot->actionState == 1 && result && now - slot->actionAt < requestMs;
    if (counter && (counter < slot->lastCounter || (counter == slot->lastCounter && !confirming))) return;
    if (counter) slot->lastCounter = counter;
    slot->clientId = id; slot->version = version; slot->polledAt = now;
    slot->management = management;
    if (ack && ack == slot->actionId && slot->actionState == 1 && result && now - slot->actionAt < requestMs)
      slot->actionState = result == 1 ? 2 : 3;
    if (failed > slot->failed) slot->failed = failed;
    if (version >= slot->failed) slot->failed = 0;
    if (slot->failed >= slot->requested) slot->requested = 0;
    if (version >= slot->requested) slot->requested = 0;
  }
  uint64_t target(uint32_t id, uint32_t now) const {
    const auto *entry = find(id);
    return entry && entry->requested > entry->version && now - entry->requestedAt < requestMs ? entry->requested : 0;
  }
  bool request(uint32_t id, uint64_t release, uint32_t now) {
    for (auto &entry : entries) if (id && entry.clientId == id &&
        now - entry.polledAt < freshMs && release > entry.version && release > entry.failed && release && entry.actionState != 1) {
      if (!target(id, now)) { entry.requested = release; entry.requestedAt = now; }
      return true;
    }
    return false;
  }
  const Entry *pendingAction(uint32_t id, uint32_t now) const {
    const auto *entry = find(id);
    return entry && entry->actionState == 1 && now - entry->actionAt < requestMs ? entry : nullptr;
  }
  bool requestAction(uint32_t id, Action action, uint64_t actionId, const char *name, uint32_t now) {
    if (!actionId || action == Action::None || !name || std::strlen(name) > 48) return false;
    for (auto &entry : entries) if (id && entry.clientId == id && entry.management &&
        now - entry.polledAt < freshMs && entry.actionState != 1 && !target(id, now)) {
      entry.action = action; entry.actionId = actionId; entry.actionAt = now; entry.actionState = 1;
      std::strncpy(entry.actionName, name, sizeof(entry.actionName) - 1);
      entry.actionName[sizeof(entry.actionName) - 1] = 0;
      return true;
    }
    return false;
  }
  void expire(uint32_t now) {
    for (auto &entry : entries) {
      if (entry.requested && now - entry.requestedAt >= requestMs) entry.requested = 0;
      if (entry.actionState == 1 && now - entry.actionAt >= requestMs) entry.actionState = 4;
    }
  }
};
} // namespace agent_update
