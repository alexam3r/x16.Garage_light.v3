# Архитектура прошивки гаражного света

## Обзор

ESP8266 (Wemos D1 mini) управляет двумя группами света по MQTT и PIR-датчику,
публикует статус и поддерживает автономность через watchdog-и. Вся логика — в
`src/main.cpp`; конфигурация в `src/Config.h`, креды в `src/Secrets.h`.

## Блок-схема

```
   ┌─────────┐     ┌───────────┐     ┌───────────────┐     ┌──────────────┐
   │  Wi-Fi  │────▶│   MQTT    │────▶│ onMqttMessage │────▶│  Действия    │
   │ connect │     │  connect  │     │   (callback)  │     │ turnLight*  │
   └─────────┘     │  + LWT    │     └───────────────┘     │ applyMode   │
                   │  + subs   │                           │ extendTimer │
                   └─────┬─────┘                           └──────┬───────┘
                         │ publish (retain)                       │
                         ▼                                        │
                  ┌───────────────┐    ┌────────────┐              │
                  │  MQTT-топики  │◀───│  pollPir   │◀─────────────┘
                  │  state/status │    │ (front)    │
                  └───────────────┘    └────────────┘
```

`loop()` неблокирующий: держит Wi-Fi/MQTT, опрашивает PIR, ведёт таймер света,
читает AM2320 в кэш, раз в минуту публикует JSON, проверяет connectivity-watchdog.

## Конечный автомат света

Свет описывается тремя величинами: `lightState` (on/off), `currentSource`
(`SRC_NONE`/`SRC_PIR`/`SRC_MQTT`) и `currentMode` (`MODE_MAIN`/`MODE_EDISON`).
Один таймер `lightOffAt` (deadline в `millis()`).

| Событие | Условие | Действие |
| --- | --- | --- |
| PIR-фронт LOW→HIGH | свет выключен, `motionEnabled` | `turnLightOn(PIR, mode)` — таймер 10 мин |
| PIR-фронт LOW→HIGH | свет горит, `motionEnabled` | `extendTimer()` — обнулить на длительность текущего источника |
| `light/set ON` | любое | `turnLightOn(MQTT, mode)` — источник MQTT, таймер 1 час |
| `light/set OFF` | любое | `turnLightOff()` — выключить, `mode→main`, `motionEnabled→true` |
| `mode/set X` | свет горит | `applyMode(X)` — переключить лампу, таймер/источник не трогать |
| `mode/set X` | свет выключен | `turnLightOn(MQTT, X)` — включить в режиме X на 1 час |
| `motion/set OFF` | любое | `motionEnabled=false` (без публикации флага) |
| Таймер истёк | `lightOffAt` прошёл | `turnLightOff()` |
| PIR-фронт (любой) | всегда | `publishMotionState()` (факт движения, без retain) |

### Роль источника в продлении таймера

`extendTimer()` ставит таймер на длительность **текущего** источника:
- `SRC_PIR` → `LIGHT_TIMEOUT_PIR_MS` (10 мин);
- `SRC_MQTT` → `LIGHT_TIMEOUT_MQTT_MS` (1 час).

Источник меняется только явной MQTT-командой. PIR не переподчиняет MQTT-сеанс —
если свет включён по MQTT на час, PIR лишь продлевает на час (не на 10 минут).

### `mode/set` — диаграмма решений

```
mode/set <X>
   │
   ├── свет горит? ─── ДА ──▶ applyMode(X) + publishModeState()
   │                            (таймер и источник не меняются)
   │
   └── НЕТ ───────────────▶ turnLightOn(MQTT, X)
                                (включить в режиме X на 1 час)
```

При любом выключении режим сбрасывается на `main` (в `turnLightOff()`).

## PIR: debounce и развязка «наблюдение vs реакция»

Опрос в `pollPir()` каждый цикл `loop()`:

1. Сырое чтение изменилось → перезапуск debounce-таймера, выход.
2. Сырое чтение стабильно, отличается от зафиксированного, debounce (`PIR_DEBOUNCE_MS`) истёк → фиксация фронта.
3. На любой фронт → `publishMotionState()` (факт движения).
4. Фронт LOW→HIGH: если `motionEnabled` — реакция (включение/продление света); если `!motionEnabled` — только публикация.

**Развязка:** `motion/state` (факт движения) публикуется всегда, независимо от
`motionEnabled`. `motionEnabled` влияет только на включение света. Это позволяет
наблюдать активность датчика даже при отключённой реакции.

Флаг `motionEnabled` намеренно **не публикуется** ни в каком retain-топике — это
разовая команда; сбрасывается в ON при выключении света; при рестарте — default ON.

## MQTT-дерево

Корень `garage/light`. Направление: `in` — подписка (команды), `out` — публикация.

| Топик | Напр. | Retain | Payload |
| --- | --- | --- | --- |
| `availability` | out | да | `online`/`offline` |
| `light/set` | in | — | `ON`/`OFF` |
| `light/state` | out | да | `ON`/`OFF` |
| `mode/set` | in | — | `main`/`edison` |
| `mode/state` | out | да | `main`/`edison` |
| `motion/set` | in | — | `ON`/`OFF` (флаг реакции) |
| `motion/state` | out | **нет** | `ON`/`OFF` (факт движения) |
| `status` | out | да | JSON (раз в минуту) |

**LWT:** в `connectMqtt()` регистрируется will-сообщение `availability=offline`
(retain). При корректной связи мы публикуем `availability=online` на коннекте.
При обрыве связи брокер сам публикует `offline`.

**Порядок в `connectMqtt()`:** подписки на `set`-топики → публикация актуального
состояния. Мы не подписаны на собственные `state`-топики, поэтому echo своих
retained не превращается в команды.

## Глобальные переменные

| Переменная | Тип | Назначение |
| --- | --- | --- |
| `lightState` | `bool` | свет вкл/выкл |
| `currentMode` | `LightMode` | `MODE_MAIN`/`MODE_EDISON` |
| `currentSource` | `LightSource` | `SRC_NONE`/`SRC_PIR`/`SRC_MQTT` |
| `lightOffAt` | `unsigned long` | deadline выключения (`millis()+timeout`), 0 = неактивен |
| `pirStable`/`pirRawLast`/`pirDebounceTs` | `bool`/`bool`/`unsigned long` | debounce PIR |
| `motionEnabled` | `bool` | реакция на PIR |
| `sensorOk`/`lastTemp`/`lastHum`/`lastSensorRead` | — | кэш AM2320 |
| `lastWifiAttempt`/`lastMqttAttempt`/`lastStatusPub` | `unsigned long` | тайминги loop |
| `lastAllOkTs` | `unsigned long` | connectivity watchdog |
| `statusBuf[256]` | `char[]` | буфер JSON-статуса |

## Порядок `setup()`

1. Serial (115200).
2. Пины реле → OUTPUT, `applyRelay(*, false)` (безопасное стартовое состояние).
3. PIR → `pinMode`, чтение начального состояния в `pirStable`/`pirRawLast`.
4. `WiFi.persistent(false)`, `WiFi.mode(WIFI_STA)`.
5. `Wire.begin(I2C_SDA, I2C_SCL)` — **до** `am2320.begin()`.
6. `am2320.begin()` (если `CFG_ENABLE_AM2320`).
7. `mqtt.setServer`/`setCallback`/`setBufferSize(512)`.
8. `lastAllOkTs = millis()` (база connectivity watchdog).
9. `connectWifi()` — первая попытка.

## Цикл `loop()`

1. Неблокирующий реконнект Wi-Fi (раз в `WIFI_RECONNECT_DELAY_MS`).
2. Неблокирующий реконнект MQTT (раз в `MQTT_RECONNECT_DELAY_MS`, только при Wi-Fi).
3. `mqtt.loop()` — только если подключены.
4. `pollPir()` — каждый цикл.
5. `handleLightTimer()` — каждый цикл.
6. `readAm2320()` — обновление кэша раз в `SENSOR_READ_INTERVAL_MS`.
7. `publishStatusJson()` — раз в `STATUS_INTERVAL_MS` (только при MQTT).
8. Connectivity watchdog (см. ниже).

## Watchdog — два уровня

### 1. Connectivity watchdog (программный)

`lastAllOkTs` обновляется каждый цикл, если `WiFi.status()==WL_CONNECTED &&
mqtt.connected()`. Если с момента последнего «всё ОК» прошло `CONNECT_TIMEOUT_MS`
(10 минут) — `ESP.restart()`. Работает параллельно с реконнектами, не блокирует их.

### 2. Аппаратный watchdog (core ESP8266)

ESP8266 core содержит два watchdog-таймера — software (SDK) и hardware
(финальная защита). Оба активны по умолчанию; их нужно регулярно «кормить»,
иначе — перезагрузка. Кормление происходит через:
- `yield()` / `delay()` — отдаёт управление Wi-Fi-стеку и кормит WDT;
- `mqtt.loop()`.

В долгих местах явно вызывается `yield()`: перед/после чтения AM2320
(I²C-транзакция ~0.5–1 с), после сборки JSON. `applyMode()` содержит
`delay(RELAY_SWITCH_DELAY_MS)`, который также кормит WDT и защищает от
индуктивного выброса реле. Отдельный `ESP.wdtEnable()` не используется —
core-WDT достаточен.

## Разностная форма `millis()` (защита от переполнения)

`millis()` переполняется через ~49.7 дней. Сравнение `millis() >= deadline`
**некорректно** при переполнении (deadline близко к концу цикла, `millis()`
обнулился). Вместо этого используется разностная арифметика `unsigned long`:

```
(millis() - lastXxx) >= INTERVAL
```

При переполнении разность `(unsigned long)(now - prev)` благодаря modular
арифметике даёт корректное значение. Все таймеры в `loop()` и `handleLightTimer()`
используют эту форму. `handleLightTimer()` дополнительно проверяет `lightOffAt != 0`.

## AM2320 — кэш-паттерн

Чтение датчика (~0.5–1 с с wake-up) вынесено из публикации статуса:
`readAm2320()` обновляет кэш `lastTemp`/`lastHum` раз в
`SENSOR_READ_INTERVAL_MS` (20 с), а `publishStatusJson()` публикует из кэша —
мгновенно. Валидация: `isnan()` + sanity-диапазон (`TEMP_MIN..TEMP_MAX`,
`HUM_MIN..HUM_MAX`); при ошибке — `NAN` → в JSON `null`. `Wire.begin()` вызывается
до `am2320.begin()` (библиотека не инициализирует Wire сама).

## JSON-статус

Топик `garage/light/status`, retain, раз в 60 с. Сборка через `dtostrf` +
конкатенация (без зависимости от float-printf). Поля:
`light`, `mode`, `motion`, `temperature`, `humidity`, `uptime`, `wifi_rssi`.
Недоступный датчик → `null`. Сборка в `statusBuf[256]` (вписывается в
`MQTT_MAX_PACKET_SIZE=512`).

## Безопасность и ограничения

- **Нет Home Assistant discovery** — только `set`/`state`/`availability` + JSON.
- **Нет OTA** — прошивка только по USB (`pio run -t upload`).
- **Нет persistence** — состояние сбрасывается при рестарте (свет выкл, режим
  `main`, `motionEnabled=true`); retained-сообщения обновляются при коннекте.
- **`cleanSession=true`** (PubSubClient default) — нет persistent session,
  очередь команд брокером не хранится.
- **Секреты не в git** — `Secrets.h` в `.gitignore`.
- **MQTT payload** — в `onMqttMessage` копируется в локальный буфер с нулём-
  терминатором перед парсингом (`byte*` не нуль-терминирован).
- **Публикация внутри callback** безопасна — payload уже скопирован; подписки
  выполняются только в `connectMqtt()`, не в callback.
