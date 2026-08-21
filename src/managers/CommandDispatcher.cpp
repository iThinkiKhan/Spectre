#include "CommandDispatcher.h"

#include <Arduino.h>
#include <string.h>

#include "../core/DebugLog.h"
#include "../core/EventBus.h"
#include "../core/ScreenEnum.h"
#include "../core/ScreenNavigation.h"
#include "../core/SpectreState.h"
#include "BLEManager.h"
#include "DashboardStreamer.h"
#include "LogStreamer.h"
#include "MQTTManager.h"
#include "PhoneTransportRouter.h"
#include "PhoneOffloadManager.h"
#include "PowerManager.h"
#include "RadioArbiter.h"
#include "StorageManager.h"
#include "WioNrfAccessory.h"

namespace {
constexpr const char* TAG = "CMD";
}

// Defined in main.cpp.  Reads g_state under STATE_READ_BEGIN/END and packs
// into a PhoneStorageFrameV1 — identical to the data the device notifies on
// the storage characteristic, just request-driven here.
extern PhoneStorageFrameV1 _buildPhoneStorageFrame();
extern void companionRequestEnrichNow();
extern void companionRequestUploadNow();

bool CommandDispatcher::dispatch(const uint8_t* request,
                                 size_t requestLen,
                                 uint8_t* response,
                                 size_t responseCap,
                                 size_t& responseLen) {
    responseLen = 0;
    if (!response || responseCap < PHONE_COMMAND_RESP_HEADER_SIZE) {
        return false;
    }

    if (!request || requestLen < PHONE_COMMAND_REQ_HEADER_SIZE) {
        // Reply with BAD_PAYLOAD using the all-zero header echo.
        responseLen = writeResponseHeader(0, 0,
                                          CMD_STATUS_BAD_PAYLOAD,
                                          0,
                                          response, responseCap);
        return true;
    }

    PhoneCommandRequestV1 req;
    memcpy(&req, request, sizeof(req));

    if (req.version != PHONE_COMMAND_VERSION) {
        responseLen = writeResponseHeader(req.opcode, req.requestId,
                                          CMD_STATUS_BAD_PAYLOAD,
                                          0,
                                          response, responseCap);
        return true;
    }

    uint8_t* payloadOut = response + PHONE_COMMAND_RESP_HEADER_SIZE;
    const size_t payloadCap = responseCap - PHONE_COMMAND_RESP_HEADER_SIZE;
    const size_t effectiveCap =
        (payloadCap > PHONE_COMMAND_PAYLOAD_MAX) ? PHONE_COMMAND_PAYLOAD_MAX
                                                 : payloadCap;

    const uint8_t* reqPayload    = request + PHONE_COMMAND_REQ_HEADER_SIZE;
    const size_t   reqPayloadLen = requestLen - PHONE_COMMAND_REQ_HEADER_SIZE;

    int payloadLen = -1;
    uint8_t status = CMD_STATUS_OK;
    const uint32_t startMs = millis();

    PHONE_OFFLOAD.expireIfStale();

    switch (req.opcode) {
        case CMD_OP_GET_STATUS:
            payloadLen = handleStatus(payloadOut, effectiveCap);
            break;
        case CMD_OP_GET_HEALTH:
            payloadLen = handleHealth(payloadOut, effectiveCap);
            break;
        case CMD_OP_GET_STORAGE:
            payloadLen = handleStorage(payloadOut, effectiveCap);
            break;
        case CMD_OP_GET_WIO_STATUS:
            payloadLen = handleWioStatus(payloadOut, effectiveCap);
            break;
        case CMD_OP_GET_LOG_TAIL:
            payloadLen = handleLogTail(payloadOut, effectiveCap);
            break;
        case CMD_OP_GET_DASHBOARD:
            payloadLen = handleDashboard(payloadOut, effectiveCap);
            break;
        case CMD_OP_START_LOG_STREAM: {
            if (reqPayloadLen < sizeof(CmdStartLogStreamRequestV1) ||
                effectiveCap < sizeof(CmdStartLogStreamResponseV1)) {
                status = CMD_STATUS_BAD_PAYLOAD;
                payloadLen = 0;
                break;
            }
            CmdStartLogStreamRequestV1 startReq;
            memcpy(&startReq, reqPayload, sizeof(startReq));
            const auto result = LOG_STREAMER.start(startReq.lineCap,
                                                   startReq.leaseDurationMs);
            if (!result.ok) {
                status = CMD_STATUS_INTERNAL_ERROR;
                payloadLen = 0;
                break;
            }
            CmdStartLogStreamResponseV1 startResp = {};
            startResp.streamId          = result.streamId;
            startResp.reserved          = 0;
            startResp.grantedLineCap    = result.grantedLineCap;
            startResp.grantedDurationMs = result.grantedDurationMs;
            memcpy(payloadOut, &startResp, sizeof(startResp));
            payloadLen = static_cast<int>(sizeof(startResp));
            break;
        }
        case CMD_OP_STOP_LOG_STREAM: {
            if (reqPayloadLen < sizeof(CmdStopLogStreamRequestV1)) {
                status = CMD_STATUS_BAD_PAYLOAD;
                payloadLen = 0;
                break;
            }
            CmdStopLogStreamRequestV1 stopReq;
            memcpy(&stopReq, reqPayload, sizeof(stopReq));
            LOG_STREAMER.stop(stopReq.streamId, /*flushFinal=*/true, "phone_stop");
            payloadLen = 0;
            break;
        }
        case CMD_OP_START_DASHBOARD_STREAM: {
            if (reqPayloadLen < sizeof(CmdStartDashboardStreamRequestV1) ||
                effectiveCap < sizeof(CmdStartDashboardStreamResponseV1)) {
                status = CMD_STATUS_BAD_PAYLOAD;
                payloadLen = 0;
                break;
            }
            CmdStartDashboardStreamRequestV1 startReq;
            memcpy(&startReq, reqPayload, sizeof(startReq));
            const auto result = DASHBOARD_STREAMER.start(startReq.intervalMs,
                                                         startReq.leaseDurationMs);
            if (!result.ok) {
                status = CMD_STATUS_INTERNAL_ERROR;
                payloadLen = 0;
                break;
            }
            CmdStartDashboardStreamResponseV1 startResp = {};
            startResp.streamId          = result.streamId;
            startResp.reserved          = 0;
            startResp.reserved2         = 0;
            startResp.grantedIntervalMs = result.grantedIntervalMs;
            startResp.grantedDurationMs = result.grantedDurationMs;
            memcpy(payloadOut, &startResp, sizeof(startResp));
            payloadLen = static_cast<int>(sizeof(startResp));
            break;
        }
        case CMD_OP_STOP_DASHBOARD_STREAM: {
            if (reqPayloadLen < sizeof(CmdStopDashboardStreamRequestV1)) {
                status = CMD_STATUS_BAD_PAYLOAD;
                payloadLen = 0;
                break;
            }
            CmdStopDashboardStreamRequestV1 stopReq;
            memcpy(&stopReq, reqPayload, sizeof(stopReq));
            DASHBOARD_STREAMER.stop(stopReq.streamId,
                                    /*flushFinal=*/true, "phone_stop");
            payloadLen = 0;
            break;
        }
        case CMD_OP_ENRICH_NOW:
            status = handleEnrichNow() ? CMD_STATUS_OK : CMD_STATUS_NOT_READY;
            payloadLen = 0;
            break;
        case CMD_OP_UPLOAD_NOW:
            status = handleUploadNow() ? CMD_STATUS_OK : CMD_STATUS_NOT_READY;
            payloadLen = 0;
            break;
        case CMD_OP_TAG_SESSION:
            status = handleTagSession(reqPayload, reqPayloadLen)
                ? CMD_STATUS_OK : CMD_STATUS_BAD_PAYLOAD;
            payloadLen = 0;
            break;
        case CMD_OP_SAVE_LOCATION:
            status = handleSaveLocation(reqPayload, reqPayloadLen)
                ? CMD_STATUS_OK : CMD_STATUS_NOT_READY;
            payloadLen = 0;
            break;
        case CMD_OP_SCREEN_CHANGE:
            status = handleScreenChange(reqPayload, reqPayloadLen)
                ? CMD_STATUS_OK : CMD_STATUS_BAD_PAYLOAD;
            payloadLen = 0;
            break;
        case CMD_OP_DEBRIEF_REQUEST:
            status = handleDebriefRequest() ? CMD_STATUS_OK : CMD_STATUS_INTERNAL_ERROR;
            payloadLen = 0;
            break;
        case CMD_OP_OFFLOAD_BEGIN: {
            if (effectiveCap < sizeof(CmdOffloadBeginResponseV1)) {
                status = CMD_STATUS_PAYLOAD_TOO_BIG;
                payloadLen = 0;
                break;
            }
            CmdOffloadBeginResponseV1 beginResp = {};
            if (!PHONE_OFFLOAD.begin(beginResp)) {
                status = CMD_STATUS_NOT_READY;
                payloadLen = 0;
                break;
            }
            memcpy(payloadOut, &beginResp, sizeof(beginResp));
            payloadLen = static_cast<int>(sizeof(beginResp));
            break;
        }
        case CMD_OP_OFFLOAD_NEXT: {
            if (reqPayloadLen < sizeof(CmdOffloadNextRequestV1)) {
                status = CMD_STATUS_BAD_PAYLOAD;
                payloadLen = 0;
                break;
            }
            CmdOffloadNextRequestV1 nextReq = {};
            memcpy(&nextReq, reqPayload, sizeof(nextReq));
            size_t nextLen = 0;
            if (!PHONE_OFFLOAD.next(nextReq, payloadOut, effectiveCap, nextLen)) {
                status = CMD_STATUS_NOT_READY;
                payloadLen = 0;
                break;
            }
            payloadLen = static_cast<int>(nextLen);
            break;
        }
        case CMD_OP_OFFLOAD_ACK: {
            if (reqPayloadLen < sizeof(CmdOffloadAckRequestV1) ||
                effectiveCap < sizeof(CmdOffloadAckResponseV1)) {
                status = CMD_STATUS_BAD_PAYLOAD;
                payloadLen = 0;
                break;
            }
            CmdOffloadAckRequestV1 ackReq = {};
            CmdOffloadAckResponseV1 ackResp = {};
            memcpy(&ackReq, reqPayload, sizeof(ackReq));
            if (!PHONE_OFFLOAD.ack(ackReq, ackResp)) {
                status = CMD_STATUS_NOT_READY;
                payloadLen = 0;
                break;
            }
            memcpy(payloadOut, &ackResp, sizeof(ackResp));
            payloadLen = static_cast<int>(sizeof(ackResp));
            break;
        }
        case CMD_OP_OFFLOAD_END: {
            if (reqPayloadLen < sizeof(CmdOffloadEndRequestV1)) {
                status = CMD_STATUS_BAD_PAYLOAD;
                payloadLen = 0;
                break;
            }
            CmdOffloadEndRequestV1 endReq = {};
            memcpy(&endReq, reqPayload, sizeof(endReq));
            status = PHONE_OFFLOAD.end(endReq.transferId, "phone_end")
                         ? CMD_STATUS_OK
                         : CMD_STATUS_BAD_PAYLOAD;
            payloadLen = 0;
            break;
        }
        case CMD_OP_WIFI_OFFLOAD_BEGIN:
            status = PHONE_OFFLOAD.startWifiBulk(reqPayload, reqPayloadLen)
                         ? CMD_STATUS_OK : CMD_STATUS_NOT_READY;
            payloadLen = 0;
            break;
        default:
            status = CMD_STATUS_UNKNOWN_OP;
            payloadLen = 0;
            break;
    }

    if (status == CMD_STATUS_OK && payloadLen < 0) {
        status = CMD_STATUS_INTERNAL_ERROR;
        payloadLen = 0;
    }

    const size_t finalPayloadLen = (payloadLen > 0) ? static_cast<size_t>(payloadLen) : 0;
    responseLen = writeResponseHeader(req.opcode, req.requestId,
                                      status, finalPayloadLen,
                                      response, responseCap);

    DLOG_INFO(TAG,
              "op=0x%02x reqid=%u status=%u payload=%u elapsedMs=%lu",
              static_cast<unsigned>(req.opcode),
              static_cast<unsigned>(req.requestId),
              static_cast<unsigned>(status),
              static_cast<unsigned>(finalPayloadLen),
              static_cast<unsigned long>(millis() - startMs));

    return true;
}

size_t CommandDispatcher::writeResponseHeader(uint8_t opcode,
                                              uint16_t requestId,
                                              uint8_t status,
                                              size_t payloadLen,
                                              uint8_t* response,
                                              size_t responseCap) {
    if (!response || responseCap < PHONE_COMMAND_RESP_HEADER_SIZE) {
        return 0;
    }
    PhoneCommandResponseV1 hdr = {};
    hdr.version    = PHONE_COMMAND_VERSION;
    hdr.opcode     = opcode;
    hdr.requestId  = requestId;
    hdr.status     = status;
    hdr.reserved   = 0;
    hdr.payloadLen = static_cast<uint16_t>(payloadLen);
    memcpy(response, &hdr, sizeof(hdr));
    return PHONE_COMMAND_RESP_HEADER_SIZE + payloadLen;
}

int CommandDispatcher::handleStatus(uint8_t* payload, size_t cap) {
    if (cap < sizeof(CmdStatusResponseV1)) return -1;

    CmdStatusResponseV1 out = {};
    out.uptimeMs = millis();

    uint8_t missionProfile = 0;
    Screen  currentScreen  = DEFAULT_GENERAL_SCREEN;
    STATE_READ_BEGIN();
    missionProfile = g_state.activeMissionProfile;
    currentScreen  = g_state.currentScreen;
    STATE_READ_END();

    out.missionProfile = missionProfile;
    out.screenEnum     = static_cast<uint8_t>(currentScreen);
    out.radioOwner     = static_cast<uint8_t>(RADIO_ARB.currentOwner());
    out.transportKind  = static_cast<uint8_t>(PHONE_XPORT.kind());
    // bootMs is "uptime at the moment a reference event happened"; for slice
    // #2 we just echo uptimeMs so the phone can compare consecutive snapshots.
    out.bootMs         = out.uptimeMs;

    memcpy(payload, &out, sizeof(out));
    return static_cast<int>(sizeof(out));
}

int CommandDispatcher::handleHealth(uint8_t* payload, size_t cap) {
    if (cap < sizeof(CmdHealthResponseV1)) return -1;

    const PowerSnapshot p = PowerManager::getInstance().snapshot();

    CmdHealthResponseV1 out = {};
    out.batteryPct  = static_cast<uint8_t>((p.percent < 0) ? 0 :
                                           (p.percent > 100 ? 100 : p.percent));
    out.charging    = p.charging ? 1 : 0;
    out.batteryMv   = p.voltageMv;
    out.freeHeap    = static_cast<uint32_t>(ESP.getFreeHeap());
    out.minFreeHeap = static_cast<uint32_t>(ESP.getMinFreeHeap());
    out.uptimeMs    = millis();

    memcpy(payload, &out, sizeof(out));
    return static_cast<int>(sizeof(out));
}

int CommandDispatcher::handleStorage(uint8_t* payload, size_t cap) {
    if (cap < sizeof(PhoneStorageFrameV1)) return -1;
    const PhoneStorageFrameV1 frame = _buildPhoneStorageFrame();
    memcpy(payload, &frame, sizeof(frame));
    return static_cast<int>(sizeof(frame));
}

int CommandDispatcher::handleWioStatus(uint8_t* payload, size_t cap) {
    if (cap < sizeof(CmdWioStatusResponseV1)) return -1;

    const PhoneTransportState& xport = PHONE_XPORT.state();
    const uint32_t now = millis();

    CmdWioStatusResponseV1 out = {};
    out.transportKind     = static_cast<uint8_t>(xport.kind);
    out.previousKind      = static_cast<uint8_t>(xport.previous);
    out.reserved          = 0;
    out.lastChangeAgeMs   = xport.lastChangeMs ? now - xport.lastChangeMs : 0;
    out.transitions       = xport.transitions;

#if WIO_NRF_ACCESSORY_ENABLED
    const uint32_t lastSeen = WIO_NRF.lastSeenMs();
    out.wioLastSeenAgeMs  = lastSeen ? now - lastSeen : 0;
    out.phoneRssi         = static_cast<int8_t>(WIO_NRF.phoneRssi());
    out.phoneConnected    = WIO_NRF.phoneConnected() ? 1 : 0;
    out.bleProxy          = WIO_NRF.hasBleProxy() ? 1 : 0;
    out.sx1262Present     = WIO_NRF.hasSx1262() ? 1 : 0;
#else
    out.wioLastSeenAgeMs  = 0;
    out.phoneRssi         = 0;
    out.phoneConnected    = 0;
    out.bleProxy          = 0;
    out.sx1262Present     = 0;
#endif

    memcpy(payload, &out, sizeof(out));
    return static_cast<int>(sizeof(out));
}

int CommandDispatcher::handleDashboard(uint8_t* payload, size_t cap) {
    if (cap < sizeof(CmdDashboardSnapshotV1)) return -1;
    CmdDashboardSnapshotV1 out = {};
    if (!populateDashboardSnapshot(out)) return -1;
    memcpy(payload, &out, sizeof(out));
    return static_cast<int>(sizeof(out));
}

bool CommandDispatcher::populateDashboardSnapshot(CmdDashboardSnapshotV1& out) {
    out = {};

    // Single read pass under the state lock so the snapshot is internally
    // consistent.
    uint8_t  missionProfile = 0;
    Screen   currentScreen = DEFAULT_GENERAL_SCREEN;
    uint8_t  companionEnabled = 0;
    uint8_t  companionPhone = 0;
    uint8_t  companionWork = 0;
    bool     bleConnected = false;
    bool     wifiConnected = false;
    int      wifiNetworkCount = 0;
    int      probePacketCount = 0;
    int      pmkidCaptured = 0;
    bool     loraReady = false;
    int      subGhzNodeCount = 0;
    int      loraRssi = 0;
    int      loraSnr = 0;
    int      loraPacketCount = 0;
    bool     uploadActive = false;
    uint16_t uploadPercent = 0;
    uint32_t uploadPublished = 0;
    uint32_t uploadTotal = 0;
    int      sessionNetworks = 0;
    int      sessionDevices = 0;
    int      sessionProbes = 0;
    int      sessionPMKIDs = 0;
    int      sessionDrones = 0;
    int      droneCount = 0;
    bool     droneAlert = false;

    STATE_READ_BEGIN();
    missionProfile = g_state.activeMissionProfile;
    currentScreen = g_state.currentScreen;
    companionEnabled = g_state.companionEnabled;
    companionPhone = g_state.companionPhone;
    companionWork = g_state.companionWork;
    bleConnected = g_state.bleConnected;
    wifiConnected = g_state.wifiConnected;
    wifiNetworkCount = g_state.wifiNetworkCount;
    probePacketCount = g_state.probePacketCount;
    pmkidCaptured = g_state.pmkidCaptured;
    loraReady = g_state.loraReady;
    subGhzNodeCount = g_state.subGhzNodeCount;
    loraRssi = g_state.loraRSSI;
    loraSnr = g_state.loraSNR;
    loraPacketCount = g_state.loraPacketCount;
    uploadActive = g_state.uploadActive;
    uploadPercent = g_state.uploadPercent;
    uploadPublished = g_state.uploadPublished;
    uploadTotal = g_state.uploadTotal;
    sessionNetworks = g_state.sessionNetworks;
    sessionDevices = g_state.sessionDevices;
    sessionProbes = g_state.sessionProbes;
    sessionPMKIDs = g_state.sessionPMKIDs;
    sessionDrones = g_state.sessionDrones;
    droneCount = g_state.droneCount;
    droneAlert = g_state.droneAlert;
    STATE_READ_END();

    auto clampU16 = [](int v) -> uint16_t {
        if (v < 0) return 0;
        if (v > 0xFFFF) return 0xFFFF;
        return static_cast<uint16_t>(v);
    };
    auto clampI16 = [](int v) -> int16_t {
        if (v < INT16_MIN) return INT16_MIN;
        if (v > INT16_MAX) return INT16_MAX;
        return static_cast<int16_t>(v);
    };
    auto clampU32 = [](int v) -> uint32_t {
        return v < 0 ? 0u : static_cast<uint32_t>(v);
    };

    out.uptimeMs           = millis();
    out.missionProfile     = missionProfile;
    out.screenEnum         = static_cast<uint8_t>(currentScreen);
    out.radioOwner         = static_cast<uint8_t>(RADIO_ARB.currentOwner());
    out.transportKind      = static_cast<uint8_t>(PHONE_XPORT.kind());

    out.companionEnabled   = companionEnabled;
    out.companionPhone     = companionPhone;
    out.companionWork      = companionWork;
    out.bleConnected       = bleConnected ? 1 : 0;

    out.wifiConnected      = wifiConnected ? 1 : 0;
    out.wifiNetworkCount   = clampU16(wifiNetworkCount);
    out.probePacketCount   = clampU32(probePacketCount);
    out.pmkidCaptured      = clampU32(pmkidCaptured);

    out.loraReady          = loraReady ? 1 : 0;
    out.subGhzNodeCount    = clampU16(subGhzNodeCount);
    out.loraRssi           = clampI16(loraRssi);
    out.loraSnr            = clampI16(loraSnr);
    out.loraPacketCount    = clampU32(loraPacketCount);

    out.uploadActive       = uploadActive ? 1 : 0;
    // uploadPercent is already 0..100 in g_state; express as basis points
    // so the wire can carry richer values later without a version bump.
    out.uploadPercentBp    = static_cast<uint16_t>(
        uploadPercent > 100 ? 10000 : static_cast<uint16_t>(uploadPercent) * 100);
    out.uploadPublished    = uploadPublished;
    out.uploadTotal        = uploadTotal;

    out.sessionNetworks    = clampU32(sessionNetworks);
    out.sessionDevices     = clampU32(sessionDevices);
    out.sessionProbes      = clampU32(sessionProbes);
    out.sessionPMKIDs      = clampU32(sessionPMKIDs);
    out.sessionDrones      = clampU32(sessionDrones);

    out.droneCount         = clampU16(droneCount);
    out.droneAlert         = droneAlert ? 1 : 0;

    return true;
}

// ── Slice #5 — safe write commands ──────────────────────────────────────────

bool CommandDispatcher::handleEnrichNow() {
    companionRequestEnrichNow();
    DLOG_INFO(TAG, "phone enrich-now requested");
    return true;
}

bool CommandDispatcher::handleUploadNow() {
    // Defer until after the encrypted response has been written. TaskHardware
    // then closes the companion link, builds the upload index, and takes Wi-Fi.
    companionRequestUploadNow();
    DLOG_INFO(TAG, "phone upload-now accepted");
    return true;
}

bool CommandDispatcher::handleTagSession(const uint8_t* payload, size_t len) {
    if (len < sizeof(CmdTagPayloadHeaderV1)) return false;
    const uint8_t tagLen = payload[0];
    if (tagLen == 0 || tagLen > CMD_TAG_PAYLOAD_MAX_LEN) return false;
    if (len < sizeof(CmdTagPayloadHeaderV1) + tagLen) return false;

    char tag[32] = {};
    const size_t copyLen = (tagLen < sizeof(tag) - 1) ? tagLen : sizeof(tag) - 1;
    memcpy(tag, payload + sizeof(CmdTagPayloadHeaderV1), copyLen);
    tag[copyLen] = '\0';

    STATE_WRITE_BEGIN();
    strlcpy(g_state.sessionTag, tag, sizeof(g_state.sessionTag));
    g_state.sessionTagSet = true;
    STATE_WRITE_END();

    DLOG_INFO(TAG, "phone tag-session set tag=%s", tag);
    return true;
}

bool CommandDispatcher::handleSaveLocation(const uint8_t* payload, size_t len) {
    if (len < sizeof(CmdTagPayloadHeaderV1)) return false;
    const uint8_t tagLen = payload[0];
    if (tagLen == 0 || tagLen > CMD_TAG_PAYLOAD_MAX_LEN) return false;
    if (len < sizeof(CmdTagPayloadHeaderV1) + tagLen) return false;

    char tag[24] = {};
    const size_t copyLen = (tagLen < sizeof(tag) - 1) ? tagLen : sizeof(tag) - 1;
    memcpy(tag, payload + sizeof(CmdTagPayloadHeaderV1), copyLen);
    tag[copyLen] = '\0';

    bool gpsOk = false;
    float lat = 0.0f;
    float lon = 0.0f;
    int   count = 0;
    STATE_READ_BEGIN();
    gpsOk = g_state.gpsAvailable;
    lat = g_state.gpsLat;
    lon = g_state.gpsLon;
    count = g_state.knownLocCount;
    STATE_READ_END();

    if (!gpsOk) {
        DLOG_WARN(TAG, "phone save-location refused: no current fix");
        return false;
    }
    if (count >= SpectreState::KNOWN_LOC_COUNT) {
        DLOG_WARN(TAG, "phone save-location refused: slot table full");
        return false;
    }

    SpectreState::KnownLocation locations[SpectreState::KNOWN_LOC_COUNT] = {};
    int newCount = 0;
    STATE_WRITE_BEGIN();
    strlcpy(g_state.knownLocations[count].tag, tag,
            sizeof(g_state.knownLocations[count].tag));
    g_state.knownLocations[count].lat     = lat;
    g_state.knownLocations[count].lon     = lon;
    g_state.knownLocations[count].radiusM = 75.0f;
    g_state.knownLocCount++;
    newCount = g_state.knownLocCount;
    memcpy(locations, g_state.knownLocations, sizeof(locations));
    STATE_WRITE_END();

    STORAGE.saveKnownLocations(locations, newCount);
    DLOG_INFO(TAG, "phone save-location stored tag=%s lat=%.6f lon=%.6f slot=%d",
              tag, lat, lon, newCount);
    return true;
}

bool CommandDispatcher::handleScreenChange(const uint8_t* payload, size_t len) {
    if (len < sizeof(CmdScreenChangeRequestV1)) return false;
    CmdScreenChangeRequestV1 req;
    memcpy(&req, payload, sizeof(req));
    if (req.targetScreen >= static_cast<uint8_t>(SCREEN_COUNT)) {
        return false;
    }

    // Only allow screens that don't have nested modal state machines (mission
    // menus, badusb editors).  This keeps the phone from yanking the operator
    // out of an in-progress flow.
    const Screen target = static_cast<Screen>(req.targetScreen);
    switch (target) {
        case SCREEN_LORA:
        case SCREEN_WIFI:
        case SCREEN_RECON:
        case SCREEN_SYSTEM:
        case SCREEN_MISSION_SUMMARY:
        case SCREEN_MESHTASTIC:
        case SCREEN_BLE:
            break;
        default:
            DLOG_WARN(TAG, "phone screen-change refused: screen %u not phone-safe",
                      static_cast<unsigned>(target));
            return false;
    }

    STATE_WRITE_BEGIN();
    g_state.currentScreen = target;
    g_state.screenChanged = true;
    STATE_WRITE_END();
    DLOG_INFO(TAG, "phone screen-change to %u",
              static_cast<unsigned>(target));
    return true;
}

bool CommandDispatcher::handleDebriefRequest() {
    if (!BUS.publishUiCommand(static_cast<int32_t>(UI_CMD_OPEN_DEBRIEF))) {
        DLOG_WARN(TAG, "phone debrief-request: UI queue full");
        return false;
    }
    DLOG_INFO(TAG, "phone debrief-request queued");
    return true;
}

int CommandDispatcher::handleLogTail(uint8_t* payload, size_t cap) {
    if (cap < sizeof(CmdLogTailResponseV1Header)) return -1;

    uint8_t* linesOut = payload + sizeof(CmdLogTailResponseV1Header);
    const size_t linesCap = cap - sizeof(CmdLogTailResponseV1Header);

    size_t outBytes = 0;
    uint16_t outLines = 0;
    DebugLog::copyTail(linesOut, linesCap, /*maxLines=*/16, outBytes, outLines);

    CmdLogTailResponseV1Header hdr = {};
    hdr.lineCount  = outLines;
    hdr.totalBytes = static_cast<uint16_t>(outBytes);
    memcpy(payload, &hdr, sizeof(hdr));

    return static_cast<int>(sizeof(hdr) + outBytes);
}
