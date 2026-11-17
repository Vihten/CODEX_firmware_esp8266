// Private implementation unit; included only by network.cpp.
// All configuration mutations, flash operations, and telemetry-table writes belong to this task.
void serviceCommands() {
  RecoveryOwner recoveryOwner; if (!recoveryOwner) return;
  NetworkCommand command;
  for (unsigned count = 0; count < 8 && xQueueReceive(commandQueue, &command, 0) == pdTRUE; ++count) {
    switch (command.kind) {
      case CommandKind::AirAlertSave: {
        AirAlertSettings next = model.airSettings;
        next.enabled = command.enabled;
        if (next.enabled != model.airSettings.enabled || model.airSaveFailed) {
          model.airSettings = next; model.airDirty = true;
          ++model.airGeneration; model.airNextAt = uptimeMs();
          model.air.stopDemo(); model.city.stopDemo(); model.cityNextAt = uptimeMs();
        }
        break;
      }
      case CommandKind::AirAlertTest:
        if (model.control.on) {
          model.air.startDemo(command.value == 5 ? air_alert::Level::Yellow : air_alert::Level(command.value), uptimeMs());
          if (command.value == 5) model.city.startDemo(uptimeMs());
          else model.city.stopDemo();
        }
        else message("Матриця вимкнена. Перевірку тривоги не запущено.");
        break;
      case CommandKind::AirAlertStop: model.air.stopDemo(); model.city.stopDemo(); break;
      case CommandKind::BlackHolePlay:
        if (model.control.on && scheduledOn() && !model.restarting &&
            model.control.maintenance == Maintenance::None && !model.memorial.active &&
            !model.control.memorialActive && !model.render.memorialTransition) {
          model.blackHolePreview = true;
          model.blackHolePreviewStartedAt = uptimeMs();
        }
        break;
      case CommandKind::BlackHoleStop:
        model.blackHolePreview = false;
        break;
      case CommandKind::MemorialSave:
        if (model.memorialEnabled != command.enabled || model.memorialSaveFailed) {
          model.memorialEnabled = command.enabled; model.memorialDirty = true;
        }
        break;
      case CommandKind::MemorialPlay:
        if (!scheduledOn()) message("Матриця вимкнена. Спочатку ввімкніть її.");
        else if (!model.memorial.start(uptimeMs())) message("Хвилина мовчання вже відтворюється.");
        break;
      case CommandKind::MemorialStop:
        model.memorial.update(uptimeMs(), memorialClock(), model.memorialEnabled, scheduledOn());
        model.memorial.stop(); break;
      case CommandKind::AgentPoll:
        if (knownAgent(model.control, command.clientId) || (command.agentActionAck && uninstallAcknowledgmentWindow(model.agentUpdates, command.clientId, uptimeMs())))
          model.agentUpdates.poll(command.clientId, command.agentVersion, uptimeMs(), command.agentNonceCounter,
            command.agentFailedVersion, command.agentManagement, command.agentActionAck, command.agentActionResult);
        break;
      case CommandKind::AgentRename:
      case CommandKind::AgentUninstall:
        if (!model.actionSequence) {
          for (unsigned i = 0; i < 12; ++i) model.actionSequence = model.actionSequence * 16 + unsigned(hexDigit(agentUpdateBoot[i]));
        }
        if (knownAgent(model.control, command.clientId) && model.agentUpdates.requestAction(command.clientId,
            command.kind == CommandKind::AgentRename ? agent_update::Action::Rename : agent_update::Action::Uninstall,
            ++model.actionSequence, command.agentName, uptimeMs())) message("Команду надіслано; очікування агента.");
        else message("Канал керування агентом недоступний.");
        break;
      case CommandKind::AgentUpgrade:
        if (knownAgent(model.control, command.clientId) && model.agentUpdates.request(command.clientId, agent_release::version, uptimeMs()))
          message("Agent update queued; waiting for confirmation.");
        else message("Agent update channel is not ready; request not queued.");
        break;
      case CommandKind::Wifi: {
        const bool sameSsid = !std::strcmp(command.ssid, model.settings.ssid);
        if (command.password[0] || !sameSsid) copy(model.settings.password, command.password);
        copy(model.settings.ssid, command.ssid); model.wifiChanged = true; dirty(); break;
      }
      case CommandKind::Power: model.settings.power = command.value; dirty(); break;
      case CommandKind::Brightness: model.settings.brightness = command.value; dirty(); break;
      case CommandKind::ExternalLight:
        if (command.start <= 100 && command.end <= 100 && external_light::ready() &&
            (model.light.first != command.start || model.light.second != command.end || model.light.followMatrix != command.enabled)) {
          model.light.first = uint8_t(command.start); model.light.second = uint8_t(command.end);
          model.light.followMatrix = command.enabled; model.lightDirty = true;
        }
        break;
      case CommandKind::BuzzerTest:
        if (!model.restarting && model.control.maintenance == Maintenance::None)
          (void)buzzer::start(uptimeMs(), model.buzzerVolume);
        else if (!model.restarting && model.control.maintenance == Maintenance::Persist)
          model.buzzerPending = true;
        break;
      case CommandKind::BuzzerVolume:
        if (command.value <= 100 && command.value != model.buzzerVolume) {
          model.buzzerVolume = command.value; model.buzzerDirty = true; model.buzzerDirtyAt = uptimeMs();
        }
        break;
      case CommandKind::Schedule:
        model.settings.scheduleEnabled = command.enabled; model.settings.start = command.start;
        model.settings.end = command.end; model.settings.days = command.days; dirty(); break;
      case CommandKind::QuotaRotation:
        if (command.value >= 1 && command.value <= 60 && model.control.quotaRotationMinutes != command.value) {
          model.control.quotaRotationMinutes = command.value;
          model.skyColorsDirty = true;
        }
        break;
      case CommandKind::SkyColors:
        if (command.codexSkyColor <= 0xffffff && command.claudeSkyColor <= 0xffffff &&
            command.value <= 60 &&
            (model.control.codexSkyColor != command.codexSkyColor || model.control.claudeSkyColor != command.claudeSkyColor ||
             (command.value && model.control.quotaRotationMinutes != command.value))) {
          model.control.codexSkyColor = command.codexSkyColor;
          model.control.claudeSkyColor = command.claudeSkyColor;
          if (command.value) model.control.quotaRotationMinutes = command.value;
          model.skyColorsDirty = true;
        }
        break;
      case CommandKind::Color: {
        size_t agentSlot = kAgentCapacity;
        for (size_t i = 0; i < kAgentCapacity; ++i)
          if ((model.control.agents[i].present || model.control.agents[i].name[0]) && model.control.agents[i].clientId == command.clientId) { agentSlot = i; break; }
        ColorOverride *slot = nullptr;
        for (auto &entry : model.settings.colors) if (entry.used && entry.clientId == command.clientId) { slot = &entry; break; }
        bool historical = false;
        for (const auto &entry : model.weekly.entries)
          if (entry.clientId == command.clientId && entry.name[0]) { historical = true; break; }
        if (agentSlot == kAgentCapacity && !slot && !historical) { message("Unknown agent; waiting for its first heartbeat."); break; }
        if (command.automatic) {
          if (slot) { *slot = {}; dirty(); }
          if (agentSlot != kAgentCapacity) model.control.agents[agentSlot].rgb = distinctColor(model.requestedRgb[agentSlot], command.clientId);
        } else {
          if (!slot) for (auto &entry : model.settings.colors) if (!entry.used) { slot = &entry; break; }
          if (!slot) { message("Color override storage is full; reset an unused override first."); break; }
          *slot = {command.clientId, command.rgb, 1, {0, 0, 0}};
          if (agentSlot != kAgentCapacity) model.control.agents[agentSlot].rgb = command.rgb;
          dirty();
        }
        break;
      }
      case CommandKind::Restart:
        if (!model.restarting) { model.restarting = true; model.restartAt = uptimeMs(); message("Restart queued."); }
        break;
      case CommandKind::Update:
        if (!model.manifestBusy) { model.updateRequested = true; model.otaAt = uptimeMs(); otaMessage("Update check queued"); }
        else message("Update check already running.");
        break;
    }
  }
}
