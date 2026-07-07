// =============================================================================
// Прошивка гаражного света на ESP8266 (Wemos D1 mini).
//
// Назначение: управление двумя группами света (основная D5 / декоративная
// Edison D6) в двух взаимоисключающих режимах, с автоматикой по PIR-датчику,
// MQTT-командами, минутным JSON-статусом и отключаемой реакцией на движение.
//
// Вся логика ведётся в этом файле. Конфиг — Config.h, креды — Secrets.h.
// =============================================================================

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>

#include "Config.h"   // должен идти ДО условного include AM2320 (содержит CFG_ENABLE_AM2320)
#include "Secrets.h"

#if CFG_ENABLE_AM2320
#include <Adafruit_AM2320.h>
#endif

// -----------------------------------------------------------------------------
// Глобальные объекты
// -----------------------------------------------------------------------------
WiFiClient     espClient;
PubSubClient   mqtt(espClient);
#if CFG_ENABLE_AM2320
Adafruit_AM2320 am2320;
#endif

// -----------------------------------------------------------------------------
// Режимы и источники света
// -----------------------------------------------------------------------------
enum LightMode   : uint8_t { MODE_MAIN = 0, MODE_EDISON = 1 };
enum LightSource : uint8_t { SRC_NONE = 0, SRC_PIR = 1, SRC_MQTT = 2 };

// -----------------------------------------------------------------------------
// Состояние
// -----------------------------------------------------------------------------
// Свет и таймер
bool          lightState    = false;   // вкл/выкл
LightMode     currentMode   = MODE_MAIN;
LightSource   currentSource = SRC_NONE;
unsigned long lightOffAt    = 0;       // deadline выключения (millis()+timeout), 0 = таймер неактивен

// PIR (опрос с антидребезгом по фронту)
bool          pirStable     = false;   // стабильное состояние после debounce
bool          pirRawLast    = false;   // последнее «сырое» чтение пина
unsigned long pirDebounceTs = 0;       // когда последний раз видели изменение
bool          motionEnabled = true;    // реакция на PIR (motion/set ON/OFF)
unsigned long motionEnableAt = 0;      // deadline авто-возврата реакции, 0 = таймер неактивен

// AM2320 (кэш значений — чтение развязано с публикацией статуса)
#if CFG_ENABLE_AM2320
bool          sensorOk      = false;
float         lastTemp      = NAN;
float         lastHum       = NAN;
unsigned long lastSensorRead = 0;
#endif

// Тайминги loop()
unsigned long lastWifiAttempt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastStatusPub   = 0;
bool          statusDirty     = false;  // состояние изменилось — статус уйдёт немедленно

// Connectivity watchdog: последний момент, когда и Wi-Fi, и MQTT были подключены.
unsigned long lastAllOkTs     = 0;

// Буфер для JSON-статуса
char statusBuf[256];

// -----------------------------------------------------------------------------
// Прототипы функций
// -----------------------------------------------------------------------------
void applyRelay(uint8_t pin, bool on);
void applyMode(LightMode mode);
void turnLightOn(LightSource src, LightMode mode);
void turnLightOff();
void extendTimer();
void handleLightTimer();
void handleMotionTimer();
void pollPir();
void readAm2320();
void handleLightSet(const char* cmd);
void handleModeSet(const char* cmd);
void handleMotionSet(const char* cmd);
void onMqttMessage(char* topic, byte* payload, unsigned int length);
bool connectWifi();
bool connectMqtt();
void publishAvailability(bool online);
void publishLightState();
void publishModeState();
void publishMotionState();
void publishMotionEnabledState();
void publishStatusJson();

// =============================================================================
// Реле и режимы
// =============================================================================

// Инкапсулирует полярность управления реле.
void applyRelay(uint8_t pin, bool on) {
    digitalWrite(pin, RELAY_ACTIVE_HIGH ? on : !on);
}

// Зажигает пин активного режима, второй гасит. Режимы взаимоисключающие.
// Пауза между гашением и зажиганием — защита от индуктивного выброса реле.
void applyMode(LightMode mode) {
    applyRelay(PIN_LIGHT_MAIN, false);
    applyRelay(PIN_LIGHT_EDISON, false);
    delay(RELAY_SWITCH_DELAY_MS);   // также кормит core WDT
    if (mode == MODE_MAIN) {
        applyRelay(PIN_LIGHT_MAIN, true);
    } else {
        applyRelay(PIN_LIGHT_EDISON, true);
    }
    currentMode = mode;
}

// =============================================================================
// Логика света и таймера
// =============================================================================

// Включить свет от источника src в режиме mode, поставить таймер выключения.
void turnLightOn(LightSource src, LightMode mode) {
    currentSource = src;
    unsigned long timeout = (src == SRC_PIR) ? LIGHT_TIMEOUT_PIR_MS : LIGHT_TIMEOUT_MQTT_MS;
    lightOffAt  = millis() + timeout;
    lightState  = true;
    applyMode(mode);
    publishLightState();
    publishModeState();
    statusDirty = true;

    Serial.print(F("Light ON  src="));
    Serial.print(src == SRC_PIR ? F("PIR") : F("MQTT"));
    Serial.print(F(" mode="));
    Serial.print(mode == MODE_MAIN ? PAYLOAD_MODE_MAIN : PAYLOAD_MODE_EDISON);
    Serial.print(F(" timeout_ms="));
    Serial.println(timeout);
}

// Выключить свет, сбросить источник и режим на main.
// Реле гасятся напрямую (без applyMode — тот повторно зажёг бы реле режима).
// Флаг motionEnabled НЕ трогаем: реакция на PIR управляется только motion/set.
void turnLightOff() {
    lightState    = false;
    currentSource = SRC_NONE;
    lightOffAt    = 0;
    currentMode   = MODE_MAIN;   // режим по умолчанию (логически, без энергизации реле)
    applyRelay(PIN_LIGHT_MAIN, false);
    applyRelay(PIN_LIGHT_EDISON, false);
    publishLightState();
    publishModeState();
    statusDirty = true;

    Serial.println(F("Light OFF, mode reset to main"));
}

// Обнулить таймер на длительность текущего источника (PIR -> 10 мин, MQTT -> 1 ч).
void extendTimer() {
    if (!lightState) return;     // нечего продлевать
    unsigned long timeout = (currentSource == SRC_PIR) ? LIGHT_TIMEOUT_PIR_MS : LIGHT_TIMEOUT_MQTT_MS;
    lightOffAt = millis() + timeout;

    Serial.print(F("Timer extended to "));
    Serial.print(timeout);
    Serial.print(F(" ms (source="));
    Serial.print(currentSource == SRC_PIR ? F("PIR") : F("MQTT"));
    Serial.println(')');
}

// Проверка истечения таймера выключения. Разностная форма корректна через
// переполнение millis() (~49.7 дней).
void handleLightTimer() {
    if (lightOffAt == 0) return;
    if ((long)(millis() - lightOffAt) >= 0) {
        Serial.println(F("Timer expired"));
        turnLightOff();
    }
}

// =============================================================================
// PIR (опрос с антидребезгом по фронту)
// =============================================================================

void pollPir() {
    bool rawNow = digitalRead(PIN_PIR);

    // Сырое чтение изменилось — перезапускаем debounce-таймер.
    if (rawNow != pirRawLast) {
        pirRawLast    = rawNow;
        pirDebounceTs = millis();
        return;
    }

    // Сырое чтение стабильно и отличается от зафиксированного состояния,
    // debounce истёк — фиксируем новый фронт.
    if (rawNow != pirStable && (millis() - pirDebounceTs) >= PIR_DEBOUNCE_MS) {
        bool prev = pirStable;
        pirStable = rawNow;

        // Факт движения публикуем всегда (развязка «наблюдение vs реакция»).
        publishMotionState();
        statusDirty = true;

        // Фронт LOW -> HIGH: движение началось.
        if (!prev && pirStable) {
            Serial.println(F("PIR: motion START"));
            if (motionEnabled) {
                if (!lightState) {
                    turnLightOn(SRC_PIR, currentMode);   // свет был выключен — включаем на 10 мин
                } else {
                    extendTimer();                        // свет горит — обнуляем таймер текущего источника
                }
            } else {
                Serial.println(F("  motion disabled, no action"));
            }
        } else if (prev && !pirStable) {
            // Фронт HIGH -> LOW: движение закончилось — только публикация (уже сделана).
            Serial.println(F("PIR: motion END"));
        }
    }
}

// =============================================================================
// AM2320 (кэш значений)
// =============================================================================

void readAm2320() {
#if CFG_ENABLE_AM2320
    if (!sensorOk) return;
    if ((millis() - lastSensorRead) < SENSOR_READ_INTERVAL_MS) return;
    lastSensorRead = millis();

    yield();   // кормит core WDT перед долгим I²C-чтением (~0.5–1 с)
    float t = am2320.readTemperature();
    float h = am2320.readHumidity();
    yield();

    // Валидация: isnan + sanity-диапазон.
    if (!isnan(t) && t >= TEMP_MIN && t <= TEMP_MAX) {
        lastTemp = t;
    } else {
        lastTemp = NAN;
    }
    if (!isnan(h) && h >= HUM_MIN && h <= HUM_MAX) {
        lastHum = h;
    } else {
        lastHum = NAN;
    }

    if (isnan(lastTemp) || isnan(lastHum)) {
        Serial.println(F("AM2320: read failed"));
    }
#endif
}

// =============================================================================
// Обработка MQTT-команд
// =============================================================================

void handleLightSet(const char* cmd) {
    if (strcasecmp(cmd, PAYLOAD_ON) == 0) {
        turnLightOn(SRC_MQTT, currentMode);
    } else if (strcasecmp(cmd, PAYLOAD_OFF) == 0) {
        turnLightOff();
    } else {
        Serial.print(F("light/set: unknown payload: "));
        Serial.println(cmd);
    }
}

void handleModeSet(const char* cmd) {
    LightMode target;
    if (strcasecmp(cmd, PAYLOAD_MODE_MAIN) == 0) {
        target = MODE_MAIN;
    } else if (strcasecmp(cmd, PAYLOAD_MODE_EDISON) == 0) {
        target = MODE_EDISON;
    } else {
        Serial.print(F("mode/set: unknown payload: "));
        Serial.println(cmd);
        return;
    }

    if (lightState) {
        // Свет горит — переключить активный пин, таймер и источник не трогать.
        applyMode(target);
        publishModeState();
        statusDirty = true;
        Serial.print(F("Mode switched (light on) -> "));
        Serial.println(target == MODE_MAIN ? PAYLOAD_MODE_MAIN : PAYLOAD_MODE_EDISON);
    } else {
        // Свет выключен — включить в выбранном режиме на 1 час (источник MQTT).
        turnLightOn(SRC_MQTT, target);
    }
}

void handleMotionSet(const char* cmd) {
    if (strcasecmp(cmd, PAYLOAD_ON) == 0) {
        motionEnabled  = true;
        motionEnableAt = 0;          // авто-возврат больше не нужен
        Serial.println(F("Motion reaction: ON"));
    } else if (strcasecmp(cmd, PAYLOAD_OFF) == 0) {
        motionEnabled  = false;
        motionEnableAt = millis() + MOTION_AUTO_ENABLE_MS;   // сам включится через 6 ч
        Serial.println(F("Motion reaction: OFF (auto-enable in 6h)"));
    } else {
        Serial.print(F("motion/set: unknown payload: "));
        Serial.println(cmd);
        return;
    }
    // Состояние реакции публикуем отдельным retain-топиком (без задержки статуса).
    publishMotionEnabledState();
    statusDirty = true;
}

// Авто-возврат реакции на PIR: если её отключили и прошло MOTION_AUTO_ENABLE_MS,
// снова включаем (сброс к дефолту). Разностная форма корректна через overflow millis().
void handleMotionTimer() {
    if (motionEnableAt == 0) return;
    if ((long)(millis() - motionEnableAt) >= 0) {
        motionEnabled  = true;
        motionEnableAt = 0;
        Serial.println(F("Motion reaction: auto-enabled after timeout"));
        publishMotionEnabledState();
        statusDirty = true;
    }
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
    if (!mqtt.connected()) return;
    if (length == 0) return;   // пустой payload игнорируем

    // payload не нуль-терминирован — копируем в локальный буфер с '\0'.
    char buf[16];
    size_t n = (length < sizeof(buf) - 1) ? length : (sizeof(buf) - 1);
    memcpy(buf, payload, n);
    buf[n] = '\0';

    Serial.print(F("[MQTT] "));
    Serial.print(topic);
    Serial.print(F(" => "));
    Serial.println(buf);

    // Роутинг по топику. Публикации внутри callback безопасны — payload уже скопирован.
    if (strcmp(topic, TOPIC_LIGHT_SET) == 0) {
        handleLightSet(buf);
    } else if (strcmp(topic, TOPIC_MODE_SET) == 0) {
        handleModeSet(buf);
    } else if (strcmp(topic, TOPIC_MOTION_SET) == 0) {
        handleMotionSet(buf);
    }
}

// =============================================================================
// Подключение Wi-Fi и MQTT
// =============================================================================

bool connectWifi() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.print(F("Wi-Fi: connecting to "));
    Serial.println(WIFI_SSID);

    // WiFi.mode/ persistent уже выставлены в setup().
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Ждём подключения ~10 секунд (20 × 500 мс).
    uint8_t attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts++ < 20) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print(F("Wi-Fi: connected, IP="));
        Serial.println(WiFi.localIP());
        return true;
    }

    Serial.println(F("Wi-Fi: connection failed"));
    return false;
}

bool connectMqtt() {
    Serial.print(F("MQTT: connecting to "));
    Serial.print(MQTT_HOST);
    Serial.print(':');
    Serial.println(MQTT_PORT);

    // LWT: при потере связи брокер сам опубликует availability=offline (retain).
    bool ok;
    if (strlen(MQTT_USER) > 0) {
        ok = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD,
                          TOPIC_AVAILABILITY, 0, true, PAYLOAD_OFFLINE);
    } else {
        ok = mqtt.connect(MQTT_CLIENT_ID, nullptr, nullptr,
                          TOPIC_AVAILABILITY, 0, true, PAYLOAD_OFFLINE);
    }

    if (!ok) {
        Serial.print(F("MQTT: failed, rc="));
        Serial.println(mqtt.state());   // 4 = bad creds, -2 = connect failed, ...
        return false;
    }

    Serial.println(F("MQTT: connected"));

    // Сначала подписки, затем публикация состояния — чтобы не получить собственные
    // retained обратно как команды (мы не подписаны на state-топики).
    mqtt.subscribe(TOPIC_LIGHT_SET);
    mqtt.subscribe(TOPIC_MODE_SET);
    mqtt.subscribe(TOPIC_MOTION_SET);
    Serial.println(F("MQTT: subscribed to set-topics"));

    // Публикуем актуальное состояние (retain, кроме motion/state).
    publishAvailability(true);
    publishLightState();
    publishModeState();
    publishMotionState();
    publishMotionEnabledState();
    statusDirty = true;   // свежий статус сразу после (пере)подключения
    return true;
}

// =============================================================================
// Публикации
// =============================================================================

void publishAvailability(bool online) {
    if (!mqtt.connected()) return;
    mqtt.publish(TOPIC_AVAILABILITY, online ? PAYLOAD_ONLINE : PAYLOAD_OFFLINE, true);
}

void publishLightState() {
    if (!mqtt.connected()) return;
    mqtt.publish(TOPIC_LIGHT_STATE, lightState ? PAYLOAD_ON : PAYLOAD_OFF, true);
}

void publishModeState() {
    if (!mqtt.connected()) return;
    mqtt.publish(TOPIC_MODE_STATE,
                 currentMode == MODE_MAIN ? PAYLOAD_MODE_MAIN : PAYLOAD_MODE_EDISON, true);
}

// Факт движения — без retain (публикация только по фронту).
void publishMotionState() {
    if (!mqtt.connected()) return;
    mqtt.publish(TOPIC_MOTION_STATE, pirStable ? PAYLOAD_ON : PAYLOAD_OFF, false);
}

// Состояние реакции на PIR — retain (HA читает мгновенно, без ожидания статуса).
void publishMotionEnabledState() {
    if (!mqtt.connected()) return;
    mqtt.publish(TOPIC_MOTION_ENABLED, motionEnabled ? PAYLOAD_ON : PAYLOAD_OFF, true);
}

// JSON-статус (раз в минуту + немедленно при изменении состояния): light, mode,
// motion, motion_enabled, temperature, humidity, uptime, wifi_rssi.
// Сборка через dtostrf + конкатенация — без зависимости от float-printf.
void publishStatusJson() {
    if (!mqtt.connected()) return;

    char num[12];

    strcpy(statusBuf, "{\"light\":\"");
    strcat(statusBuf, lightState ? "on" : "off");
    strcat(statusBuf, "\",\"mode\":\"");
    strcat(statusBuf, currentMode == MODE_MAIN ? "main" : "edison");
    strcat(statusBuf, "\",\"motion\":\"");
    strcat(statusBuf, pirStable ? "on" : "off");
    strcat(statusBuf, "\",\"motion_enabled\":\"");
    strcat(statusBuf, motionEnabled ? "on" : "off");
    strcat(statusBuf, "\",\"temperature\":");

#if CFG_ENABLE_AM2320
    if (isnan(lastTemp)) {
        strcat(statusBuf, "null");
    } else {
        dtostrf(lastTemp, 2, 1, num);
        strcat(statusBuf, num);
    }
#else
    strcat(statusBuf, "null");
#endif
    strcat(statusBuf, ",\"humidity\":");
#if CFG_ENABLE_AM2320
    if (isnan(lastHum)) {
        strcat(statusBuf, "null");
    } else {
        dtostrf(lastHum, 2, 1, num);
        strcat(statusBuf, num);
    }
#else
    strcat(statusBuf, "null");
#endif

    strcat(statusBuf, ",\"uptime\":");
    utoa((unsigned)(millis() / 1000UL), num, 10);
    strcat(statusBuf, num);

    strcat(statusBuf, ",\"wifi_rssi\":");
    int rssi = WiFi.RSSI();
    if (WiFi.status() == WL_CONNECTED) {
        itoa(rssi, num, 10);
        strcat(statusBuf, num);
    } else {
        strcat(statusBuf, "null");
    }
    strcat(statusBuf, "}");

    yield();
    mqtt.publish(TOPIC_STATUS, statusBuf, true);
}

// =============================================================================
// setup / loop
// =============================================================================

void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println();
    Serial.println(F("=== Garage light (ESP8266) ==="));

    // Пины реле — выходы, безопасное стартовое состояние (выключено).
    pinMode(PIN_LIGHT_MAIN, OUTPUT);
    pinMode(PIN_LIGHT_EDISON, OUTPUT);
    applyRelay(PIN_LIGHT_MAIN, false);
    applyRelay(PIN_LIGHT_EDISON, false);

    // PIR.
    pinMode(PIN_PIR, PIR_PIN_MODE);
    pirStable  = digitalRead(PIN_PIR);
    pirRawLast = pirStable;

    // Wi-Fi: только STA, без записи в flash при каждом begin.
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);

    // I²C для AM2320 (Wire.begin ДО am2320.begin()).
    Wire.begin(I2C_SDA, I2C_SCL);

#if CFG_ENABLE_AM2320
    sensorOk = am2320.begin();
    Serial.print(F("AM2320: "));
    Serial.println(sensorOk ? F("initialized") : F("not found"));
#else
    Serial.println(F("AM2320: disabled (CFG_ENABLE_AM2320=0)"));
#endif

    // MQTT.
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqttMessage);
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);

    lastAllOkTs = millis();   // базовая точка для connectivity watchdog

    connectWifi();            // первый запуск — пробуем сразу
}

void loop() {
    unsigned long now = millis();

    // --- Неблокирующее восстановление Wi-Fi ---
    if (WiFi.status() != WL_CONNECTED) {
        if ((now - lastWifiAttempt) >= WIFI_RECONNECT_DELAY_MS) {
            lastWifiAttempt = now;
            connectWifi();
        }
    }

    // --- Неблокирующее восстановление MQTT ---
    if (WiFi.status() == WL_CONNECTED && !mqtt.connected()) {
        if ((now - lastMqttAttempt) >= MQTT_RECONNECT_DELAY_MS) {
            lastMqttAttempt = now;
            if (connectMqtt()) {
                lastMqttAttempt = 0;   // сразу ретраить при следующем сбое
            }
        }
    }

    // --- Обработка входящих MQTT-пакетов ---
    if (mqtt.connected()) {
        mqtt.loop();
    }

    // --- Основная логика ---
    pollPir();
    handleLightTimer();
    handleMotionTimer();
    readAm2320();

    // --- JSON-статус: раз в минуту либо немедленно при изменении состояния.
    // Немедленная отправка обнуляет минутный таймер — без дублей подряд.
    if (mqtt.connected() && (statusDirty || (millis() - lastStatusPub) >= STATUS_INTERVAL_MS)) {
        statusDirty   = false;
        lastStatusPub = millis();
        publishStatusJson();
    }

    // --- Connectivity watchdog: 10 минут без Wi-Fi+MQTT -> restart ---
    if (WiFi.status() == WL_CONNECTED && mqtt.connected()) {
        lastAllOkTs = millis();
    } else if ((millis() - lastAllOkTs) >= CONNECT_TIMEOUT_MS) {
        Serial.println(F("Watchdog: no Wi-Fi+MQTT for 10 min, restarting..."));
        delay(200);   // успеть отправить лог
        ESP.restart();
    }
}
