

#include "DisplayManager.h"
#include "../config.h"
#include "../SecretsConfig.h"
#include "../core/Session.h"
#include "../core/SpectreState.h"
#include "../core/NotifTypes.h"
#include "../core/DebugLog.h"
#include "../managers/BadUsbManager.h"
#include "../managers/SubGhzTypes.h"
#include "../managers/RadioArbiter.h"
#include "../managers/SettingsManager.h"
#include "../ui/LVGLDriver.h"
#include <cstdio>
#include <esp_heap_caps.h>


// RENDER OWNERSHIP
// ----------------
// Runtime UI is LVGL-only.
// Direct TFT access is allowed only:
//   1) inside the LVGL flush driver
//   2) in explicit boot/fatal fallback code
// Do not add new runtime TFT or sprite rendering paths here.


static const int DIV_W = 8;
static const int LIST_VISIBLE_ROWS = 8;
static const int WIFI_LIST_ROW_Y = 17;
static const int WIFI_LIST_ROW_STEP = 12;
static const int WIFI_LIST_TEXT_W = 186;
static const int WIFI_LIST_BAR_X = 204;
// Canvas pixel buffers — allocated from PSRAM on first use to keep the
// divider buffer out of internal BSS.  LVGL's software
// renderer only accesses them via the CPU, so PSRAM is safe here.
uint8_t* DisplayManager::_divBuf    = nullptr;

namespace {

lv_obj_t* makeClippedLabel(lv_obj_t* parent, const char* text,
                           uint32_t color, const lv_font_t* font,
                           int x, int y, int width,
                           lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text ? text : "");
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_set_width(lbl, width);
    return lbl;
}

uint32_t scaleHexColor(uint32_t color, uint8_t numer, uint8_t denom) {
    if (denom == 0) {
        return color;
    }

    uint32_t r = ((color >> 16) & 0xFFU) * numer / denom;
    uint32_t g = ((color >> 8)  & 0xFFU) * numer / denom;
    uint32_t b = (color & 0xFFU) * numer / denom;
    return ((r & 0xFFU) << 16) | ((g & 0xFFU) << 8) | (b & 0xFFU);
}

uint32_t displayAccentColor() {
    return CLR_YELLOW;
}

uint32_t displayAccentDimColor() {
    return scaleHexColor(displayAccentColor(), 2, 5);
}

void animSetX(void* obj, int32_t value) {
    lv_obj_set_x(static_cast<lv_obj_t*>(obj), value);
}

void animSetOpa(void* obj, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value), 0);
}

void animSetBgOpa(void* obj, int32_t value) {
    lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(value), 0);
}

void animSetWidth(void* obj, int32_t value) {
    lv_obj_set_width(static_cast<lv_obj_t*>(obj), value);
}

void animateObject(lv_obj_t* obj,
                   lv_anim_exec_xcb_t execCb,
                   int32_t from,
                   int32_t to,
                   uint32_t duration,
                   uint32_t delay,
                   lv_anim_path_cb_t pathCb) {
    if (!obj || !execCb) return;
    lv_anim_del(obj, execCb);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, execCb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, duration);
    lv_anim_set_delay(&a, delay);
    if (pathCb) {
        lv_anim_set_path_cb(&a, pathCb);
    }
    lv_anim_start(&a);
}

void deleteObjectLater(lv_obj_t* obj, uint32_t delayMs) {
    if (!obj) return;
    lv_anim_t d;
    lv_anim_init(&d);
    lv_anim_set_var(&d, obj);
    lv_anim_set_exec_cb(&d, [](void*, int32_t) {});
    lv_anim_set_duration(&d, delayMs);
    lv_anim_set_deleted_cb(&d, [](lv_anim_t* a) {
        lv_obj_delete(static_cast<lv_obj_t*>(a->var));
    });
    lv_anim_start(&d);
}

int clampListScroll(int selected, int scroll, int visibleCount, int totalCount) {
    if (totalCount <= 0) {
        return 0;
    }

    if (selected < scroll) {
        scroll = selected;
    }
    if (selected >= scroll + visibleCount) {
        scroll = selected - visibleCount + 1;
    }

    const int maxScroll = max(0, totalCount - visibleCount);
    if (scroll < 0) {
        scroll = 0;
    }
    if (scroll > maxScroll) {
        scroll = maxScroll;
    }
    return scroll;
}

int wrapListSelection(int selected, int delta, int totalCount) {
    if (totalCount <= 0) {
        return 0;
    }

    selected += delta;
    while (selected < 0) {
        selected += totalCount;
    }
    return selected % totalCount;
}

const char* missionProfileSummary(MissionProfile profile) {
    switch (profile) {
        case MISSION_RECON:  return "Passive wifi + subghz collection";
        case MISSION_PWNY:   return "Target lock and active pressure";
        case MISSION_UPLINK: return "Sync, export, and relay pipeline";
        default:             return "Mission staging";
    }
}

const char* missionProfileListSummary(MissionProfile profile) {
    switch (profile) {
        case MISSION_RECON:  return "Passive wifi + subghz";
        case MISSION_PWNY:   return "Target lock + pressure";
        case MISSION_UPLINK: return "Sync + export relay";
        default:             return "Mission staging";
    }
}

ButtonBindingSet displayMissionBindings(MissionProfile profile) {
    switch (profile) {
        case MISSION_RECON:
            return {BUTTON_ACTION_WIFI_REFRESH, BUTTON_ACTION_MISSION_EXIT,
                    BUTTON_ACTION_SUBGHZ_MODE_CYCLE, BUTTON_ACTION_WIFI_SCAN_LIST};
        case MISSION_PWNY: {
            char pwnyStatus[48] = "";
            STATE_READ_BEGIN();
            strlcpy(pwnyStatus, g_state.pwnyStatus, sizeof(pwnyStatus));
            STATE_READ_END();
            if (strncmp(pwnyStatus, "PAUSED", 6) == 0 ||
                strncmp(pwnyStatus, "IDLE", 4) == 0) {
                return {BUTTON_ACTION_NONE, BUTTON_ACTION_MISSION_EXIT,
                        BUTTON_ACTION_NONE, BUTTON_ACTION_NONE};
            }
            return {BUTTON_ACTION_PWNY_FORCE_DEAUTH, BUTTON_ACTION_MISSION_EXIT,
                    BUTTON_ACTION_NONE, BUTTON_ACTION_NONE};
        }
        case MISSION_UPLINK:
            return {BUTTON_ACTION_UPLINK_TRIGGER, BUTTON_ACTION_MISSION_EXIT,
                    BUTTON_ACTION_SYSTEM_DEBRIEF, BUTTON_ACTION_NONE};
        default:
            return {BUTTON_ACTION_NONE, BUTTON_ACTION_MISSION_EXIT,
                    BUTTON_ACTION_NONE, BUTTON_ACTION_NONE};
    }
}

ButtonBindingSet displayBadUsbBindings(bool ready, bool armed, bool running) {
    if (!ready) {
        return {BUTTON_ACTION_NONE, BUTTON_ACTION_NONE,
                BUTTON_ACTION_BADUSB_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
    }

    if (running) {
        return {BUTTON_ACTION_NONE, BUTTON_ACTION_BADUSB_CANCEL,
                BUTTON_ACTION_BADUSB_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
    }

    if (armed) {
        return {BUTTON_ACTION_NONE, BUTTON_ACTION_BADUSB_RUN,
                BUTTON_ACTION_BADUSB_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
    }

    return {BUTTON_ACTION_BADUSB_ARM, BUTTON_ACTION_BADUSB_ARM,
            BUTTON_ACTION_BADUSB_LIST_OPEN, BUTTON_ACTION_SCREEN_NEXT};
}

ButtonBindingSet displayBindingsFromState() {
    uint8_t runContext = RUN_CONTEXT_GENERAL;
    uint8_t activeMissionProfile = MISSION_RECON;
    Screen currentScreen = SCREEN_LORA;
    bool wifiListActive = false;
    bool missionListActive = false;
    bool badUsbListActive = false;
    bool debriefActive = false;
    bool badUsbReady = false;
    bool badUsbArmed = false;
    bool badUsbRunning = false;

    STATE_READ_BEGIN();
    runContext = g_state.runContext;
    activeMissionProfile = g_state.activeMissionProfile;
    currentScreen = g_state.currentScreen;
    wifiListActive = g_state.wifiListActive;
    missionListActive = g_state.missionListActive;
    badUsbListActive = g_state.badUsbListActive;
    debriefActive = g_state.debriefActive;
    badUsbReady = g_state.badUsbReady;
    badUsbArmed = g_state.badUsbArmed;
    badUsbRunning = g_state.badUsbRunning;
    STATE_READ_END();

    if (missionListActive) {
        return spectreMissionListBindings();
    }
    if (wifiListActive) {
        return spectreWifiListBindings();
    }
    if (badUsbListActive) {
        return spectreBadUsbListBindings();
    }
    if (debriefActive) {
        return spectreDebriefBindings();
    }
    if (runContext == RUN_CONTEXT_MISSION) {
        return displayMissionBindings(static_cast<MissionProfile>(activeMissionProfile));
    }
    if (currentScreen == SCREEN_BADUSB) {
        return displayBadUsbBindings(badUsbReady, badUsbArmed, badUsbRunning);
    }
    return spectreScreenBindings(currentScreen);
}

}

bool DisplayManager::_ensureCanvasBuffers() {
    if (!_divBuf) {
        constexpr size_t sz = DIV_W * (THEME_CONTENT_H + THEME_ACTION_H) * sizeof(lv_color_t);
        _divBuf = static_cast<uint8_t*>(
            heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!_divBuf) {
            _divBuf = static_cast<uint8_t*>(malloc(sz));
        }
        if (!_divBuf) return false;
    }
    return true;
}

void DisplayManager::begin() {
    _ensureCanvasBuffers();
    lv_obj_t* root = lv_screen_active();
    lv_obj_set_style_bg_color(root, lv_color_hex(CLR_BLACK), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    _buildStatusBar();
    _buildMascotPanel();
    _buildDivider();
    _buildActionBar();

    _buildScreenLora();
    _buildScreenPwny();
    _buildScreenMeshtastic();
    _buildScreenWifi();
    _buildScreenBadUsb();
    _buildScreenRecon();
    _buildScreenSystem();

    _buildRadarSweep();

    // Panel borders: fixed at full brightness (amber)
    _setPanelBorderColor(_statusBar,   0xD09000);
    _setPanelBorderColor(_actionBar,   0xD09000);
    _setPanelBorderColor(_mascotPanel, 0xD09000);

    setScreen(SCREEN_LORA);
    drawLora("NONE", "OFF", 0, 0, "--", 0, 0, 0);
}

void DisplayManager::setActionHints(const ButtonBindingSet& bindings) {
    bool overlayActive = false;
    STATE_READ_BEGIN();
    overlayActive = g_state.wifiListActive ||
                    g_state.missionListActive ||
                    g_state.badUsbListActive ||
                    g_state.debriefActive;
    STATE_READ_END();
    if (overlayActive) {
        _syncActionHintsFromState();
        return;
    }
    _setActionHints(bindings);
}

void DisplayManager::_syncActionHintsFromState() {
    bool busy = false;
    STATE_READ_BEGIN();
    busy = g_state.wifiScanPending;
    STATE_READ_END();
    _setActionHints(displayBindingsFromState(), busy);
}

void DisplayManager::_buildNotifPanel() {
    if (_notifPanel) return;

    _notifPanel = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_notifPanel, THEME_SCREEN_W, 38);
    lv_obj_set_pos(_notifPanel, 0, -38);  // hidden above screen
    lv_obj_set_style_bg_color(_notifPanel,
        lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_notifPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_notifPanel, 0, 0);
    lv_obj_set_style_border_width(_notifPanel, 2, 0);
    lv_obj_set_style_border_side(_notifPanel,
        LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(_notifPanel, 0, 0);
    lv_obj_clear_flag(_notifPanel, LV_OBJ_FLAG_SCROLLABLE);

    _notifIcon = lv_label_create(_notifPanel);
    lv_obj_set_style_text_font(_notifIcon, FONT_SMALL, 0);
    lv_obj_set_pos(_notifIcon, 6, 10);

    _notifLabel = lv_label_create(_notifPanel);
    lv_obj_set_style_text_font(_notifLabel, FONT_BODY, 0);
    lv_obj_set_pos(_notifLabel, 30, 8);
    lv_obj_set_size(_notifLabel, THEME_SCREEN_W - 36, 22);
    lv_label_set_long_mode(_notifLabel, LV_LABEL_LONG_CLIP);
}

void DisplayManager::_showNotif(uint8_t type,
                                 const char* text) {
    if (!_notifPanel) _buildNotifPanel();

    lv_color_t col = lv_color_hex(0x00F0FF);  // default cyan
    const char* icon = ">";

    switch (type) {
        case NOTIF_DRONE:
            col  = lv_color_hex(0xFF003C);
            icon = "!";
            break;
        case NOTIF_PMKID:
            col  = lv_color_hex(0xFCE700);
            icon = "*";
            break;
        case NOTIF_DEAUTH:
            col  = lv_color_hex(0xFF003C);
            icon = "!";
            break;
        case NOTIF_HANDSHAKE:
            col  = lv_color_hex(0x00FF9C);
            icon = "+";
            break;
        case NOTIF_DEVICE_NEW:
            col  = lv_color_hex(0x00F0FF);
            icon = ">";
            break;
        case NOTIF_HOMELAB_SYNC:
            col  = lv_color_hex(0x00FF9C);
            icon = "~";
            break;
        case NOTIF_EXPORT:
            col  = lv_color_hex(0x00FF9C);
            icon = "^";
            break;
        case NOTIF_STORAGE:
            col  = lv_color_hex(0xFF9A00);
            icon = "#";
            break;
        case NOTIF_POWER:
            col  = lv_color_hex(0xFF4D4D);
            icon = "!";
            break;
    }

    lv_obj_set_style_border_color(_notifPanel, col, 0);
    lv_obj_set_style_bg_color(_notifPanel,
        lv_color_hex(0x080808), 0);

    lv_obj_set_style_text_color(_notifIcon, col, 0);
    lv_label_set_text(_notifIcon, icon);

    lv_obj_set_style_text_color(_notifLabel, col, 0);
    lv_label_set_text(_notifLabel, text);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _notifPanel);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        lv_obj_set_y((lv_obj_t*)obj, v);
    });
    lv_anim_set_values(&a, -38, 0);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    if (!_notifActive) {
        STATE_READ_BEGIN();
        _priorMascot = g_state.mascotState;
        STATE_READ_END();
    }

    STATE_WRITE_BEGIN();
    g_state.mascotState = MASCOT_ALERT;
    STATE_WRITE_END();

    _notifActive  = true;
    _notifShowMs  = millis();
}

void DisplayManager::showNotification(uint8_t type, const char* text) {
    if (!text || !text[0]) return;
    _showNotif(type, text);
}

void DisplayManager::pulseMascot(MascotState state, uint32_t durationMs) {
    _mascotPulseState = state;
    _mascotPulseUntilMs = millis() + durationMs;
    _mascotPulseActive = true;
}

void DisplayManager::syncFromState() {
    STATE_READ_BEGIN();
    const Screen screen = g_state.currentScreen;
    const MissionProfile selectedProfile =
        static_cast<MissionProfile>(g_state.missionSelection);
    const MissionProfile activeProfile =
        static_cast<MissionProfile>(g_state.activeMissionProfile);
    const float battV = g_state.battVoltage;
    const unsigned long uptimeMs = g_state.uptimeMs;
    char storage[32];
    strlcpy(storage, g_state.storageStr, sizeof(storage));
    STATE_READ_END();

    switch (screen) {
        case SCREEN_BADUSB:
            drawBadUsb();
            break;
        case SCREEN_RECON:
            drawRecon(selectedProfile);
            break;
        case SCREEN_MISSION:
            drawMission(activeProfile);
            break;
        case SCREEN_SYSTEM:
            drawSystem(battV, uptimeMs, storage);
            break;
        default:
            break;
    }
}

void DisplayManager::_dismissNotif() {
    if (!_notifPanel || !_notifActive) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _notifPanel);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        lv_obj_set_y((lv_obj_t*)obj, v);
    });
    lv_anim_set_values(&a, 0, -38);
    lv_anim_set_duration(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);

    STATE_WRITE_BEGIN();
    g_state.mascotState  = _priorMascot;
    g_state.droneAlert   = false;
    STATE_WRITE_END();

    _notifActive = false;
}

void DisplayManager::tickNotif() {
    // Auto-dismiss after 8 seconds
    if (_notifActive &&
        millis() - _notifShowMs > 8000) {
        _dismissNotif();
    }
}

// ─── Screen switching ─────────────────────────────────────────

void DisplayManager::setScreen(Screen s) {
    auto contentForScreen = [&](Screen screen) -> lv_obj_t* {
        switch (screen) {
            case SCREEN_LORA:       return _loraContent;
            case SCREEN_MESHTASTIC: return _meshContent;
            case SCREEN_WIFI:       return _wifiContent;
            case SCREEN_BADUSB:     return _badUsbContent;
            case SCREEN_RECON:      return _reconContent;
            case SCREEN_MISSION:    return _pwnyContent;
            case SCREEN_SYSTEM:     return _sysContent;
            default:                return nullptr;
        }
    };

    lv_obj_t* nextContent = contentForScreen(s);
    if (!nextContent) {
        return;
    }

    if (_currentScreen == s) {
        const char* names[] = {"LRA","MSH","WFI","USB","MIS","RCN","SYS"};
        if (_lblScreen) lv_label_set_text(_lblScreen, names[s]);
        return;
    }

    lv_obj_t* currentContent = contentForScreen(_currentScreen);
    if (currentContent && currentContent != nextContent) {
        lv_anim_del(currentContent, animSetX);
        lv_anim_del(currentContent, animSetOpa);
        lv_obj_add_flag(currentContent, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(currentContent, THEME_CONTENT_X);
        lv_obj_set_style_opa(currentContent, LV_OPA_COVER, 0);
    }

    if (_loraContent  && _loraContent  != nextContent) lv_obj_add_flag(_loraContent,  LV_OBJ_FLAG_HIDDEN);
    if (_meshContent  && _meshContent  != nextContent) lv_obj_add_flag(_meshContent,  LV_OBJ_FLAG_HIDDEN);
    if (_wifiContent  && _wifiContent  != nextContent) lv_obj_add_flag(_wifiContent,  LV_OBJ_FLAG_HIDDEN);
    if (_badUsbContent && _badUsbContent != nextContent) lv_obj_add_flag(_badUsbContent, LV_OBJ_FLAG_HIDDEN);
    if (_reconContent && _reconContent != nextContent) lv_obj_add_flag(_reconContent, LV_OBJ_FLAG_HIDDEN);
    if (_pwnyContent  && _pwnyContent  != nextContent) lv_obj_add_flag(_pwnyContent,  LV_OBJ_FLAG_HIDDEN);
    if (_sysContent   && _sysContent   != nextContent) lv_obj_add_flag(_sysContent,   LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(nextContent, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(nextContent);
    lv_obj_set_x(nextContent, THEME_CONTENT_X + 14);
    lv_obj_set_style_opa(nextContent, LV_OPA_TRANSP, 0);
    animateObject(nextContent, animSetX,
                  THEME_CONTENT_X + 14, THEME_CONTENT_X,
                  180, 0, lv_anim_path_overshoot);
    animateObject(nextContent, animSetOpa,
                  LV_OPA_TRANSP, LV_OPA_COVER,
                  180, 0, lv_anim_path_ease_out);

    lv_obj_t* sweep = lv_obj_create(nextContent);
    lv_obj_set_pos(sweep, -24, 0);
    lv_obj_set_size(sweep, 24, THEME_CONTENT_H);
    lv_obj_set_style_bg_color(sweep, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_bg_opa(sweep, 34, 0);
    lv_obj_set_style_border_width(sweep, 0, 0);
    lv_obj_set_style_radius(sweep, 0, 0);
    lv_obj_clear_flag(sweep, LV_OBJ_FLAG_SCROLLABLE);
    animateObject(sweep, animSetX,
                  -24, THEME_CONTENT_W + 8,
                  240, 0, lv_anim_path_linear);
    animateObject(sweep, animSetBgOpa,
                  34, 0,
                  220, 0, lv_anim_path_ease_out);
    deleteObjectLater(sweep, 260);

    _currentScreen = s;

    const char* names[] = {"LRA","MSH","WFI","USB","MIS","RCN","SYS"};
    if (_lblScreen) lv_label_set_text(_lblScreen, names[s]);

    if (_radarLine) {
        const bool radarActive = (s == SCREEN_WIFI);
        if (radarActive) {
            lv_obj_remove_flag(_radarLine, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(_radarLine, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ─── Status bar ───────────────────────────────────────────────

void DisplayManager::_buildStatusBar() {
    const uint32_t accent = displayAccentColor();
    _statusBar = _makePanel(lv_screen_active(),
                            0, 0, THEME_SCREEN_W, THEME_STATUS_H,
                            0x111111, accent);

    _makeLabel(_statusBar, "SPECTRE", accent, FONT_BODY,
                LV_ALIGN_LEFT_MID, 4, 0);

    _lblBatt = _makeLabel(_statusBar, "PWR", CLR_GREEN, FONT_SMALL,
                          LV_ALIGN_LEFT_MID, 68, 0);

    _lblWifi = _makeLabel(_statusBar, "W:--", CLR_GREY, FONT_SMALL,
                          LV_ALIGN_LEFT_MID, 104, 0);

    _lblBle = _makeLabel(_statusBar, "BLE", CLR_GREY, FONT_SMALL,
                         LV_ALIGN_LEFT_MID, 154, 0);

    _lblLora = _makeLabel(_statusBar, "LORA", CLR_YELLOW, FONT_SMALL,
                          LV_ALIGN_LEFT_MID, 194, 0);

    _lblScreen = _makeLabel(_statusBar, "LRA", CLR_CYAN, FONT_SMALL,
                            LV_ALIGN_RIGHT_MID, -4, 0);

    static lv_point_precise_t pts[] = {{0, THEME_STATUS_H - 1},
                                       {THEME_SCREEN_W, THEME_STATUS_H - 1}};
    lv_obj_t* line = lv_line_create(lv_screen_active());
    lv_line_set_points(line, pts, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(accent), 0);
    lv_obj_set_style_line_width(line, 2, 0);
}

void DisplayManager::updateStatus(const StatusBar& sb) {
    if (!_lblBatt) return;

    char buf[16];
    if (sb.powerSource == POWER_SOURCE_USB) {
        snprintf(buf, sizeof(buf), "%s", sb.charging ? "USB+" : "USB");
    } else if (sb.powerState == POWER_STATE_BATTERY_CRITICAL &&
               sb.runtimeMinutes > 0) {
        const uint16_t runtimeDisplay =
            sb.runtimeMinutes > 99 ? 99 : sb.runtimeMinutes;
        snprintf(buf, sizeof(buf), "%02uM",
                 static_cast<unsigned>(runtimeDisplay));
    } else {
        snprintf(buf, sizeof(buf), "%d%%", sb.battPercent);
    }
    lv_label_set_text(_lblBatt, buf);

    uint32_t battColor = (sb.battPercent > 20) ? CLR_GREEN : CLR_RED;
    if (sb.powerSource == POWER_SOURCE_USB) {
        battColor = sb.charging ? CLR_GREEN : CLR_CYAN;
    } else if (sb.powerState == POWER_STATE_BATTERY_ECONOMY) {
        battColor = CLR_YELLOW;
    } else if (sb.powerState == POWER_STATE_BATTERY_CRITICAL) {
        battColor = ((millis() / 320UL) % 2UL) ? CLR_RED : 0xFF9A00;
    }

    lv_obj_set_style_text_color(_lblBatt, lv_color_hex(battColor), 0);
    lv_obj_set_style_shadow_color(_lblBatt, lv_color_hex(battColor), 0);
    lv_obj_set_style_shadow_width(_lblBatt,
                                  sb.charging || sb.powerState == POWER_STATE_BATTERY_CRITICAL
                                      ? 10 : 0,
                                  0);
    lv_obj_set_style_shadow_opa(_lblBatt,
                                sb.charging || sb.powerState == POWER_STATE_BATTERY_CRITICAL
                                    ? LV_OPA_60 : LV_OPA_TRANSP,
                                0);

    const bool criticalFx = sb.powerState == POWER_STATE_BATTERY_CRITICAL;
    _setCriticalPowerFx(criticalFx);
    if (criticalFx) {
        const uint32_t edgeColor =
            ((millis() / 260UL) % 2UL) ? 0xA00000 : 0xFF4D00;
        _setPanelBorderColor(_statusBar, edgeColor);
        _setPanelBorderColor(_actionBar, edgeColor);
        _setPanelBorderColor(_mascotPanel, edgeColor);
    }

    const char* radioTxt;
    uint32_t    radioCol;
    switch ((RadioOwner)sb.radioOwner) {
        case RADIO_WIFI_CAPTURE:
            radioTxt = "W:RX";  radioCol = CLR_CYAN;   break;
        case RADIO_WIFI_UPLOAD:
            radioTxt = "W:UP";  radioCol = CLR_GREEN;  break;
        case RADIO_WIFI_SCAN:
            radioTxt = "W:SCN"; radioCol = CLR_YELLOW; break;
        case RADIO_WIFI_PMKID:
            radioTxt = "W:PWN"; radioCol = CLR_RED;    break;
        case RADIO_BLE_GPS:
            radioTxt = "B:GPS"; radioCol = CLR_YELLOW; break;
        case RADIO_BLE_TEXT:
            radioTxt = "B:TXT"; radioCol = CLR_YELLOW; break;
        default:
            radioTxt = "W:--";  radioCol = CLR_GREY;   break;
    }
    lv_label_set_text(_lblWifi, radioTxt);
    lv_obj_set_style_text_color(_lblWifi,
        lv_color_hex(radioCol), 0);

    lv_obj_set_style_text_color(_lblBle,
        sb.bleConnected ? lv_color_hex(CLR_CYAN) : lv_color_hex(CLR_GREY), 0);

    lv_obj_set_style_text_color(_lblLora,
        sb.loraActive ? lv_color_hex(CLR_YELLOW) : lv_color_hex(CLR_GREY), 0);
}

// ─── Mascot panel ─────────────────────────────────────────────

void DisplayManager::_buildMascotPanel() {
    _mascotPanel = _makePanel(lv_screen_active(),
                              0, THEME_STATUS_H,
                              THEME_MASCOT_W,
                              THEME_CONTENT_H + THEME_ACTION_H,
                              CLR_BLACK, CLR_BLACK);

    _nativeMascotReady = _nativeMascot.begin(_mascotPanel);
}

// ─── Divider ──────────────────────────────────────────────────

void DisplayManager::_divTimerCb(lv_timer_t* t) {
    DisplayManager* dm = (DisplayManager*)lv_timer_get_user_data(t);
    dm->_updateDivCanvas();
}

void DisplayManager::_buildDivider() {
    int h = THEME_CONTENT_H + THEME_ACTION_H;

    _divCanvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(_divCanvas, _divBuf,
                         DIV_W, h, LV_COLOR_FORMAT_NATIVE);
    lv_obj_set_pos(_divCanvas, THEME_DIVIDER_X - 3, THEME_STATUS_H);
    lv_canvas_fill_bg(_divCanvas,
                      lv_color_hex(CLR_BLACK), LV_OPA_COVER);

    _divTimer = lv_timer_create(_divTimerCb, 90, this);

    _divArcL = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_divArcL, 10, 2);
    lv_obj_set_style_bg_color(_divArcL, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_bg_opa(_divArcL, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_divArcL, 0, 0);
    lv_obj_set_style_radius(_divArcL, 0, 0);
    lv_obj_clear_flag(_divArcL, LV_OBJ_FLAG_SCROLLABLE);

    _divArcR = lv_obj_create(lv_screen_active());
    lv_obj_set_size(_divArcR, 10, 2);
    lv_obj_set_style_bg_color(_divArcR, lv_color_hex(CLR_YELLOW), 0);
    lv_obj_set_style_bg_opa(_divArcR, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(_divArcR, 0, 0);
    lv_obj_set_style_radius(_divArcR, 0, 0);
    lv_obj_clear_flag(_divArcR, LV_OBJ_FLAG_SCROLLABLE);

    _sparkTimer = lv_timer_create([](lv_timer_t* t) {
        DisplayManager* dm = (DisplayManager*)lv_timer_get_user_data(t);
        dm->_fireSpark();
    }, 650, this);
}

void DisplayManager::_updateDivCanvas() {
    int h = THEME_CONTENT_H + THEME_ACTION_H;

    lv_canvas_fill_bg(_divCanvas,
                      lv_color_hex(CLR_BLACK), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(_divCanvas, &layer);

    auto drawBar = [&](int x, int y, int w, int height,
                       lv_color_t color, lv_opa_t opa, int radius) {
        if (w <= 0 || height <= 0) return;
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.bg_color = color;
        rect.bg_opa = opa;
        rect.radius = radius;
        rect.border_width = 0;
        lv_area_t area = {
            static_cast<int32_t>(x),
            static_cast<int32_t>(y),
            static_cast<int32_t>(x + w - 1),
            static_cast<int32_t>(y + height - 1)
        };
        lv_draw_rect(&layer, &rect, &area);
    };

    drawBar(3, 0, 1, h, lv_color_hex(CLR_DIMYELLOW), LV_OPA_COVER, 0);

    _divPulseY += 3;
    if (_divPulseY > h) _divPulseY = 0;

    const int trailLen = 24;
    for (int i = 0; i < trailLen; i++) {
        int py = _divPulseY - i;
        if (py < 0 || py >= h) continue;

        uint32_t color;
        uint8_t  opa;
        if (i == 0) {
            color = 0xFFFFFF; opa = 255;
        } else if (i < 3) {
            color = 0xFCE700; opa = 220;
        } else if (i < 8) {
            color = 0x00F0FF; opa = 180 - i * 12;
        } else if (i < 16) {
            color = 0x007880; opa = 120 - i * 6;
        } else {
            color = 0x003040; opa = 60 - i * 2;
        }

        int w = (i < 2) ? 3 : (i < 6) ? 2 : 1;
        int x = 3 - w / 2;
        drawBar(x, py, w, 1, lv_color_hex(color), opa, 1);
    }

    lv_canvas_finish_layer(_divCanvas, &layer);
    lv_obj_invalidate(_divCanvas);
}

void DisplayManager::_fireSpark() {
    if (!_divArcL || !_divArcR) return;

    int pulseY = _divPulseY + THEME_STATUS_H;
    int x = THEME_DIVIDER_X;

    lv_obj_set_pos(_divArcL, x - 10, pulseY + 8);
    lv_obj_set_pos(_divArcR, x + 4,  pulseY + 8);
    lv_obj_set_style_bg_opa(_divArcL, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(_divArcR, LV_OPA_COVER, 0);

    int offset = random(-20, 20);
    int sy = pulseY + 8 + offset;
    int top = THEME_STATUS_H;
    int bot = THEME_ACTION_Y;
    if (sy < top) sy = top;
    if (sy > bot - 4) sy = bot - 4;

    lv_obj_t* arcL2 = lv_obj_create(lv_screen_active());
    lv_obj_set_size(arcL2, 14, 2);
    lv_obj_set_pos(arcL2, x - 14, sy);
    lv_obj_set_style_bg_color(arcL2, lv_color_hex(CLR_YELLOW), 0);
    lv_obj_set_style_bg_opa(arcL2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(arcL2, 0, 0);
    lv_obj_set_style_radius(arcL2, 0, 0);
    lv_obj_clear_flag(arcL2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* arcR2 = lv_obj_create(lv_screen_active());
    lv_obj_set_size(arcR2, 14, 2);
    lv_obj_set_pos(arcR2, x + 4, sy);
    lv_obj_set_style_bg_color(arcR2, lv_color_hex(CLR_CYAN), 0);
    lv_obj_set_style_bg_opa(arcR2, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(arcR2, 0, 0);
    lv_obj_set_style_radius(arcR2, 0, 0);
    lv_obj_clear_flag(arcR2, LV_OBJ_FLAG_SCROLLABLE);

    auto fadeOut = [&](lv_obj_t* obj, int dur) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, obj);
        lv_anim_set_exec_cb(&a, [](void* o, int32_t v) {
            lv_obj_set_style_bg_opa((lv_obj_t*)o, v, 0);
        });
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_duration(&a, dur);
        lv_anim_start(&a);
    };

    fadeOut(_divArcL, 150);
    fadeOut(_divArcR, 150);
    fadeOut(arcL2, 200);
    fadeOut(arcR2, 200);

    auto delayDelete = [&](lv_obj_t* obj) {
        lv_anim_t d;
        lv_anim_init(&d);
        lv_anim_set_var(&d, obj);
        lv_anim_set_exec_cb(&d, [](void* o, int32_t v) {});
        lv_anim_set_duration(&d, 220);
        lv_anim_set_deleted_cb(&d, [](lv_anim_t* a){
            lv_obj_delete((lv_obj_t*)a->var);
        });
        lv_anim_start(&d);
    };

    delayDelete(arcL2);
    delayDelete(arcR2);

    lv_timer_set_period(_sparkTimer, 550 + random(0, 650));
}

void DisplayManager::updateDivider() {
    if (_glitchPending) {
        _runGlitchTransition(_glitchTarget);
    }
}

// ─── Panel border colour ──────────────────────────────────────

void DisplayManager::_setPanelBorderColor(lv_obj_t* panel, uint32_t color) {
    if (!panel) return;
    lv_obj_set_style_border_color(panel, lv_color_hex(color), 0);
}

void DisplayManager::_setCriticalPowerFx(bool active) {
    if (active == _criticalPowerFxActive) {
        return;
    }

    _criticalPowerFxActive = active;
    if (active) {
        _setPanelBorderColor(_statusBar,   0xC02020);
        _setPanelBorderColor(_actionBar,   0xC02020);
        _setPanelBorderColor(_mascotPanel, 0xC02020);
    } else {
        _setPanelBorderColor(_statusBar,   0xD09000);
        _setPanelBorderColor(_actionBar,   0xD09000);
        _setPanelBorderColor(_mascotPanel, 0xD09000);
    }
}

// ─── Radar sweep ──────────────────────────────────────────────

void DisplayManager::_buildRadarSweep() {
    _radarLine = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(_radarLine, THEME_CONTENT_X, THEME_CONTENT_Y);
    lv_obj_set_size(_radarLine, THEME_CONTENT_W, 2);
    lv_obj_set_style_bg_color(_radarLine, lv_color_hex(0x3AFFF0), 0);
    lv_obj_set_style_bg_opa(_radarLine, 60, 0);
    lv_obj_set_style_border_width(_radarLine, 0, 0);
    lv_obj_set_style_radius(_radarLine, 0, 0);
    lv_obj_clear_flag(_radarLine, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_init(&_radarAnim);
    lv_anim_set_var(&_radarAnim, _radarLine);
    lv_anim_set_exec_cb(&_radarAnim, [](void* obj, int32_t yPos) {
        lv_obj_set_y((lv_obj_t*)obj, yPos);
    });
    lv_anim_set_values(&_radarAnim, THEME_CONTENT_Y, THEME_ACTION_Y - 2);
    lv_anim_set_duration(&_radarAnim, 4000);
    lv_anim_set_repeat_count(&_radarAnim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&_radarAnim, lv_anim_path_linear);
    lv_anim_start(&_radarAnim);
}

// ─── Glitch transition ────────────────────────────────────────

void DisplayManager::triggerGlitchTransition(Screen target) {
    _glitchPending = true;
    _glitchTarget  = target;
}

void DisplayManager::_runGlitchTransition(Screen target) {
    setScreen(target);

    for (int i = 0; i < 4; i++) {
        lv_obj_t* bar = lv_obj_create(lv_screen_active());
        const int barY = THEME_CONTENT_Y + random(0, max(8, THEME_CONTENT_H - 10));
        const int barH = 3 + random(0, 8);
        lv_obj_set_pos(bar, THEME_CONTENT_X - 6, barY);
        lv_obj_set_size(bar, THEME_CONTENT_W + 12, barH);
        lv_obj_set_style_bg_color(
            bar,
            lv_color_hex((i % 2 == 0) ? CLR_CYAN : CLR_YELLOW),
            0);
        lv_obj_set_style_bg_opa(bar, 50 + random(0, 50), 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        animateObject(bar, animSetBgOpa,
                      90, 0,
                      120 + i * 20, i * 18, lv_anim_path_ease_out);
        animateObject(bar, animSetWidth,
                      THEME_CONTENT_W + 12, THEME_CONTENT_W - 16,
                      120 + i * 20, i * 18, lv_anim_path_ease_in);
        deleteObjectLater(bar, 220 + i * 18);
    }

    _glitchPending = false;
}

// ─── Data pulse on packet RX ──────────────────────────────────

void DisplayManager::triggerDataPulse() {
    if (!_rssiLabel || !_snrLabel) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _rssiLabel);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        uint32_t color = v > 128 ? 0xFFFFFF : 0x00FF9C;
        lv_obj_set_style_text_color((lv_obj_t*)obj,
                                    lv_color_hex(color), 0);
    });
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_playback_duration(&a, 200);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, _snrLabel);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        uint32_t color = v > 128 ? 0xFFFFFF : 0x00F0FF;
        lv_obj_set_style_text_color((lv_obj_t*)obj,
                                    lv_color_hex(color), 0);
    });
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_playback_duration(&a, 200);
    lv_anim_start(&a);
}

void DisplayManager::drawMascotFrame(MascotState state, int frame) {
    if (!_nativeMascotReady) return;

    if (_mascotPulseActive) {
        if (millis() < _mascotPulseUntilMs) {
            if (!_notifActive) {
                state = _mascotPulseState;
            }
        } else {
            _mascotPulseActive = false;
        }
    }

    const int x = (THEME_MASCOT_W - SPR_FULL_W) / 2;
    const int y = 8;
    _nativeMascot.draw(x, y, state, frame);
}

// ─── Action bar ───────────────────────────────────────────────

void DisplayManager::_buildActionBar() {
    const uint32_t accent = displayAccentColor();
    const uint32_t accentDim = displayAccentDimColor();
    _actionBar = _makePanel(lv_screen_active(),
                            0, THEME_ACTION_Y,
                            THEME_SCREEN_W, THEME_ACTION_H,
                            0x111111, accent);

    _lblActionA = makeClippedLabel(_actionBar, "A", accentDim, FONT_SMALL,
                                    4, 2, 72);
    _lblActionAL = makeClippedLabel(_actionBar, "LA", accentDim, FONT_SMALL,
                                     80, 2, 72, LV_TEXT_ALIGN_CENTER);
    _lblActionBL = makeClippedLabel(_actionBar, "LB", CLR_DIMCYAN, FONT_SMALL,
                                    164, 2, 72, LV_TEXT_ALIGN_CENTER);
    _lblActionB = makeClippedLabel(_actionBar, "B", CLR_DIMCYAN, FONT_SMALL,
                                   244, 2, 72, LV_TEXT_ALIGN_RIGHT);
}

void DisplayManager::_setActionHints(const char* aShort, const char* aLong,
                                     const char* bLong, const char* bShort) {
    const uint32_t accent = displayAccentColor();
    const uint32_t accentDim = displayAccentDimColor();
    auto setSlot = [](lv_obj_t* lbl, const char* prefix, const char* action,
                      bool active, uint32_t activeColor, uint32_t inactiveColor) {
        if (!lbl) return;
        char text[24] = {};
        if (action && action[0]) {
            snprintf(text, sizeof(text), "%s %s", prefix, action);
        }
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl,
                                    lv_color_hex(active ? activeColor : inactiveColor),
                                    0);
    };

    setSlot(_lblActionA, "A", aShort, aShort && aShort[0], accent, accentDim);
    setSlot(_lblActionAL, "LA", aLong, aLong && aLong[0], accent, accentDim);
    setSlot(_lblActionBL, "LB", bLong, bLong && bLong[0], accent, accentDim);
    setSlot(_lblActionB, "B", bShort, bShort && bShort[0], accent, accentDim);
}

void DisplayManager::_setActionHints(const ButtonBindingSet& bindings, bool busy) {
    _setActionHints(spectreButtonActionLabel(bindings.aShort, busy),
                    spectreButtonActionLabel(bindings.aLong),
                    spectreButtonActionLabel(bindings.bLong),
                    spectreButtonActionLabel(bindings.bShort));
}

// ─── Screen builders ──────────────────────────────────────────
void DisplayManager::_buildScreenLora() {
    _loraContent = _makePanel(lv_screen_active(),
                              THEME_CONTENT_X, THEME_CONTENT_Y,
                              THEME_CONTENT_W, THEME_CONTENT_H,
                              CLR_BLACK, CLR_BLACK);

    const uint32_t accent = displayAccentColor();
    _makeLabel(_loraContent, "SUB-GHZ RECON", accent, FONT_HEADER,
               LV_ALIGN_TOP_LEFT, 4, 4);

    static lv_point_precise_t sep[] = {{0,26},{THEME_CONTENT_W,26}};
    lv_obj_t* line = lv_line_create(_loraContent);
    lv_line_set_points(line, sep, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(accent), 0);
    lv_obj_set_style_line_width(line, 1, 0);

    _makeLabel(_loraContent, "RSSI", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 4, 32);
    _makeLabel(_loraContent, "SNR", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 90, 32);
    _makeLabel(_loraContent, "PACKETS", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 180, 32);
    _makeLabel(_loraContent, "LAST SIGNAL", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 4, 72);
    _subGhzMetaValue = makeClippedLabel(_loraContent, "NONE / OFF / 0.0 MHz / NODES 0",
                                        CLR_DIMYELLOW, FONT_SMALL, 4, 108, THEME_CONTENT_W - 8);

    _rssiLabel = makeClippedLabel(_loraContent, "0 dB", CLR_GREY, FONT_BODY, 4, 46, 72);
    _snrLabel = makeClippedLabel(_loraContent, "0 dB", CLR_GREY, FONT_BODY, 90, 46, 72);
    _loraPacketsValue = makeClippedLabel(_loraContent, "00000", CLR_WHITE, FONT_BODY, 180, 46, 64);
    _loraPayloadValue = makeClippedLabel(_loraContent, "--", CLR_CYAN, FONT_BODY, 4, 86, THEME_CONTENT_W - 8);
}

void DisplayManager::_buildScreenMeshtastic() {
    _meshContent = _makePanel(lv_screen_active(),
                              THEME_CONTENT_X, THEME_CONTENT_Y,
                              THEME_CONTENT_W, THEME_CONTENT_H,
                              CLR_BLACK, CLR_BLACK);

    const uint32_t accent = displayAccentColor();
    _makeLabel(_meshContent, "MESHTASTIC", accent, FONT_HEADER,
               LV_ALIGN_TOP_LEFT, 4, 4);

    static lv_point_precise_t sep[] = {{0,26},{THEME_CONTENT_W,26}};
    lv_obj_t* line = lv_line_create(_meshContent);
    lv_line_set_points(line, sep, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(accent), 0);
    lv_obj_set_style_line_width(line, 1, 0);

    _makeLabel(_meshContent, "NODE", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 4, 32);
    _makeLabel(_meshContent, "CHANNEL", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 120, 32);
    _makeLabel(_meshContent, "LAST MESSAGE", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 4, 72);

    _meshNodeValue = makeClippedLabel(_meshContent, "--", CLR_CYAN, FONT_BODY, 4, 46, 108);
    _meshChannelValue = makeClippedLabel(_meshContent, "LONGFAST", CLR_YELLOW, FONT_BODY, 120, 46, 116);
    _meshLastMessageValue = makeClippedLabel(_meshContent, "--", CLR_WHITE, FONT_BODY, 4, 86, THEME_CONTENT_W - 8);
    _meshStatsValue = makeClippedLabel(_meshContent, "PACKETS RX: 0    NODES SEEN: 0",
                                       CLR_GREY, FONT_SMALL, 4, 116, THEME_CONTENT_W - 8);
    lv_obj_add_flag(_meshContent, LV_OBJ_FLAG_HIDDEN);
}

void DisplayManager::_buildScreenWifi() {
    _wifiContent = _makePanel(lv_screen_active(),
                              THEME_CONTENT_X, THEME_CONTENT_Y,
                              THEME_CONTENT_W, THEME_CONTENT_H,
                              CLR_BLACK, CLR_BLACK);

    const uint32_t accent = displayAccentColor();
    _makeLabel(_wifiContent, "WIFI INTEL", accent, FONT_HEADER,
               LV_ALIGN_TOP_LEFT, 4, 4);

    static lv_point_precise_t sep[] = {{0,26},{THEME_CONTENT_W,26}};
    lv_obj_t* line = lv_line_create(_wifiContent);
    lv_line_set_points(line, sep, 2);
    lv_obj_set_style_line_color(line, lv_color_hex(accent), 0);
    lv_obj_set_style_line_width(line, 1, 0);

    _makeLabel(_wifiContent, "NETWORKS", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 4, 32);
    _makeLabel(_wifiContent, "DEVICES", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 100, 32);
    _makeLabel(_wifiContent, "PROBES", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 180, 32);
    _makeLabel(_wifiContent, "LAST PROBE", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 4, 68);

    _wifiNetworksValue = makeClippedLabel(_wifiContent, "0", CLR_GREY, FONT_BODY, 4, 46, 64);
    _wifiDevicesValue = makeClippedLabel(_wifiContent, "0", CLR_GREY, FONT_BODY, 100, 46, 64);
    _wifiProbesValue = makeClippedLabel(_wifiContent, "0", CLR_GREY, FONT_BODY, 180, 46, 64);
    _wifiLastSSIDValue = makeClippedLabel(_wifiContent, "--", CLR_CYAN, FONT_BODY, 4, 82, THEME_CONTENT_W - 8);
    _wifiLastMACValue = makeClippedLabel(_wifiContent, "--", CLR_GREY, FONT_SMALL, 4, 100, THEME_CONTENT_W - 8);
    _wifiChannelValue = makeClippedLabel(_wifiContent, "CH: 0", CLR_DIMYELLOW, FONT_SMALL, 4, 114, 72);
    lv_obj_add_flag(_wifiContent, LV_OBJ_FLAG_HIDDEN);
}

void DisplayManager::_buildScreenPwny() {
    _pwnyContent = _makePanel(lv_screen_active(),
                              THEME_CONTENT_X, THEME_CONTENT_Y,
                              THEME_CONTENT_W, THEME_CONTENT_H,
                              CLR_BLACK, CLR_BLACK);

    const uint32_t accent = displayAccentColor();
    _makeLabel(_pwnyContent, "MISSION", accent, FONT_HEADER,
               LV_ALIGN_TOP_LEFT, 4, 4);
    _pwnyTitleValue = makeClippedLabel(_pwnyContent,
        "RECON", accent, FONT_HEADER,
        110, 4, THEME_CONTENT_W - 114, LV_TEXT_ALIGN_RIGHT);

    _pwnyStatusValue = makeClippedLabel(_pwnyContent,
        "IDLE", CLR_CYAN, FONT_BODY,
        4, 30, THEME_CONTENT_W - 60);

    _pwnyStatsValue = makeClippedLabel(_pwnyContent,
        "READY", CLR_GREY, FONT_SMALL,
        4, 46, THEME_CONTENT_W - 60);

    _pwnyEapolPanel = lv_obj_create(_pwnyContent);
    lv_obj_set_pos(_pwnyEapolPanel, THEME_CONTENT_W - 48, 28);
    lv_obj_set_size(_pwnyEapolPanel, 40, 40);
    lv_obj_set_style_bg_opa(_pwnyEapolPanel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(_pwnyEapolPanel, lv_color_hex(CLR_GREY), 0);
    lv_obj_set_style_border_width(_pwnyEapolPanel, 1, 0);
    lv_obj_set_style_radius(_pwnyEapolPanel, 0, 0);
    lv_obj_set_style_pad_all(_pwnyEapolPanel, 0, 0);
    lv_obj_clear_flag(_pwnyEapolPanel, LV_OBJ_FLAG_SCROLLABLE);

    _pwnyEapolDivV = lv_obj_create(_pwnyEapolPanel);
    lv_obj_set_pos(_pwnyEapolDivV, 19, 1);
    lv_obj_set_size(_pwnyEapolDivV, 1, 38);
    lv_obj_set_style_bg_color(_pwnyEapolDivV, lv_color_hex(CLR_GREY), 0);
    lv_obj_set_style_bg_opa(_pwnyEapolDivV, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_pwnyEapolDivV, 0, 0);
    lv_obj_set_style_radius(_pwnyEapolDivV, 0, 0);

    _pwnyEapolDivH = lv_obj_create(_pwnyEapolPanel);
    lv_obj_set_pos(_pwnyEapolDivH, 1, 19);
    lv_obj_set_size(_pwnyEapolDivH, 38, 1);
    lv_obj_set_style_bg_color(_pwnyEapolDivH, lv_color_hex(CLR_GREY), 0);
    lv_obj_set_style_bg_opa(_pwnyEapolDivH, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_pwnyEapolDivH, 0, 0);
    lv_obj_set_style_radius(_pwnyEapolDivH, 0, 0);

    const int quadPos[4][2] = {{1, 1}, {20, 1}, {1, 20}, {20, 20}};
    const char* quadNum[4] = {"1", "2", "3", "4"};
    for (int i = 0; i < 4; ++i) {
        _pwnyEapolQuads[i] = lv_obj_create(_pwnyEapolPanel);
        lv_obj_set_pos(_pwnyEapolQuads[i], quadPos[i][0], quadPos[i][1]);
        lv_obj_set_size(_pwnyEapolQuads[i], 18, 18);
        lv_obj_set_style_bg_opa(_pwnyEapolQuads[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(_pwnyEapolQuads[i], 0, 0);
        lv_obj_set_style_radius(_pwnyEapolQuads[i], 0, 0);
        lv_obj_set_style_pad_all(_pwnyEapolQuads[i], 0, 0);
        lv_obj_clear_flag(_pwnyEapolQuads[i], LV_OBJ_FLAG_SCROLLABLE);

        _pwnyEapolNums[i] = makeClippedLabel(_pwnyEapolQuads[i], quadNum[i], CLR_GREY,
                                             FONT_SMALL, 0, 0, 18, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_text_align(_pwnyEapolNums[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(_pwnyEapolNums[i], lv_color_hex(CLR_GREY), 0);
    }

    for (int i = 0; i < 8; i++) {
        _pwnyTargetRows[i] = makeClippedLabel(_pwnyContent,
            "", CLR_GREY, FONT_SMALL,
            4, 64 + i * 12, THEME_CONTENT_W - 8);
    }

    _updatePwnyHandshakeViz(0);
    // Panel is PWNY-only; drawMission toggles visibility per mission profile.
    lv_obj_add_flag(_pwnyEapolPanel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(_pwnyContent, LV_OBJ_FLAG_HIDDEN);
}

void DisplayManager::_buildScreenBadUsb() {
    _badUsbContent = _makePanel(lv_screen_active(),
                                THEME_CONTENT_X, THEME_CONTENT_Y,
                                THEME_CONTENT_W, THEME_CONTENT_H,
                                CLR_BLACK, CLR_BLACK);

    const uint32_t accent = displayAccentColor();
    _makeLabel(_badUsbContent, "BADUSB", accent, FONT_HEADER,
               LV_ALIGN_TOP_LEFT, 4, 4);
    _badUsbTitleValue = makeClippedLabel(_badUsbContent,
        "READY", accent, FONT_SMALL,
        168, 8, THEME_CONTENT_W - 172, LV_TEXT_ALIGN_RIGHT);

    _makeLabel(_badUsbContent, "PAYLOAD", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 4, 28);
    _badUsbScriptValue = makeClippedLabel(_badUsbContent, "--", CLR_WHITE, FONT_BODY,
                                          4, 40, THEME_CONTENT_W - 8);
    _badUsbStatusValue = makeClippedLabel(_badUsbContent, "IDLE", CLR_CYAN, FONT_BODY,
                                          4, 58, THEME_CONTENT_W - 8);
    _badUsbDetailValue = makeClippedLabel(_badUsbContent, "--", CLR_GREY, FONT_SMALL,
                                          4, 76, THEME_CONTENT_W - 8);

    for (int i = 0; i < 4; ++i) {
        _badUsbOpLabels[i] = makeClippedLabel(_badUsbContent, "", CLR_GREY, FONT_SMALL,
                                              4, 94 + i * 10, THEME_CONTENT_W - 8);
    }

    lv_obj_add_flag(_badUsbContent, LV_OBJ_FLAG_HIDDEN);
}

void DisplayManager::_buildScreenRecon() {
    _reconContent = _makePanel(lv_screen_active(),
                               THEME_CONTENT_X, THEME_CONTENT_Y,
                               THEME_CONTENT_W, THEME_CONTENT_H,
                               CLR_BLACK, CLR_BLACK);

    const uint32_t accent = displayAccentColor();
    _makeLabel(_reconContent, "MISSION LAUNCH", accent, FONT_HEADER,
               LV_ALIGN_TOP_LEFT, 4, 4);

    _makeLabel(_reconContent, "PROFILE", CLR_GREY, FONT_SMALL,
               LV_ALIGN_TOP_LEFT, 4, 24);
    _reconModeValue = makeClippedLabel(_reconContent, "IDLE", accent, FONT_BODY, 4, 38, THEME_CONTENT_W - 8);

    _reconScriptLabel = _makeLabel(_reconContent, "OBJECTIVE", CLR_GREY, FONT_SMALL,
                                   LV_ALIGN_TOP_LEFT, 4, 58);
    _reconScriptValue = makeClippedLabel(_reconContent, "--", CLR_WHITE, FONT_BODY,
                                         4, 72, THEME_CONTENT_W - 8);

    _reconStatusValue = makeClippedLabel(_reconContent, "READY", CLR_CYAN, FONT_BODY,
                                         4, 90, THEME_CONTENT_W - 8);
    _reconDetailValue = makeClippedLabel(_reconContent, "--", CLR_GREY, FONT_SMALL,
                                         4, 106, THEME_CONTENT_W - 8);

    for (int i = 0; i < 4; i++) {
        _reconOpLabels[i] = makeClippedLabel(_reconContent, "", CLR_GREY, FONT_SMALL,
                                             4, 118 + i * 8, THEME_CONTENT_W - 8);
    }
    lv_obj_add_flag(_reconContent, LV_OBJ_FLAG_HIDDEN);
}

void DisplayManager::_buildScreenSystem() {
    _sysContent = _makePanel(lv_screen_active(),
                             THEME_CONTENT_X, THEME_CONTENT_Y,
                             THEME_CONTENT_W, THEME_CONTENT_H,
                             CLR_BLACK, CLR_BLACK);
    lv_obj_add_flag(_sysContent, LV_OBJ_FLAG_HIDDEN);

    auto makeSubPanel = [&](lv_obj_t* parent) -> lv_obj_t* {
        lv_obj_t* panel = lv_obj_create(parent);
        lv_obj_set_pos(panel, 0, 0);
        lv_obj_set_size(panel, THEME_CONTENT_W, THEME_CONTENT_H);
        lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(panel, 0, 0);
        lv_obj_set_style_radius(panel, 0, 0);
        lv_obj_set_style_pad_all(panel, 0, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
        return panel;
    };

    _sysLivePanel = makeSubPanel(_sysContent);
    _debriefPanel = makeSubPanel(_sysContent);
    lv_obj_add_flag(_debriefPanel, LV_OBJ_FLAG_HIDDEN);

    const uint32_t accent = displayAccentColor();
    static lv_point_precise_t sep[] = {{0,26},{THEME_CONTENT_W,26}};

    _makeLabel(_sysLivePanel, "SYSTEM", accent, FONT_HEADER,
               LV_ALIGN_TOP_LEFT, 4, 4);
    _sysHeaderStatus = makeClippedLabel(_sysLivePanel, "STORAGE OK", CLR_GREEN,
                                        FONT_SMALL, 140, 8, 100, LV_TEXT_ALIGN_RIGHT);
    lv_obj_t* sysLine = lv_line_create(_sysLivePanel);
    lv_line_set_points(sysLine, sep, 2);
    lv_obj_set_style_line_color(sysLine, lv_color_hex(accent), 0);
    lv_obj_set_style_line_width(sysLine, 1, 0);

    auto makeSystemRow = [&](int y, const char* leftLabel, lv_obj_t** leftValue,
                             const char* rightLabel, lv_obj_t** rightValue) {
        makeClippedLabel(_sysLivePanel, leftLabel, CLR_GREY, FONT_SMALL, 4, y, 100);
        *leftValue = makeClippedLabel(_sysLivePanel, "--", CLR_WHITE, FONT_SMALL, 4, y + 12, 100);
        makeClippedLabel(_sysLivePanel, rightLabel, CLR_GREY, FONT_SMALL, 140, y, 100);
        *rightValue = makeClippedLabel(_sysLivePanel, "--", CLR_WHITE, FONT_SMALL, 140, y + 12, 100);
    };

    makeSystemRow(30, "STORAGE", &_sysStorageValue, "FREE", &_sysFreeValue);
    makeSystemRow(54, "PENDING", &_sysPendingValue, "DEDUPE", &_sysDedupeValue);
    makeSystemRow(78, "MODE", &_sysModeValue, "POLICY", &_sysPolicyValue);
    _sysTimeLabel = makeClippedLabel(_sysLivePanel, "UPTIME", CLR_GREY, FONT_SMALL, 4, 102, 100);
    _sysTimeValue = makeClippedLabel(_sysLivePanel, "--", CLR_CYAN, FONT_SMALL, 4, 114, 100);
    makeClippedLabel(_sysLivePanel, "DUMP", CLR_GREY, FONT_SMALL, 140, 102, 100);
    _sysDumpValue = makeClippedLabel(_sysLivePanel, "READY", CLR_GREEN, FONT_SMALL, 140, 114, 100);
    makeClippedLabel(_sysLivePanel, "RADIO", CLR_GREY, FONT_SMALL, 4, 126, 100);
    _sysRadioValue = makeClippedLabel(_sysLivePanel, "IDLE", CLR_CYAN, FONT_SMALL, 4, 138, 100);
    makeClippedLabel(_sysLivePanel, "CFG", CLR_GREY, FONT_SMALL, 140, 126, 100);
    _sysCfgValue = makeClippedLabel(_sysLivePanel, "SET ?", CLR_WHITE, FONT_SMALL, 140, 138, 100);

    _makeLabel(_debriefPanel, "SESSION DEBRIEF",
               accent, FONT_HEADER, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_t* debriefLine = lv_line_create(_debriefPanel);
    lv_line_set_points(debriefLine, sep, 2);
    lv_obj_set_style_line_color(debriefLine, lv_color_hex(accent), 0);
    lv_obj_set_style_line_width(debriefLine, 1, 0);

    auto makeDebriefPair = [&](int x, int y, const char* label, lv_obj_t** value) {
        makeClippedLabel(_debriefPanel, label, CLR_GREY, FONT_SMALL, x, y, 100);
        *value = makeClippedLabel(_debriefPanel, "0", CLR_WHITE, FONT_BODY, x, y + 14, 100);
    };

    makeDebriefPair(4, 32, "DURATION", &_debriefDurationValue);
    makeDebriefPair(4, 64, "NETWORKS", &_debriefNetworksValue);
    makeDebriefPair(4, 96, "DEVICES", &_debriefDevicesValue);
    makeDebriefPair(4, 128, "PROBES", &_debriefProbesValue);
    makeDebriefPair(140, 32, "PMKIDs", &_debriefPMKIDsValue);
    makeDebriefPair(140, 64, "DRONES", &_debriefDronesValue);
    makeDebriefPair(140, 96, "EXPORT", &_debriefExportValue);
    _debriefLowerLabel = makeClippedLabel(_debriefPanel, "TAG", CLR_GREY, FONT_SMALL, 140, 128, 100);
    _debriefLowerValue = makeClippedLabel(_debriefPanel, "NONE", CLR_GREY, FONT_BODY, 140, 142, 100);
}

// ─── Screen data updates ──────────────────────────────────────

void DisplayManager::drawLora(const char* moduleName, const char* modeName,
                              uint32_t frequencyHz, uint16_t nodeCount,
                              const char* lastPayload, int rssi,
                              int snr, int packets) {
    if (!_loraContent) return;
    const ButtonBindingSet bindings = spectreScreenBindings(SCREEN_LORA);
    _setActionHints(bindings);
    const char* payloadText = (lastPayload && lastPayload[0]) ? lastPayload : "--";
    char metaStr[80];
    snprintf(metaStr, sizeof(metaStr), "%s / %s / %lu.%lu MHz / NODES %u",
             (moduleName && moduleName[0]) ? moduleName : "NONE",
             (modeName && modeName[0]) ? modeName : "OFF",
             static_cast<unsigned long>(frequencyHz / 1000000UL),
             static_cast<unsigned long>((frequencyHz % 1000000UL) / 100000UL),
             static_cast<unsigned>(nodeCount));

    uint32_t rssiColor = rssi == 0    ? CLR_GREY  :
                         rssi > -80   ? CLR_GREEN  :
                         rssi > -100  ? CLR_YELLOW : CLR_RED;
    char rssiStr[16];
    snprintf(rssiStr, sizeof(rssiStr), "%d dB", rssi);
    lv_label_set_text(_rssiLabel, rssiStr);
    lv_obj_set_style_text_color(_rssiLabel, lv_color_hex(rssiColor), 0);

    char snrStr[16];
    snprintf(snrStr, sizeof(snrStr), "%d dB", snr);
    lv_label_set_text(_snrLabel, snrStr);
    lv_obj_set_style_text_color(_snrLabel,
                                lv_color_hex(snr == 0 ? CLR_GREY : CLR_CYAN),
                                0);

    char pktStr[16];
    snprintf(pktStr, sizeof(pktStr), "%05d", packets);
    lv_label_set_text(_loraPacketsValue, pktStr);
    lv_label_set_text(_loraPayloadValue, payloadText);
    if (_subGhzMetaValue) {
        lv_label_set_text(_subGhzMetaValue, metaStr);
    }
}

void DisplayManager::_updatePwnyHandshakeViz(uint8_t eapolMask) {
    if (!_pwnyEapolPanel) return;

    constexpr uint32_t PMKID_FLASH = 0xB000FF;
    // Pwny screen refreshes every 750ms (ExecutionPolicy::pwnyIntervalMs),
    // so use a single solid window long enough to span at least two refresh
    // ticks — a shorter blink animation can be entirely skipped between
    // refreshes and never render.
    constexpr uint32_t PMKID_FLASH_WINDOW_MS = 1200UL;
    bool flashActive = _pwnyPmkidFlashActive;

    if (flashActive) {
        const uint32_t elapsedMs = millis() - _pwnyPmkidFlashStartedMs;
        if (elapsedMs >= PMKID_FLASH_WINDOW_MS) {
            _pwnyPmkidFlashActive = false;
            flashActive = false;
        }
    }

    const uint32_t lineColor = flashActive ? PMKID_FLASH : CLR_GREY;
    lv_obj_set_style_border_color(_pwnyEapolPanel, lv_color_hex(lineColor), 0);
    if (_pwnyEapolDivV) {
        lv_obj_set_style_bg_color(_pwnyEapolDivV, lv_color_hex(lineColor), 0);
    }
    if (_pwnyEapolDivH) {
        lv_obj_set_style_bg_color(_pwnyEapolDivH, lv_color_hex(lineColor), 0);
    }

    for (int i = 0; i < 4; ++i) {
        if (!_pwnyEapolQuads[i]) continue;

        if (flashActive) {
            lv_obj_set_style_bg_color(_pwnyEapolQuads[i], lv_color_hex(PMKID_FLASH), 0);
            lv_obj_set_style_bg_opa(_pwnyEapolQuads[i], LV_OPA_70, 0);
            if (_pwnyEapolNums[i]) {
                lv_obj_set_style_text_color(_pwnyEapolNums[i], lv_color_hex(CLR_WHITE), 0);
            }
            continue;
        }

        const bool captured = (eapolMask & (1U << i)) != 0U;
        lv_obj_set_style_bg_color(_pwnyEapolQuads[i],
                                  lv_color_hex(captured ? CLR_GREEN : CLR_BLACK), 0);
        lv_obj_set_style_bg_opa(_pwnyEapolQuads[i],
                                captured ? LV_OPA_70 : LV_OPA_TRANSP, 0);
        if (_pwnyEapolNums[i]) {
            lv_obj_set_style_text_color(_pwnyEapolNums[i],
                                        lv_color_hex(captured ? CLR_BLACK : CLR_GREY), 0);
        }
    }
}

void DisplayManager::_detectPwnyPmkidFlash(const SpectreState::PwnyTargetDisplay* targets,
                                           int count,
                                           const char* statusText) {
    const bool captureLatched =
        statusText && strncmp(statusText, "CAPTURED", 8) == 0;

    for (int i = 0; i < 8; ++i) {
        if (!targets || i >= count) {
            _pwnyPrevSlotSsid[i][0] = '\0';
            _pwnyPrevSlotAttackCount[i] = 0;
            _pwnyPrevSlotPmkid[i] = false;
            continue;
        }

        const SpectreState::PwnyTargetDisplay& t = targets[i];
        const bool sameTarget =
            strncmp(_pwnyPrevSlotSsid[i], t.ssid, sizeof(_pwnyPrevSlotSsid[i])) == 0 &&
            _pwnyPrevSlotAttackCount[i] == t.attackCount;
        const bool eapolFull = (t.eapolMask & 0x0F) == 0x0F;
        if (captureLatched && sameTarget && t.pmkid && !eapolFull && !_pwnyPrevSlotPmkid[i]) {
            _pwnyPmkidFlashActive = true;
            _pwnyPmkidFlashStartedMs = millis();
        }

        strlcpy(_pwnyPrevSlotSsid[i], t.ssid, sizeof(_pwnyPrevSlotSsid[i]));
        _pwnyPrevSlotAttackCount[i] = t.attackCount;
        _pwnyPrevSlotPmkid[i] = t.pmkid;
    }
}

void DisplayManager::drawMission(MissionProfile profile) {
    if (!_pwnyContent) return;
    _syncActionHintsFromState();

    uint32_t titleColor = displayAccentColor();
    if (profile == MISSION_PWNY) {
        titleColor = CLR_RED;
    } else if (profile == MISSION_UPLINK) {
        titleColor = CLR_GREEN;
    } else if (profile == MISSION_RECON) {
        titleColor = CLR_YELLOW;
    }
    if (_pwnyTitleValue) {
        lv_label_set_text(_pwnyTitleValue, missionProfileName(profile));
        lv_obj_set_style_text_color(_pwnyTitleValue, lv_color_hex(titleColor), 0);
    }

    for (lv_obj_t* row : _pwnyTargetRows) {
        if (!row) continue;
        lv_label_set_text(row, "");
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(row, lv_color_hex(CLR_GREY), 0);
    }

    auto setRow = [&](int idx, const char* text, uint32_t color) {
        if (idx < 0 || idx >= 8 || !_pwnyTargetRows[idx]) return;
        lv_label_set_text(_pwnyTargetRows[idx], text ? text : "");
        lv_obj_set_style_text_color(_pwnyTargetRows[idx], lv_color_hex(color), 0);
        lv_obj_remove_flag(_pwnyTargetRows[idx], LV_OBJ_FLAG_HIDDEN);
    };

    if (profile != MISSION_PWNY) {
        _pwnyPmkidFlashActive = false;
        _updatePwnyHandshakeViz(0);
        if (_pwnyEapolPanel) {
            lv_obj_add_flag(_pwnyEapolPanel, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (_pwnyEapolPanel) {
        lv_obj_remove_flag(_pwnyEapolPanel, LV_OBJ_FLAG_HIDDEN);
    }

    if (profile == MISSION_PWNY) {
        uint8_t tCount = 0;
        uint8_t curIdx = 0;
        uint16_t captures = 0;
        uint16_t attempts = 0;
        uint32_t sessionMs = 0;
        char statusBuf[48] = "";
        SpectreState::PwnyTargetDisplay targets[8] = {};

        STATE_READ_BEGIN();
        tCount     = g_state.pwnyTargetCount;
        curIdx     = g_state.pwnyCurrentIdx;
        captures   = g_state.pwnyTotalCaptures;
        attempts   = g_state.pwnyTotalAttempts;
        sessionMs  = g_state.pwnySessionMs;
        strlcpy(statusBuf, g_state.pwnyStatus, sizeof(statusBuf));
        const int copyCount = min((int)tCount, 8);
        memcpy(targets, g_state.pwnyTargets,
               sizeof(SpectreState::PwnyTargetDisplay) * copyCount);
        STATE_READ_END();

        uint32_t statusColor = CLR_CYAN;
        if (strncmp(statusBuf, "DEAUTH", 6) == 0 ||
            strncmp(statusBuf, "TARGETING", 9) == 0) {
            statusColor = CLR_RED;
        } else if (strncmp(statusBuf, "CAPTURED", 8) == 0) {
            statusColor = CLR_GREEN;
        } else if (strncmp(statusBuf, "COOLDOWN", 8) == 0) {
            statusColor = CLR_YELLOW;
        }

        lv_label_set_text(_pwnyStatusValue, statusBuf);
        lv_obj_set_style_text_color(_pwnyStatusValue, lv_color_hex(statusColor), 0);

        const uint32_t secs = sessionMs / 1000UL;
        char statsBuf[48];
        if (secs < 60UL) {
            snprintf(statsBuf, sizeof(statsBuf),
                     "CAP:%-2u ATT:%-2u TIME:%lus",
                     static_cast<unsigned>(captures),
                     static_cast<unsigned>(attempts),
                     static_cast<unsigned long>(secs));
        } else {
            snprintf(statsBuf, sizeof(statsBuf),
                     "CAP:%-2u ATT:%-2u TIME:%lum%lus",
                     static_cast<unsigned>(captures),
                     static_cast<unsigned>(attempts),
                     static_cast<unsigned long>(secs / 60UL),
                     static_cast<unsigned long>(secs % 60UL));
        }
        lv_label_set_text(_pwnyStatsValue, statsBuf);

        _detectPwnyPmkidFlash(targets, static_cast<int>(tCount), statusBuf);

        int indicatorIdx = -1;
        if (tCount > 0) {
            // Prefer the currently-attacked target when it's still in
            // progress; only fall back to scanning for another in-progress
            // slot when curIdx is complete or cooling down. This keeps the
            // viz locked onto the active attack instead of jumping to a
            // peer target on every refresh.
            if (curIdx < tCount &&
                !targets[curIdx].complete &&
                !targets[curIdx].onCooldown) {
                indicatorIdx = static_cast<int>(curIdx);
            } else {
                indicatorIdx = (curIdx < tCount) ? static_cast<int>(curIdx) : 0;
                for (int i = 0; i < tCount; ++i) {
                    if (!targets[i].complete && !targets[i].onCooldown) {
                        indicatorIdx = i;
                        break;
                    }
                }
            }
        }
        _updatePwnyHandshakeViz(indicatorIdx >= 0 ? targets[indicatorIdx].eapolMask : 0);

        const bool showArmHint =
            strncmp(statusBuf, "PAUSED", 6) == 0 ||
            strncmp(statusBuf, "IDLE", 4) == 0;
        const int visibleTargets = min(static_cast<int>(tCount), showArmHint ? 3 : 4);
        for (int i = 0; i < visibleTargets; i++) {
            if (!_pwnyTargetRows[i]) continue;

            const SpectreState::PwnyTargetDisplay& t = targets[i];
            char rowBuf[52];
            const char* phaseChar = " ";
            if (i == curIdx && !t.complete && !t.onCooldown) {
                phaseChar = (t.phase == 1) ? "!" : ">";
            }

            const bool eapolFull = (t.eapolMask & 0x0F) == 0x0F;

            if (t.complete && t.pmkid && eapolFull) {
                snprintf(rowBuf, sizeof(rowBuf),
                         "[**] %-12s PMKID+4W", t.ssid);
            } else if (t.complete && t.pmkid) {
                snprintf(rowBuf, sizeof(rowBuf),
                         "[OK] %-12s PMKID", t.ssid);
            } else if (t.complete && eapolFull) {
                snprintf(rowBuf, sizeof(rowBuf),
                         "[OK] %-14s 4-WAY", t.ssid);
            } else if (t.complete && t.crackable) {
                snprintf(rowBuf, sizeof(rowBuf),
                         "[OK] %-12s HSHK", t.ssid);
            } else if (t.onCooldown) {
                snprintf(rowBuf, sizeof(rowBuf),
                         "[--] %-12s WAIT", t.ssid);
            } else {
                snprintf(rowBuf, sizeof(rowBuf),
                         "[%s] %-12s %dc s%d",
                         phaseChar,
                         t.ssid[0] ? t.ssid : "<hidden>",
                         t.clients,
                         t.score > 99 ? 99 : t.score);
            }

            uint32_t rowColor = CLR_GREY;
            if (t.complete || t.crackable) rowColor = CLR_GREEN;
            else if (t.onCooldown)         rowColor = 0x444444;
            else if (i == curIdx)          rowColor = (t.phase == 1) ? CLR_RED : CLR_CYAN;
            setRow(i, rowBuf, rowColor);
        }

        if (showArmHint) {
            setRow(3, "A+B ARM!", CLR_RED);
        }

        if (visibleTargets == 0) {
            setRow(0, "Waiting for viable targets", CLR_GREY);
            setRow(1, showArmHint ? "Press A+B to arm mission" : "Capture posture armed", showArmHint ? CLR_RED : CLR_CYAN);
            setRow(2, showArmHint ? "Targets are staged but paused" : "Select a target to pressure", CLR_GREY);
        }
        return;
    }

    if (profile == MISSION_RECON) {
        int wifiNetworks = 0;
        int probes = 0;
        int nodes = 0;
        int pending = 0;
        uint8_t subGhzMode = 0;
        uint8_t radioOwner = 0;
        char lastMac[18] = "";
        char wifiSsid[32] = "";
        char subGhzModule[24] = "";

        STATE_READ_BEGIN();
        wifiNetworks = g_state.wifiNetworkCount;
        probes = g_state.probePacketCount;
        nodes = g_state.subGhzNodeCount;
        pending = STORAGE.isReady()
            ? static_cast<int>(STORAGE.getAuthoritativePendingEventCount())
            : g_state.sessionFilesPending;
        subGhzMode = g_state.subGhzMode;
        radioOwner = g_state.radioOwner;
        strlcpy(lastMac, g_state.lastProbedMAC, sizeof(lastMac));
        strlcpy(wifiSsid, g_state.wifiSSID, sizeof(wifiSsid));
        strlcpy(subGhzModule, g_state.subGhzModule, sizeof(subGhzModule));
        STATE_READ_END();

        lv_label_set_text(_pwnyStatusValue, "Recon walk locked");
        lv_obj_set_style_text_color(_pwnyStatusValue, lv_color_hex(CLR_YELLOW), 0);

        char statsBuf[48];
        snprintf(statsBuf, sizeof(statsBuf), "NET %d  PROBE %d  NODE %d  Q %d",
                 wifiNetworks, probes, nodes, pending);
        lv_label_set_text(_pwnyStatsValue, statsBuf);

        char rowBuf[56];
        snprintf(rowBuf, sizeof(rowBuf), "Radio  %s",
                 RadioArbiter::ownerName(static_cast<RadioOwner>(radioOwner)));
        setRow(0, rowBuf, CLR_CYAN);
        snprintf(rowBuf, sizeof(rowBuf), "Focus  %s", wifiSsid[0] ? wifiSsid : "--");
        setRow(1, rowBuf, CLR_WHITE);
        snprintf(rowBuf, sizeof(rowBuf), "Probe  %s", lastMac[0] ? lastMac : "--");
        setRow(2, rowBuf, CLR_WHITE);
        snprintf(rowBuf, sizeof(rowBuf), "Sub    %s %s",
                 subGhzModule[0] ? subGhzModule : "NONE",
                 subGhzModeName(static_cast<SubGhzMode>(subGhzMode)));
        setRow(3, rowBuf, CLR_WHITE);
        return;
    }

    bool uploadActive = false;
    bool wifiConnected = false;
    uint32_t published = 0;
    uint32_t total = 0;
    uint16_t percent = 0;
    int pending = 0;
    char phase[16] = "";
    char timeLocal[24] = "";

    STATE_READ_BEGIN();
    uploadActive = g_state.uploadActive;
    wifiConnected = g_state.wifiConnected;
    published = g_state.uploadPublished;
    total = g_state.uploadTotal;
    percent = g_state.uploadPercent;
    pending = STORAGE.isReady()
        ? static_cast<int>(STORAGE.getAuthoritativePendingEventCount())
        : g_state.sessionFilesPending;
    strlcpy(phase, g_state.uploadPhase, sizeof(phase));
    strlcpy(timeLocal, g_state.timeLocal, sizeof(timeLocal));
    STATE_READ_END();

    lv_label_set_text(_pwnyStatusValue,
                      uploadActive ? "Uplink locked + syncing" : "Uplink locked");
    lv_obj_set_style_text_color(_pwnyStatusValue,
                                lv_color_hex(uploadActive ? CLR_GREEN : CLR_CYAN), 0);

    char statsBuf[48];
    snprintf(statsBuf, sizeof(statsBuf),
             "PEND:%d PUB:%lu/%lu %u%%",
             pending,
             static_cast<unsigned long>(published),
             static_cast<unsigned long>(total),
             static_cast<unsigned>(percent));
    lv_label_set_text(_pwnyStatsValue, statsBuf);

    char rowBuf[8][52] = {};
    snprintf(rowBuf[0], sizeof(rowBuf[0]), "Phase  %s", phase[0] ? phase : "IDLE");
    snprintf(rowBuf[1], sizeof(rowBuf[1]), "Link   %s", wifiConnected ? "ONLINE" : "OFFLINE");
    snprintf(rowBuf[2], sizeof(rowBuf[2]), "Clock  %s", timeLocal[0] ? timeLocal : "--");
    snprintf(rowBuf[3], sizeof(rowBuf[3]), "Queue  %d pending", pending);

    for (int i = 0; i < 4; ++i) {
        setRow(i, rowBuf[i], i < 3 ? CLR_WHITE : CLR_GREY);
    }
}

void DisplayManager::drawMeshtastic(const char* node, const char* channel) {
    if (!_meshContent) return;
    const ButtonBindingSet bindings = spectreScreenBindings(SCREEN_MESHTASTIC);
    _setActionHints(bindings);
    const char* nodeText = (node && node[0]) ? node : "--";
    const char* channelText = (channel && channel[0]) ? channel : "LONGFAST";

    lv_label_set_text(_meshNodeValue, nodeText);
    lv_label_set_text(_meshChannelValue, channelText);
    lv_label_set_text(_meshLastMessageValue, "--");
    lv_label_set_text(_meshStatsValue, "PACKETS RX: 0    NODES SEEN: 0");
}

void DisplayManager::drawWifi(const char* ssid, int networks,
                              const char* probeActivity) {
    if (!_wifiContent) return;
    (void)ssid;
    (void)probeActivity;
    bool scanPending = false;
    bool overlayActive = false;
    STATE_READ_BEGIN();
    scanPending = g_state.wifiScanPending;
    overlayActive = g_state.wifiListActive ||
                    g_state.missionListActive ||
                    g_state.badUsbListActive ||
                    g_state.debriefActive;
    STATE_READ_END();
    const ButtonBindingSet bindings = spectreScreenBindings(SCREEN_WIFI);
    if (overlayActive) {
        _syncActionHintsFromState();
    } else {
        _setActionHints(bindings, scanPending);
    }
    char netStr[8];
    snprintf(netStr, sizeof(netStr), "%d", networks);
    lv_label_set_text(_wifiNetworksValue, netStr);
    lv_obj_set_style_text_color(_wifiNetworksValue,
                                lv_color_hex(networks > 0 ? CLR_YELLOW : CLR_GREY),
                                0);

    int devices, probes;
    char lastMAC[18], lastSSID[33];
    STATE_READ_BEGIN();
    devices = g_state.probeDeviceCount;
    probes  = g_state.probePacketCount;
    strlcpy(lastMAC,  g_state.lastProbedMAC,  18);
    strlcpy(lastSSID, g_state.lastProbedSSID, 33);
    STATE_READ_END();

    char devStr[8];
    snprintf(devStr, sizeof(devStr), "%d", devices);
    lv_label_set_text(_wifiDevicesValue, devStr);
    lv_obj_set_style_text_color(_wifiDevicesValue,
                                lv_color_hex(devices > 0 ? CLR_CYAN : CLR_GREY),
                                0);

    char probeStr[8];
    snprintf(probeStr, sizeof(probeStr), "%d", probes);
    lv_label_set_text(_wifiProbesValue, probeStr);
    lv_obj_set_style_text_color(_wifiProbesValue,
                                lv_color_hex(probes > 0 ? CLR_GREEN : CLR_GREY),
                                0);
    lv_label_set_text(_wifiLastSSIDValue, lastSSID[0] ? lastSSID : "--");
    lv_label_set_text(_wifiLastMACValue, lastMAC[0] ? lastMAC : "--");

    uint8_t ch;
    STATE_READ_BEGIN();
    ch = g_state.wifiChannel;
    STATE_READ_END();
    char chStr[16];
    snprintf(chStr, sizeof(chStr), "CH: %d", ch);
    lv_label_set_text(_wifiChannelValue, chStr);
}

void DisplayManager::_buildWifiList() {
    if (_wifiListPanel) return;

    const uint32_t accent = displayAccentColor();
    _wifiListPanel = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(_wifiListPanel,
                   THEME_DIVIDER_X + 2, THEME_STATUS_H + 1);
    lv_obj_set_size(_wifiListPanel,
                    THEME_SCREEN_W - THEME_DIVIDER_X - 3,
                    THEME_ACTION_Y - THEME_STATUS_H - 2);
    lv_obj_set_style_bg_color(_wifiListPanel,
                               lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_wifiListPanel,
                             LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_wifiListPanel, 0, 0);
    lv_obj_set_style_radius(_wifiListPanel, 0, 0);
    lv_obj_clear_flag(_wifiListPanel,
                       LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_wifiListPanel, LV_OBJ_FLAG_HIDDEN);

    _wifiListTitle = lv_label_create(_wifiListPanel);
    lv_label_set_text(_wifiListTitle, "SSID TARGETS");
    lv_obj_set_style_text_font(_wifiListTitle, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_wifiListTitle, lv_color_hex(accent), 0);
    lv_obj_set_pos(_wifiListTitle, 4, 1);

    _wifiListEmptyLabel = lv_label_create(_wifiListPanel);
    lv_label_set_text(_wifiListEmptyLabel, "WAITING FOR NETWORKS");
    lv_obj_set_style_text_font(_wifiListEmptyLabel, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_wifiListEmptyLabel, lv_color_hex(CLR_GREY), 0);
    lv_obj_set_width(_wifiListEmptyLabel, THEME_SCREEN_W - THEME_DIVIDER_X - 18);
    lv_label_set_long_mode(_wifiListEmptyLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(_wifiListEmptyLabel, 4, 22);
    lv_obj_add_flag(_wifiListEmptyLabel, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < LIST_VISIBLE_ROWS; i++) {
        _wifiListRows[i] = makeClippedLabel(_wifiListPanel, "", CLR_WHITE,
                                            FONT_SMALL, 4,
                                            WIFI_LIST_ROW_Y + i * WIFI_LIST_ROW_STEP,
                                            WIFI_LIST_TEXT_W);
        lv_obj_add_flag(_wifiListRows[i], LV_OBJ_FLAG_HIDDEN);

        for (int b = 0; b < 4; b++) {
            _wifiListBars[i][b] = lv_obj_create(_wifiListPanel);
            lv_obj_set_size(_wifiListBars[i][b], 3, 4 + b * 2);
            lv_obj_set_pos(_wifiListBars[i][b],
                           WIFI_LIST_BAR_X + b * 5,
                           WIFI_LIST_ROW_Y + 9 + i * WIFI_LIST_ROW_STEP - b * 2);
            lv_obj_set_style_bg_opa(_wifiListBars[i][b], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(_wifiListBars[i][b], 0, 0);
            lv_obj_set_style_radius(_wifiListBars[i][b], 0, 0);
            lv_obj_clear_flag(_wifiListBars[i][b], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(_wifiListBars[i][b], LV_OBJ_FLAG_HIDDEN);
        }
    }

    _wifiListScrollbar = lv_obj_create(_wifiListPanel);
    lv_obj_set_style_bg_color(_wifiListScrollbar, lv_color_hex(0xFCE700), 0);
    lv_obj_set_style_bg_opa(_wifiListScrollbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_wifiListScrollbar, 0, 0);
    lv_obj_set_style_radius(_wifiListScrollbar, 0, 0);
    lv_obj_clear_flag(_wifiListScrollbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_wifiListScrollbar, LV_OBJ_FLAG_HIDDEN);
}

void DisplayManager::openWifiList() {
    if (_wifiListOpen) return;

    _buildWifiList();
    lv_obj_move_foreground(_wifiListPanel);
    lv_obj_remove_flag(_wifiListPanel, LV_OBJ_FLAG_HIDDEN);

    STATE_WRITE_BEGIN();
    g_state.wifiListActive = true;
    STATE_WRITE_END();

    _wifiListOpen = true;
    _updateWifiList();

    const ButtonBindingSet bindings = spectreWifiListBindings();
    _setActionHints(bindings);
}

void DisplayManager::refreshWifiList() {
    _updateWifiList();
}

void DisplayManager::_updateWifiList() {
    if (!_wifiListOpen || !_wifiListPanel) return;
    lv_obj_move_foreground(_wifiListPanel);

    SpectreState::WiFiNetworkSnapshot nets[LIST_VISIBLE_ROWS] = {};
    int netCount, selected, scroll;
    STATE_READ_BEGIN();
    netCount = g_state.wifiSnapCount;
    selected = g_state.wifiListSelected;
    scroll   = g_state.wifiListScroll;
    if (netCount > SpectreState::WIFI_SNAP_COUNT) {
        netCount = SpectreState::WIFI_SNAP_COUNT;
    }
    const int originalSelected = selected;
    const int originalScroll = scroll;
    if (netCount <= 0 || selected < 0) selected = 0;
    if (selected >= netCount && netCount > 0) selected = netCount - 1;
    scroll = clampListScroll(selected, scroll, LIST_VISIBLE_ROWS, netCount);
    const int visibleCount = min(LIST_VISIBLE_ROWS, max(0, netCount - scroll));
    for (int i = 0; i < visibleCount; ++i) {
        nets[i] = g_state.wifiSnap[scroll + i];
    }
    STATE_READ_END();

    if (selected != originalSelected || scroll != originalScroll) {
        STATE_WRITE_BEGIN();
        g_state.wifiListSelected = selected;
        g_state.wifiListScroll = scroll;
        STATE_WRITE_END();
    }

    if (_wifiListEmptyLabel) {
        lv_label_set_text(_wifiListEmptyLabel,
                          netCount > 0 ? "" : "WAITING FOR NETWORKS\nB HOLD TO EXIT");
        if (netCount > 0) {
            lv_obj_add_flag(_wifiListEmptyLabel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(_wifiListEmptyLabel, LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (int i = 0; i < LIST_VISIBLE_ROWS; i++) {
        lv_obj_t* row = _wifiListRows[i];

        int netIdx = scroll + i;
        if (netIdx >= netCount) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            for (int b = 0; b < 4; b++) {
                if (_wifiListBars[i][b]) {
                    lv_obj_add_flag(_wifiListBars[i][b], LV_OBJ_FLAG_HIDDEN);
                }
            }
            continue;
        }
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);

        SpectreState::WiFiNetworkSnapshot& net = nets[i];
        bool isSelected  = (netIdx == selected);
        bool hasPMKID    = net.hasPMKID;

        char secChar = '-';
        if (strcmp(net.security, "WPA") == 0) {
            secChar = 'W';
        } else if (strcmp(net.security, "WPA2") == 0 ||
                   strcmp(net.security, "WPA3") == 0) {
            secChar = net.security[3] ? net.security[3] : '2';
        } else if (strcmp(net.security, "OPEN") == 0) {
            secChar = 'O';
        }

        char ssidTrunc[12];
        strlcpy(ssidTrunc, net.ssid[0] ? net.ssid : "<hidden>",
                sizeof(ssidTrunc));

        int rssi = net.rssi;
        int bars = 0;
        if (rssi >= -50) bars = 4;
        else if (rssi >= -65) bars = 3;
        else if (rssi >= -75) bars = 2;
        else if (rssi >= -85) bars = 1;

        char rowText[48];
        if (net.clientCount > 0) {
            snprintf(rowText, sizeof(rowText), "%c %-10s %c%02u %uc %4d %s",
                     isSelected ? '>' : ' ',
                     ssidTrunc,
                     secChar,
                     static_cast<unsigned>(net.channel),
                     static_cast<unsigned>(net.clientCount),
                     rssi,
                     hasPMKID ? "KEY" : "");
        } else {
            snprintf(rowText, sizeof(rowText), "%c %-10s %c%02u    %4d %s",
                     isSelected ? '>' : ' ',
                     ssidTrunc,
                     secChar,
                     static_cast<unsigned>(net.channel),
                     rssi,
                     hasPMKID ? "KEY" : "");
        }

        lv_label_set_text(row, rowText);
        lv_obj_set_style_text_color(row,
                                    lv_color_hex(isSelected ? CLR_YELLOW :
                                                 (hasPMKID ? CLR_GREEN : CLR_WHITE)),
                                    0);

        for (int b = 0; b < 4; b++) {
            if (!_wifiListBars[i][b]) continue;
            lv_obj_set_style_bg_color(_wifiListBars[i][b],
                                      lv_color_hex(b < bars ? CLR_CYAN : 0x1A1A1A),
                                      0);
            lv_obj_remove_flag(_wifiListBars[i][b], LV_OBJ_FLAG_HIDDEN);
        }
    }

    _setActionHints(spectreWifiListBindings());

    if (netCount > LIST_VISIBLE_ROWS) {
        constexpr int trackH = LIST_VISIBLE_ROWS * WIFI_LIST_ROW_STEP;
        int barH = (LIST_VISIBLE_ROWS * trackH) / netCount;
        if (barH < 8) barH = 8;
        int barY = (scroll * trackH) / netCount;
        lv_obj_set_size(_wifiListScrollbar, 2, barH);
        lv_obj_set_pos(_wifiListScrollbar,
            THEME_SCREEN_W - THEME_DIVIDER_X - 5,
            WIFI_LIST_ROW_Y + barY);
        lv_obj_remove_flag(_wifiListScrollbar, LV_OBJ_FLAG_HIDDEN);
    } else if (_wifiListScrollbar) {
        lv_obj_add_flag(_wifiListScrollbar, LV_OBJ_FLAG_HIDDEN);
    }
}

void DisplayManager::_closeWifiList() {
    if (_wifiListPanel) {
        lv_obj_add_flag(_wifiListPanel, LV_OBJ_FLAG_HIDDEN);
    }
    _wifiListOpen = false;
}

void DisplayManager::closeWifiList() {
    _closeWifiList();

    STATE_WRITE_BEGIN();
    g_state.wifiListActive = false;
    g_state.wifiListScroll = clampListScroll(g_state.wifiListSelected,
                                             g_state.wifiListScroll,
                                             LIST_VISIBLE_ROWS,
                                             g_state.wifiSnapCount);
    STATE_WRITE_END();
    _syncActionHintsFromState();
}

void DisplayManager::scrollWifiList(int delta) {
    if (!_wifiListOpen) return;

    int netCount, scroll, selected;
    STATE_READ_BEGIN();
    netCount = g_state.wifiSnapCount;
    if (netCount > SpectreState::WIFI_SNAP_COUNT) {
        netCount = SpectreState::WIFI_SNAP_COUNT;
    }
    scroll   = g_state.wifiListScroll;
    selected = g_state.wifiListSelected;
    STATE_READ_END();

    if (netCount <= 0) {
        return;
    }

    selected = wrapListSelection(selected, delta, netCount);
    scroll = clampListScroll(selected, scroll, LIST_VISIBLE_ROWS, netCount);

    STATE_WRITE_BEGIN();
    g_state.wifiListSelected = selected;
    g_state.wifiListScroll   = scroll;
    STATE_WRITE_END();

    _updateWifiList();
}

void DisplayManager::wifiListSelect() {
    if (!_wifiListOpen) return;

    SpectreState::WiFiNetworkSnapshot net = {};
    int selected;
    int netCount;
    STATE_READ_BEGIN();
    selected = g_state.wifiListSelected;
    netCount = g_state.wifiSnapCount;
    if (netCount > SpectreState::WIFI_SNAP_COUNT) {
        netCount = SpectreState::WIFI_SNAP_COUNT;
    }
    if (selected >= 0 && selected < netCount) {
        net = g_state.wifiSnap[selected];
    }
    STATE_READ_END();

    if (selected < 0 || selected >= netCount) return;

    char bssidStr[18];
    snprintf(bssidStr, sizeof(bssidStr),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             net.bssid[0], net.bssid[1], net.bssid[2],
             net.bssid[3], net.bssid[4], net.bssid[5]);

    STATE_WRITE_BEGIN();
    strlcpy(g_state.lastProbedSSID,
            net.ssid[0] ? net.ssid : "<hidden>",
            sizeof(g_state.lastProbedSSID));
    strlcpy(g_state.lastProbedMAC, bssidStr,
            sizeof(g_state.lastProbedMAC));
    g_state.wifiChannel = net.channel;
    g_state.dataRefresh = true;
    STATE_WRITE_END();
    closeWifiList();

    char notifText[48];
    snprintf(notifText, sizeof(notifText), "SELECTED: %s",
             net.ssid[0] ? net.ssid : "<hidden>");
    showNotification(NOTIF_DEVICE_NEW, notifText);
}

void DisplayManager::wifiListHunt() {
    if (!_wifiListOpen) return;

    SpectreState::WiFiNetworkSnapshot net = {};
    int selected = 0;
    int netCount = 0;
    STATE_READ_BEGIN();
    selected = g_state.wifiListSelected;
    netCount = min(g_state.wifiSnapCount, SpectreState::WIFI_SNAP_COUNT);
    if (selected >= 0 && selected < netCount) {
        net = g_state.wifiSnap[selected];
    }
    STATE_READ_END();

    if (selected < 0 || selected >= netCount) return;
    if (strcmp(net.security, "OPEN") == 0) {
        showNotification(NOTIF_DEVICE_NEW, "OPEN NETWORK - NO KEY");
        return;
    }

    char bssidStr[18];
    snprintf(bssidStr, sizeof(bssidStr),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             net.bssid[0], net.bssid[1], net.bssid[2],
             net.bssid[3], net.bssid[4], net.bssid[5]);

    STATE_WRITE_BEGIN();
    g_state.wifiHuntRequest = true;
    strlcpy(g_state.wifiHuntSSID, net.ssid, sizeof(g_state.wifiHuntSSID));
    strlcpy(g_state.wifiHuntBSSID, bssidStr, sizeof(g_state.wifiHuntBSSID));
    g_state.dataRefresh = true;
    STATE_WRITE_END();

    char notifText[48];
    snprintf(notifText, sizeof(notifText), "HUNTING: %s", net.ssid);
    showNotification(NOTIF_PMKID, notifText);
    DLOG_INFO("WIFI", "PMKID hunt: %s %s", net.ssid, bssidStr);
}

void DisplayManager::_buildMissionList() {
    if (_missionListPanel) return;

    const uint32_t accent = displayAccentColor();
    _missionListPanel = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(_missionListPanel, THEME_DIVIDER_X + 10, THEME_STATUS_H + 16);
    lv_obj_set_size(_missionListPanel, THEME_SCREEN_W - THEME_DIVIDER_X - 20, 90);
    lv_obj_set_style_bg_color(_missionListPanel, lv_color_hex(CLR_BLACK), 0);
    lv_obj_set_style_bg_opa(_missionListPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_missionListPanel, 0, 0);
    lv_obj_set_style_radius(_missionListPanel, 0, 0);
    lv_obj_set_style_pad_all(_missionListPanel, 0, 0);
    lv_obj_clear_flag(_missionListPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_missionListPanel, LV_OBJ_FLAG_HIDDEN);

    _missionListTitle = lv_label_create(_missionListPanel);
    lv_label_set_text(_missionListTitle, "MISSIONS");
    lv_obj_set_style_text_font(_missionListTitle, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_missionListTitle, lv_color_hex(accent), 0);
    lv_obj_set_pos(_missionListTitle, 6, 2);

    for (int i = 0; i < 3; ++i) {
        _missionListRows[i] = lv_obj_create(_missionListPanel);
        lv_obj_set_pos(_missionListRows[i], 2, 18 + i * 22);
        lv_obj_set_size(_missionListRows[i], THEME_SCREEN_W - THEME_DIVIDER_X - 24, 20);
        lv_obj_set_style_bg_opa(_missionListRows[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(_missionListRows[i], 0, 0);
        lv_obj_set_style_radius(_missionListRows[i], 0, 0);
        lv_obj_set_style_pad_all(_missionListRows[i], 0, 0);
        lv_obj_clear_flag(_missionListRows[i], LV_OBJ_FLAG_SCROLLABLE);

        _missionListNameLabels[i] = makeClippedLabel(_missionListRows[i], "", CLR_WHITE,
                                                     FONT_BODY, 4, 1, 92);
        _missionListMetaLabels[i] = makeClippedLabel(_missionListRows[i], "", CLR_CYAN,
                                                     FONT_SMALL, 100, 3,
                                                     THEME_SCREEN_W - THEME_DIVIDER_X - 126);
    }
}

void DisplayManager::openMissionList() {
    if (_missionListOpen) return;
    _buildMissionList();
    lv_obj_move_foreground(_missionListPanel);
    lv_obj_remove_flag(_missionListPanel, LV_OBJ_FLAG_HIDDEN);

    STATE_WRITE_BEGIN();
    g_state.missionListActive = true;
    STATE_WRITE_END();

    _missionListOpen = true;
    _updateMissionList();
    _setActionHints(spectreMissionListBindings());
}

void DisplayManager::_updateMissionList() {
    if (!_missionListOpen || !_missionListPanel) return;
    lv_obj_move_foreground(_missionListPanel);

    const MissionProfile profiles[3] = {MISSION_RECON, MISSION_PWNY, MISSION_UPLINK};
    int selected = 0;
    STATE_READ_BEGIN();
    selected = static_cast<int>(g_state.missionSelection);
    STATE_READ_END();

    for (int i = 0; i < 3; ++i) {
        const bool isSelected = (i == selected);
        lv_obj_set_style_bg_color(_missionListRows[i],
                                  lv_color_hex(isSelected ? 0x1A1A00 : CLR_BLACK), 0);
        lv_obj_set_style_bg_opa(_missionListRows[i],
                                isSelected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_label_set_text(_missionListNameLabels[i], missionProfileName(profiles[i]));
        lv_obj_set_style_text_color(_missionListNameLabels[i],
                                    lv_color_hex(isSelected ? displayAccentColor() : CLR_WHITE), 0);
        lv_label_set_text(_missionListMetaLabels[i], missionProfileListSummary(profiles[i]));
    }
}

void DisplayManager::_closeMissionList() {
    if (_missionListPanel) {
        lv_obj_add_flag(_missionListPanel, LV_OBJ_FLAG_HIDDEN);
    }
    _missionListOpen = false;
}

void DisplayManager::closeMissionList() {
    _closeMissionList();
    STATE_WRITE_BEGIN();
    g_state.missionListActive = false;
    STATE_WRITE_END();
    _syncActionHintsFromState();
}

void DisplayManager::scrollMissionList(int delta) {
    if (!_missionListOpen) return;

    int selected = 0;
    STATE_READ_BEGIN();
    selected = static_cast<int>(g_state.missionSelection);
    STATE_READ_END();

    selected = wrapListSelection(selected, delta, static_cast<int>(MISSION_PROFILE_COUNT));

    STATE_WRITE_BEGIN();
    g_state.missionSelection = static_cast<uint8_t>(selected);
    g_state.dataRefresh = true;
    STATE_WRITE_END();

    _updateMissionList();
}

void DisplayManager::missionListSelect() {
    if (!_missionListOpen) return;
    closeMissionList();
}

void DisplayManager::_buildBadUsbList() {
    if (_badUsbListPanel) return;
    const uint32_t accent = displayAccentColor();

    _badUsbListPanel = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(_badUsbListPanel, THEME_DIVIDER_X + 6, THEME_STATUS_H + 8);
    lv_obj_set_size(_badUsbListPanel,
                    THEME_SCREEN_W - THEME_DIVIDER_X - 14,
                    THEME_ACTION_Y - THEME_STATUS_H - 16);
    lv_obj_set_style_bg_color(_badUsbListPanel, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(_badUsbListPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_badUsbListPanel, 0, 0);
    lv_obj_set_style_radius(_badUsbListPanel, 0, 0);
    lv_obj_set_style_pad_all(_badUsbListPanel, 0, 0);
    lv_obj_clear_flag(_badUsbListPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_badUsbListPanel, LV_OBJ_FLAG_HIDDEN);

    _badUsbListTitle = lv_label_create(_badUsbListPanel);
    lv_label_set_text(_badUsbListTitle, "BADUSB SCRIPTS");
    lv_obj_set_style_text_font(_badUsbListTitle, FONT_SMALL, 0);
    lv_obj_set_style_text_color(_badUsbListTitle, lv_color_hex(accent), 0);
    lv_obj_set_pos(_badUsbListTitle, 6, 2);

    for (int i = 0; i < LIST_VISIBLE_ROWS; ++i) {
        _badUsbListRows[i] = lv_obj_create(_badUsbListPanel);
        lv_obj_set_pos(_badUsbListRows[i], 2, 18 + i * 14);
        lv_obj_set_size(_badUsbListRows[i],
                        THEME_SCREEN_W - THEME_DIVIDER_X - 18, 13);
        lv_obj_set_style_bg_opa(_badUsbListRows[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(_badUsbListRows[i], 0, 0);
        lv_obj_set_style_radius(_badUsbListRows[i], 0, 0);
        lv_obj_set_style_pad_all(_badUsbListRows[i], 0, 0);
        lv_obj_clear_flag(_badUsbListRows[i], LV_OBJ_FLAG_SCROLLABLE);

        _badUsbListNameLabels[i] = lv_label_create(_badUsbListRows[i]);
        lv_obj_set_style_text_font(_badUsbListNameLabels[i], FONT_SMALL, 0);
        lv_obj_set_pos(_badUsbListNameLabels[i], 4, 0);

        _badUsbListMetaLabels[i] = lv_label_create(_badUsbListRows[i]);
        lv_obj_set_style_text_font(_badUsbListMetaLabels[i], FONT_SMALL, 0);
        lv_obj_set_style_text_color(_badUsbListMetaLabels[i], lv_color_hex(CLR_CYAN), 0);
        lv_obj_set_pos(_badUsbListMetaLabels[i], 148, 0);
    }

    _badUsbListScrollbar = lv_obj_create(_badUsbListPanel);
    lv_obj_set_style_bg_color(_badUsbListScrollbar, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(_badUsbListScrollbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_badUsbListScrollbar, 0, 0);
    lv_obj_set_style_radius(_badUsbListScrollbar, 0, 0);
    lv_obj_clear_flag(_badUsbListScrollbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_badUsbListScrollbar, LV_OBJ_FLAG_HIDDEN);
}

void DisplayManager::openBadUsbList() {
    if (_badUsbListOpen) return;

    _buildBadUsbList();
    lv_obj_move_foreground(_badUsbListPanel);
    lv_obj_remove_flag(_badUsbListPanel, LV_OBJ_FLAG_HIDDEN);
    _badUsbListOpen = true;
    _updateBadUsbList();

    const ButtonBindingSet bindings = spectreBadUsbListBindings();
    _setActionHints(bindings);
}

void DisplayManager::_updateBadUsbList() {
    if (!_badUsbListOpen || !_badUsbListPanel) return;
    lv_obj_move_foreground(_badUsbListPanel);
    const uint32_t accent = displayAccentColor();

    int selected = 0;
    int scroll = 0;
    bool armed = false;
    bool running = false;
    STATE_READ_BEGIN();
    selected = g_state.badUsbListSelected;
    scroll = g_state.badUsbListScroll;
    armed = g_state.badUsbArmed;
    running = g_state.badUsbRunning;
    STATE_READ_END();

    const BadUsbScriptInfo* scripts = BADUSB_MGR.scripts();
    const int scriptCount = BADUSB_MGR.scriptCount();

    for (int i = 0; i < LIST_VISIBLE_ROWS; ++i) {
        const int scriptIdx = scroll + i;
        if (scriptIdx >= scriptCount) {
            lv_obj_add_flag(_badUsbListRows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        lv_obj_remove_flag(_badUsbListRows[i], LV_OBJ_FLAG_HIDDEN);

        const bool isSelected = (scriptIdx == selected);
        const bool isActive = (scriptIdx == selected) && (armed || running);

        lv_obj_set_style_bg_color(_badUsbListRows[i],
                                  lv_color_hex(isSelected ? 0x1A1A00 : 0x000000), 0);
        lv_obj_set_style_bg_opa(_badUsbListRows[i],
                                isSelected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

        lv_label_set_text(_badUsbListNameLabels[i], scripts[scriptIdx].name);
        lv_obj_set_style_text_color(_badUsbListNameLabels[i],
                                    lv_color_hex(isSelected ? accent : CLR_WHITE), 0);

        char meta[12] = {};
        if (running && isActive) {
            strlcpy(meta, "RUN", sizeof(meta));
        } else if (armed && isActive) {
            strlcpy(meta, "ARM", sizeof(meta));
        } else {
            snprintf(meta, sizeof(meta), "%uL",
                     static_cast<unsigned>(scripts[scriptIdx].lineCount));
        }
        lv_label_set_text(_badUsbListMetaLabels[i], meta);
        lv_obj_set_style_text_color(_badUsbListMetaLabels[i],
                                    lv_color_hex(isActive ? CLR_GREEN : CLR_CYAN), 0);
    }

    if (scriptCount > LIST_VISIBLE_ROWS) {
        const int trackH = LIST_VISIBLE_ROWS * 14;
        const int barH = max(12, (LIST_VISIBLE_ROWS * trackH) / scriptCount);
        const int maxScroll = max(1, scriptCount - LIST_VISIBLE_ROWS);
        const int barY = 18 + ((scroll * (trackH - barH)) / maxScroll);
        lv_obj_set_size(_badUsbListScrollbar, 2, barH);
        lv_obj_set_pos(_badUsbListScrollbar,
                       THEME_SCREEN_W - THEME_DIVIDER_X - 22, barY);
        lv_obj_remove_flag(_badUsbListScrollbar, LV_OBJ_FLAG_HIDDEN);
    } else if (_badUsbListScrollbar) {
        lv_obj_add_flag(_badUsbListScrollbar, LV_OBJ_FLAG_HIDDEN);
    }
}

void DisplayManager::_closeBadUsbList() {
    if (_badUsbListPanel) {
        lv_obj_add_flag(_badUsbListPanel, LV_OBJ_FLAG_HIDDEN);
    }
    _badUsbListOpen = false;
}

void DisplayManager::closeBadUsbList() {
    if (!_badUsbListOpen) return;
    _closeBadUsbList();

    STATE_WRITE_BEGIN();
    g_state.badUsbListActive = false;
    g_state.badUsbListScroll = clampListScroll(g_state.badUsbListSelected,
                                               g_state.badUsbListScroll,
                                               LIST_VISIBLE_ROWS,
                                               g_state.badUsbScriptCount);
    g_state.dataRefresh = true;
    STATE_WRITE_END();
    _syncActionHintsFromState();
}

void DisplayManager::scrollBadUsbList(int delta) {
    if (!_badUsbListOpen) return;

    int selected = 0;
    int scroll = 0;
    int totalScripts = 0;
    STATE_READ_BEGIN();
    selected = g_state.badUsbListSelected;
    scroll = g_state.badUsbListScroll;
    totalScripts = g_state.badUsbScriptCount;
    STATE_READ_END();

    if (totalScripts <= 0) {
        return;
    }

    selected = wrapListSelection(selected, delta, totalScripts);
    scroll = clampListScroll(selected, scroll, LIST_VISIBLE_ROWS, totalScripts);

    STATE_WRITE_BEGIN();
    g_state.badUsbListSelected = selected;
    g_state.badUsbListScroll = scroll;
    STATE_WRITE_END();

    _updateBadUsbList();
}

void DisplayManager::badUsbListSelect() {
    if (!_badUsbListOpen) return;

    int selected = 0;
    int scriptCount = BADUSB_MGR.scriptCount();
    const BadUsbScriptInfo* scripts = BADUSB_MGR.scripts();
    STATE_READ_BEGIN();
    selected = g_state.badUsbListSelected;
    STATE_READ_END();

    if (selected < 0 || selected >= scriptCount) {
        return;
    }

    _closeBadUsbList();

    STATE_WRITE_BEGIN();
    g_state.badUsbListActive = false;
    strlcpy(g_state.badUsbScriptName, scripts[selected].name,
            sizeof(g_state.badUsbScriptName));
    strlcpy(g_state.badUsbScriptDesc, scripts[selected].desc,
            sizeof(g_state.badUsbScriptDesc));
    strlcpy(g_state.badUsbStatus, "IDLE", sizeof(g_state.badUsbStatus));
    g_state.badUsbHasError = false;
    g_state.badUsbError[0] = '\0';
    g_state.dataRefresh = true;
    STATE_WRITE_END();
}

void DisplayManager::drawBadUsb() {
    if (!_badUsbContent) return;
    _syncActionHintsFromState();

    bool ready = false;
    bool armed = false;
    bool running = false;
    bool hasError = false;
    uint16_t countdownMs = 0;
    uint16_t progressLine = 0;
    int scriptCount = 0;
    char scriptName[sizeof(g_state.badUsbScriptName)] = {};
    char scriptDesc[sizeof(g_state.badUsbScriptDesc)] = {};
    char status[sizeof(g_state.badUsbStatus)] = {};
    char error[sizeof(g_state.badUsbError)] = {};

    STATE_READ_BEGIN();
    ready = g_state.badUsbReady;
    armed = g_state.badUsbArmed;
    running = g_state.badUsbRunning;
    hasError = g_state.badUsbHasError;
    countdownMs = g_state.badUsbCountdownMs;
    progressLine = g_state.badUsbProgressLine;
    scriptCount = g_state.badUsbScriptCount;
    strlcpy(scriptName, g_state.badUsbScriptName, sizeof(scriptName));
    strlcpy(scriptDesc, g_state.badUsbScriptDesc, sizeof(scriptDesc));
    strlcpy(status, g_state.badUsbStatus, sizeof(status));
    strlcpy(error, g_state.badUsbError, sizeof(error));
    STATE_READ_END();

    const char* titleText = !ready ? "OFFLINE"
                           : hasError ? "FAULT"
                           : running ? "RUNNING"
                           : armed ? "ARMED"
                                   : "READY";
    uint32_t titleColor = !ready ? CLR_GREY
                        : hasError ? CLR_RED
                        : running ? CLR_GREEN
                        : armed ? CLR_YELLOW
                                : displayAccentColor();
    lv_label_set_text(_badUsbTitleValue, titleText);
    lv_obj_set_style_text_color(_badUsbTitleValue, lv_color_hex(titleColor), 0);

    const char* payloadText = scriptName[0] ? scriptName
                            : scriptCount > 0 ? "NO PAYLOAD SELECTED"
                                              : "NO PAYLOADS AVAILABLE";
    lv_label_set_text(_badUsbScriptValue, payloadText);
    lv_obj_set_style_text_color(_badUsbScriptValue,
                                lv_color_hex(scriptName[0] ? CLR_WHITE : CLR_GREY), 0);

    const char* statusText = status[0] ? status : (!ready ? "UNAVAILABLE" : "IDLE");
    uint32_t statusColor = !ready ? CLR_GREY
                         : hasError ? CLR_RED
                         : running ? (countdownMs > 0 ? CLR_YELLOW : CLR_GREEN)
                         : armed ? CLR_YELLOW
                                 : CLR_CYAN;
    lv_label_set_text(_badUsbStatusValue, statusText);
    lv_obj_set_style_text_color(_badUsbStatusValue, lv_color_hex(statusColor), 0);

    char detailBuf[64] = {};
    if (hasError) {
        snprintf(detailBuf, sizeof(detailBuf), "%s",
                 error[0] ? error : "PAYLOAD EXECUTION FAILED");
    } else if (!ready) {
        snprintf(detailBuf, sizeof(detailBuf), "USB HID BACKEND NOT READY");
    } else if (running && countdownMs > 0) {
        snprintf(detailBuf, sizeof(detailBuf), "COUNTDOWN %us",
                 static_cast<unsigned>((countdownMs + 999U) / 1000U));
    } else if (running && progressLine > 0) {
        snprintf(detailBuf, sizeof(detailBuf), "EXECUTING LINE %u",
                 static_cast<unsigned>(progressLine));
    } else if (scriptDesc[0]) {
        snprintf(detailBuf, sizeof(detailBuf), "%s", scriptDesc);
    } else if (scriptCount <= 0) {
        snprintf(detailBuf, sizeof(detailBuf), "ADD DUCKY FILES TO /badusb");
    } else {
        snprintf(detailBuf, sizeof(detailBuf), "LB opens payload list");
    }
    lv_label_set_text(_badUsbDetailValue, detailBuf);
    lv_obj_set_style_text_color(_badUsbDetailValue,
                                lv_color_hex(hasError ? CLR_RED : CLR_GREY), 0);

    char rowBuf[4][56] = {};
    uint32_t rowColor[4] = {CLR_GREY, CLR_GREY, CLR_GREY, CLR_GREY};

    if (!ready) {
        snprintf(rowBuf[0], sizeof(rowBuf[0]), "Host HID backend unavailable");
        snprintf(rowBuf[1], sizeof(rowBuf[1]), "LB scripts  B next");
        rowColor[0] = CLR_RED;
        rowColor[1] = CLR_CYAN;
    } else if (running) {
        snprintf(rowBuf[0], sizeof(rowBuf[0]), "LA stop run  LB scripts  B next");
        snprintf(rowBuf[1], sizeof(rowBuf[1]), "Line %u",
                 static_cast<unsigned>(progressLine));
        snprintf(rowBuf[2], sizeof(rowBuf[2]), "%d payloads loaded", scriptCount);
        rowColor[0] = CLR_YELLOW;
        rowColor[1] = CLR_CYAN;
        rowColor[2] = CLR_GREY;
    } else if (armed) {
        snprintf(rowBuf[0], sizeof(rowBuf[0]), "LA run armed payload");
        snprintf(rowBuf[1], sizeof(rowBuf[1]), "LB scripts  B next");
        snprintf(rowBuf[2], sizeof(rowBuf[2]), "%d payloads loaded", scriptCount);
        rowColor[0] = CLR_GREEN;
        rowColor[1] = CLR_CYAN;
        rowColor[2] = CLR_GREY;
    } else if (scriptName[0]) {
        snprintf(rowBuf[0], sizeof(rowBuf[0]), "A or LA arm selected payload");
        snprintf(rowBuf[1], sizeof(rowBuf[1]), "LB scripts  B next");
        rowColor[0] = CLR_GREEN;
        rowColor[1] = CLR_CYAN;
    } else {
        snprintf(rowBuf[0], sizeof(rowBuf[0]), "LB opens payload list");
        snprintf(rowBuf[1], sizeof(rowBuf[1]), "%d payloads discovered", scriptCount);
        snprintf(rowBuf[2], sizeof(rowBuf[2]), "B next");
        rowColor[0] = CLR_CYAN;
        rowColor[1] = scriptCount > 0 ? CLR_YELLOW : CLR_GREY;
    }

    for (int i = 0; i < 4; ++i) {
        if (!_badUsbOpLabels[i]) continue;
        if (rowBuf[i][0]) {
            lv_label_set_text(_badUsbOpLabels[i], rowBuf[i]);
            lv_obj_set_style_text_color(_badUsbOpLabels[i], lv_color_hex(rowColor[i]), 0);
            lv_obj_remove_flag(_badUsbOpLabels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(_badUsbOpLabels[i], "");
            lv_obj_add_flag(_badUsbOpLabels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void DisplayManager::drawRecon(MissionProfile selectedProfile) {
    if (!_reconContent) return;
    _syncActionHintsFromState();

    const uint32_t accent = displayAccentColor();
    lv_label_set_text(_reconModeValue, missionProfileName(selectedProfile));
    lv_obj_set_style_text_color(_reconModeValue, lv_color_hex(accent), 0);

    if (_reconScriptLabel) {
        lv_label_set_text(_reconScriptLabel, "OBJECTIVE");
        lv_obj_remove_flag(_reconScriptLabel, LV_OBJ_FLAG_HIDDEN);
    }
    if (_reconScriptValue) {
        lv_label_set_text(_reconScriptValue, missionProfileListSummary(selectedProfile));
        lv_obj_set_style_text_color(_reconScriptValue, lv_color_hex(CLR_WHITE), 0);
        lv_obj_remove_flag(_reconScriptValue, LV_OBJ_FLAG_HIDDEN);
    }
    if (_reconStatusValue) {
        lv_label_set_text(_reconStatusValue, "Select to lock runtime");
        lv_obj_set_style_text_color(_reconStatusValue, lv_color_hex(CLR_CYAN), 0);
        lv_obj_remove_flag(_reconStatusValue, LV_OBJ_FLAG_HIDDEN);
    }
    if (_reconDetailValue) {
        lv_label_set_text(_reconDetailValue, "Chosen mission owns the screen and controls");
        lv_obj_set_style_text_color(_reconDetailValue, lv_color_hex(CLR_GREY), 0);
        lv_obj_remove_flag(_reconDetailValue, LV_OBJ_FLAG_HIDDEN);
    }

    char posture[56] = {};
    switch (selectedProfile) {
        case MISSION_RECON:
            snprintf(posture, sizeof(posture), "Bias: passive wifi sweep and subghz monitor");
            break;
        case MISSION_PWNY:
            snprintf(posture, sizeof(posture), "Bias: target lock and capture pressure");
            break;
        case MISSION_UPLINK:
            snprintf(posture, sizeof(posture), "Bias: sync pipeline and export relay");
            break;
        default:
            posture[0] = '\0';
            break;
    }

    for (int i = 0; i < 4; ++i) {
        if (!_reconOpLabels[i]) continue;
        if (i == 0 && posture[0]) {
            lv_label_set_text(_reconOpLabels[i], posture);
            lv_obj_set_style_text_color(_reconOpLabels[i], lv_color_hex(CLR_GREY), 0);
            lv_obj_remove_flag(_reconOpLabels[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(_reconOpLabels[i], "");
            lv_obj_add_flag(_reconOpLabels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void DisplayManager::drawSystem(float battV, unsigned long uptimeMs,
                                const char* storage) {
    if (!_sysContent) return;
    (void)storage;
    lv_obj_remove_flag(_sysLivePanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(_debriefPanel, LV_OBJ_FLAG_HIDDEN);

    bool ext;
    bool settingsReady;
    bool timeValid;
    bool storageNearlyFull;
    bool storageFull;
    bool storageOverrun;
    bool storageDumpAdvised;
    uint8_t storageMode;
    uint8_t storagePolicy;
    uint16_t storageUsedPct;
    uint32_t storageFreeBytes;
    uint32_t storagePending;
    uint32_t storageDropped;
    uint32_t storageDeduped;
    uint16_t battVoltageMv;
    uint16_t battRuntimeMin;
    int battPercent;
    uint8_t powerSource;
    uint8_t powerState;
    bool charging;
    uint8_t radioOwner;
    char timeLocal[24];
    char timeSource[12];
    char storagePolicyText[20];

    STATE_READ_BEGIN();
    ext = g_state.antennaExternal;
    timeValid = g_state.timeValid;
    storageNearlyFull = g_state.storageNearlyFull;
    storageFull = g_state.storageFull;
    storageOverrun = g_state.storageOverrun;
    storageDumpAdvised = g_state.storageDumpAdvised;
    storageMode = g_state.storageMode;
    storagePolicy = g_state.storagePolicy;
    storageUsedPct = g_state.storageUsedPct;
    storageFreeBytes = g_state.storageFreeBytes;
    storagePending = g_state.storagePending;
    storageDropped = g_state.storageDropped;
    storageDeduped = g_state.storageDeduped;
    battVoltageMv = g_state.battVoltageMv;
    battRuntimeMin = g_state.battRuntimeMin;
    battPercent = g_state.battPercent;
    powerSource = g_state.powerSource;
    powerState = g_state.powerState;
    charging = g_state.charging;
    radioOwner = g_state.radioOwner;
    strlcpy(timeLocal, g_state.timeLocal, sizeof(timeLocal));
    strlcpy(timeSource, g_state.timeSource, sizeof(timeSource));
    strlcpy(storagePolicyText, g_state.storagePolicyText, sizeof(storagePolicyText));
    STATE_READ_END();

    settingsReady = SETTINGS.isReady();
    RuntimeSettings settings = settingsReady ? SETTINGS.snapshot() : RuntimeSettings();

    const uint32_t accent = displayAccentColor();

    const char* headerStatus = "STORAGE OK";
    uint32_t headerColor = CLR_GREEN;

    if (storageOverrun) {
        headerStatus = "OVERRUN";
        headerColor = CLR_RED;
    } else if (storageFull) {
        headerStatus = "DUMP ADVISED";
        headerColor = CLR_YELLOW;
    } else if (storageNearlyFull) {
        headerStatus = "WATCH";
        headerColor = CLR_CYAN;
    } else if (!settingsReady) {
        headerStatus = "SETTINGS OFFLINE";
        headerColor = CLR_RED;
    }

    auto modeName = [&](uint8_t mode) -> const char* {
        switch (mode) {
            case 0: return "NORMAL";
            case 1: return "WATCH";
            case 2: return "FULL";
            case 3: return "OVERRUN";
            default: return "UNKNOWN";
        }
    };

    auto policyName = [&](uint8_t policy, const char* fallback) -> const char* {
        if (fallback && fallback[0]) return fallback;
        switch (policy) {
            case 0: return "NORMAL";
            case 1: return "REDUCED";
            case 2: return "CRITICAL";
            default: return "UNKNOWN";
        }
    };

    char usedLine[24];
    snprintf(usedLine, sizeof(usedLine), "%u%% USED", storageUsedPct);

    char freeLine[24];
    if (storageFreeBytes >= (1024UL * 1024UL)) {
        snprintf(freeLine, sizeof(freeLine), "%luMB FREE",
                 static_cast<unsigned long>(storageFreeBytes / (1024UL * 1024UL)));
    } else {
        snprintf(freeLine, sizeof(freeLine), "%luKB FREE",
                 static_cast<unsigned long>(storageFreeBytes / 1024UL));
    }

    char pendingLine[24];
    snprintf(pendingLine, sizeof(pendingLine), "%lu EVT",
             static_cast<unsigned long>(storagePending));

    char dedupeLine[24];
    snprintf(dedupeLine, sizeof(dedupeLine), "%lu DD %lu DR",
             static_cast<unsigned long>(storageDeduped),
             static_cast<unsigned long>(storageDropped));

    char modeLine[24];
    snprintf(modeLine, sizeof(modeLine), "%s", modeName(storageMode));

    char policyLine[24];
    snprintf(policyLine, sizeof(policyLine), "%s",
             policyName(storagePolicy, storagePolicyText));

    unsigned long s  = uptimeMs / 1000UL;
    unsigned long m  = s / 60UL;
    unsigned long h  = m / 60UL;

    char timeLine[24];
    if (timeValid && timeLocal[0]) {
        snprintf(timeLine, sizeof(timeLine), "%.23s", timeLocal);
    } else {
        snprintf(timeLine, sizeof(timeLine), "UP %luh %lum",
                 static_cast<unsigned long>(h),
                 static_cast<unsigned long>(m % 60UL));
    }

    const char* linkState = "IDLE";
    switch ((RadioOwner)radioOwner) {
        case RADIO_WIFI_CAPTURE: linkState = "WIFI RX"; break;
        case RADIO_WIFI_UPLOAD:  linkState = "WIFI UP"; break;
        case RADIO_WIFI_SCAN:    linkState = "WIFI SCN"; break;
        case RADIO_WIFI_PMKID:   linkState = "PMKID";   break;
        case RADIO_BLE_GPS:      linkState = "BLE GPS"; break;
        case RADIO_BLE_TEXT:     linkState = "BLE TXT"; break;
        default: break;
    }

    char radioLine[24];
    snprintf(radioLine, sizeof(radioLine), "%s %s", linkState, ext ? "EXT" : "INT");

    char cfgLine[24];
    if (powerSource == POWER_SOURCE_USB) {
        snprintf(cfgLine, sizeof(cfgLine), "USB %s %.2fV",
                 charging ? "CHG" : "EXT",
                 battVoltageMv > 0 ? (static_cast<float>(battVoltageMv) / 1000.0f) : battV);
    } else if (powerState == POWER_STATE_BATTERY_CRITICAL && battRuntimeMin > 0) {
        snprintf(cfgLine, sizeof(cfgLine), "CRT %uM %.2fV",
                 static_cast<unsigned>(battRuntimeMin),
                 battVoltageMv > 0 ? (static_cast<float>(battVoltageMv) / 1000.0f) : battV);
    } else if (battVoltageMv > 0) {
        snprintf(cfgLine, sizeof(cfgLine), "BAT %d%% %.2fV",
                 battPercent,
                 static_cast<float>(battVoltageMv) / 1000.0f);
    } else if (settingsReady) {
        snprintf(cfgLine, sizeof(cfgLine), "%u AP %lus",
                 settings.wifiNetworkCount,
                 static_cast<unsigned long>(settings.displayTimeoutMs / 1000UL));
    } else {
        snprintf(cfgLine, sizeof(cfgLine), "SET ?");
    }

    uint32_t usedColor = CLR_GREEN;
    if (storageOverrun) usedColor = CLR_RED;
    else if (storageFull) usedColor = CLR_YELLOW;
    else if (storageNearlyFull) usedColor = CLR_CYAN;

    uint32_t pendingColor = storagePending > 0 ? CLR_YELLOW : CLR_WHITE;
    if (storageOverrun) pendingColor = CLR_RED;

    uint32_t modeColor = CLR_GREEN;
    if (storageOverrun) modeColor = CLR_RED;
    else if (storageFull) modeColor = CLR_YELLOW;
    else if (storageNearlyFull) modeColor = CLR_CYAN;

    uint32_t dumpColor = storageDumpAdvised ? CLR_YELLOW : CLR_GREEN;
    const char* dumpText = storageDumpAdvised ? "PHONE ON" : "READY";

    lv_label_set_text(_sysHeaderStatus, headerStatus);
    lv_obj_set_style_text_color(_sysHeaderStatus, lv_color_hex(headerColor), 0);

    lv_label_set_text(_sysStorageValue, usedLine);
    lv_obj_set_style_text_color(_sysStorageValue, lv_color_hex(usedColor), 0);
    lv_label_set_text(_sysFreeValue, freeLine);
    lv_obj_set_style_text_color(_sysFreeValue, lv_color_hex(CLR_WHITE), 0);

    lv_label_set_text(_sysPendingValue, pendingLine);
    lv_obj_set_style_text_color(_sysPendingValue, lv_color_hex(pendingColor), 0);
    lv_label_set_text(_sysDedupeValue, dedupeLine);
    lv_obj_set_style_text_color(_sysDedupeValue,
                                lv_color_hex(storageDropped > 0 ? CLR_YELLOW : CLR_CYAN), 0);

    lv_label_set_text(_sysModeValue, modeLine);
    lv_obj_set_style_text_color(_sysModeValue, lv_color_hex(modeColor), 0);
    lv_label_set_text(_sysPolicyValue, policyLine);
    lv_obj_set_style_text_color(_sysPolicyValue, lv_color_hex(CLR_WHITE), 0);

    lv_label_set_text(_sysTimeLabel, timeValid ? timeSource : "UPTIME");
    lv_label_set_text(_sysTimeValue, timeLine);
    lv_obj_set_style_text_color(_sysTimeValue, lv_color_hex(timeValid ? CLR_GREEN : CLR_CYAN), 0);

    lv_label_set_text(_sysDumpValue, dumpText);
    lv_obj_set_style_text_color(_sysDumpValue, lv_color_hex(dumpColor), 0);
    lv_label_set_text(_sysRadioValue, radioLine);
    lv_label_set_text(_sysCfgValue, cfgLine);
    lv_obj_set_style_text_color(_sysCfgValue,
                                lv_color_hex(powerState == POWER_STATE_BATTERY_CRITICAL
                                                 ? CLR_RED
                                                 : (powerState == POWER_STATE_BATTERY_ECONOMY
                                                        ? CLR_YELLOW
                                                        : (powerSource == POWER_SOURCE_USB
                                                               ? (charging ? CLR_GREEN : CLR_CYAN)
                                                               : accent))),
                                0);
}

void DisplayManager::drawDebrief() {
    if (!_sysContent) return;
    const ButtonBindingSet bindings = spectreDebriefBindings();
    _setActionHints(bindings);
    lv_obj_add_flag(_sysLivePanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(_debriefPanel, LV_OBJ_FLAG_HIDDEN);

    int nets, devs, probes, pmkids, drones, files;
    uint32_t uptime;
    char tag[32];
    bool tagSet;
    bool exportLastOk;
    uint32_t exportLastEvents;
    uint16_t exportLastFiles;
    uint32_t exportLastBytes;
    uint32_t exportLastPending;
    char exportLastISO[24];
    char exportLastSessionId[40];
    STATE_READ_BEGIN();
    nets    = g_state.sessionNetworks;
    devs    = g_state.sessionDevices;
    probes  = g_state.sessionProbes;
    pmkids  = g_state.sessionPMKIDs;
    drones  = g_state.sessionDrones;
    files   = STORAGE.isReady()
        ? static_cast<int>(STORAGE.getAuthoritativePendingEventCount())
        : g_state.sessionFilesPending;
    tagSet  = g_state.sessionTagSet;
    strlcpy(tag, g_state.sessionTag, sizeof(tag));
    uptime  = millis();
    exportLastOk = g_state.exportLastOk;
    exportLastEvents = g_state.exportLastEvents;
    exportLastFiles = g_state.exportLastFiles;
    exportLastBytes = g_state.exportLastBytes;
    exportLastPending = g_state.exportLastPending;
    strlcpy(exportLastISO, g_state.exportLastISO, sizeof(exportLastISO));
    strlcpy(exportLastSessionId, g_state.exportLastSessionId, sizeof(exportLastSessionId));
    STATE_READ_END();

    uint32_t h = uptime / 3600000;
    uint32_t m = (uptime % 3600000) / 60000;
    uint32_t s = (uptime % 60000) / 1000;

    char buf[32];

    snprintf(buf, sizeof(buf), "%luh %lum %lus", h, m, s);
    lv_label_set_text(_debriefDurationValue, buf);
    lv_obj_set_style_text_color(_debriefDurationValue, lv_color_hex(CLR_CYAN), 0);

    snprintf(buf, sizeof(buf), "%d", nets);
    lv_label_set_text(_debriefNetworksValue, buf);
    lv_obj_set_style_text_color(_debriefNetworksValue, lv_color_hex(CLR_YELLOW), 0);

    snprintf(buf, sizeof(buf), "%d", devs);
    lv_label_set_text(_debriefDevicesValue, buf);
    lv_obj_set_style_text_color(_debriefDevicesValue, lv_color_hex(CLR_YELLOW), 0);

    snprintf(buf, sizeof(buf), "%d", probes);
    lv_label_set_text(_debriefProbesValue, buf);
    lv_obj_set_style_text_color(_debriefProbesValue, lv_color_hex(CLR_YELLOW), 0);

    snprintf(buf, sizeof(buf), "%d", pmkids);
    lv_label_set_text(_debriefPMKIDsValue, buf);
    lv_obj_set_style_text_color(_debriefPMKIDsValue,
                                lv_color_hex(pmkids > 0 ? CLR_GREEN : CLR_GREY), 0);

    snprintf(buf, sizeof(buf), "%d", drones);
    lv_label_set_text(_debriefDronesValue, buf);
    lv_obj_set_style_text_color(_debriefDronesValue,
                                lv_color_hex(drones > 0 ? CLR_RED : CLR_GREY), 0);

    if (exportLastOk && exportLastFiles > 0) {
        if (exportLastPending > 0) {
            snprintf(buf, sizeof(buf), "%uF %luQ",
                     exportLastFiles,
                     static_cast<unsigned long>(exportLastPending));
        } else {
            const uint32_t exportKb = (exportLastBytes + 1023UL) / 1024UL;
            snprintf(buf, sizeof(buf), "%uF %luK",
                     exportLastFiles,
                     static_cast<unsigned long>(exportKb));
        }
    } else {
        snprintf(buf, sizeof(buf), "%d pending", files);
    }
    lv_label_set_text(_debriefExportValue, buf);
    lv_obj_set_style_text_color(_debriefExportValue,
                                lv_color_hex(exportLastPending > 0 ? CLR_YELLOW : (exportLastOk ? CLR_GREEN : (files > 0 ? CLR_YELLOW : CLR_GREY))),
                                0);

    const bool hasExportSummary =
        exportLastOk && (exportLastISO[0] || exportLastSessionId[0] || exportLastEvents > 0);
    const char* lowerLabel = tagSet ? "TAG" : (hasExportSummary ? "LAST" : "TAG");
    lv_label_set_text(_debriefLowerLabel, lowerLabel);
    if (tagSet) {
        lv_label_set_text(_debriefLowerValue, tag);
        lv_obj_set_style_text_color(_debriefLowerValue, lv_color_hex(CLR_GREEN), 0);
    } else if (exportLastOk && exportLastISO[0]) {
        char lastLine[20] = {};
        if (strlen(exportLastISO) >= 16) {
            snprintf(lastLine, sizeof(lastLine), "%.5s %.5s",
                     exportLastISO + 5,
                     exportLastISO + 11);
        } else {
            snprintf(lastLine, sizeof(lastLine), "%.19s", exportLastISO);
        }
        lv_label_set_text(_debriefLowerValue, lastLine);
        lv_obj_set_style_text_color(_debriefLowerValue, lv_color_hex(CLR_GREEN), 0);
    } else if (exportLastOk && exportLastSessionId[0]) {
        char lastId[40] = {};
        snprintf(lastId, sizeof(lastId), "%.39s", exportLastSessionId);
        lv_label_set_text(_debriefLowerValue, lastId);
        lv_obj_set_style_text_color(_debriefLowerValue,
                                    lv_color_hex(exportLastPending > 0 ? CLR_YELLOW : CLR_GREEN), 0);
    } else if (exportLastOk && exportLastEvents > 0) {
        snprintf(buf, sizeof(buf), "%lu events",
                 static_cast<unsigned long>(exportLastEvents));
        lv_label_set_text(_debriefLowerValue, buf);
        lv_obj_set_style_text_color(_debriefLowerValue, lv_color_hex(CLR_GREEN), 0);
    } else {
        lv_label_set_text(_debriefLowerValue, "NONE");
        lv_obj_set_style_text_color(_debriefLowerValue, lv_color_hex(CLR_GREY), 0);
    }
}

// ─── Helpers ──────────────────────────────────────────────────

lv_obj_t* DisplayManager::_makeLabel(lv_obj_t* parent, const char* text,
                                      uint32_t color, const lv_font_t* font,
                                      lv_align_t align, int xOfs, int yOfs) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
    lv_obj_align(lbl, align, xOfs, yOfs);
    return lbl;
}

lv_obj_t* DisplayManager::_makePanel(lv_obj_t* parent,
                                      int x, int y, int w, int h,
                                      uint32_t bgColor, uint32_t borderColor) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, lv_color_hex(bgColor), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(borderColor), 0);
    lv_obj_set_style_border_width(panel, borderColor == CLR_BLACK ? 0 : 1, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}
