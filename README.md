# Smart Bin

**CS147 Internet of Things, Fall 2026 · Group 7**
Navid Atashbar · Melih Erduran · Lucas Jimenez-Suselo

A connected trash can for a shared apartment. It senses how bad the bin smells, warns the roommates locally (LEDs, buzzer, OLED) and sends readings over Wi-Fi to a cloud MQTT broker and a live dashboard. Planned additions: a fill-level sensor and an RFID reader so roommates can log who took the trash out.

## Current status

| Feature | Status |
|---|---|
| MQ-135 gas sensor reading in volts | Working |
| Wi-Fi + MQTT over TLS to EMQX Cloud | Working |
| Node-RED dashboard (live gauge + history chart) | Working |
| Green / yellow / red odor LEDs, buzzer alarm, OLED status | Working |
| Auto-reconnect after Wi-Fi or broker drops | Working |
| VL53L0X fill-level sensor | Planned |
| RFID "who took out the trash" log | Planned |

## How it works

```
MQ-135 gas sensor
      │ analog voltage
      ▼
ESP32 ──► LEDs, buzzer, OLED  (local alert)
      │ Wi-Fi, MQTT over TLS (port 8883)
      ▼
EMQX Cloud Serverless broker
      │
      ▼
Node-RED ──► Dashboard 2.0 (gauge + history chart)
```

1. Every **1 second** the ESP32 averages 32 analog samples from the MQ-135 and converts them to volts.
2. During the first **60 seconds** it learns the bin's normal reading (the **baseline**). After that the baseline only drifts slowly, and only while the air is normal, so it follows temperature and humidity changes without "learning" a bad smell.
3. Each reading is compared with the baseline:

   | Reading vs. baseline | Level | Local output |
   |---|---|---|
   | below 1.15x | `OK` | green LED |
   | 1.15x to 1.30x | `HIGH` | yellow LED |
   | 1.30x and above | `ALERT` | red LED + beep (then every 5 s) |

   Button 1 mutes or unmutes the buzzer. Thresholds are `YELLOW_RATIO` and `RED_RATIO` at the top of `src/main.cpp`.
4. Every **10 seconds**, and immediately whenever the level changes, it publishes a JSON message to EMQX. Node-RED subscribes and updates the dashboard.

## Hardware

- ESP32 DevKit V1 (course kit)
- MQ-135 gas sensor module (extra part)
- 2 equal resistors (for example 10 kΩ) as a voltage divider on the sensor output
- From the kit: red / yellow / green LEDs with 220 Ω resistors, push button, passive buzzer, 7-pin SPI OLED

### Wiring

Everything except the MQ-135 follows the course Fall 2026 pin map (Lab 1 handout).

| Part | Part pin | ESP32 pin |
|---|---|---|
| MQ-135 | VCC | **VIN** (5 V, the heater needs it) |
| MQ-135 | GND | GND |
| MQ-135 | AO | divider midpoint → **VP (GPIO 36)** |
| MQ-135 | DO | not connected |
| Red / yellow / green LED | via 220 Ω | D2 / D4 / D15 |
| Button 1 (mute) | other leg to GND | D32 |
| Buzzer | other leg to GND | D27 |
| OLED | GND, VDD, SCK, SDA, RES, DC, CS | GND, 3V3, D18, D23, D17 (TX2), D16 (RX2), D5 |

**Voltage divider:** MQ-135 AO → resistor → VP, and VP → second resistor → GND. The module can output up to 5 V, which would damage the ESP32, so the divider halves it. The code multiplies by `DIVIDER_RATIO = 2.0` to get the real sensor voltage back.

**Why VP?** Analog pins must be on ADC1 (GPIO 32 to 39) because ADC2 stops working while Wi-Fi is on. In our build D32/D33 are buttons, D34 is the potentiometer and D35 is the MPU-6050 INT, so VP (GPIO 36) is the free ADC1 pin.

## Software setup

### 1. Firmware (PlatformIO in VS Code)

1. Clone this repo and open the folder in VS Code with the PlatformIO extension.
2. Copy `secrets.h.example` to `include/secrets.h` and fill it in (see below). This file is in `.gitignore` and must never be committed.
3. Plug in the ESP32, click **Upload**, then open the **Serial Monitor** (115200 baud).

`platformio.ini` pulls in the libraries automatically: PubSubClient, Adafruit SSD1306, Adafruit GFX.

### 2. `include/secrets.h`

```cpp
#pragma once
#define WIFI_SSID  "your-wifi-name"       // 2.4 GHz only
#define WIFI_PASS  "your-wifi-password"
#define MQTT_HOST  "xxxxxxxx.ala.us-east-1.emqxsl.com"   // EMQX Overview page
#define MQTT_USER  "esp32"
#define MQTT_PASS  "your-esp32-password"
static const char CA_CERT[] PROGMEM = R"(
-----BEGIN CERTIFICATE-----
...paste the EMQX CA certificate here...
-----END CERTIFICATE-----
)";
```

- The ESP32 only joins **2.4 GHz** networks and cannot log in to campus Wi-Fi. A phone hotspot works (on iPhone turn on **Maximize Compatibility**).
- `MQTT_HOST` is just the hostname: no `mqtts://`, no port.

### 3. EMQX Cloud broker

1. Create a free account at emqx.com and add an **EMQX Broker → Serverless** deployment (keep the spend limit at 0).
2. From the deployment **Overview** page, copy the address and download the **CA certificate**. Serverless only accepts TLS on port **8883**.
3. Under **Access Control → Authentication**, add two users: one for the ESP32 and one for Node-RED.

### 4. Node-RED dashboard

1. Install Node.js (LTS), then: `npm install -g --unsafe-perm node-red`
2. Start it with `node-red` and open http://localhost:1880
3. ☰ → **Manage palette** → install `@flowfuse/node-red-dashboard`
4. ☰ → **Import** the flow from `node-red/smartbin-flow.json`, or build it by hand:
   `mqtt in` (topic `smartbin/group7/gas`, broker on port 8883 with TLS and the Node-RED login) → `change` (set `msg.payload` to `msg.payload.gas_v`) → `gauge` and `chart`
5. Open the broker node and enter the Node-RED username and password (exports do not include passwords), then **Deploy**.
6. Dashboard: http://localhost:1880/dashboard

## Running it

1. Turn on the Wi-Fi network or hotspot.
2. Power the ESP32 (any USB power). It connects on its own; the OLED shows `WiFi` and `MQTT` when connected.
3. Run `node-red` and keep that terminal open.
4. Open http://localhost:1880/dashboard

Quick test: after the 60-second warm-up, breathe on the sensor or hold hand sanitizer near it. The LEDs should go yellow then red, the buzzer should beep, and the gauge and chart should jump.

## MQTT messages

| Topic | Payload | Notes |
|---|---|---|
| `smartbin/group7/gas` | JSON, see below | every 10 s and on level change |
| `smartbin/group7/status` | `online` / `offline` | retained; `offline` is the MQTT last will, sent by the broker if the bin drops off |

```json
{"gas_v":0.512,"baseline_v":0.480,"level":"OK","seq":42,"uptime_s":420,"rssi":-55}
```

| Field | Meaning |
|---|---|
| `gas_v` | sensor voltage (V), corrected for the divider |
| `baseline_v` | learned normal reading (V) |
| `level` | `OK`, `HIGH` or `ALERT` |
| `seq` | message counter since boot; a jump means messages were missed |
| `uptime_s` | seconds since boot; a reset means the board rebooted |
| `rssi` | Wi-Fi signal strength (dBm) |

`seq` and `uptime_s` are there for the 72-hour reliability test, so every gap in the data can be explained.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `Wi-Fi failed` | 5 GHz network, wrong password, or hotspot screen closed (iPhone) |
| `failed, state=-2` | wrong `MQTT_HOST`, wrong or missing CA certificate, or using port 1883 |
| `failed, state=4` or `5` | wrong MQTT username or password |
| Gas reading barely changes | sensor not warmed up yet, VCC not on 5 V, or AO wire on the wrong pin |
| Gas reading follows the potentiometer | AO and the pot share a pin; AO belongs on VP |
| OLED blank | check the 7 OLED wires against the table above |
| Node-RED mqtt node not "connected" | Node-RED login missing, or TLS not checked |

## Security notes

- Wi-Fi and broker credentials live only in `include/secrets.h`, which is git-ignored. `secrets.h.example` has placeholders only.
- All MQTT traffic uses TLS, and the ESP32 verifies the broker with its CA certificate.
- EMQX rejects anonymous clients; the ESP32 and Node-RED each have their own login.
- The device currently only publishes; it accepts no remote commands.
