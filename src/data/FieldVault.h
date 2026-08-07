
#pragma once

#include <Arduino.h>

// Compact persistent JSONL evidence for boot/crash/upload/enrichment field runs.
// Not a debug log: keep records short, append-only, and tolerant of torn tails.
namespace FieldVault {

bool begin();
bool isReady();
uint32_t nextSeq();

// Pass empty nowIsoOrEmpty when wall-clock time is unavailable.
bool appendBoot(uint8_t resetReason,
                const char* resetName,
                uint32_t freeHeapKb,
                uint32_t pendingUploads,
                const char* sessionId,
                const char* nowIsoOrEmpty,
                bool usbSerialAttached);

// Cold boot safety net for crash-like resets with no USB serial attached.
bool appendSeriallessResetCrashIfNeeded(uint8_t resetReason,
                                        const char* resetName,
                                        const char* sessionId,
                                        const char* nowIsoOrEmpty,
                                        const char* powerSource,
                                        uint32_t freeHeapKb,
                                        uint32_t pendingUploads,
                                        bool usbSerialAttached);

// Vault at most one newest unresolved crash breadcrumb per sequence.
bool vaultUnresolvedCrashIfNew(uint8_t resetReason,
                               const char* resetName,
                               const char* sessionId,
                               const char* nowIsoOrEmpty,
                               bool usbSerialAttached);

// Sparse battery/run telemetry for field characterization.
bool appendPowerSample(uint16_t voltageMv,
                       int percent,
                       int16_t trendMvPerMin,
                       uint16_t capacityMah,
                       uint16_t runtimeMin,
                       const char* powerSource,
                       const char* powerState,
                       bool charging,
                       uint8_t radioOwner,
                       uint32_t uptimeMs,
                       const char* reason);

// Whole-run capture/upload/enrich/radio snapshot.
bool appendRunSample(const char* sessionId,
                     uint32_t uptimeMs,
                     uint8_t radioOwner,
                     uint32_t pendingUpload,
                     uint32_t pendingEnrich,
                     uint16_t wifiCount,
                     uint32_t probeCount,
                     uint32_t loraPackets,
                     uint8_t subGhzMode,
                     uint16_t subGhzNodes,
                     bool wioAvailable,
                     bool wioBleProxy,
                     bool wioPhoneConnected,
                     bool uploadActive,
                     uint32_t heapFreeKb,
                     uint32_t internalFreeKb,
                     const char* reason);

// One line per phone/WIO enrichment attempt.
bool appendEnrichSummary(const char* transport,
                         bool success,
                         uint32_t requested,
                         uint32_t applied,
                         uint32_t failed,
                         uint32_t deferred,
                         uint32_t batches,
                         uint32_t xferMs,
                         uint32_t storageMs,
                         uint32_t totalMs,
                         uint32_t pendingUpload,
                         uint32_t pendingEnrich);

// Written after cursor flush/clear so the summary survives to next offload.
bool appendUploadSummary(const char* result,
                         uint32_t published,
                         uint32_t failed,
                         uint32_t queued,
                         uint32_t leaseMs,
                         uint32_t pendingUpload,
                         bool fieldOnly);

// FsAudit hooks: inventory/validation records emitted by maintenance owner.
bool appendFsAuditUnknown(const char* path, uint32_t sizeBytes);

// reason is a short stable token.
bool appendFsAuditInvalid(const char* path,
                          uint32_t sizeBytes,
                          const char* reason);

// action is the verb; detail is action-specific context.
bool appendFsAuditAction(const char* path,
                         const char* action,
                         const char* detail);

// One record per audit pass, even if budget/heap guard aborts.
bool appendFsAuditSummary(uint16_t totalFiles,
                          uint16_t knownValid,
                          uint16_t knownInvalid,
                          uint16_t legacy,
                          uint16_t tmpOrphan,
                          uint16_t unknown,
                          uint32_t bytesScanned,
                          uint32_t durationMs,
                          bool     completed);

// Drain cursor API. MQTTManager owns publishing; failed publishes must not
// advance the cursor.
bool peekNext(char* outLine, size_t size, uint32_t* outRecordEnd);

bool markUploadedThrough(uint32_t recordEnd);

// Use volatile marking while upload radio is active, then flush after release.
bool markUploadedThroughVolatile(uint32_t recordEnd);
bool flushUploadCursor();

// True if there are pending bytes past the current upload watermark.
bool hasPending();

// Read-only serial dump.
void dumpToSerial();

uint32_t uploadedThrough();

// Call only after peekNext() says live records are fully drained.
bool clearLive();

// Broader than clearLive: removes live and .1 backup after a serial dump.
bool clearRetained();

}  // namespace FieldVault
