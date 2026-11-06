#pragma once
// Boot-only, before any cJSON users or worker tasks. No runtime hook changes.
bool initializeJsonMemory();
