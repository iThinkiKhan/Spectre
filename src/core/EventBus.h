#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum EventType {
    EVT_LORA_PACKET_RX,
    EVT_LORA_PACKET_TX,
    EVT_LORA_TX_OK,
    EVT_LORA_TX_FAIL,
    EVT_LORA_READY,

    EVT_WIFI_CONNECTED,
    EVT_WIFI_DISCONNECTED,
    EVT_WIFI_SCAN_RESULT,
    EVT_WIFI_PROBE_CAPTURED,
    EVT_WIFI_BEACON_CAPTURED,
    EVT_WIFI_HANDSHAKE_CAPTURED,

    EVT_BLE_CONNECTED,
    EVT_BLE_DISCONNECTED,
    EVT_BLE_GPS_UPDATE,
    EVT_BLE_DATA_OFFLOAD_REQUEST,

    EVT_BATTERY_LOW,
    EVT_BATTERY_CRITICAL,
    EVT_STORAGE_NEARLY_FULL,
    EVT_STORAGE_FULL,
    EVT_DEEP_SLEEP_REQUEST,

    EVT_MODE_CHANGE_REQUEST,
    EVT_MODE_CHANGED,
    EVT_PREFLIGHT_PASS,
    EVT_PREFLIGHT_FAIL,

    EVT_BTN_A_SHORT,
    EVT_BTN_A_LONG,
    EVT_BTN_B_SHORT,
    EVT_BTN_B_LONG,

    EVT_ANTENNA_TOGGLED,

    EVT_MQTT_CONNECTED,
    EVT_MQTT_DISCONNECTED,
    EVT_MQTT_PUBLISH_OK,

    EVT_NOTIFY,
    EVT_UI_COMMAND,

    EVT_COUNT
};

struct Event {
    static constexpr size_t TEXT_SIZE = 48;

    EventType type    = EVT_LORA_PACKET_RX;
    int32_t   intData = 0;
    char      strData[TEXT_SIZE] = {};

    Event() = default;
    explicit Event(EventType t) : type(t) {}
    Event(EventType t, int32_t i) : type(t), intData(i) {}
    Event(EventType t, const char* s) : type(t) { setText(s); }
    Event(EventType t, int32_t i, const char* s) : type(t), intData(i) { setText(s); }

    void setText(const char* s) {
        if (!s) {
            strData[0] = '\0';
            return;
        }
        snprintf(strData, sizeof(strData), "%s", s);
    }
};

class EventBus {
public:
    static EventBus& getInstance() {
        static EventBus instance;
        return instance;
    }

    bool publish(const Event& event, TickType_t waitTicks = 0) {
        if (event.type >= EVT_COUNT || !_queue) return false;
        if (xQueueSend(_queue, &event, waitTicks) != pdPASS) {
            portENTER_CRITICAL(&_dropCountMux);
            _dropCount++;
            portEXIT_CRITICAL(&_dropCountMux);
            return false;
        }
        return true;
    }

    uint32_t dropCount() const {
        portENTER_CRITICAL(&_dropCountMux);
        const uint32_t count = _dropCount;
        portEXIT_CRITICAL(&_dropCountMux);
        return count;
    }

    bool publish(EventType type) { return publish(Event(type)); }
    bool publish(EventType type, const char* data) { return publish(Event(type, data)); }
    bool publish(EventType type, int32_t data) { return publish(Event(type, data)); }
    bool publishNotification(uint8_t notifType, const char* text) {
        return publish(Event(EVT_NOTIFY, static_cast<int32_t>(notifType), text));
    }
    bool publishUiCommand(int32_t command) {
        return publish(Event(EVT_UI_COMMAND, command));
    }

    bool receive(Event& event, TickType_t waitTicks = 0) {
        if (!_queue) return false;
        return xQueueReceive(_queue, &event, waitTicks) == pdPASS;
    }

private:
    static constexpr uint8_t QUEUE_DEPTH = 16;

    EventBus() {
        _queue = xQueueCreateStatic(
            QUEUE_DEPTH,
            sizeof(Event),
            _queueBuffer,
            &_queueStorage);
    }

    QueueHandle_t _queue = nullptr;
    StaticQueue_t _queueStorage {};
    uint8_t _queueBuffer[QUEUE_DEPTH * sizeof(Event)] = {};
    mutable portMUX_TYPE _dropCountMux = portMUX_INITIALIZER_UNLOCKED;
    uint32_t _dropCount = 0;
};

#define BUS EventBus::getInstance()
