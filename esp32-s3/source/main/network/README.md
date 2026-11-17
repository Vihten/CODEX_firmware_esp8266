# Private network implementation units

`network.cpp` is the sole translation unit. Its unnamed namespace owns the model,
queues, synchronization, recovery guard, and task loop. These `.inc` files contain
the implementation, not forwarding wrappers. They are included once in dependency
order and must not be registered as separate CMake/PlatformIO source files.

| Unit | Responsibility |
| --- | --- |
| `settings_store.inc` | NVS schemas, validation, load/save, and services using that persisted configuration |
| `telemetry.inc` | Packet validation, identity/slot assignment, provider samples/metadata, control and view projection |
| `wifi_manager.inc` | Wi-Fi events/reconciliation, clock recovery, UDP discovery |
| `portal_html.inc` | Streaming HTML templates and private page handlers |
| `portal_api.inc` | HTTP routes, preview worker, JSON status, request parsing and queue submission |
| `commands.inc` | Bounded owner-task command execution |
| `ota.inc` | Manifest worker/results, image verification/install, maintenance barriers and recovery |

This deliberately preserves the existing ownership model: HTTP and Wi-Fi event
workers submit bounded messages; the network task applies model/configuration and
telemetry changes, publishes a mutex-protected view, and honors the renderer's
flash-maintenance barrier. The preview worker reads published state and uses its
existing atomic session controls. Manifest work returns results through its queue.
No worker gains a second copy of model state or a new mutation API.

A shared internal header with exported mutable globals would weaken those private
boundaries without creating independent ownership. Separate compilation should
follow an explicit state/API design if later needed; it is not required to navigate
or maintain these modules. Standalone source generation recursively copies the
entire firmware subtree, so the included implementation units travel with it.
