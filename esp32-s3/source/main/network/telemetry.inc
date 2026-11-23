// Private implementation unit; included only by network.cpp.
int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
bool hexValue(const char *text, size_t minimum, size_t maximum, uint32_t &value) {
  const size_t length = std::strlen(text);
  if (length < minimum || length > maximum) return false;
  value = 0;
  for (size_t i = 0; i < length; ++i) { const int d = hexDigit(text[i]); if (d < 0) return false; value = (value << 4) | uint32_t(d); }
  return true;
}
bool unsignedValue(const char *text, uint64_t maximum, uint64_t &value) {
  if (!text || !*text) return false;
  value = 0;
  for (const char *p = text; *p; ++p) {
    if (*p < '0' || *p > '9' || uint64_t(*p - '0') > maximum || value > (maximum - uint64_t(*p - '0')) / 10) return false;
    value = value * 10 + uint64_t(*p - '0');
  }
  return true;
}
bool percent(const char *text, bool allowUnknown, int &value) {
  if (allowUnknown && !std::strcmp(text, "-1")) { value = -1; return true; }
  uint64_t number;
  if (!unsignedValue(text, 100, number)) return false;
  value = int(number); return true;
}
bool validUtf8(const char *text, size_t length) {
  for (size_t i = 0; i < length;) {
    const uint8_t c = uint8_t(text[i++]);
    if (c < 0x80) { if (c < 0x20 || c == 0x7f) return false; continue; }
    unsigned following; uint32_t point, minimum;
    if (c >= 0xc2 && c <= 0xdf) { following = 1; point = c & 31; minimum = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { following = 2; point = c & 15; minimum = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { following = 3; point = c & 7; minimum = 0x10000; }
    else return false;
    if (i + following > length) return false;
    while (following--) { const uint8_t next = uint8_t(text[i++]); if ((next & 0xc0) != 0x80) return false; point = (point << 6) | (next & 63); }
    if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) || (point >= 0x80 && point <= 0x9f)) return false;
  }
  return true;
}
uint32_t fallbackColor(uint32_t id) {
  static constexpr uint32_t colors[] = {0xff504a, 0xf5d445, 0x4be071, 0x3ad8e9, 0x5575ff, 0xee5bd5, 0xff9845, 0x91f04f};
  return colors[(id ^ (id >> 16)) % 8];
}
int colorHue(uint32_t rgb) {
  const int red = (rgb >> 16) & 255, green = (rgb >> 8) & 255, blue = rgb & 255;
  const int top = std::max({red, green, blue}), delta = top - std::min({red, green, blue});
  if (!delta) return -1;
  int hue = top == red ? 60 * (green - blue) / delta
      : top == green ? 120 + 60 * (blue - red) / delta : 240 + 60 * (red - green) / delta;
  return hue < 0 ? hue + 360 : hue;
}
unsigned hueDistance(uint32_t first, uint32_t second) {
  const int a = colorHue(first), b = colorHue(second);
  if (a < 0 || b < 0) return 180;
  const unsigned distance = unsigned(std::abs(a - b));
  return std::min(distance, 360U - distance);
}
uint32_t hueColor(unsigned hue) {
  const unsigned sector = hue / 60, fraction = (hue % 60) * 255 / 60;
  const unsigned rising = fraction, falling = 255 - fraction;
  switch (sector) {
    case 0: return 0xff0000 | (rising << 8);
    case 1: return (falling << 16) | 0x00ff00;
    case 2: return 0x00ff00 | rising;
    case 3: return (falling << 8) | 0x0000ff;
    case 4: return (rising << 16) | 0x0000ff;
    default: return 0xff0000 | falling;
  }
}
uint32_t distinctColor(uint32_t requested, uint32_t clientId) {
  const uint32_t now = uptimeMs();
  const auto nearest = [&](uint32_t rgb) {
    unsigned distance = 180;
    for (const auto &agent : model.control.agents) {
      if (!agent.present || agent.clientId == clientId || now - agent.seenAt >= kAgentTimeout) continue;
      distance = std::min(distance, hueDistance(rgb, agent.rgb));
    }
    return distance;
  };
  unsigned bestDistance = nearest(requested);
  if (bestDistance >= 35) return requested;
  uint32_t best = requested;
  // Additional hue candidates retain useful separation with eight active owners.
  for (unsigned hue = 0; hue < 360; hue += 10) {
    const uint32_t rgb = hueColor(hue); const unsigned distance = nearest(rgb);
    if (distance > bestDistance) { best = rgb; bestDistance = distance; }
  }
  return best;
}
bool parseTelemetry(const char *packet, AgentInput &agent, bool &legacy, uint8_t &remaining, uint8_t &activity) {
  const size_t length = std::strlen(packet);
  if (!length || length >= 160) return false;
  char buffer[160]; std::memcpy(buffer, packet, length + 1);
  agent = AgentInput{};
  char *fields[8] = {buffer}; size_t count = 1;
  for (size_t i = 0; i < length; ++i) if (buffer[i] == ',') {
    if (count == 8) return false;
    buffer[i] = 0; fields[count++] = buffer + i + 1;
  }
  uint32_t account;
  if (count < 2 || !hexValue(fields[1], 1, 8, account) || account != PROJECT_ACCOUNT_FINGERPRINT) return false;
  int a, r;
  if (!std::strcmp(fields[0], "LFX1")) {
    legacy = false;
    if (length > 64 || count != 5 || std::strlen(fields[1]) != 8 ||
        !hexValue(fields[2], 8, 8, agent.clientId) || !agent.clientId ||
        !percent(fields[3], false, a) || !percent(fields[4], false, r)) return false;
    agent.sourceActivityKnown = true;
    agent.codexActivity = uint8_t(a); agent.claudeActivity = uint8_t(r);
    return true;
  }
  legacy = !std::strcmp(fields[0], "LF2") || !std::strcmp(fields[0], "LF1");
  if (legacy) {
    if (count != 4 || !percent(fields[2], false, r) || !percent(fields[3], false, a)) return false;
    remaining = uint8_t(r); activity = uint8_t(a); return true;
  }
  const bool verifiedProtocol = !std::strcmp(fields[0], "LFA3");
  if ((verifiedProtocol ? count != 8 : (std::strcmp(fields[0], "LFA2") || (count != 5 && count != 7))) ||
      !hexValue(fields[2], 1, 8, agent.clientId) || !agent.clientId || !percent(fields[3], false, a) || !percent(fields[4], true, r)) return false;
  agent.activity = uint8_t(a); agent.remaining = int16_t(r); agent.rgb = fallbackColor(agent.clientId);
  uint32_t quotaAccount = 0;
  if (verifiedProtocol) {
    if (!hexValue(fields[5], 8, 8, quotaAccount)) return false;
    agent.quotaForPairedAccount = quotaAccount == PROJECT_ACCOUNT_FINGERPRINT && r >= 0;
  }
  if (count >= 7) {
    const size_t colorField = verifiedProtocol ? 6 : 5;
    const char *nameHex = fields[colorField + 1];
    if (!hexValue(fields[colorField], 6, 6, agent.rgb)) return false;
    const size_t hexLength = std::strlen(nameHex);
    if (hexLength > 96 || (hexLength & 1)) return false;
    for (size_t i = 0; i < hexLength / 2; ++i) {
      const int high = hexDigit(nameHex[i * 2]), low = hexDigit(nameHex[i * 2 + 1]);
      if (high < 0 || low < 0) return false;
      agent.name[i] = char((high << 4) | low);
    }
    if (!validUtf8(agent.name, hexLength / 2)) return false;
  }
  if (!agent.name[0]) std::snprintf(agent.name, sizeof(agent.name), "%08lX", (unsigned long)agent.clientId);
  return true;
}
bool parseWeeklyTelemetry(const char *packet, weekly::Entry &entry, uint32_t &week,
                          uint64_t *lifetimeTokens = nullptr) {
  const size_t length = std::strlen(packet);
  if (!length || length >= 200) return false;
  entry = weekly::Entry{};
  char buffer[200]; std::memcpy(buffer, packet, length + 1);
  char *fields[9] = {buffer}; size_t count = 1;
  for (size_t i = 0; i < length; ++i) if (buffer[i] == ',') {
    if (count == 9) return false;
    buffer[i] = 0; fields[count++] = buffer + i + 1;
  }
  const bool v2 = !std::strcmp(fields[0], "LFT2");
  const unsigned color = v2 ? 7 : 5, name = color + 1;
  uint32_t account;
  if (count != (v2 ? 9U : 7U) || (!v2 && std::strcmp(fields[0], "LFT1")) ||
      !hexValue(fields[1], 1, 8, account) || account != PROJECT_ACCOUNT_FINGERPRINT ||
      !hexValue(fields[2], 1, 8, entry.clientId) || !entry.clientId ||
      std::strlen(fields[3]) != 8 || !hexValue(fields[color], 6, 6, entry.rgb)) return false;
  week = 0;
  for (const char *c = fields[3]; *c; ++c) {
    if (*c < '0' || *c > '9') return false;
    week = week * 10 + unsigned(*c - '0');
  }
  if (!unsignedValue(fields[4], weekly::maximum, entry.tokens)) return false;
  if (v2) {
    uint64_t all = 0, revision = 0;
    if (!unsignedValue(fields[5], weekly::maximum, all) || all < entry.tokens ||
        !unsignedValue(fields[6], weekly::maxRevision, revision)) return false;
    weekly::setRevision(entry, revision);
    if (lifetimeTokens) *lifetimeTokens = all;
  }
  const size_t nameLength = std::strlen(fields[name]);
  if (nameLength > 96 || (nameLength & 1)) return false;
  for (size_t i = 0; i < nameLength / 2; ++i) {
    const int high = hexDigit(fields[name][i * 2]), low = hexDigit(fields[name][i * 2 + 1]);
    if (high < 0 || low < 0) return false;
    entry.name[i] = char((high << 4) | low);
  }
  if (!validUtf8(entry.name, nameLength / 2)) return false;
  if (!entry.name[0]) std::snprintf(entry.name, sizeof(entry.name), "%08lX", (unsigned long)entry.clientId);
  return true;
}
bool acceptWeeklyTelemetry(const char *packet, char (&ack)[64]) {
  weekly::Entry entry; uint32_t week = 0; uint64_t lifetimeTokens = 0;
  if (model.rankingReadFailed) return false;
  if (!parseWeeklyTelemetry(packet, entry, week, &lifetimeTokens) || !week || week != currentWeek()) return false;
  if (const auto *override = findOverride(entry.clientId)) entry.rgb = override->rgb;
  bool changed = false, allChanged = false;
  if (weekly::v2(entry)) {
    if (!weekly::mergeV2(model.weekly, model.lifetime, week, entry, lifetimeTokens, changed, allChanged)) return false;
  } else {
    // Lifetime's marker persists when the weekly table rolls over.
    for (const auto &all : model.lifetime.entries)
      if (all.clientId == entry.clientId && weekly::v2(all)) {
        std::snprintf(ack, sizeof(ack), "LFT1_ACK,%06lX,%08lX", (unsigned long)model.deviceId, (unsigned long)entry.clientId);
        return true;
      }
    serviceWeekly();
    if (!weekly::merge(model.weekly, week, entry, changed)) return false;
  }
  model.weeklyDirty |= changed;
  model.lifetimeDirty |= allChanged;
  std::snprintf(ack, sizeof(ack), "%s_ACK,%06lX,%08lX", weekly::v2(entry) ? "LFT2" : "LFT1",
      (unsigned long)model.deviceId, (unsigned long)entry.clientId);
  return true;
}
bool acceptTelemetry(const char *packet, char (&ack)[64], uint32_t receivedAt) {
  // Queue latency is bounded; keep long-lived quota ages on the 64-bit boot clock.
  const uint32_t now = uptimeMs();
  const uint64_t quotaNow = uint64_t(esp_timer_get_time() / 1000);
  const uint64_t quotaAt = quotaNow - std::min<uint64_t>(quotaNow, uint32_t(now - receivedAt));
  if (!std::strncmp(packet, "LFR1,", 5)) {
    quota_metadata::Packet incoming;
    if (!quota_metadata::parse(packet, PROJECT_ACCOUNT_FINGERPRINT, incoming)) return false;
    for (size_t i = 0; i < kAgentCapacity; ++i) {
      const auto &agent = model.control.agents[i];
      if (!agent.present || agent.clientId != incoming.clientId || now - agent.seenAt >= kAgentTimeout) continue;
      const bool codex = incoming.provider == quota_provider::Provider::Codex;
      const auto &sample = codex ? model.codexSamples[i] : model.claudeSamples[i];
      if (sample.clientId != incoming.clientId || !sample.fresh(quotaNow, codex ?
          quota_provider::codexFreshMs : quota_provider::claudeFreshMs) ||
          (codex && !agent.quotaForPairedAccount)) return false;
      auto &metadata = codex ? model.codexMetadata[i] : model.claudeMetadata[i];
      metadata.observe(incoming, quotaAt);
      std::snprintf(ack, sizeof(ack), "LFR1_ACK,%06lX,%08lX", (unsigned long)model.deviceId,
          (unsigned long)incoming.clientId);
      return true;
    }
    return false;
  }
  if (!std::strncmp(packet, "LFD1,", 5) || !std::strncmp(packet, "LFD2,", 5)) {
    const auto clock = memorialClock();
    return daily_runtime::accept(packet, clock.valid ? clock.date : 0, model.deviceId, ack);
  }
  if (!std::strncmp(packet, "LFT1,", 5) || !std::strncmp(packet, "LFT2,", 5)) return acceptWeeklyTelemetry(packet, ack);
  if (!std::strncmp(packet, "LFC1,", 5)) {
    quota_provider::Packet incoming;
    if (!quota_provider::parse(packet, PROJECT_ACCOUNT_FINGERPRINT, incoming)) return false;
    for (size_t i = 0; i < kAgentCapacity; ++i) {
      const auto &agent = model.control.agents[i];
      if (!agent.clientId || agent.clientId != incoming.clientId) continue;
      auto &sample = model.claudeSamples[i];
      if (sample.remaining != incoming.remaining || !sample.fresh(quotaNow, quota_provider::claudeFreshMs))
        model.claudeMetadata[i] = {};
      sample = {incoming.clientId, quotaAt, incoming.remaining, incoming.validForMs};
      std::snprintf(ack, sizeof(ack), "LFC1_ACK,%06lX,%08lX", (unsigned long)model.deviceId,
          (unsigned long)incoming.clientId);
      return true;
    }
    return false;
  }
  AgentInput incoming; bool legacy = false; uint8_t remaining = 100, activity = 0;
  if (!parseTelemetry(packet, incoming, legacy, remaining, activity)) return false;
  if (incoming.sourceActivityKnown) {
    for (auto &agent : model.control.agents) {
      if (!agent.present || agent.clientId != incoming.clientId || now - agent.seenAt >= kAgentTimeout) continue;
      agent.sourceActivityKnown = true;
      agent.sourceActivityAt = receivedAt;
      agent.codexActivity = incoming.codexActivity;
      agent.claudeActivity = incoming.claudeActivity;
      std::snprintf(ack, sizeof(ack), "LFX1_ACK,%06lX,%08lX", (unsigned long)model.deviceId,
          (unsigned long)incoming.clientId);
      return true;
    }
    return false;
  }
  if (legacy) {
    model.legacySeen = true; model.legacyAt = receivedAt; model.legacyRemaining = remaining; model.legacyActivity = activity;
    std::snprintf(ack, sizeof(ack), "LF1_ACK,%06lX,%d", (unsigned long)model.deviceId, model.rssi);
  } else {
    size_t slot = kAgentCapacity;
    for (size_t i = 0; i < kAgentCapacity; ++i) if (model.control.agents[i].clientId == incoming.clientId &&
        (model.control.agents[i].present || model.control.agents[i].name[0])) { slot = i; break; }
    if (slot == kAgentCapacity) for (size_t i = 0; i < kAgentCapacity; ++i)
      if (!model.control.agents[i].clientId) { slot = i; break; }
    if (slot == kAgentCapacity) {
      uint32_t oldestAge = 0;
      for (size_t i = 0; i < kAgentCapacity; ++i) {
        const auto &agent = model.control.agents[i];
        const uint32_t age = now - agent.seenAt;
        if ((!agent.present || age >= kAgentTimeout) && (slot == kAgentCapacity || age > oldestAge)) {
          slot = i; oldestAge = age;
        }
      }
    }
    if (slot == kAgentCapacity) return false;
    const auto &previous = model.control.agents[slot];
    model.quotaResetReported |= incoming.quotaForPairedAccount && previous.quotaForPairedAccount &&
        previous.present && previous.clientId == incoming.clientId &&
        now - previous.seenAt < kAgentTimeout && previous.remaining >= 0 &&
        previous.remaining < 100 && incoming.remaining == 100;
    const bool sameColor = previous.present && previous.clientId == incoming.clientId &&
        now - previous.seenAt < kAgentTimeout && model.requestedRgb[slot] == incoming.rgb;
    if (previous.clientId == incoming.clientId && previous.sourceActivityKnown &&
        now - previous.sourceActivityAt < 15000) {
      incoming.sourceActivityKnown = true;
      incoming.sourceActivityAt = previous.sourceActivityAt;
      incoming.codexActivity = previous.codexActivity;
      incoming.claudeActivity = previous.claudeActivity;
    }
    incoming.present = true; incoming.seenAt = receivedAt;
    model.requestedRgb[slot] = incoming.rgb;
    if (const auto *entry = findOverride(incoming.clientId)) incoming.rgb = entry->rgb;
    else incoming.rgb = sameColor ? previous.rgb : distinctColor(incoming.rgb, incoming.clientId);
    if (previous.clientId != incoming.clientId) {
      model.claudeSamples[slot] = {};
      model.codexMetadata[slot] = {};
      model.claudeMetadata[slot] = {};
    }
    if (!incoming.quotaForPairedAccount || model.codexSamples[slot].remaining != incoming.remaining ||
        !model.codexSamples[slot].fresh(quotaNow, quota_provider::codexFreshMs)) model.codexMetadata[slot] = {};
    model.codexSamples[slot] = {incoming.clientId, quotaAt,
        incoming.quotaForPairedAccount ? incoming.remaining : int16_t(-1)};
    model.control.agents[slot] = incoming;
    model.directSeen = true;
    std::snprintf(ack, sizeof(ack), "LFA2_ACK,%06lX,%08lX,%06lX", (unsigned long)model.deviceId,
                  (unsigned long)incoming.clientId, (unsigned long)incoming.rgb);
  }
  model.linked = true;
  return true;
}
void serviceTelemetry() {
  TelemetryRequest request;
  for (unsigned i = 0; i < 4 && xQueueReceive(telemetryQueue, &request, 0) == pdTRUE; ++i) {
    TelemetryReply reply{}; reply.id = request.id;
    reply.accepted = uptimeMs() - request.receivedAt < 500 && acceptTelemetry(request.packet, reply.ack, request.receivedAt);
    xQueueSend(telemetryReplies, &reply, 0);
  }
  if (model.udp < 0) return;
  for (unsigned i = 0; i < 8; ++i) {
    char packet[201]; sockaddr_in peer{}; socklen_t size = sizeof(peer);
    const int received = recvfrom(model.udp, packet, sizeof(packet), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&peer), &size);
    if (received < 0) break;
    if (!received || received >= 200 || std::memchr(packet, 0, received)) continue;
    packet[received] = 0; char ack[64] = {};
    if (acceptTelemetry(packet, ack, uptimeMs())) {
      ++model.udpPackets;
      sendto(model.udp, ack, std::strlen(ack), 0, reinterpret_cast<sockaddr *>(&peer), size);
    }
  }
}
void updateControl() {
  // Scheduled memorial activation and publication must exclude stale watchdog
  // snapshots just like manual commands. Nested maintenance keeps its guard.
  RecoveryOwner recoveryOwner(true); if (!recoveryOwner) return;
  const uint32_t now = uptimeMs();
  model.agentUpdates.expire(now);
  unsigned combined = 0; int remaining = 101; bool direct = false;
  for (auto &agent : model.control.agents) {
    if (agent.present && now - agent.seenAt >= kAgentTimeout) agent.present = false;
    if (!agent.present) continue;
    direct = true; combined += agent.activity;
  }
  const bool legacy = model.legacySeen && now - model.legacyAt < 8000;
  model.control.activityPercent = direct ? uint8_t(std::min(combined, 100U)) : legacy && !model.directSeen ? model.legacyActivity : 0;
  int claudeRemaining = 101;
  const uint64_t quotaNow = uint64_t(esp_timer_get_time() / 1000);
  for (size_t i = 0; i < kAgentCapacity; ++i) {
    if (model.codexSamples[i].clientId != model.control.agents[i].clientId) model.codexSamples[i] = {};
    if (model.claudeSamples[i].clientId != model.control.agents[i].clientId) model.claudeSamples[i] = {};
    if (model.codexSamples[i].fresh(quotaNow, quota_provider::codexFreshMs))
      remaining = std::min(remaining, int(model.codexSamples[i].remaining));
    if (model.claudeSamples[i].fresh(quotaNow, quota_provider::claudeFreshMs))
      claudeRemaining = std::min(claudeRemaining, int(model.claudeSamples[i].remaining));
  }
  const bool codexRecovered = model.codexQuota.observe(remaining, model.quotaResetReported, now);
  // Claude has independent samples and filtering. Provider changes never report a refill.
  model.claudeQuota.observe(claudeRemaining, false, now);
  model.quotaResetReported = false;
  const auto previousProvider = model.control.quotaProvider;
  model.control.quotaProvider = model.quotaRotation.update(
      model.codexQuota.snapshot, model.claudeQuota.snapshot, now, uint32_t(model.control.quotaRotationMinutes) * 60000U);
  if (codexRecovered && previousProvider == model.control.quotaProvider &&
      previousProvider == quota_provider::Provider::Codex) ++model.control.quotaRecoveryEpoch;
  quota_metadata::apply(model.codexQuota.snapshot, model.codexSamples, model.codexMetadata,
      kAgentCapacity, quotaNow, quota_provider::codexFreshMs);
  quota_metadata::apply(model.claudeQuota.snapshot, model.claudeSamples, model.claudeMetadata,
      kAgentCapacity, quotaNow, quota_provider::claudeFreshMs);
  model.control.codexQuota = model.codexQuota.snapshot;
  model.control.claudeQuota = model.claudeQuota.snapshot;
  const auto &selected = model.control.quotaProvider == quota_provider::Provider::Claude
      ? model.control.claudeQuota : model.control.codexQuota;
  if (selected.known) model.control.remainingPercent = uint8_t(selected.remaining);
  model.control.quotaKnown = selected.known;
  model.control.quotaFresh = selected.fresh;
  model.control.quotaAvailable = model.quotaRotation.available(model.control.quotaProvider, selected, now);
  model.control.connected = model.connected; model.control.accountLinked = model.linked;
  model.control.brightness = model.settings.brightness; model.control.on = scheduledOn();
  model.control.lightFirst = model.light.first;
  model.control.lightSecond = model.light.second;
  model.control.lightFollowMatrix = model.light.followMatrix;
  model.control.lightForcedOff = model.restarting;
  model.control.airAlertLevel = uint8_t(model.air.effective(now, model.airSettings.enabled,
      model.connected, !model.airFetchFailed));
  model.control.airAlertBlink = model.city.active() &&
      (model.airSettings.enabled || model.city.demo) &&
      air_alert::active(air_alert::Level(model.control.airAlertLevel));
  const auto clock = memorialClock();
  model.memorial.update(uptimeMs(), clock, model.memorialEnabled, model.control.on);
  model.control.memorialActive = model.memorial.active;
  model.control.memorialScheduled = model.memorial.preparing || (model.memorial.active && !model.memorial.manual);
  model.control.memorialStartedAt = model.memorial.startedAt;
  model.control.memorialDurationMs = model.memorial.durationMs;
  if (model.blackHolePreview && uint32_t(now - model.blackHolePreviewStartedAt) >= 60000U)
    model.blackHolePreview = false;
  model.control.blackHolePreview = model.blackHolePreview;
  model.control.blackHolePreviewStartedAt = model.blackHolePreviewStartedAt;
  // Maintenance and its epoch are deliberately retained in every overwritten snapshot.
  xQueueOverwrite(controlQueue, &model.control);
}
void restoreWeeklyAgents(View &view) {
  // History belongs to the web snapshot, never to live telemetry or the renderer.
  for (const auto &entry : view.weekly.entries) {
    if (!entry.clientId || !entry.name[0]) continue;
    size_t empty = kAgentCapacity;
    bool listed = false;
    for (size_t i = 0; i < kAgentCapacity; ++i) {
      if (view.control.agents[i].clientId == entry.clientId) { listed = true; break; }
      if (!view.control.agents[i].clientId && empty == kAgentCapacity) empty = i;
    }
    if (listed || empty == kAgentCapacity) continue;
    auto &agent = view.control.agents[empty];
    agent = AgentInput{};
    agent.clientId = entry.clientId; agent.rgb = entry.rgb;
    copy(agent.name, entry.name);
    view.overrideColor[empty] = false;
    for (const auto &color : view.savedColors) if (color.used && color.clientId == entry.clientId) {
      agent.rgb = color.rgb; view.overrideColor[empty] = true; break;
    }
  }
}
void overlayRanking(weekly::Store &store) {
  for (auto &entry : store.entries) {
    if (!entry.clientId) continue;
    for (const auto &agent : model.control.agents) {
      if (agent.clientId == entry.clientId && agent.name[0]) {
        copy(entry.name, agent.name);
        entry.rgb = agent.rgb;
        break;
      }
    }
    if (const auto *color = findOverride(entry.clientId)) entry.rgb = color->rgb;
  }
}
void publishView() {
  View view;
  view.agentUpdates = model.agentUpdates;
  view.agentUpdateKeyReady = agentUpdateKey[0] != 0;
  view.weekly = model.weekly;
  view.lifetime = model.lifetime;
  view.lifetimeDirty = model.lifetimeDirty; view.lifetimeSaveFailed = model.lifetimeSaveFailed;
  view.weeklyDirty = model.weeklyDirty; view.weeklySaveFailed = model.weeklySaveFailed;
  overlayRanking(view.weekly);
  overlayRanking(view.lifetime);
  view.control = model.control; view.render = model.render;
  view.clockRetries = model.clockRecovery.retries;
  copy(view.ssid, model.settings.ssid); copy(view.ap, model.apName); copy(view.message, model.message); copy(view.ota, model.ota);
  view.deviceId = model.deviceId; view.rssi = model.rssi; view.power = model.settings.power;
  view.start = model.settings.start; view.end = model.settings.end; view.days = model.settings.days;
  view.scheduleEnabled = model.settings.scheduleEnabled; view.dirty = model.dirty; view.nvsReady = model.nvsReady;
  view.light = model.light; view.lightReady = external_light::ready(); view.lightAllowed = model.lightAllowed;
  view.buzzerReady = buzzer::ready();
  view.buzzerVolume = model.buzzerVolume; view.buzzerDirty = model.buzzerDirty; view.buzzerSaveFailed = model.buzzerSaveFailed;
  view.lightDirty = model.lightDirty; view.lightSaveFailed = model.lightSaveFailed;
  view.dirty = view.dirty || model.lightDirty || model.buzzerDirty || model.skyColorsDirty;
  view.restarting = model.restarting; view.maintenanceTimeouts = model.maintenanceTimeouts; view.udpPackets = model.udpPackets;
  view.memorialEnabled = model.memorialEnabled; view.memorialManual = model.memorial.manual;
  view.memorialSaveFailed = model.memorialSaveFailed; view.dirty = view.dirty || model.memorialDirty;
  fillAirAlertView(view); view.dirty = view.dirty || model.airDirty;
  view.otaDone = model.otaDone; view.otaTotal = model.otaTotal; view.netStack = uxTaskGetStackHighWaterMark(nullptr);
  view.otaCheckedAt = model.otaCheckedAt;
  view.recoveryOfflineMs = model.wifiRecovery.offlineAge(uptimeMs());
  view.recoveryStage = model.wifiRecovery.stage(); view.recoveryStacks = model.wifiRecovery.stacks();
  view.recoveryFaults = model.wifiRecovery.faults(); view.recoveryCode = model.recoveryCode;
  view.networkLoops = model.loops; view.disconnects = model.disconnects; view.disconnectReason = model.disconnectReason;
  for (size_t i = 0; i < kAgentCapacity; ++i) {
    view.overrideColor[i] = findOverride(model.control.agents[i].clientId) != nullptr;
    view.savedColors[i] = model.settings.colors[i];
  }
  restoreWeeklyAgents(view);
  if (xSemaphoreTake(viewMutex, pdMS_TO_TICKS(5)) == pdTRUE) { sharedView = view; xSemaphoreGive(viewMutex); }
}
bool snapshot(View &view) {
  if (xSemaphoreTake(viewMutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  view = sharedView; xSemaphoreGive(viewMutex); return true;
}
