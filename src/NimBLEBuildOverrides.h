#pragma once

// NimBLE-Arduino includes the framework sdkconfig before its user config.  The
// prebuilt Arduino S3 profile selects internal-only host allocations, which
// leaves too little contiguous internal RAM to bring WiFi back after a field
// enrichment session.  This header is force-included before each project and
// library translation unit so sdkconfig's include guard is established first;
// the NimBLE sources then see these application-specific overrides.
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(BOARD_HAS_PSRAM)
#include "sdkconfig.h"

#ifdef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL
#undef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL
#endif
#ifdef CONFIG_NIMBLE_MEM_ALLOC_MODE_INTERNAL
#undef CONFIG_NIMBLE_MEM_ALLOC_MODE_INTERNAL
#endif
#define CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL 1
#define CONFIG_NIMBLE_MEM_ALLOC_MODE_EXTERNAL 1

// Spectre supports either connection direction but uses one authenticated
// phone link at a time.  Reserving three simultaneous controller/host links
// wastes the internal memory needed by the alternating WiFi capture phase.
#ifdef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#endif
#ifdef CONFIG_NIMBLE_MAX_CONNECTIONS
#undef CONFIG_NIMBLE_MAX_CONNECTIONS
#endif
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1
#define CONFIG_NIMBLE_MAX_CONNECTIONS 1

// The framework default is 5120 bytes.  Receiving a secured enrichment
// notification enters the C++ remote-characteristic callback from this host
// task; with the full central+peripheral GATT database that task could panic
// before the first callback instruction/RTC marker.  Leave explicit room for
// the callback adapter and FIFO producer rather than running at the edge.
#ifdef CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE
#undef CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE
#endif
#ifdef CONFIG_BT_NIMBLE_TASK_STACK_SIZE
#undef CONFIG_BT_NIMBLE_TASK_STACK_SIZE
#endif
#ifdef CONFIG_NIMBLE_TASK_STACK_SIZE
#undef CONFIG_NIMBLE_TASK_STACK_SIZE
#endif
#define CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE 8192
#define CONFIG_BT_NIMBLE_TASK_STACK_SIZE 8192
#define CONFIG_NIMBLE_TASK_STACK_SIZE 8192
#endif
