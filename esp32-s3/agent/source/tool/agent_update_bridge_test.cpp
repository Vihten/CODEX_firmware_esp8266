// Compiled by test_agent_update_bridge.py against the production artifact handler.
#include "agent_update.hpp"
#include <cassert>
#include <string>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#define CONFIG_LWIP_IPV6 1

using esp_err_t = int;
struct HandlerScope {};
struct httpd_req_t { const char *uri; uint32_t peer; std::string file; };
namespace agent_release { constexpr uint64_t version = 20261201000000ULL; }
uint64_t now;
uint64_t maintenanceClockMs() { return now; }
uint32_t socketPeer;
enum class Family { IPv4, Mapped, IPv6, Short };
Family family = Family::IPv4;
int httpd_req_to_sockfd(httpd_req_t *req) { socketPeer = req->peer; return 1; }
int testGetPeer(int, sockaddr *address, socklen_t *size) {
  if (family == Family::IPv4) {
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_addr.s_addr = socketPeer;
    std::memcpy(address, &peer, sizeof(peer));
    *size = sizeof(peer);
  } else {
    sockaddr_in6 peer{};
    peer.sin6_family = AF_INET6;
    auto *ip = reinterpret_cast<uint8_t *>(&peer.sin6_addr);
    if (family == Family::IPv6) ip[0] = 0x20;
    else ip[10] = ip[11] = 0xff;
    std::memcpy(ip + 12, &socketPeer, 4);
    std::memcpy(address, &peer, sizeof(peer));
    *size = family == Family::Short ? 4 : sizeof(peer);
  }
  return 0;
}
#define getpeername testGetPeer
int serveWebFile(httpd_req_t *req, const char *path, const char *) { req->file = path; return 200; }
int error(httpd_req_t *, const char *status, const char *) { return std::stoi(status); }
agent_update::ArtifactLeases agentArtifactLeases;
#include "handlers.inc"

void checkFiles(uint32_t ip, const char *url, const char *directory) {
  for (const char *name : {"manifest.json", "package.zip"}) {
    const std::string path = std::string(url) + "/" + name;
    httpd_req_t req{path.c_str(), ip, {}};
    if (!directory) assert(agentUpdateArtifact(&req) == 409 && req.file.empty());
    else {
      assert(agentUpdateArtifact(&req) == 200);
      assert(req.file == std::string(directory) + "/" + name);
    }
  }
}

int main() {
  constexpr uint64_t bridge = agent_update::bridgeVersion, chat = 20261127120000ULL, old = bridge - 1;
  constexpr uint32_t ip = 0x0200000a, other = 0x0300000a;
  agent_update::Table table;
  now = 1;
  table.poll(1, old, 0);
  table.poll(2, chat, 0);
  assert(table.request(1, agent_release::version, 1) && table.request(2, agent_release::version, 1));
  auto offer = [&](uint32_t id, uint64_t installed, uint32_t peer) {
    httpd_req_t poll{"/agent-update-poll", peer, {}};
    const uint64_t next = table.pollTarget(id, installed, 0, now);
    return agent_update::downloadTarget(&poll, installed, next, agent_release::version, grantAgentArtifact);
  };
  checkFiles(ip, "/agent-update", nullptr);
  assert(offer(1, old, ip) == bridge);
  family = Family::Mapped;
  assert(!offer(2, chat, ip) && !offer(2, chat, 0));
  checkFiles(ip, "/agent-update", "agent-update-bridge");
  family = Family::IPv6;
  checkFiles(ip, "/agent-update", nullptr);
  family = Family::Short;
  checkFiles(ip, "/agent-update", nullptr);
  family = Family::IPv4;
  assert(offer(2, chat, other) == agent_release::version);
  checkFiles(other, "/agent-update", "agent-update");
  assert(offer(1, bridge, ip) == agent_release::version);
  table.poll(1, bridge, 2);
  assert(!offer(1, old, ip));
  checkFiles(ip, "/agent-update-final", "agent-update");
  checkFiles(ip, "/agent-update", "agent-update-bridge");
  now += agent_update::ArtifactLeases::durationMs;
  checkFiles(ip, "/agent-update", nullptr);
  assert(offer(2, chat, ip) == agent_release::version);
  checkFiles(ip, "/agent-update", "agent-update");
  table.poll(1, agent_release::version, now);
  assert(!table.target(1, now) && !offer(1, agent_release::version, ip));
  table.expire(agent_update::requestMs + 1);
  assert(!table.target(2, agent_update::requestMs + 1) && table.find(2)->updateExpired);
  table.poll(3, old, now);
  assert(table.request(3, chat, now));
  assert(table.pollTarget(3, old, 0, now) == bridge && !table.pollTarget(3, old, chat, now));
  table.poll(3, old, now, 1, chat);
  assert(!table.target(3, now) && !table.request(3, chat, now));
}
