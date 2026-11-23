// Private implementation unit; included only by network.cpp.
void wifiEvent(void *, esp_event_base_t base, int32_t id, void *data) {
  WifiEvent event{};
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) event.kind = WifiEvent::Started;
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    event.kind = WifiEvent::Disconnected; event.reason = static_cast<wifi_event_sta_disconnected_t *>(data)->reason;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    event.kind = WifiEvent::GotIp; event.ip = static_cast<ip_event_got_ip_t *>(data)->ip_info;
  } else return;
  if (xQueueSend(wifiEvents, &event, 0) != pdTRUE) wifiEventDrops.fetch_add(1, std::memory_order_relaxed);
}
bool stationConfig() {
  wifi_config_t station{};
  std::memcpy(station.sta.ssid, model.settings.ssid, std::strlen(model.settings.ssid));
  std::memcpy(station.sta.password, model.settings.password, std::strlen(model.settings.password));
  station.sta.threshold.authmode = model.settings.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  station.sta.pmf_cfg.capable = true; station.sta.pmf_cfg.required = false;
  return esp_wifi_set_config(WIFI_IF_STA, &station) == ESP_OK;
}
bool initializeWifi() {
  if (esp_event_loop_create_default() != ESP_OK) return false;
  model.sta = esp_netif_create_default_wifi_sta(); model.ap = esp_netif_create_default_wifi_ap();
  if (!model.sta || !model.ap) return false;
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT(); config.nvs_enable = false;
  if (esp_wifi_init(&config) != ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) return false;
  if (esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifiEvent, nullptr) != ESP_OK ||
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifiEvent, nullptr) != ESP_OK) return false;
  uint8_t mac[6]; if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) return false;
  model.deviceId = (uint32_t(mac[3]) << 16) | (uint32_t(mac[4]) << 8) | mac[5];
  std::snprintf(model.apName, sizeof(model.apName), "LED-Forge-%06lX", (unsigned long)model.deviceId);
  esp_netif_ip_info_t address{};
  IP4_ADDR(&address.ip, 192, 168, 4, 1); IP4_ADDR(&address.gw, 192, 168, 4, 1); IP4_ADDR(&address.netmask, 255, 255, 255, 0);
  esp_netif_dhcps_stop(model.ap);
  if (esp_netif_set_ip_info(model.ap, &address) != ESP_OK) return false;
  esp_netif_dhcps_start(model.ap);
  wifi_config_t ap{};
  std::memcpy(ap.ap.ssid, model.apName, std::strlen(model.apName)); ap.ap.ssid_len = std::strlen(model.apName);
  std::memcpy(ap.ap.password, "ledforge", 8); ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap.ap.max_connection = 4; ap.ap.channel = 1;
  if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) return false;
  model.wifiInitialized = true;
  if (!stationConfig()) return false;
  model.wifiDriverStarted = esp_wifi_start() == ESP_OK;
  if (!model.wifiDriverStarted || esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) return false;
  model.stationRetryAt = uptimeMs();
  return true;
}
void serviceWifi() {
  const uint32_t now = uptimeMs();
  const uint32_t dropped = wifiEventDrops.load(std::memory_order_relaxed);
  if (dropped != model.wifiEventsDropped) {
    model.wifiEventsDropped = dropped;
    ESP_LOGW(kTag, "Wi-Fi event queue drops=%lu", (unsigned long)dropped);
    diagnostic_log::Snapshot failure; failure.reason = int32_t(dropped);
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, uint16_t(0x0200), failure);
  }
  WifiEvent event;
  bool gotIp = false;
  for (unsigned i = 0; i < 16 && xQueueReceive(wifiEvents, &event, 0) == pdTRUE; ++i) {
    if (event.kind == WifiEvent::GotIp) gotIp = true;
    // GOT_IP is only a hint: queued events can describe an obsolete connection.
    if (event.kind == WifiEvent::Disconnected) {
      diagnostic_log::Snapshot failure; failure.reason = event.reason; failure.attempts = model.stationAttempts;
      diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi,
                         diagnostics::Code::WifiDisconnected, failure);
      model.connected = false; model.stationConnecting = false; model.disconnectReason = event.reason; ++model.disconnects;
      model.rssi = -127; model.wifiNegativeSamples = 0;
      copy(model.control.ip, "192.168.4.1");
      model.stationRetryAt = now + (model.stationAttempts < 3 ? 2000 : 30000);
    }
  }
  // Poll independently of events, which can be dropped while the owner is busy.
  // Debounce negative samples; cached IP alone never proves association.
  const bool sampleDue = !model.wifiSampled || now - model.wifiSampleAt >= 1000;
  if (sampleDue || gotIp) {
    if (sampleDue) { model.wifiSampled = true; model.wifiSampleAt = now; }
    wifi_ap_record_t accessPoint{};
    esp_netif_ip_info_t ip{};
    const bool associated = esp_wifi_sta_get_ap_info(&accessPoint) == ESP_OK;
    const bool usable = associated && model.sta && esp_netif_is_netif_up(model.sta) &&
        esp_netif_get_ip_info(model.sta, &ip) == ESP_OK && ip.ip.addr != 0;
    if (usable) {
      if (!model.connected && model.wifiSampled && model.disconnects) {
        model.recoveryCode = recovery::restoredCode;
        diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, recovery::restoredCode);
      }
      model.connected = true; model.wifiReady = true; model.wifiDriverStarted = true;
      model.stationConnecting = false; model.stationAttempts = 0;
      model.wifiNegativeSamples = 0;
      if (sampleDue) model.rssi = accessPoint.rssi;
      std::snprintf(model.control.ip, sizeof(model.control.ip), IPSTR, IP2STR(&ip.ip));
    } else {
      if (sampleDue && model.wifiNegativeSamples < 3) ++model.wifiNegativeSamples;
      if (model.connected && model.wifiNegativeSamples >= 3) {
        model.connected = false; model.rssi = -127;
        copy(model.control.ip, "192.168.4.1");
        ++model.wifiReconciledLosses; ++model.disconnects;
        diagnostic_log::Snapshot failure; failure.reason = associated ? 1 : 0;
        failure.attempts = model.stationAttempts;
        diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, uint16_t(0x0201), failure);
        model.stationRetryAt = now + (model.stationAttempts < 3 ? 2000 : 30000);
        ESP_LOGW(kTag, "Wi-Fi state reconciled: associated=%u netif/IP unavailable; losses=%lu",
                 unsigned(associated), (unsigned long)model.wifiReconciledLosses);
      }
      // Do not interrupt DHCP or static-IP netif bring-up with repeated connect calls.
      if (associated && !model.connected && !model.stationConnecting && due(now, model.stationRetryAt)) {
        model.stationConnecting = true; model.stationAt = now;
      }
    }
  }
  if (model.stationConnecting && now - model.stationAt >= 20000) {
    diagnostic_log::Snapshot failure; failure.elapsed_ms = now - model.stationAt;
    failure.attempts = model.stationAttempts;
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, uint16_t(0x0204), failure);
    esp_wifi_disconnect(); model.stationConnecting = false;
    model.stationRetryAt = now + 30000;
  }
  if (!model.connected && !model.stationConnecting && model.settings.ssid[0] && due(now, model.stationRetryAt)) {
    const esp_err_t connected = esp_wifi_connect();
    model.stationConnecting = connected == ESP_OK; model.stationAt = now;
    if (connected != ESP_OK) {
      if (connected == ESP_ERR_WIFI_NOT_STARTED) model.wifiDriverStarted = false;
      if (connected == ESP_ERR_WIFI_NOT_INIT || connected == ESP_ERR_WIFI_NOT_STARTED || connected == ESP_ERR_WIFI_CONN)
        model.wifiRecovery.fault();
      diagnostic_log::Snapshot failure; failure.reason = connected; failure.attempts = model.stationAttempts;
      diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, uint16_t(0x0203), failure);
    }
    if (model.stationAttempts < 3) ++model.stationAttempts;
    model.stationRetryAt = now + 30000;
  }
}
void serviceClock() {
  const uint32_t now = uptimeMs();
  const bool enabled = esp_sntp_enabled();
  if (!model.clockRecovery.request(now, model.connected, std::time(nullptr) >= 1700000000, enabled)) return;
  if (enabled) esp_sntp_stop();
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, const_cast<char *>(clock_recovery::servers[model.clockRecovery.serverIndex]));
  esp_sntp_set_time_sync_notification_cb([](struct timeval *) {
    lastClockSyncAt.store(uptimeMs(), std::memory_order_relaxed);
    clockSyncSeen.store(true, std::memory_order_release);
  });
  esp_sntp_init();
  if (model.clockRecovery.retries)
    ESP_LOGW(kTag, "Clock synchronization retry %lu using server %u",
             (unsigned long)model.clockRecovery.retries, unsigned(model.clockRecovery.serverIndex));
  if (model.clockRecovery.retries == 1 ||
      (model.clockRecovery.retries && model.clockRecovery.retries % 5 == 0)) {
    diagnostic_log::Snapshot failure;
    failure.attempts = model.clockRecovery.retries;
    failure.detail = int32_t(model.clockRecovery.serverIndex);
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::System, uint16_t(0x0107), failure);
  }
}
void serviceWifiRecovery() {
  const uint32_t now = uptimeMs();
  const bool protectedWork = model.control.maintenance != Maintenance::None || model.releasePending ||
      model.restarting || model.manifestReady || model.wifiChanged ||
      model.memorial.active || model.control.memorialActive || model.render.memorialTransition ||
      activeHandlers.load() != 0 || web_auth::busy() || air_alert::busy();
  const auto action = model.wifiRecovery.step(now, {model.settings.ssid[0] != 0 && model.wifiInitialized,
      model.connected, protectedWork, recoveryBudgetAvailable.load() && !model.confirmationPending});
  if (action == recovery::Action::None) return;
  RecoveryOwner recoveryOwner; if (!recoveryOwner) return;
  diagnostic_log::Snapshot event; event.reason = model.disconnectReason;
  event.attempts = model.stationAttempts;
  if (action == recovery::Action::RestartStation) {
    model.recoveryCode = recovery::stackCode;
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, recovery::stackCode, event);
    // AP remains available: remove only STA, then recreate its control block.
    // Do not deinit the driver/netifs or erase RAM/NVS configuration.
    esp_err_t result = esp_wifi_set_mode(WIFI_MODE_AP);
    if (result == ESP_OK) result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result == ESP_OK && !stationConfig()) result = ESP_FAIL;
    if (result == ESP_OK && !model.wifiDriverStarted) {
      result = esp_wifi_start(); model.wifiDriverStarted = result == ESP_OK;
    }
    if (result == ESP_OK) result = esp_wifi_set_ps(WIFI_PS_NONE);
    model.wifiReady = result == ESP_OK;
    model.stationConnecting = false; model.stationAttempts = 0;
    model.stationRetryAt = now + 2000;
    if (result != ESP_OK) {
      if (result != ESP_ERR_NO_MEM) model.wifiRecovery.fault();
      model.recoveryCode = recovery::failedCode; event.reason = result;
      diagnostics::event(diagnostics::Level::Error, diagnostics::Module::Wifi, recovery::failedCode, event);
    }
  } else if (action == recovery::Action::QueueRestart && reserveRecoveryRestart()) {
    model.recoveryRestart = model.restarting = true; model.restartAt = now;
    model.recoveryCode = recovery::queuedCode;
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, recovery::queuedCode, event);
    message("Wi-Fi recovery restart queued; settings preserved.");
  } else {
    model.recoveryCode = recovery::exhaustedCode;
    diagnostics::event(diagnostics::Level::Warning, diagnostics::Module::Wifi, recovery::exhaustedCode, event);
    message("Wi-Fi recovery budget exhausted; AP and connection retries remain available.");
  }
}
void initializeUdp() {
  model.udpRetryAt = uptimeMs() + 5000;
  model.udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (model.udp < 0) {
    diagnostic_log::Snapshot failure; failure.reason = errno;
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0103), failure);
    message("UDP socket unavailable."); return;
  }
  int enabled = 1;
  if (setsockopt(model.udp, SOL_SOCKET, SO_BROADCAST, &enabled, sizeof(enabled)) < 0) {
    diagnostic_log::Snapshot failure; failure.reason = errno; failure.detail = 3;
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0103), failure);
    close(model.udp); model.udp = -1; message("UDP broadcast setup failed.");
    return;
  }
  if (fcntl(model.udp, F_SETFL, O_NONBLOCK) < 0) {
    diagnostic_log::Snapshot failure; failure.reason = errno;
    failure.detail = 1;
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0103), failure);
    close(model.udp); model.udp = -1;
    message("UDP nonblocking setup failed.");
    ESP_LOGE(kTag, "UDP nonblocking setup failed");
    return;
  }
  sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(kUdpPort); address.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(model.udp, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
    diagnostic_log::Snapshot failure; failure.reason = errno; failure.detail = 2;
    diagnostics::event(diagnostics::Level::Error, diagnostics::Module::System, uint16_t(0x0103), failure);
    close(model.udp); model.udp = -1; message("UDP port 4210 is unavailable.");
  }
}
void serviceUdpRetry() {
  if (model.udp < 0 && due(uptimeMs(), model.udpRetryAt)) initializeUdp();
}
void discover() {
  const uint32_t now = uptimeMs();
  if (!model.connected || model.udp < 0 || now - model.discoveryAt < 2000) return;
  model.discoveryAt = now;
  esp_netif_ip_info_t ip{}; if (esp_netif_get_ip_info(model.sta, &ip) != ESP_OK) return;
  sockaddr_in peer{}; peer.sin_family = AF_INET; peer.sin_port = htons(kUdpPort); peer.sin_addr.s_addr = ip.ip.addr | ~ip.netmask.addr;
  char packet[48]; std::snprintf(packet, sizeof(packet), "LF1_HELLO,%06lX,%d", (unsigned long)model.deviceId, model.rssi);
  sendto(model.udp, packet, std::strlen(packet), 0, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
}
