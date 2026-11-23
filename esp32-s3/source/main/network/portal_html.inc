// Private implementation unit; included only by network.cpp.
class Html {
  httpd_req_t *request_; char buffer_[1024]; size_t size_ = 0; esp_err_t result_ = ESP_OK;
public:
  explicit Html(httpd_req_t *request) : request_(request) {}
  void flush() {
    if (result_ == ESP_OK && size_) result_ = httpd_resp_send_chunk(request_, buffer_, size_);
    size_ = 0;
  }
  void text(const char *text) {
    if (result_ != ESP_OK) return;
    while (*text) { if (size_ == sizeof(buffer_)) flush(); if (result_ != ESP_OK) return; buffer_[size_++] = *text++; }
  }
  void format(const char *format, ...) {
    char value[320]; va_list arguments; va_start(arguments, format);
    const int size = std::vsnprintf(value, sizeof(value), format, arguments); va_end(arguments);
    if (size < 0 || size >= int(sizeof(value))) { result_ = ESP_FAIL; return; }
    text(value);
  }
  void escaped(const char *value) {
    for (; *value; ++value) switch (*value) {
      case '&': text("&amp;"); break; case '<': text("&lt;"); break; case '>': text("&gt;"); break;
      case '"': text("&quot;"); break; case '\'': text("&#39;"); break;
      default: { char character[2] = {*value, 0}; text(character); }
    }
  }
  void file(const char *relativePath) {
    if (result_ != ESP_OK) return;
    FILE *input = openWebFile(relativePath);
    if (!input) { result_ = ESP_FAIL; return; }
    char chunk[256];
    size_t count;
    while (result_ == ESP_OK && (count = std::fread(chunk, 1, sizeof(chunk), input)) != 0) {
      for (size_t i = 0; i < count && result_ == ESP_OK; ++i) {
        if (size_ == sizeof(buffer_)) flush();
        if (result_ == ESP_OK) buffer_[size_++] = chunk[i];
      }
    }
    if (std::ferror(input)) result_ = ESP_FAIL;
    std::fclose(input);
  }
  template <typename Writer> void render(const char *relativePath, Writer writeToken) {
    if (result_ != ESP_OK) return;
    FILE *input = openWebFile(relativePath);
    if (!input) { result_ = ESP_FAIL; return; }
    // Values stream directly and are never reparsed as templates.
    int character;
    while (result_ == ESP_OK && (character = std::fgetc(input)) != EOF) {
      if (character == '{') {
        if (std::fgetc(input) != '{') { result_ = ESP_FAIL; break; }
        unsigned token = 0, digits = 0;
        while ((character = std::fgetc(input)) >= '0' && character <= '9') {
          if (++digits > 3) { result_ = ESP_FAIL; break; }
          token = token * 10 + unsigned(character - '0');
        }
        if (result_ != ESP_OK) break;
        if (!digits || character != '}' || std::fgetc(input) != '}') { result_ = ESP_FAIL; break; }
        writeToken(token);
      } else {
        if (size_ == sizeof(buffer_)) flush();
        if (result_ == ESP_OK) buffer_[size_++] = char(character);
      }
    }
    if (std::ferror(input)) result_ = ESP_FAIL;
    std::fclose(input);
  }
  void attribute(const char *value) {
    if (!std::strcmp(value, "@current")) file("templates/current.html");
    else if (!std::strcmp(value, "@checked")) file("templates/checked.html");
    else if (!std::strcmp(value, "@disabled")) file("templates/disabled.html");
  }
  esp_err_t end() { flush(); if (result_ == ESP_OK) result_ = httpd_resp_send_chunk(request_, nullptr, 0); return result_; }
};
void writeAgentUpgrade(Html &html, const View &view, uint32_t id, bool online) {
  const auto *entry = view.agentUpdates.find(id);
  const bool ready = online && view.agentUpdateKeyReady && agent_release::version && entry &&
      uptimeMs() - entry->polledAt < agent_update::freshMs;
  html.file("templates/agent/update-open.html");
  if (!entry) html.text("Очікування агента з підтримкою оновлень");
  else if (entry->failed && entry->failed >= agent_release::version) html.text("Оновлення не вдалося; збережено попередню версію");
  else if (view.agentUpdates.target(id, uptimeMs())) html.text("Запит у черзі; очікування підтвердження нової версії");
  else if (agent_release::version && entry->version >= agent_release::version) html.text("Установлена актуальна версія");
  else if (!online) html.text("Агент не на зв’язку");
  else if (!agent_release::version) html.text("Підписаний пакет ще не підготовлено");
  else html.format("Версія %llu · доступна %llu", (unsigned long long)entry->version, (unsigned long long)agent_release::version);
  html.render("templates/agent/update-form.html", [&](unsigned token) {
    if (!token) html.format("%08lX", (unsigned long)id);
    else if (token == 1) html.attribute(ready && entry->version < agent_release::version && entry->failed < agent_release::version &&
        !view.agentUpdates.target(id, uptimeMs()) ? "" : "@disabled");
  });
  const bool managementReady = ready && entry->management && entry->actionState != 1 && !view.agentUpdates.target(id, uptimeMs());
  html.file("templates/agent/manage-open.html");
  if (entry && entry->actionState == 1) html.text("Команда в черзі; очікування відповіді агента");
  else if (entry && entry->actionState == 2) html.text("Агент підтвердив виконання");
  else if (entry && entry->actionState == 3) html.text("Не вдалося виконати команду на ПК");
  else if (entry && entry->actionState == 4) html.text("Час очікування минув; команду скасовано");
  else if (!entry || !entry->management) html.text("Для керування потрібна нова версія агента");
  html.render("templates/agent/manage-form.html", [&](unsigned token) {
    if (!token) html.format("%08lX", (unsigned long)id);
    else if (token == 1) html.attribute(managementReady ? "" : "@disabled");
    else if (token == 2) for (const auto &agent : view.control.agents) if (agent.clientId == id) { html.escaped(agent.name); break; }
  });
}
void writeAgentControls(Html &html, const char *name, uint32_t id, uint32_t rgb,
                        bool online, unsigned activity, bool custom, const View *view = nullptr) {
  html.render("templates/agent/heading-open.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%08lX", (unsigned long)id); break;
    }
  });
  html.file("templates/agent/swatch-open.html");
  html.format("%06lX", (unsigned long)rgb);
  html.file("templates/agent/swatch-close.html"); html.escaped(name); html.file("templates/agent/heading-close.html");
  html.render("templates/agent/status.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%s", online ? " online" : ""); break;
      case 1: html.format("%s", online ? portal_strings::online : portal_strings::offline); break;
    }
  });
  html.render("templates/agent/metadata.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%08lX", (unsigned long)id); break;
      case 1: html.format("%s", custom ? "Власний колір" : "Автоматичний колір"); break;
    }
  });
  html.render("templates/agent/activity.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", online ? activity : 0); break;
      case 1: html.format("%u", online ? activity : 0); break;
      case 2: html.format("%u", online ? activity : 0); break;
      case 3: html.format("%06lX", (unsigned long)(rgb & 0xFFFFFFU)); break;
    }
  });
  html.file("templates/agent/color-form-open.html");
  html.render("templates/agent/client-input.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%08lX", (unsigned long)id); break;
    }
  });
  html.render("templates/agent/color-input-open.html", [&](unsigned token) {
    if (!token) html.format("%08lX", (unsigned long)id);
  });
  html.escaped(name);
  html.render("templates/agent/color-input-value.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%06lX", (unsigned long)rgb); break;
      case 1: html.format("%06lX", (unsigned long)rgb); break;
    }
  });
  html.file("templates/agent/controls-close.html");
  if (view) writeAgentUpgrade(html, *view, id, online);
  html.file("templates/summary/section-close.html");
}

void writeAgents(Html &html, const View &view) {
  unsigned connected = 0;
  for (const auto &agent : view.control.agents) connected += agent.present;
  html.render("templates/agents/heading.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", connected); break;
      case 1: html.format("%u", unsigned(kAgentCapacity)); break;
    }
  });
  html.file("templates/shell/snapshot-notice.html");
  html.file("templates/agents/filters.html");
  html.file("templates/agents/list-open.html");
  unsigned known = 0;
  for (const bool online : {true, false}) for (size_t i = 0; i < kAgentCapacity; ++i) {
    const auto &agent = view.control.agents[i];
    if (agent.present != online || (!agent.present && !agent.name[0])) continue;
    ++known;
    writeAgentControls(html, agent.name, agent.clientId, agent.rgb,
                       agent.present, agent.activity, view.overrideColor[i], &view);
  }
  for (const auto &entry : view.savedColors) {
    if (!entry.used) continue;
    bool listed = false;
    for (const auto &agent : view.control.agents)
      if ((agent.present || agent.name[0]) && agent.clientId == entry.clientId) listed = true;
    if (listed) continue;
    char name[24]; std::snprintf(name, sizeof(name), "Агент %08lX", (unsigned long)entry.clientId);
    ++known;
    writeAgentControls(html, name, entry.clientId, entry.rgb, false, 0, true, &view);
  }
  if (!known) html.file("templates/agents/empty.html");
  html.file("templates/agents/list-close.html");
  html.file("templates/agents/filter-footer.html");
  html.file("templates/agents/template-open.html");
  writeAgentControls(html, "", 0, 0x56d39b, false, 0, false, &view);
  html.file("templates/agents/template-close.html");
}

unsigned portalAgentCount(const View &view) {
  unsigned count = 0;
  for (const auto &agent : view.control.agents) count += agent.present;
  return count;
}
const char *portalMatrixState(const View &view) {
  if (!view.render.assetsReady || view.render.ledError) return "Помилка відмальовування";
  if (!view.control.on) return view.render.sceneVisible && view.render.sceneProgress ? "Вимикається" : portal_strings::off;
  if (view.control.memorialActive) return "Хвилина мовчання";
  if (view.control.memorialScheduled) return "Підготовка до хвилини мовчання";
  if (view.render.memorialTransition) return "Повернення до неба";
  if (!view.render.sceneVisible) return "Показ IP-адреси";
  return view.render.sceneProgress < PROJECT_SCENE_DURATION_MS ? "Запускається" : portal_strings::on;
}
const char *portalOtaLabel(const char *value) {
  if (!std::strcmp(value, "Up to date")) return "Встановлено актуальну версію";
  if (!std::strcmp(value, "GitHub manifest older than installed firmware")) return "GitHub повернув старіший маніфест. Перевірка повториться.";
  if (!std::strcmp(value, "Not checked")) return "Перевірку ще не виконано";
  if (!std::strcmp(value, "Manual update check expired before it could start")) return "Час очікування минув. Перевірку не розпочато.";
  if (!std::strcmp(value, "Checking S3 firmware manifest")) return "Перевіряємо GitHub";
  if (!std::strcmp(value, "New firmware verified; waiting for LEDs")) return "Підготовка до встановлення";
  if (!std::strcmp(value, "Firmware verified; restarting")) return "Оновлення перевірено, перезапуск";
  if (!std::strcmp(value, "Update check queued")) return "Перевірка в черзі";
  if (!std::strcmp(value, "Previously rejected firmware; manual check required to retry")) return "Цю версію раніше відхилено. Для повторної спроби запустіть перевірку вручну.";
  if (!std::strcmp(value, "Manifest HTTPS request failed")) return "GitHub тимчасово недоступний";
  if (!std::strcmp(value, "Firmware HTTPS request failed")) return "Не вдалося завантажити прошивку";
  if (!std::strcmp(value, "Firmware transfer or raw SHA-256 verification failed")) return "Помилка передачі або контрольної суми";
  return value;
}
void writePortalStats(Html &html, const View &view) {
  html.file("templates/shell/snapshot-notice.html");
  html.text("<div class='quota-cards' aria-label='Ліміти сервісів'>");
  for (unsigned service = 0; service < 2; ++service) {
    const auto &quota = service ? view.control.claudeQuota : view.control.codexQuota;
    const bool known = quota.known && quota.remaining >= 0 && quota.remaining <= 100;
    const bool current = known && view.control.on && view.control.quotaAvailable &&
        (service ? quota_provider::Provider::Claude : quota_provider::Provider::Codex) == view.control.quotaProvider;
    html.render("templates/stats/quota-card.html", [&](unsigned token) {
      switch (token) {
        case 0: html.text(service ? "claude" : "codex"); break;
        case 1: html.text(service ? "Claude Code" : "Codex"); break;
        case 2: if (!current) html.file("templates/hidden.html"); break;
        case 3: if (known) html.format("%u%%", unsigned(quota.remaining)); else html.text("—"); break;
        case 4: html.format("%u", known ? unsigned(quota.remaining) : 0); break;
        case 5: {
          if (!known) break;
          const time_t epoch = std::time(nullptr);
          const bool resetKnown = quota.resetAt && epoch >= 1700000000;
          if (resetKnown) {
            if (uint64_t(epoch) < quota.resetAt) {
              const uint32_t minutes = (quota.resetAt - uint32_t(epoch) + 59) / 60;
              if (minutes >= 1440)
                html.format("↻ через %lu д %lu год", (unsigned long)(minutes / 1440), (unsigned long)(minutes / 60 % 24));
              else html.format("↻ через %lu:%02lu", (unsigned long)(minutes / 60), (unsigned long)(minutes % 60));
            } else html.text("↻ очікується");
          }
          if (quota.ageSeconds != UINT32_MAX) {
            if (resetKnown) html.text(" · ");
            else html.text("Оновлено ");
            if (quota.ageSeconds < 5) html.text("щойно");
            else if (quota.ageSeconds < 60) html.format("%lu с тому", (unsigned long)quota.ageSeconds);
            else if (quota.ageSeconds < 3600) html.format("%lu хв тому", (unsigned long)(quota.ageSeconds / 60));
            else html.format("%lu год тому", (unsigned long)(quota.ageSeconds / 3600));
          }
          if (!quota.fresh) {
            if (resetKnown || quota.ageSeconds != UINT32_MAX) html.text(" · ");
            html.text(portal_strings::stale);
          }
          break;
        }
        case 6: if (!known) html.file("templates/hidden.html"); break;
        case 7: html.format("#%06lX", (unsigned long)(service ? view.control.claudeSkyColor : view.control.codexSkyColor)); break;
        case 8: html.text(known ? "true" : "false"); break;
        case 9: if (known) html.file("templates/hidden.html"); break;
        case 10: if (known) html.text(quota.remaining <= 5 ? "critical" : quota.remaining < 20 ? "low" : "normal"); break;
      }
    });
  }
  html.text("</div><p class='quota-empty-notice' data-quota-guidance");
  const auto known = [](const auto &quota) { return quota.known && quota.remaining >= 0 && quota.remaining <= 100; };
  const bool codex = known(view.control.codexQuota), claude = known(view.control.claudeQuota);
  if (codex && claude) html.file("templates/hidden.html");
  html.text("><span data-quota-missing>");
  if (!codex || !claude) html.format("Немає даних про ліміт %s.", codex ? "Claude Code" : claude ? "Codex" : "Codex і Claude Code");
  html.text("</span> <a href='/agents#agent-connections'>Перевірити агентів</a></p>");
  html.text("<div class='overview-status'><dl class='stats'><div><dt>Активність</dt><dd data-live='activity'>");
  html.format("%u%%", view.control.activityPercent);
  html.text("</dd></div><div><dt>Агенти на зв’язку</dt><dd><span data-live='agents'>");
  html.format("%u", portalAgentCount(view));
  html.format("</span><span class='stat-capacity'> / %u</span></dd></div>", unsigned(kAgentCapacity));
  html.file("templates/stats/wifi-open.html"); html.text(view.control.connected ? "Підключено" : "Точка доступу");
  html.file("templates/stats/stats-close.html");
}
void writeSchedule(Html &html, const View &view) {
  html.file("templates/schedule/section-open.html");
  const time_t timestamp = std::time(nullptr);
  if (timestamp < 1700000000) html.text("очікуємо NTP");
  else {
    tm local{}; localtime_r(&timestamp, &local);
    char clock[32]; std::strftime(clock, sizeof(clock), "%d.%m.%Y %H:%M", &local); html.text(clock);
  }
  html.file("templates/schedule/form-open.html");
  html.render("templates/schedule/enabled.html", [&](unsigned token) {
    switch (token) {
      case 0: html.attribute(view.scheduleEnabled ? "@checked" : ""); break;
    }
  });
  html.file("templates/schedule/start-input-open.html");
  html.format("%02u:%02u", view.start / 60, view.start % 60);
  html.file("templates/schedule/end-input-open.html");
  html.format("%02u:%02u", view.end / 60, view.end % 60);
  html.file("templates/schedule/days-open.html");
  static constexpr const char *days[] = {"Нд", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};
  for (unsigned day = 1; day <= 7; ++day) {
    const unsigned index = day % 7;
    html.render("templates/schedule/day-checkbox.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", index); break;
      case 1: html.attribute(view.days & (1U << index) ? "@checked" : ""); break;
      case 2: html.format("%s", days[index]); break;
    }
  });
  }
  html.file("templates/schedule/presets.html");
  if (view.power != 0) html.file("templates/schedule/manual-notice.html");
  html.file("templates/schedule/form-close.html");
}
void writeAgentSummaryRow(Html &html, const AgentInput &agent) {
  html.render("templates/summary/agent-open.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%08lX", (unsigned long)agent.clientId); break;
      case 1: html.format("%06lX", (unsigned long)agent.rgb); break;
    }
  });
  html.escaped(agent.name);
  html.file("templates/summary/status-open.html");
  if (agent.present) html.text(" online");
  html.file("templates/summary/status-value-open.html"); html.text(agent.present ? portal_strings::online : portal_strings::offline);
  html.render("templates/summary/activity-close.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", agent.present ? agent.activity : 0); break;
      case 1: html.format("%u", agent.present ? agent.activity : 0); break;
      case 2: html.format("%u", agent.present ? agent.activity : 0); break;
    }
  });
}
void writeAgentSummary(Html &html, const View &view) {
  html.file("templates/summary/section-open.html");
  html.text("<details><summary>Активність агентів</summary>");
  html.file("templates/summary/list-open.html");
  const AgentInput *ordered[kAgentCapacity] = {};
  unsigned known = 0;
  for (const auto &agent : view.control.agents)
    if (agent.present) ordered[known++] = &agent;
  std::sort(ordered, ordered + known, [](const AgentInput *a, const AgentInput *b) {
    return a->activity != b->activity ? a->activity > b->activity : a->clientId < b->clientId;
  });
  for (unsigned i = 0; i < known; ++i) writeAgentSummaryRow(html, *ordered[i]);
  if (!known) html.file("templates/summary/empty.html");
  html.file("templates/agents/list-close.html");
  html.file("templates/summary/template-open.html");
  writeAgentSummaryRow(html, AgentInput{});
  html.file("templates/agents/template-close.html");
  html.text("<a class='summary-link' href='/agents'>Усі агенти</a></details>");
  html.file("templates/summary/section-close.html");
}
void writeOverview(Html &html, const View &view) {
  const uint8_t official = view.airOfficialLevel;
  const bool active = air_alert::active(static_cast<air_alert::Level>(official));
  const auto level = view.airEnabled && (active || view.airFresh) ? official : uint8_t(air_alert::Level::Unknown);
  html.render("templates/overview/air-alert.html", [&](unsigned token) {
    if (token == 0) html.text(airLevelName(level));
    else if (token == 1) html.text(!view.airEnabled ? "Моніторинг вимкнено" :
        level == 2 ? "Ракетна загроза" : level == 1 ? "Дронова загроза" :
        level == 4 ? "Тривога: тип загрози невідомий" :
        level == 0 ? "Тривогу не оголошено" : "Немає актуальних даних");
    else if (token == 2) {
      if (active && !view.airFresh && view.airEnabled) html.text("Останній відомий стан · дані застаріли");
      else if (view.airManual) html.text("На матриці активний ручний тест");
    }
  });
  writePortalStats(html, view);
  html.text("<p class='muted rotation-summary'><span data-quota-rotation-status>Небо чергує сервіси кожні ");
  html.format("%u", unsigned(view.control.quotaRotationMinutes));
  html.text(" хв</span><a class='info-link' href='/settings#settings-appearance' aria-label='Налаштувати чергування сервісів' title='Налаштувати чергування сервісів'><svg width='18' height='18' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' aria-hidden='true'><circle cx='12' cy='12' r='9'/><path d='M12 11v6M12 7v1'/></svg></a></p>");
  html.text("<div class='overview-device'><div class='overview-toolbar' data-overview-toolbar data-device-controls><section class='matrix-quick-controls'><div class='section-head'><h2>Матриця</h2><span class='muted' data-live='matrix'>");
  html.text(portalMatrixState(view)); html.file("templates/overview/power-heading.html");
  html.file("templates/overview/power-form-open.html");
  for (unsigned mode = 0; mode < 3; ++mode)
    html.render("templates/overview/power-button.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%s", mode == 0 ? "auto" : mode == 1 ? "on" : "off"); break;
      case 1: html.format("%s", view.power == mode ? "true" : "false"); break;
      case 2: html.attribute(view.restarting || view.control.maintenance == Maintenance::Ota ? "@disabled" : ""); break;
      case 3: html.format("%s", mode == 0 ? "Авто" : mode == 1 ? "Увімкнути" : "Вимкнути"); break;
    }
  });
  html.file("templates/overview/brightness-form-open.html");
  html.render("templates/overview/brightness-output.html", [&](unsigned token) {
    switch (token) {
      case 0:
        if (view.control.brightness == 1) html.text("&lt;1");
        else html.format("%u", (unsigned(view.control.brightness) * 100 + 127) / 255);
        break;
    }
  });
  html.render("templates/overview/brightness-input.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", view.control.brightness); break;
    }
  });
  html.file("templates/overview/brightness-actions.html");
  html.text("<div class='actions overview-advanced-actions'><a class='button secondary' href='#schedule-panel' data-device-settings='schedule' aria-haspopup='dialog'>Розклад…</a><a class='button secondary' href='#external-light-panel' data-device-settings='lighting' aria-haspopup='dialog'>Підсвітка…</a></div></section></div>");
  html.text("<div data-device-advanced id='schedule-panel'>"); writeSchedule(html, view); html.text("</div>");
  html.text("<div data-device-advanced>");
  html.render("templates/overview/external-light.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", view.light.first); break;
      case 1: html.format("%u", view.light.second); break;
      case 2: html.attribute(view.light.followMatrix ? "@checked" : ""); break;
      case 3: html.text(!view.lightReady ? "Помилка виходів. Підсвітку вимкнено." :
          !view.lightAllowed && (view.light.first || view.light.second) ? "Вимкнено разом із матрицею або на час обслуговування." :
          view.light.first || view.light.second ? portal_strings::on : portal_strings::off); break;
      case 4: html.text(view.lightSaveFailed ? "Не вдалося зберегти. Повторюємо спробу." : view.lightDirty ? "Збереження…" : ""); break;
      case 5: html.attribute(!view.lightReady || view.restarting || view.control.maintenance == Maintenance::Ota ? "@disabled" : ""); break;
    }
  });
  html.text("</div>");
  html.file("templates/overview/matrix-preview.html");
  html.text("</div>");
  writeAgentSummary(html, view);
}
void writeSettings(Html &html, const View &view) {
  html.text("<div class='settings-groups'>");
  html.text("<details class='settings-group' id='settings-appearance'><summary><h2>Вигляд і ліміти</h2></summary><div class='settings-group-body'>");
  html.render("templates/settings/appearance.html", [&](unsigned token) {
    if (token < 2) html.format("#%06lX", (unsigned long)(token == 0 ? view.control.codexSkyColor : view.control.claudeSkyColor));
    else if (token == 2) html.format("%u", unsigned(view.control.quotaRotationMinutes));
  });
  html.render("templates/settings/black-hole.html", [&](unsigned token) {
    const bool active = view.control.blackHolePreview &&
        uint32_t(uptimeMs() - view.control.blackHolePreviewStartedAt) < 60000U;
    if (token == 0) html.text(active ? "Відтворюється" : "");
    else if (token == 1) html.attribute(!view.control.on || view.control.memorialActive ||
        view.render.memorialTransition || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 2) html.attribute(!active || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 3 && !active) html.file("templates/hidden.html");
  });
  html.text("</div></details>");
  html.text("<details class='settings-group' id='settings-alerts'><summary><h2>Сповіщення та звук</h2></summary><div class='settings-group-body'>");
  html.render("templates/settings/air-alert.html", [&](unsigned token) {
    if (token == 0) html.attribute(view.airEnabled ? "@checked" : "");
    else if (token == 1) html.text(view.airStatus);
    else if (token == 3) { html.text(view.airSource); html.text(". "); html.text(view.airDetails); }
    else if (token == 4) html.attribute(!view.control.on || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 5) html.attribute(!view.airManual || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 8) html.text(view.cityDemo ? "Ручний тест блимання та звуку · 60 с" : view.cityReported ? "Є свіже повідомлення про об’єкт над містом" :
        view.cityFailed ? "Джерело Повітряних сил тимчасово недоступне" :
        view.cityFresh ? "Свіжих повідомлень про об’єкти над містом не знайдено" :
        "Очікування даних Повітряних сил");
    else if (token == 9) html.text(view.cityFailed ? "Зв’язок із каналом Повітряних сил втрачено. Підтверджений допис діє до завершення 5 хв від публікації." : "");
    else if (token == 6) {
      if (view.airAge == UINT32_MAX) html.text("Оновлення: ще немає даних");
      else html.format("Оновлено %lu с тому%s", (unsigned long)view.airAge, view.airFresh ? "" : " · дані застаріли");
    } else if (token == 7) {
      if (view.airManual) html.format("Ручний тест · %lu с", (unsigned long)view.airRemaining);
      else html.text("Ручний тест не активний");
    }
  });
  html.text("<section class='section' aria-labelledby='buzzer-heading'><div class='section-head'><h2 id='buzzer-heading'>Звук</h2></div><form id='buzzer-volume-form' method='post' action='/buzzer-volume'><label for='buzzer-volume'>Гучність <output id='buzzer-volume-value' for='buzzer-volume'>");
  html.format("%u%%", unsigned(view.buzzerVolume));
  html.text("</output></label><input id='buzzer-volume' type='range' name='value' min='0' max='100' step='1' value='");
  html.format("%u", unsigned(view.buzzerVolume));
  html.text("' aria-label='Гучність бузера, 0% — беззвучно'><button type='submit'>Зберегти</button></form><p class='muted' data-buzzer-save role='status'>");
  html.text(view.buzzerSaveFailed ? "Не вдалося зберегти. Повторюємо спробу." : view.buzzerDirty ? "Збереження…" : "");
  html.text("</p><form method='post' action='/buzzer-test'><button type='submit' class='secondary' data-buzzer-test ");
  html.attribute(!view.buzzerReady || view.restarting || view.control.maintenance != Maintenance::None ? "@disabled" : "");
  html.text(">Тест звуку</button></form></section>");
  html.render("templates/settings/memorial.html", [&](unsigned token) {
    if (token == 0) html.attribute(view.memorialEnabled ? "@checked" : "");
    else if (token == 1) html.text(view.control.memorialActive ? "Відтворюється" : "Не відтворюється");
    else if (token == 2) html.attribute(!view.control.on || view.control.memorialActive || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 3) html.attribute(!view.control.memorialActive || view.restarting ||
        view.control.maintenance != Maintenance::None ? "@disabled" : "");
    else if (token == 4) html.text(view.memorialSaveFailed ? "Не вдалося зберегти. Спробуйте ще раз." :
        view.dirty ? "Збереження…" : "");
    else if (token == 5) html.text(!view.control.on ? "Матриця вимкнена. Для відтворення ввімкніть її." :
        std::time(nullptr) < 1700000000 ? "Очікування часу NTP. Ручне відтворення доступне." : "");
  });
  html.text("</div></details>");
  html.text("<details class='settings-group' id='settings-network'><summary><h2>Мережа й оновлення</h2></summary><div class='settings-group-body'>");
  html.file("templates/settings/workspace-open.html");
  html.text(view.control.connected ? "Підключено" : "Точка доступу"); html.file("templates/settings/wifi-heading-close.html");
  html.render("templates/settings/ip.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%s", view.control.ip); break;
    }
  });
  html.file("templates/settings/wifi-form-open.html");
  html.escaped(view.ssid);
  html.file("templates/settings/password-input.html");
  html.file("templates/settings/wifi-actions.html");
  html.file("templates/settings/ota-open.html");
  html.escaped(portalOtaLabel(view.ota));
  html.text("</p><p>Останнє успішне порівняння маніфесту: <span data-live='ota-last-compared'>");
  const time_t checked = time_t(view.otaCheckedAt);
  tm comparedUtc{};
  char comparedText[32] = {};
  if (checked >= 1700000000 && gmtime_r(&checked, &comparedUtc) &&
      std::strftime(comparedText, sizeof(comparedText), "%Y-%m-%d %H:%M:%S UTC", &comparedUtc)) html.text(comparedText);
  else html.text("Час невідомий");
  html.text("</span>");
  html.file("templates/settings/ota-progress-open.html");
  if (!view.otaTotal) html.file("templates/hidden.html");
  html.render("templates/settings/ota-progress-value.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", view.otaTotal ? unsigned(uint64_t(view.otaDone) * 100 / view.otaTotal) : 0); break;
    }
  });
  // Prefer the actual build time; legacy fixtures only provide a version ID.
  const uint64_t firmwareVersion = PROJECT_FIRMWARE_VERSION;
#ifdef PROJECT_BUILD_EPOCH
  const time_t firmwareTime = time_t(PROJECT_BUILD_EPOCH);
#else
  const time_t firmwareTime = time_t(firmwareVersion / 1000);
#endif
  tm firmwareUtc{};
  html.format("<p>Дата прошивки: <time data-firmware-date data-firmware-version='%llu' data-build-epoch='%llu'>", (unsigned long long)firmwareVersion, (unsigned long long)firmwareTime);
  if (gmtime_r(&firmwareTime, &firmwareUtc)) {
    char date[32]; std::strftime(date, sizeof(date), "%d.%m.%Y", &firmwareUtc); html.text(date);
  } else html.text("Невідома");
  html.text("</time></p><details><summary>Докладніше про прошивку</summary><p class='muted'>Точний ID: <code>");
  html.format("%llu", (unsigned long long)firmwareVersion);
  html.text("</code></p></details>");
  html.file("templates/settings/ota-actions.html");
  html.text("</div></details>");
  html.text("<details class='settings-group' id='settings-system'><summary><h2>Система та діагностика</h2></summary><div class='settings-group-body'>");
  html.file("templates/settings/system-open.html");
  html.render("templates/settings/device-id.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%06lX", (unsigned long)view.deviceId); break;
    }
  });
  html.file("templates/settings/uptime-open.html");
  const uint32_t seconds = uptimeMs() / 1000;
  html.format("%lu д %02lu:%02lu:%02lu", (unsigned long)(seconds / 86400), (unsigned long)(seconds / 3600 % 24), (unsigned long)(seconds / 60 % 60), (unsigned long)(seconds % 60));
  html.file("templates/settings/heap-open.html");
  html.format("%u КБ", unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024));
  html.render("templates/settings/rssi.html", [&](unsigned token) {
    switch (token) {
      case 0:
        if (view.control.connected && view.rssi != -127) html.format("%d дБм", view.rssi);
        else html.text(portal_strings::missing);
        break;
    }
  });
  html.render("templates/settings/frame-rate.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", unsigned(OUTPUT_FPS)); break;
    }
  });
  html.render("templates/settings/matrix-pin.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%u", unsigned(PROJECT_DATA_PIN)); break;
    }
  });
  html.file("templates/settings/diagnostics.html");
  html.file("templates/settings/maintenance-open.html");
  if (view.restarting) html.file("templates/disabled.html");
  html.file("templates/settings/maintenance-close.html");
  html.text("</div></details>");
  html.text("</div>");
}

void writeTokenCount(Html &html, uint64_t value) {
  char digits[21]; std::snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value);
  const size_t length = std::strlen(digits);
  for (size_t i = 0; i < length; ++i) {
    if (i && (length - i) % 3 == 0) html.text(" ");
    const char digit[2] = {digits[i], 0}; html.text(digit);
  }
}
void writeRanking(Html &html, const View &view, bool lifetime) {
  daily_runtime::WeeklySnapshot snapshot;
  if (!lifetime) {
    const auto clock = memorialClock();
    daily_runtime::weeklySummary(clock.valid ? clock.date : 0, snapshot);
  }
  const bool dailyReady = snapshot.codexReady || snapshot.claudeReady;
  const uint32_t week = snapshot.codex.week ? snapshot.codex.week : snapshot.claude.week;
  auto rating = weeklyRatingRows(snapshot);
  if (lifetime) for (const auto &entry : view.lifetime.entries) {
    if (entry.clientId) rating.rows[rating.count++] = {entry.tokens, 0, entry.clientId, 1, 0};
  }
  std::sort(rating.rows, rating.rows + rating.count, [](const RatingRow &a, const RatingRow &b) {
    if (bool(a.codexDays) != bool(b.codexDays)) return bool(a.codexDays);
    const uint64_t aTokens = a.codexDays ? a.codex : a.claude;
    const uint64_t bTokens = b.codexDays ? b.codex : b.claude;
    return aTokens != bTokens ? aTokens > bTokens : a.clientId < b.clientId;
  });
  uint64_t total = 0;
  for (size_t i = 0; i < rating.count; ++i) total += rating.rows[i].codex;

  html.file(lifetime ? "templates/lifetime/open.html" : "templates/weekly/open.html");
  if (lifetime) html.text("За доступними журналами агентів");
  else if (week) html.format("%02lu.%02lu.%04lu · понеділок–неділя, Київ", (unsigned long)(week % 100),
      (unsigned long)(week / 100 % 100), (unsigned long)(week / 10000));
  else html.text("Очікування щоденних даних і синхронізації часу");
  const unsigned elapsed = snapshot.codex.elapsedDays ? snapshot.codex.elapsedDays : snapshot.claude.elapsedDays;
  if (!lifetime) {
    html.text("</p><p class='muted ranking-note' data-weekly-coverage>");
    if (dailyReady && elapsed) {
      if (!rating.coverageComplete) html.text("Статистика за тиждень поки неповна. ");
      html.text("Дні з даними · Codex: ");
      if (snapshot.codexReady) html.format("%u/%u", unsigned(snapshot.codex.observedDays), elapsed);
      else html.text("немає даних");
      html.text(" · Claude: ");
      if (snapshot.claudeReady) html.format("%u/%u", unsigned(snapshot.claude.observedDays), elapsed);
      else html.text("немає даних");
    }
  }
  html.file(lifetime ? "templates/lifetime/table.html" : "templates/weekly/table.html");
  for (size_t i = 0; i < rating.count; ++i) {
    const auto &row = rating.rows[i];
    const weekly::Entry entry = lifetime ? *std::find_if(std::begin(view.lifetime.entries), std::end(view.lifetime.entries),
        [&](const weekly::Entry &item) { return item.clientId == row.clientId; }) : weeklyRatingAgent(view, row.clientId);
    html.text("<tr><td>");
    if (row.codexDays) html.format("%u", unsigned(i + 1)); else html.text("—");
    html.format("</td><td><span class='weekly-agent-color' style='background:#%06lX' aria-hidden='true'></span>",
        (unsigned long)entry.rgb);
    html.escaped(entry.name);
    if (!lifetime && elapsed) {
      const auto partial = [&](const char *provider, unsigned days) {
        if (days < elapsed) html.format("<small class='agent-coverage'>%s: %u/%u дн.</small>", provider, days, elapsed);
      };
      if (snapshot.codexReady) partial("Codex", row.codexDays);
      if (snapshot.claudeReady) partial("Claude", row.claudeDays);
    }
    html.text("</td><td>");
    if (row.codexDays) writeTokenCount(html, row.codex); else html.text("—");
    if (!lifetime) {
      html.text("</td><td>");
      if (row.claudeDays) writeTokenCount(html, row.claude); else html.text("—");
    }
    html.text("</td><td>");
    if (!row.codexDays) html.text("—");
    else {
      unsigned tenths = 0;
      if (total) tenths = lifetime ? unsigned(1000.0 * double(row.codex) / double(total) + 0.5) :
          unsigned((1000 * row.codex + total / 2) / total);
      if (row.codex && (lifetime ? !tenths : 1000 * row.codex < total)) html.text("&lt;0,1%");
      else {
        html.format("%u", tenths / 10);
        if (tenths % 10) html.format(",%u", tenths % 10);
        html.text("%");
      }
    }
    html.text("</td></tr>");
  }
  html.file(lifetime ? "templates/lifetime/close.html" : "templates/weekly/close.html");
  html.format("<p class='muted' data-%s-empty%s>%s</p></section>", lifetime ? "lifetime" : "weekly",
      rating.count ? " hidden" : "", lifetime ? "Ще немає даних із журналів агентів." : !dailyReady ? "Щоденні дані тимчасово недоступні. Оновіть сторінку." : "Ще немає щоденних даних за цей тиждень.");
}
void writeWeekly(Html &html, const View &view) { writeRanking(html, view, false); }
void writeLifetime(Html &html, const View &) { html.file("templates/history/calendar.html"); }
esp_err_t page(httpd_req_t *request) {
  HandlerScope scope; View view;
  if (!snapshot(view)) return error(request, "503 Service Unavailable", "Controller busy; retry.");
  httpd_resp_set_type(request, "text/html; charset=utf-8"); httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  if (setPortalSecurityHeaders(request) != ESP_OK) return error(request, "503 Service Unavailable", "Web security policy unavailable.");
  const bool agentsPage = !std::strcmp(request->uri, "/agents");
  const bool settingsPage = !std::strcmp(request->uri, "/settings");
  Html html(request);
  html.file("templates/shell/document-head.html");
  // Bootstrap and its watchdog fit in the first HTML chunk: no blocking script fetch.
  html.file("assets/js/boot.js");
  html.file("templates/shell/document-assets.html");
  html.file("templates/shell/shell-open.html");
  html.render("templates/shell/device-meta.html", [&](unsigned token) {
    switch (token) {
      case 0: html.format("%06lX", (unsigned long)view.deviceId); break;
      case 1: html.format("%s", view.control.ip); break;
    }
  });
  html.file("templates/shell/header-status.html");
  html.file("templates/shell/nav-open.html");
  html.render("templates/shell/nav-links.html", [&](unsigned token) {
    switch (token) {
      case 0: html.attribute(!agentsPage && !settingsPage ? "@current" : ""); break;
      case 1: html.attribute(agentsPage ? "@current" : ""); break;
      case 2: html.attribute(settingsPage ? "@current" : ""); break;
    }
  });
  html.file("templates/shell/main-open.html");
  html.text(agentsPage ? "Агенти" : settingsPage ? "Налаштування" : "Огляд");
  html.file("templates/shell/heading-close.html");
  html.text("<div id='session-banner' data-session-banner role='alert' hidden><span>Сеанс завершено. </span><a href='/login'>Увійти знову</a></div>");
  if (view.message[0]) { html.file("templates/shell/notice-open.html"); html.escaped(view.message); html.file("templates/shell/paragraph-close.html"); }
  if (view.dirty) html.file("templates/shell/saving-notice.html");
  if (view.restarting) html.file("templates/shell/restart-notice.html");
  if (!view.render.assetsReady || view.render.ledError) html.file("templates/shell/matrix-error-notice.html");
  if (agentsPage) {
    html.text("<nav class='agent-page-nav' aria-label='Розділи агентів'><a href='#agent-connections'><svg width='18' height='18' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' aria-hidden='true'><rect x='3' y='4' width='18' height='12' rx='2'/><path d='M8 21h8m-4-5v5'/></svg>Підключення</a><a href='#agent-rankings'><svg width='18' height='18' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' aria-hidden='true'><path d='M4 21V11h5v10m1 0V3h5v18m1 0V7h5v14'/></svg>Рейтинг</a><a href='#agent-history'><svg width='18' height='18' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' aria-hidden='true'><rect x='3' y='5' width='18' height='16' rx='2'/><path d='M16 3v4M8 3v4M3 11h18'/></svg>Історія</a></nav>");
    html.text("<section id='agent-connections' class='section'>"); writeAgents(html, view); html.text("</section>");
    html.text("<div id='agent-rankings'>"); writeWeekly(html, view); html.text("</div>");
    html.text("<div id='agent-history'>"); writeLifetime(html, view); html.text("</div>");
  }
  else if (settingsPage) writeSettings(html, view);
  else writeOverview(html, view);
  html.file("templates/shell/footer.html");
  if (agentsPage) html.text("<script src='/assets/js/token-history.js' defer></script>");
  if (!agentsPage && !settingsPage) html.file("templates/shell/preview-script.html");
  html.file("templates/shell/document-close.html");
  return html.end();
}
esp_err_t deviceImage(httpd_req_t *request) {
  HandlerScope scope;
  return serveWebFile(request, "images/device.webp", "image/webp");
}
esp_err_t favicon(httpd_req_t *request) {
  HandlerScope scope;
  return serveWebFile(request, "images/favicon.svg", "image/svg+xml");
}
