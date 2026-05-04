

#include <Arduino.h>
#include <unity.h>

#include "../../src/core/ExecutionPolicy.h"

namespace {

constexpr ExecutionPolicy::StatusBarStateView kBaseStatus{
    72,
    false,
    false,
    false,
    static_cast<uint8_t>(RADIO_NONE)
};

constexpr ExecutionPolicy::StatusBarStateView kWifiStatus{
    72,
    true,
    false,
    false,
    static_cast<uint8_t>(RADIO_WIFI_CAPTURE)
};

constexpr ExecutionPolicy::UiRefreshMarks kZeroMarks{};
constexpr ExecutionPolicy::UiRefreshSchedule kSchedule{
    2000UL,
    750UL,
    1000UL
};

static_assert(ExecutionPolicy::deriveOperationalMode(RADIO_WIFI_CAPTURE,
                                                     MODE_STANDBY) == MODE_WIFI_RECON,
              "capture owner should map to wifi recon");
static_assert(ExecutionPolicy::deriveOperationalMode(RADIO_WIFI_PMKID,
                                                     MODE_STANDBY) == MODE_PWNY,
              "pmkid owner should map to pwny");
static_assert(ExecutionPolicy::deriveOperationalMode(RADIO_NONE,
                                                     MODE_PWNY) == MODE_STANDBY,
              "pwny fallback must not survive without a lease");
static_assert(ExecutionPolicy::shouldTickWiFi(RADIO_WIFI_SCAN),
              "wifi scan should service the wifi manager");
static_assert(!ExecutionPolicy::shouldTickWiFi(RADIO_WIFI_UPLOAD),
              "upload owner should not service wifi manager ticks");
static_assert(ExecutionPolicy::shouldTickBle(RADIO_BLE_GPS),
              "ble gps should service ble manager ticks");
static_assert(!ExecutionPolicy::shouldTickBle(RADIO_WIFI_CAPTURE),
              "wifi owner should not service ble manager ticks");
static_assert(ExecutionPolicy::statusBarChanged(kBaseStatus, kWifiStatus),
              "status bar diff should detect wifi/radio changes");
static_assert(ExecutionPolicy::dueUiRefresh(SCREEN_PWNY,
                                            false,
                                            1000UL,
                                            kZeroMarks,
                                            kSchedule) == ExecutionPolicy::UI_REFRESH_PWNY,
              "pwny screen should honor its refresh cadence");
static_assert(ExecutionPolicy::dueUiRefresh(SCREEN_SYSTEM,
                                            false,
                                            500UL,
                                            kZeroMarks,
                                            kSchedule) == ExecutionPolicy::UI_REFRESH_NONE,
              "system screen should wait until its interval elapses");

}  // namespace

void test_mark_ui_refresh_updates_only_requested_bucket() {
    ExecutionPolicy::UiRefreshMarks marks{};
    marks.wifiMs = 10;
    marks.pwnyMs = 20;
    marks.systemMs = 30;

    ExecutionPolicy::markUiRefresh(ExecutionPolicy::UI_REFRESH_SYSTEM, 1234UL, marks);

    TEST_ASSERT_EQUAL_UINT32(10UL, marks.wifiMs);
    TEST_ASSERT_EQUAL_UINT32(20UL, marks.pwnyMs);
    TEST_ASSERT_EQUAL_UINT32(1234UL, marks.systemMs);
}

void test_due_ui_refresh_uses_debrief_flag_for_system_refresh() {
    const ExecutionPolicy::UiRefreshMarks marks{ 100UL, 200UL, 300UL };
    const auto reason = ExecutionPolicy::dueUiRefresh(SCREEN_RECON,
                                                      true,
                                                      1501UL,
                                                      marks,
                                                      kSchedule);
    TEST_ASSERT_EQUAL_UINT8(ExecutionPolicy::UI_REFRESH_SYSTEM, reason);
}

void test_mode_consistency_detects_stale_effective_mode() {
    TEST_ASSERT_TRUE(ExecutionPolicy::isModeConsistent(RADIO_WIFI_CAPTURE,
                                                       MODE_STANDBY,
                                                       MODE_WIFI_RECON));
    TEST_ASSERT_FALSE(ExecutionPolicy::isModeConsistent(RADIO_WIFI_CAPTURE,
                                                        MODE_STANDBY,
                                                        MODE_HOMELAB_SYNC));
}

void setup() {
    delay(1000);
    UNITY_BEGIN();
    RUN_TEST(test_mark_ui_refresh_updates_only_requested_bucket);
    RUN_TEST(test_due_ui_refresh_uses_debrief_flag_for_system_refresh);
    RUN_TEST(test_mode_consistency_detects_stale_effective_mode);
    UNITY_END();
}

void loop() {}



