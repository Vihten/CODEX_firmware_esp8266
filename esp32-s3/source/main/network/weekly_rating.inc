struct RatingRow {
  uint64_t codex = 0, claude = 0;
  uint32_t clientId = 0;
  uint8_t codexDays = 0, claudeDays = 0;
};

struct RatingRows {
  RatingRow rows[2 * daily::kAgents] = {};
  size_t count = 0;
};

RatingRows weeklyRatingRows(const daily_runtime::WeeklySnapshot &snapshot) {
  RatingRows result;
  if (snapshot.codexReady) for (const auto &entry : snapshot.codex.entries) {
    if (!entry.identity || !entry.observedDays) continue;
    result.rows[result.count++] = {entry.tokens, 0, uint32_t(entry.identity), entry.observedDays, 0};
  }
  if (snapshot.claudeReady) for (const auto &entry : snapshot.claude.entries) {
    if (!entry.identity || !entry.observedDays) continue;
    size_t i = 0;
    while (i < result.count && result.rows[i].clientId != entry.identity) ++i;
    if (i == result.count) ++result.count;
    auto &row = result.rows[i];
    row.clientId = uint32_t(entry.identity);
    row.claude = entry.tokens;
    row.claudeDays = entry.observedDays;
  }
  return result;
}

weekly::Entry weeklyRatingAgent(const View &view, uint32_t clientId) {
  for (const auto &agent : view.control.agents) if (agent.present && agent.clientId == clientId) {
    weekly::Entry entry;
    entry.clientId = clientId;
    entry.rgb = agent.rgb;
    std::snprintf(entry.name, sizeof(entry.name), "%s", agent.name);
    return entry;
  }
  for (const auto &reported : view.weekly.entries)
    if (reported.clientId == clientId) return reported;
  weekly::Entry entry;
  entry.clientId = clientId;
  entry.rgb = 0xd400ff;
  std::snprintf(entry.name, sizeof(entry.name), "%08lX", (unsigned long)clientId);
  return entry;
}
