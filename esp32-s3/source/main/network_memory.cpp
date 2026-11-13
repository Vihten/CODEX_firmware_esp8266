#include "network_memory.hpp"

#include <atomic>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcpip.h"

namespace network_memory {
namespace {
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
Snapshot state;
std::atomic<bool> outstanding{false};

uint32_t saturatingAdd(uint32_t value, uint32_t amount) {
  return amount > UINT32_MAX - value ? UINT32_MAX : value + amount;
}

void countSegments(const tcp_seg *segment, Snapshot &result) {
  for (; segment; segment = segment->next) {
    result.queuedSegments = saturatingAdd(result.queuedSegments, 1);
    // tot_len already includes the complete pbuf chain for this segment.
    if (segment->p) result.queuedBytes = saturatingAdd(result.queuedBytes, segment->p->tot_len);
  }
}

void sample(void *) {
  Snapshot result;
  result.sampledAt = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  // Only the tcpip thread dereferences lwIP's PCB/segment lists. The configured
  // PCB and queue limits bound traversal without allocating a copied list.
  for (const tcp_pcb *pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
    result.active = saturatingAdd(result.active, 1);
    countSegments(pcb->unacked, result);
    countSegments(pcb->unsent, result);
#if TCP_QUEUE_OOSEQ
    countSegments(pcb->ooseq, result);
#endif
  }
  for (const tcp_pcb *pcb = tcp_tw_pcbs; pcb; pcb = pcb->next) {
    result.timeWait = saturatingAdd(result.timeWait, 1);
  }
  result.sampled = true;
  portENTER_CRITICAL(&stateMux);
  result.callbackFailures = state.callbackFailures;
  state = result;
  portEXIT_CRITICAL(&stateMux);
  outstanding.store(false, std::memory_order_release);
}
}  // namespace

void request() {
  if (outstanding.exchange(true, std::memory_order_acquire)) return;
  // lwIP owns its bounded callback message allocation and disposal. The adapter
  // allocates nothing and never has more than one queued/running callback.
  if (tcpip_try_callback(sample, nullptr) != ERR_OK) {
    portENTER_CRITICAL(&stateMux);
    state.callbackFailures = saturatingAdd(state.callbackFailures, 1);
    portEXIT_CRITICAL(&stateMux);
    outstanding.store(false, std::memory_order_release);
  }
}

Snapshot snapshot() {
  portENTER_CRITICAL(&stateMux);
  const Snapshot result = state;
  portEXIT_CRITICAL(&stateMux);
  return result;
}

}  // namespace network_memory
