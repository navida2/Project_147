#include <Arduino.h>
// Smart Bin - CS147 Group 7
// MQ-135 gas sensor -> ESP32 -> Wi-Fi -> EMQX (TLS 8883) -> Node-RED
// Local feedback: green/yellow/red LEDs, buzzer alarm, OLED status screen.
// Pins follow the course Fall 2026 pin map (Lab 1 handout).

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, MQTT_HOST, MQTT_USER, MQTT_PASS, CA_CERT

// ---------- Pins (course pin map) ----------
const int GAS_PIN   = 36;   // VP. D34 = potentiometer, D35 = MPU-6050 INT
const int LED_RED   = 2;
const int LED_YEL   = 4;
const int LED_GRN   = 15;
const int BUZZER    = 27;
const int BUTTON_1  = 32;   // press to mute/unmute the buzzer
const int OLED_CS = 5, OLED_DC = 16, OLED_RST = 17;   // SCK 18, MOSI 23

// ---------- Settings ----------
const float DIVIDER_RATIO = 2.0;          // two equal resistors on AO -> multiply by 2
const int   MQTT_PORT     = 8883;         // EMQX Serverless only allows TLS
const char* TOPIC_GAS     = "smartbin/group7/gas";
const char* TOPIC_STATUS  = "smartbin/group7/status";
const unsigned long SAMPLE_EVERY_MS  = 1000;    // read sensor, update LEDs/OLED every 1 s
const unsigned long PUBLISH_EVERY_MS = 10000;   // send to EMQX every 10 s

// Alert levels, relative to the bin's normal ("baseline") reading.
// Breathing on it gave about 1.35x in testing; hand sanitizer gives more.
const float YELLOW_RATIO = 1.15;   // 15% above normal -> yellow
const float RED_RATIO    = 1.30;   // 30% above normal -> red + buzzer
const unsigned long WARMUP_MS = 60000;   // first minute: just learn the baseline

enum Level { LEVEL_OK, LEVEL_HIGH, LEVEL_ALERT };
const char* LEVEL_NAME[] = {"OK", "HIGH", "ALERT"};

WiFiClientSecure net;
PubSubClient mqtt(net);
Adafruit_SSD1306 oled(128, 64, &SPI, OLED_DC, OLED_RST, OLED_CS);
bool oledOk = false;

String clientId;
unsigned long lastSample = 0, lastPublish = 0, lastWifiRetry = 0, lastMqttRetry = 0;
unsigned long lastBeep = 0;
unsigned long beepUntil = 0;
const int BUZZER_CH = 0;   // LEDC channel for the buzzer
unsigned long msgCount = 0;
float gasVolts = 0, baseline = 0;
Level level = LEVEL_OK;
bool muted = false;
bool lastButton = HIGH;

// ---------- Sensor ----------
// Average many samples because the ESP32 ADC is noisy
float readGasVolts() {
  const int N = 32;
  long sum = 0;
  for (int i = 0; i < N; i++) {
    sum += analogRead(GAS_PIN);
    delay(2);
  }
  return (sum / (float)N) / 4095.0 * 3.3 * DIVIDER_RATIO;
}

// Baseline follows slow drift (temperature, humidity) but ignores sudden smells
void updateBaseline(float v) {
  if (baseline <= 0) { baseline = v; return; }
  if (millis() < WARMUP_MS) {
    baseline = 0.8 * baseline + 0.2 * v;          // learn quickly while warming up
  } else if (v < baseline) {
    baseline = 0.9 * baseline + 0.1 * v;          // air got cleaner: follow it down quickly
  } else if (level == LEVEL_OK) {
    baseline = 0.99 * baseline + 0.01 * v;        // drift up slowly, only when air is normal
  }
}

Level classify(float v) {
  if (millis() < WARMUP_MS || baseline <= 0) return LEVEL_OK;
  float ratio = v / baseline;
  if (ratio >= RED_RATIO)    return LEVEL_ALERT;
  if (ratio >= YELLOW_RATIO) return LEVEL_HIGH;
  return LEVEL_OK;
}

// ---------- Outputs ----------
void showLeds(Level l) {
  digitalWrite(LED_GRN, l == LEVEL_OK);
  digitalWrite(LED_YEL, l == LEVEL_HIGH);
  digitalWrite(LED_RED, l == LEVEL_ALERT);
}

// Non-blocking beep using the LEDC PWM hardware directly
void beep(int freq, int ms) {
  ledcWriteTone(BUZZER_CH, freq);
  beepUntil = millis() + ms;
}

void updateBeep() {
  if (beepUntil && millis() >= beepUntil) {
    ledcWriteTone(BUZZER_CH, 0);
    beepUntil = 0;
  }
}

void handleBuzzer(Level l, Level previous) {
  if (l != LEVEL_ALERT || muted) return;
  // Beep right away when it first turns red, then every 5 s while it stays red
  if (previous != LEVEL_ALERT || millis() - lastBeep > 5000) {
    beep(2000, 300);
    lastBeep = millis();
  }
}

void drawOled() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print("SMART BIN");
  oled.setCursor(72, 0);
  oled.print(WiFi.status() == WL_CONNECTED ? "WiFi" : "----");
  oled.setCursor(104, 0);
  oled.print(mqtt.connected() ? "MQTT" : "----");

  oled.setTextSize(2);
  oled.setCursor(0, 14);
  oled.printf("%.3f V", gasVolts);

  oled.setTextSize(1);
  oled.setCursor(0, 36);
  if (millis() < WARMUP_MS) {
    oled.printf("Warming up... %lus", (WARMUP_MS - millis()) / 1000);
  } else {
    oled.printf("Odor: %s  (%.2fx)", LEVEL_NAME[level], gasVolts / baseline);
  }
  oled.setCursor(0, 48);
  oled.printf("Normal: %.3f V", baseline);
  oled.setCursor(0, 57);
  oled.printf("Sent: %lu %s", msgCount, muted ? " MUTED" : "");
  oled.display();
}

// ---------- Network (non-blocking, so LEDs/buzzer keep working during outages) ----------
void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiRetry > 30000) {   // give each attempt 30 s to finish
    lastWifiRetry = millis();
    Serial.printf("Wi-Fi down (status %d), retrying...\n", WiFi.status());
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void maintainMQTT() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttRetry < 5000) return;
  lastMqttRetry = millis();
  Serial.print("Connecting to EMQX... ");
  // Last will: if the bin drops off, the broker marks it offline for us
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, TOPIC_STATUS, 1, true, "offline")) {
    Serial.println("connected");
    mqtt.publish(TOPIC_STATUS, "online", true);
  } else {
    Serial.print("failed, state=");
    Serial.println(mqtt.state());   // -2 = network/TLS problem, 4/5 = bad username/password
  }
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YEL, OUTPUT);
  pinMode(LED_GRN, OUTPUT);
  pinMode(BUTTON_1, INPUT_PULLUP);
  ledcSetup(BUZZER_CH, 2000, 8);
  ledcAttachPin(BUZZER, BUZZER_CH);
  beep(1000, 200);   // startup beep: proves the buzzer works and marks every reboot
  analogReadResolution(12);
  analogSetPinAttenuation(GAS_PIN, ADC_11db);   // full 0-3.3 V range

  SPI.begin();
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC);
  if (oledOk) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(2);
    oled.setCursor(0, 0);
    oled.println("SMART BIN");
    oled.setTextSize(1);
    oled.println("\nConnecting Wi-Fi...");
    oled.display();
  } else {
    Serial.println("OLED not found (check the 7 OLED wires)");
  }

  clientId = "smartbin-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  net.setCACert(CA_CERT);            // verify we are really talking to EMQX
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(512);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiRetry = millis();
  lastPublish = millis();   // first message goes out after the first full readings
}

void loop() {
  updateBeep();
  maintainWiFi();
  maintainMQTT();
  mqtt.loop();

  // Button 1 toggles mute (pressed = LOW). Ignores bounces within 250 ms.
  static unsigned long lastPress = 0;
  bool b = digitalRead(BUTTON_1);
  if (b == LOW && lastButton == HIGH && millis() - lastPress > 250) {
    lastPress = millis();
    muted = !muted;
    Serial.println(muted ? "Buzzer muted" : "Buzzer on");
    beep(muted ? 600 : 1500, 80);   // low chirp = muted, high chirp = sound on
    drawOled();                     // show MUTED right away
  }
  lastButton = b;

  // Every second: read sensor, update LEDs, buzzer, OLED
  if (millis() - lastSample >= SAMPLE_EVERY_MS) {
    lastSample = millis();
    gasVolts = readGasVolts();
    updateBaseline(gasVolts);
    Level previous = level;
    level = classify(gasVolts);
    showLeds(level);
    handleBuzzer(level, previous);
    drawOled();
    Serial.printf("gas=%.3f V  normal=%.3f V  ratio=%.2fx  %s\n",
                  gasVolts, baseline, gasVolts / baseline, LEVEL_NAME[level]);

    // Send right away when the level changes, so the dashboard reacts fast
    if (level != previous) lastPublish = 0;
  }

  // Every 10 s (or on a level change): send to EMQX
  if (lastPublish == 0 || millis() - lastPublish >= PUBLISH_EVERY_MS) {
    lastPublish = millis();
    msgCount++;
    char payload[200];
    snprintf(payload, sizeof(payload),
             "{\"gas_v\":%.3f,\"baseline_v\":%.3f,\"level\":\"%s\",\"seq\":%lu,\"uptime_s\":%lu,\"rssi\":%d}",
             gasVolts, baseline, LEVEL_NAME[level], msgCount, millis() / 1000, WiFi.RSSI());
    Serial.println(payload);
    if (mqtt.connected()) mqtt.publish(TOPIC_GAS, payload);
  }
}