#pragma once

// =============================================================================
// Конфигурация прошивки гаражного света (ESP8266, Wemos D1 mini).
// Здесь НЕТ секретов — креды лежат в Secrets.h.
// =============================================================================

// -----------------------------------------------------------------------------
// Пины (распиновка Wemos D1 mini)
// -----------------------------------------------------------------------------
#define PIN_LIGHT_MAIN     D5   // GPIO14 — основное освещение (реле 1)
#define PIN_LIGHT_EDISON   D6   // GPIO12 — декоративные лампы Эдисона (реле 2)
#define PIN_PIR            D7   // GPIO13 — датчик движения (PIR)

#define I2C_SDA            D1   // GPIO5 — I²C SDA (AM2320)
#define I2C_SCL            D2   // GPIO4 — I²C SCL (AM2320)

// -----------------------------------------------------------------------------
// Реле
// -----------------------------------------------------------------------------
// Полярность управления реле. 1: HIGH = включено (active-HIGH модуль);
// 0: LOW = включено (active-LOW модуль). Инвертируется в applyRelay().
#define RELAY_ACTIVE_HIGH       1
// Пауза между гашением одного реле и зажиганием другого при переключении
// режима — защита от индуктивного выброса и brownout ESP8266.
#define RELAY_SWITCH_DELAY_MS   10

// -----------------------------------------------------------------------------
// PIR (датчик движения)
// -----------------------------------------------------------------------------
// INPUT — для PIR с активным выходом (HC-SR501 и клоны).
// Если сигнал плавает (открытый коллектор) — заменить на INPUT_PULLUP.
#define PIR_PIN_MODE            INPUT
#define PIR_DEBOUNCE_MS         50   // антидребезг PIR, мс

// -----------------------------------------------------------------------------
// Таймеры света
// -----------------------------------------------------------------------------
#define LIGHT_TIMEOUT_PIR_MS    600000UL    // 10 минут — источник PIR
#define LIGHT_TIMEOUT_MQTT_MS   3600000UL   // 1 час   — источник MQTT

// -----------------------------------------------------------------------------
// Интервалы loop()
// -----------------------------------------------------------------------------
#define STATUS_INTERVAL_MS      60000UL     // публикация JSON-статуса раз в минуту
#define SENSOR_READ_INTERVAL_MS 20000UL     // обновление кэша AM2320 (>= 2 с минимум датчика)
#define WIFI_RECONNECT_DELAY_MS 10000UL     // задержка между попытками Wi-Fi
#define MQTT_RECONNECT_DELAY_MS 5000UL      // задержка между попытками MQTT
#define CONNECT_TIMEOUT_MS      600000UL    // connectivity watchdog: 10 минут без Wi-Fi+MQTT -> ESP.restart()

// -----------------------------------------------------------------------------
// MQTT
// -----------------------------------------------------------------------------
#define MQTT_BUFFER_SIZE        512         // PubSubClient::setBufferSize()
#define MQTT_PORT               1883

// -----------------------------------------------------------------------------
// Топики (корень garage/light, одно устройство)
// -----------------------------------------------------------------------------
#define TOPIC_AVAILABILITY      "garage/light/availability"   // retain: online/offline (LWT)
#define TOPIC_LIGHT_SET         "garage/light/light/set"      // cmd: ON/OFF
#define TOPIC_LIGHT_STATE       "garage/light/light/state"    // retain: ON/OFF
#define TOPIC_MODE_SET          "garage/light/mode/set"       // cmd: main/edison
#define TOPIC_MODE_STATE        "garage/light/mode/state"     // retain: main/edison
#define TOPIC_MOTION_SET        "garage/light/motion/set"     // cmd: ON/OFF (enable/disable реакции)
#define TOPIC_MOTION_STATE      "garage/light/motion/state"   // без retain: ON/OFF факт движения
#define TOPIC_STATUS            "garage/light/status"         // retain: JSON раз в минуту

// -----------------------------------------------------------------------------
// Payload-константы
// -----------------------------------------------------------------------------
#define PAYLOAD_ON              "ON"
#define PAYLOAD_OFF             "OFF"
#define PAYLOAD_ONLINE          "online"
#define PAYLOAD_OFFLINE         "offline"
#define PAYLOAD_MODE_MAIN       "main"
#define PAYLOAD_MODE_EDISON     "edison"

// -----------------------------------------------------------------------------
// AM2320 (датчик температуры/влажности)
// -----------------------------------------------------------------------------
// Переключатель: 1 — датчик включён; 0 — temp/humidity в JSON = null
// (позволяет отладить логику света без подключённого датчика).
#define CFG_ENABLE_AM2320       1

// Sanity-диапазоны (вне диапазона -> null, защита от артефактов).
#define TEMP_MIN                (-40.0f)
#define TEMP_MAX                85.0f
#define HUM_MIN                 0.0f
#define HUM_MAX                 100.0f
