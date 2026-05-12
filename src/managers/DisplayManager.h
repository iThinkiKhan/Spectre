
#pragma once
#include <Arduino.h>
#include <lvgl.h>
#include "../config.h"
#include "../core/RunContext.h"
#include "../core/ScreenEnum.h"
#include "../core/SpectreState.h"
#include "../ui/Mascot_LVGL.h"
#include "../ui/Theme.h"
#include "../ui/MascotState.h"

// OWNERSHIP CONTRACT
// - TaskDisplay is the only task allowed to mutate LVGL objects.
// - Hardware managers feed DisplayManager through snapshots and events only.
// - No direct hardware/radio/storage side effects belong here.

// Status bar data
struct StatusBar {
    int     battPercent  = 0;
    uint16_t runtimeMinutes = 0;
    uint8_t powerSource = POWER_SOURCE_UNKNOWN;
    uint8_t powerState = POWER_STATE_BATTERY_NORMAL;
    bool    charging = false;
    bool    wifiConnected = false;
    bool    bleConnected  = false;
    bool    loraActive    = false;
    uint8_t radioOwner    = 0;
};

class DisplayManager {
public:
    void begin();
    void setScreen(Screen s);
    Screen getScreen() { return _currentScreen; }
    void tick() { _animTick++; if (_animTick > 10000) _animTick = 0; }

    // Status bar
    void updateStatus(const StatusBar& sb);

    //Buttons
    void setActionHints(const ButtonBindingSet& bindings);

    // Screen draws — called from TaskDisplay with snapshot data
    void drawLora(const char* moduleName, const char* modeName,
                  uint32_t frequencyHz, uint16_t nodeCount,
                  const char* lastPayload, int rssi, int snr, int packets);
    void drawBadUsb();
    void drawMeshtastic(const char* node, const char* channel);
    void drawMission(MissionProfile profile);
    void drawWifi(const char* ssid, int networks, const char* probeActivity);
    void drawRecon(MissionProfile selectedProfile);
    void drawSystem(float battV, unsigned long uptimeMs, const char* storage);
    void drawDebrief();
    void syncFromState();

    // Divider animation — called every frame
    void updateDivider();

    void triggerGlitchTransition(Screen target);
    void triggerDataPulse();
    void _fireSpark();  // called by spark timer callback
    void drawMascotFrame(MascotState state, int frame);
    void tickNotif();  // call every frame from TaskDisplay
    void showNotification(uint8_t type, const char* text);
    void pulseMascot(MascotState state, uint32_t durationMs);
    void openWifiList();
    void refreshWifiList();
    void closeWifiList();
    void scrollWifiList(int delta);
    void wifiListSelect();
    void wifiListHunt();
    void openMissionList();
    void closeMissionList();
    void scrollMissionList(int delta);
    void missionListSelect();
    void openBadUsbList();
    void closeBadUsbList();
    void scrollBadUsbList(int delta);
    void badUsbListSelect();

private:
    Screen      _currentScreen = SCREEN_COUNT;
    int         _animTick      = 0;

    // LVGL screen objects — one per screen
    lv_obj_t*   _screens[SCREEN_COUNT] = {};
    lv_obj_t*   _statusBar   = nullptr;
    lv_obj_t*   _actionBar   = nullptr;
    lv_obj_t*   _mascotPanel = nullptr;
    MascotLVGL  _nativeMascot;
    bool        _nativeMascotReady = false;

    // Status bar labels
    lv_obj_t*   _lblBatt     = nullptr;
    lv_obj_t*   _lblWifi     = nullptr;
    lv_obj_t*   _lblBle      = nullptr;
    lv_obj_t*   _lblLora     = nullptr;
    lv_obj_t*   _lblScreen   = nullptr;

    // Action bar labels
    lv_obj_t*   _lblActionA  = nullptr;
    lv_obj_t*   _lblActionAL = nullptr;
    lv_obj_t*   _lblActionBL = nullptr;
    lv_obj_t*   _lblActionB  = nullptr;

    // Per-screen content containers
    lv_obj_t*   _loraContent  = nullptr;
    lv_obj_t*   _meshContent  = nullptr;
    lv_obj_t*   _badUsbContent = nullptr;
    lv_obj_t*   _badUsbTitleValue = nullptr;
    lv_obj_t*   _badUsbScriptValue = nullptr;
    lv_obj_t*   _badUsbStatusValue = nullptr;
    lv_obj_t*   _badUsbDetailValue = nullptr;
    lv_obj_t*   _badUsbOpLabels[4] = {};
    lv_obj_t*   _pwnyContent       = nullptr;
    lv_obj_t*   _pwnyTitleValue    = nullptr;
    lv_obj_t*   _pwnyStatusValue   = nullptr;
    lv_obj_t*   _pwnyStatsValue    = nullptr;
    lv_obj_t*   _pwnySessionValue  = nullptr;
    lv_obj_t*   _pwnyTargetRows[8] = {};  // one label per target
    lv_obj_t*   _pwnyEapolPanel    = nullptr;
    lv_obj_t*   _pwnyEapolDivV     = nullptr;
    lv_obj_t*   _pwnyEapolDivH     = nullptr;
    lv_obj_t*   _pwnyEapolQuads[4] = {};
    lv_obj_t*   _pwnyEapolNums[4]  = {};
    lv_obj_t*   _wifiContent  = nullptr;
    lv_obj_t*   _reconContent = nullptr;
    lv_obj_t*   _sysContent   = nullptr;
    lv_obj_t*   _sysLivePanel = nullptr;
    lv_obj_t*   _debriefPanel = nullptr;

    // ─── Animation objects ────────────────────────────────────────
    lv_obj_t*   _divCanvas      = nullptr;
    lv_timer_t* _divTimer       = nullptr;
    lv_timer_t* _sparkTimer     = nullptr;
    lv_obj_t*   _divArcL        = nullptr;
    lv_obj_t*   _divArcR        = nullptr;
    int         _divPulseY      = 0;
    lv_obj_t*   _radarLine      = nullptr;
    lv_anim_t   _radarAnim;

    // Border animation styles
    lv_style_t  _styleBorderDim;
    lv_style_t  _styleBorderBright;

    // Glitch state
    bool        _reducedEffects = false;
    bool        _glitchPending  = false;
    Screen      _glitchTarget   = SCREEN_LORA;
    bool        _criticalPowerFxActive = false;
    bool        _pwnyPmkidFlashActive = false;
    uint32_t    _pwnyPmkidFlashStartedMs = 0;
    char        _pwnyPrevSlotSsid[8][24] = {};
    uint8_t     _pwnyPrevSlotAttackCount[8] = {};
    bool        _pwnyPrevSlotPmkid[8] = {};

    // Last packet alert
    bool        _alertPending   = false;
    lv_obj_t*   _rssiLabel      = nullptr;
    lv_obj_t*   _snrLabel       = nullptr;
    lv_obj_t*   _loraPacketsValue = nullptr;
    lv_obj_t*   _loraPayloadValue = nullptr;
    lv_obj_t*   _subGhzMetaValue = nullptr;
    lv_obj_t*   _meshNodeValue = nullptr;
    lv_obj_t*   _meshChannelValue = nullptr;
    lv_obj_t*   _meshLastMessageValue = nullptr;
    lv_obj_t*   _meshStatsValue = nullptr;
    lv_obj_t*   _wifiNetworksValue = nullptr;
    lv_obj_t*   _wifiDevicesValue = nullptr;
    lv_obj_t*   _wifiProbesValue = nullptr;
    lv_obj_t*   _wifiLastSSIDValue = nullptr;
    lv_obj_t*   _wifiLastMACValue = nullptr;
    lv_obj_t*   _wifiChannelValue = nullptr;
    lv_obj_t*   _reconModeValue = nullptr;
    lv_obj_t*   _reconScriptLabel = nullptr;
    lv_obj_t*   _reconScriptValue = nullptr;
    lv_obj_t*   _reconStatusValue = nullptr;
    lv_obj_t*   _reconDetailValue = nullptr;
    lv_obj_t*   _reconOpLabels[4] = {};
    lv_obj_t*   _sysHeaderStatus = nullptr;
    lv_obj_t*   _sysStorageValue = nullptr;
    lv_obj_t*   _sysFreeValue = nullptr;
    lv_obj_t*   _sysPendingValue = nullptr;
    lv_obj_t*   _sysDedupeValue = nullptr;
    lv_obj_t*   _sysModeValue = nullptr;
    lv_obj_t*   _sysPolicyValue = nullptr;
    lv_obj_t*   _sysTimeLabel = nullptr;
    lv_obj_t*   _sysTimeValue = nullptr;
    lv_obj_t*   _sysDumpValue = nullptr;
    lv_obj_t*   _sysRadioValue = nullptr;
    lv_obj_t*   _sysCfgValue = nullptr;
    lv_obj_t*   _debriefDurationValue = nullptr;
    lv_obj_t*   _debriefNetworksValue = nullptr;
    lv_obj_t*   _debriefDevicesValue = nullptr;
    lv_obj_t*   _debriefProbesValue = nullptr;
    lv_obj_t*   _debriefPMKIDsValue = nullptr;
    lv_obj_t*   _debriefDronesValue = nullptr;
    lv_obj_t*   _debriefExportValue = nullptr;
    lv_obj_t*   _debriefLowerLabel = nullptr;
    lv_obj_t*   _debriefLowerValue = nullptr;

    // Notification overlay
    lv_obj_t*  _notifPanel    = nullptr;
    lv_obj_t*  _notifLabel    = nullptr;
    lv_obj_t*  _notifIcon     = nullptr;
    uint32_t   _notifShowMs   = 0;
    bool       _notifActive   = false;
    MascotState _priorMascot  = MASCOT_STANDBY;
    bool       _mascotPulseActive = false;
    MascotState _mascotPulseState = MASCOT_STANDBY;
    uint32_t   _mascotPulseUntilMs = 0;

    // WiFi list overlay
    lv_obj_t* _wifiListPanel  = nullptr;
    lv_obj_t* _wifiListTitle = nullptr;
    lv_obj_t* _wifiListEmptyLabel = nullptr;
    lv_obj_t* _wifiListRows[8] = {};
    lv_obj_t* _wifiListSecLabel[8] = {};
    lv_obj_t* _wifiListSSIDLabel[8] = {};
    lv_obj_t* _wifiListChannelLabel[8] = {};
    lv_obj_t* _wifiListBars[8][4] = {};
    lv_obj_t* _wifiListTagLabel[8] = {};
    lv_obj_t* _wifiListScrollbar = nullptr;
    bool      _wifiListOpen   = false;

    // Mission list overlay
    lv_obj_t* _missionListPanel = nullptr;
    lv_obj_t* _missionListRows[3] = {};
    lv_obj_t* _missionListTitle = nullptr;
    lv_obj_t* _missionListNameLabels[3] = {};
    lv_obj_t* _missionListMetaLabels[3] = {};
    bool      _missionListOpen = false;

    // BadUSB list overlay
    lv_obj_t* _badUsbListPanel = nullptr;
    lv_obj_t* _badUsbListRows[8] = {};
    lv_obj_t* _badUsbListTitle = nullptr;
    lv_obj_t* _badUsbListNameLabels[8] = {};
    lv_obj_t* _badUsbListMetaLabels[8] = {};
    lv_obj_t* _badUsbListScrollbar = nullptr;
    bool      _badUsbListOpen = false;

    static uint8_t* _divBuf;

    // Builders
    bool _ensureCanvasBuffers();
    void _buildStatusBar();
    void _buildActionBar();
    void _buildMascotPanel();
    void _buildDivider();
    void _updateDivCanvas();
    static void _divTimerCb(lv_timer_t* t);
    void _buildScreenLora();
    void _buildScreenMeshtastic();
    void _buildScreenBadUsb();
    void _buildScreenPwny();
    void _buildScreenWifi();
    void _buildScreenRecon();
    void _buildScreenSystem();

    // Helpers
    lv_obj_t* _makeLabel(lv_obj_t* parent, const char* text,
                         uint32_t color, const lv_font_t* font,
                         lv_align_t align, int xOfs, int yOfs);
    lv_obj_t* _makePanel(lv_obj_t* parent,
                         int x, int y, int w, int h,
                         uint32_t bgColor, uint32_t borderColor);
    void      _setActionHints(const char* aShort, const char* aLong,
                              const char* bLong, const char* bShort);
    void      _setActionHints(const ButtonBindingSet& bindings,
                              bool busy = false);
    void      _buildRadarSweep();
    void      _setPanelBorderColor(lv_obj_t* panel, uint32_t color);
    void      _setCriticalPowerFx(bool active);
    void      _runGlitchTransition(Screen target);

    void _buildNotifPanel();
    void _showNotif(uint8_t type, const char* text);
    void _dismissNotif();

    void _buildWifiList();
    void _updateWifiList();
    void _closeWifiList();
    void _buildMissionList();
    void _updateMissionList();
    void _closeMissionList();
    void _buildBadUsbList();
    void _updateBadUsbList();
    void _closeBadUsbList();
    void _updatePwnyHandshakeViz(uint8_t eapolMask);
    void _detectPwnyPmkidFlash(const SpectreState::PwnyTargetDisplay* targets,
                               int count,
                               const char* statusText);
    void _syncActionHintsFromState();
};

