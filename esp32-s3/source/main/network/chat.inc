struct ChatBinding {
  uint32_t id;
  uint8_t key[32];
  char name[49];
};
struct ChatService {
  chat::Relay relay;
  ChatBinding bindings[chat::peers]{};
  std::atomic<unsigned> staged{0}, durable{0};
  SemaphoreHandle_t mutex = nullptr;
  chat::Window ingress;
  char canonical[3584]{}, response[3072]{};
};
StaticSemaphore_t chatMutexStorage;
std::atomic<ChatService *> chatState{nullptr};

void loadChat() {
  if (!model.nvsReady || !agentUpdateBoot[0]) return;
  auto *memory = heap_caps_malloc(sizeof(ChatService), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) return;
  auto *s = new(memory) ChatService{};
  s->mutex = xSemaphoreCreateMutexStatic(&chatMutexStorage);
  nvs_handle_t handle;
  auto status = nvs_open("lf_chat1", NVS_READONLY, &handle);
  if (status == ESP_OK) {
    for (unsigned i = 0; i < chat::peers; ++i) {
      char slot[16]; std::snprintf(slot, sizeof(slot), "pc%u", i);
      size_t bytes = sizeof(ChatBinding);
      auto &b = s->bindings[i];
      status = nvs_get_blob(handle, slot, &b, &bytes);
      if (status == ESP_ERR_NVS_NOT_FOUND) { status = ESP_OK; break; }
      if (status != ESP_OK || bytes != sizeof(b) || !std::memchr(b.name, 0, sizeof(b.name)) ||
          !validUtf8(b.name, std::strlen(b.name)) || s->relay.lookup(b.id) >= 0 ||
          s->relay.enroll(b.id, b.name, b.key) != 200) { status = ESP_FAIL; break; }
      ++s->durable;
    }
    nvs_close(handle);
  } else if (status == ESP_ERR_NVS_NOT_FOUND) status = ESP_OK;
  if (status != ESP_OK || !s->mutex) {
    if (s->mutex) vSemaphoreDelete(s->mutex);
    s->~ChatService(); heap_caps_free(memory);
    return;
  }
  s->staged.store(s->durable.load());
  chatState.store(s);
}

bool chatSavePending() {
  const auto *s = chatState.load();
  return s && s->staged.load() != s->durable.load();
}
bool persistChat() {
  if (!chatSavePending()) return true;
  auto &s = *chatState.load();
  nvs_handle_t handle;
  if (nvs_open("lf_chat1", NVS_READWRITE, &handle) != ESP_OK) return false;
  const unsigned end = s.staged.load();
  bool ok = true;
  for (unsigned i = s.durable.load(); i < end && ok; ++i) {
    char slot[16]; std::snprintf(slot, sizeof(slot), "pc%u", i);
    ok = nvs_set_blob(handle, slot, &s.bindings[i], sizeof(ChatBinding)) == ESP_OK;
  }
  if (ok) ok = nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (ok) s.durable.store(end);
  return ok;
}

bool takeBoardMessage(ChatBoardMessage &message) {
  auto *s = chatState.load();
  if (!s || xSemaphoreTake(s->mutex, 0) != pdTRUE) return false;
  auto *record = s->relay.first(chat::board, maintenanceClockMs());
  if (record) {
    message = record->message;
    record->pending &= ~chat::board;
  }
  xSemaphoreGive(s->mutex);
  return record != nullptr;
}

bool chatMachinePath(const char *uri) {
  for (const char *p : {"/chat/state", "/chat/roster", "/chat/send", "/chat/inbox", "/chat/ack"})
    if (!std::strcmp(uri, p)) return true;
  return false;
}
bool chatId(const char *value, uint32_t &id) {
  if (!hexValue(value, 8, 8, id) || !id) return false;
  return !std::strpbrk(value, "abcdef");
}
bool chatUuid(const char *value) {
  if (std::strlen(value) != 36) return false;
  for (unsigned i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
    else if (hexDigit(value[i]) < 0 || (value[i] >= 'A' && value[i] <= 'F')) return false;
  }
  return true;
}
bool chatText(char *body) {
  // Existing strict UTF-8 validator bans controls. Allow LF as the sole exception.
  char *line = body;
  for (;;) {
    char *end = std::strchr(line, '\n');
    if (!validUtf8(line, end ? size_t(end - line) : std::strlen(line))) return false;
    if (!end) return *body != 0;
    line = end + 1;
  }
}
template<size_t N> bool chatHeader(httpd_req_t *req, const char *name, char (&value)[N]) {
  const size_t size = httpd_req_get_hdr_value_len(req, name);
  value[0] = 0;
  return !size || (size < N && httpd_req_get_hdr_value_str(req, name, value, N) == ESP_OK);
}
int chatTargets(chat::Relay &r, char *list, uint16_t &mask) {
  while (*list) {
    char *comma = std::strchr(list, ',');
    if (comma) *comma = 0;
    uint16_t bit = chat::board;
    if (std::strcmp(list, "board")) {
      uint32_t id;
      if (!chatId(list, id)) return 400;
      int i = r.lookup(id);
      if (i < 0) return 409;
      bit = uint16_t(1u << i);
    }
    if (mask & bit) return 400;
    mask |= bit;
    if (!comma) return 200;
    list = comma + 1;
  }
  return 400;
}

// Inputs are already validated UTF-8 without controls except LF.
template<size_t N> void chatQuote(const char *input, char (&escaped)[N]) {
  size_t n = 0;
  for (; *input && n + 2 < N; ++input) {
    if (*input == '"' || *input == '\\' || *input == '\n') escaped[n++] = '\\';
    escaped[n++] = *input == '\n' ? 'n' : *input;
  }
  escaped[n] = 0;
}
esp_err_t chatFailure(httpd_req_t *req, int status) {
  const char *reason = status == 403 ? "403 Forbidden" : status == 409 ? "409 Conflict" :
      status == 413 ? "413 Payload Too Large" : status == 429 ? "429 Too Many Requests" :
      status == 503 ? "503 Service Unavailable" : "400 Bad Request";
  return error(req, reason, "Chat request declined.");
}
const AgentInput *onlineChatAgent(const ControlFrame &control, uint32_t id, uint32_t now) {
  for (const auto &a : control.agents)
    if (a.present && a.clientId == id && now - a.seenAt < kAgentTimeout) return &a;
  return nullptr;
}
esp_err_t chatEnroll(httpd_req_t *req, ChatService &s, const ControlFrame &control, const char *body) {
  Form form;
  uint32_t id;
  if (!form.parse(body) || form.count != 2 || !chatId(form.get("client"), id) ||
      !lowerHex(form.get("key"), 64)) return chatFailure(req, 400);
  const AgentInput *agent = onlineChatAgent(control, id, uptimeMs());
  if (!agent) return chatFailure(req, 409);
  uint8_t key[32];
  const char *hex = form.get("key");
  for (unsigned i = 0; i < 32; ++i) key[i] = uint8_t(hexDigit(hex[2*i])*16 + hexDigit(hex[2*i+1]));
  int result = s.relay.enroll(id, agent->name, key);
  if (result != 200) return chatFailure(req, result);
  const unsigned i = unsigned(s.relay.lookup(id));
  const bool newBinding = i == s.staged.load();
  if (newBinding) {
    auto &b = s.bindings[i]; b.id = id; copy(b.name, agent->name);
    std::memcpy(b.key, key, sizeof(key));
    s.staged.store(i + 1);
  }
  if (newBinding || i >= s.durable.load()) {
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "Прив’язку прийнято. Поверніться до чату. Контролер зберігає дозвіл.");
  }
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}
esp_err_t chatPost(httpd_req_t *req) {
  HandlerScope scope;
  auto *state = chatState.load();
  if (!state) return chatFailure(req, 503);
  auto &s = *state;
  const uint64_t now = maintenanceClockMs();
  if (!s.ingress.admit(now, 128, 1000)) return chatFailure(req, 429);
  if (req->content_len > chat::textLimit) return chatFailure(req, 413);
  char body[chat::textLimit + 1];
  if (!readBody(req, body, sizeof(body))) return chatFailure(req, 400);
  ControlFrame control;
  if (xSemaphoreTake(viewMutex, pdMS_TO_TICKS(20)) != pdTRUE) return chatFailure(req, 503);
  control = sharedView.control;
  bool blocked = sharedView.restarting || control.maintenance != Maintenance::None;
  xSemaphoreGive(viewMutex);
  if (blocked) return chatFailure(req, 503);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  if (!std::strcmp(req->uri, "/chat/enroll")) return chatEnroll(req, s, control, body);
  if (!chatMachinePath(req->uri)) return chatFailure(req,400);
  if (!std::strcmp(req->uri, "/chat/state")) {
    if (*body) return chatFailure(req, 400);
    std::snprintf(s.response, sizeof(s.response), "{\"boot\":\"%s\"}", agentUpdateBoot);
    return httpd_resp_sendstr(req, s.response);
  }
  char client[9], seq[17], mac[65], id[37], to[114];
  uint32_t sender;
  uint64_t sequence;
  if (!chatHeader(req,"X-Chat-Client",client) || !chatHeader(req,"X-Chat-Seq",seq) ||
      !chatHeader(req,"X-Chat-Mac",mac) || !chatHeader(req,"X-Chat-Id",id) ||
      !chatHeader(req,"X-Chat-To",to) || !chatId(client,sender) || !lowerHex(mac,64) ||
      !unsignedValue(seq,9007199254740991ULL,sequence) || seq[0] == '0') return chatFailure(req,400);
  auto &r = s.relay;
  const int index = r.lookup(sender);
  if (index < 0 || unsigned(index) >= s.durable.load()) return chatFailure(req,403);
  auto &p = r.agents[index];
  std::snprintf(s.canonical,sizeof(s.canonical),"LFCHAT1\n%s\n%s\n%s\n%s\n%s\n%s\n%s",
                agentUpdateBoot,client,seq,req->uri,id,to,body);
  uint8_t digest[32]; char expected[65];
  if (!agentMac(p.key,32,s.canonical,digest)) return chatFailure(req,503);
  hexBytes(digest,expected);
  unsigned diff = 0;
  for (unsigned i=0; i<64; ++i) diff |= unsigned(mac[i] ^ expected[i]);
  if (diff) return chatFailure(req,403);
  if (sequence <= p.sequence) return chatFailure(req,409);
  p.sequence = sequence;
  if (!p.requests.admit(now,60,10000)) return chatFailure(req,429);
  const bool send = !std::strcmp(req->uri,"/chat/send"), ack = !std::strcmp(req->uri,"/chat/ack");
  if ((send || ack ? !chatUuid(id) : *id != 0) || (send ? !chatText(body) || !*to : *body || *to))
    return chatFailure(req,400);
  uint16_t online = chat::board;
  const uint32_t telemetryNow = uptimeMs();
  for (unsigned i=0; i<s.durable.load(); ++i)
    if (onlineChatAgent(control,r.agents[i].id,telemetryNow)) online |= 1u << i;
  int result = 200;
  copy(s.response,"{\"ok\":true}");
  const uint16_t own = uint16_t(1u << index);
  if (!std::strcmp(req->uri,"/chat/roster")) {
    size_t n = std::snprintf(s.response,sizeof(s.response),"{\"self_id\":\"%s\",\"agents\":[",client);
    for (unsigned i=0; i<s.durable.load(); ++i) {
      char name[100]; chatQuote(r.agents[i].name,name);
      n += std::snprintf(s.response+n,sizeof(s.response)-n,"{\"id\":\"%08lX\",\"name\":\"%s\",\"online\":%s},",
          (unsigned long)r.agents[i].id,name,(i == unsigned(index) || (online & (1u<<i))) ? "true" : "false");
    }
    std::snprintf(s.response+n,sizeof(s.response)-n,"{\"id\":\"board\",\"name\":\"Табло\",\"online\":true}]}");
  } else {
    if (xSemaphoreTake(s.mutex,pdMS_TO_TICKS(10)) != pdTRUE) return chatFailure(req,503);
    if (send) {
      uint16_t targets = 0;
      result = chatTargets(r,to,targets);
      if (result == 200) result = r.send(index,targets,online,id,body,now);
    } else if (ack) r.ack(own,id);
    else {
      auto *m = r.first(own,now);
      if (!m) copy(s.response,"{\"messages\":[]}");
      else {
        char name[100], text[1027];
        chatQuote(m->message.name,name); chatQuote(m->message.text,text);
        std::snprintf(s.response,sizeof(s.response),"{\"messages\":[{\"id\":\"%s\",\"sender_id\":\"%08lX\",\"sender_name\":\"%s\",\"text\":\"%s\"}]}",
            m->message.id,(unsigned long)m->message.sender,name,text);
      }
    }
    xSemaphoreGive(s.mutex);
  }
  if (result != 200) return chatFailure(req,result);
  std::snprintf(s.canonical,sizeof(s.canonical),"LFCHAT1-REPLY\n%s\n%s\n%s\n%s",agentUpdateBoot,client,seq,s.response);
  if (!agentMac(p.key,32,s.canonical,digest)) return chatFailure(req,503);
  hexBytes(digest,expected);
  httpd_resp_set_hdr(req,"X-Chat-Reply",expected);
  return httpd_resp_sendstr(req,s.response);
}

esp_err_t chatPairPage(httpd_req_t *req) {
  char query[32], client[9];
  uint32_t id;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      std::strncmp(query, "client=", 7) || !chatId(query + 7, id))
    return chatFailure(req, 400);
  copy(client, query + 7);
  char page[1400];
  std::snprintf(page, sizeof(page),
      "<!doctype html><html lang=uk><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>Підключення чату</title><main><h1>Підключення чату</h1><p>ПК <strong>%s</strong></p>"
      "<form method=post action='/chat/enroll' autocomplete=off>"
      "<input type=hidden name=client value='%s'><label>Ключ чату з цього ПК "
      "<input type=password name=key required minlength=64 maxlength=64 pattern='[0-9a-f]{64}' autocomplete=off></label>"
      "<p><button>Дозволити чат для цього ПК</button></p></form></main></html>", client, client);
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "Referrer-Policy", "same-origin");
  httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'none'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'");
  return httpd_resp_sendstr(req, page);
}
