// ================================================================
//  SERRE ARROSAGE AUTONOME v1.1
//  Matériel : ESP8266 + Relais HW-803 + Électrovanne 24VAC
//  DS3231   : OPTIONNEL (fallback hors WiFi)
//  Heure    : NTP en priorité, DS3231 si WiFi absent
//  Temp.    : Capteur Zigbee via HA → MQTT
// ================================================================
// Brochage :
//   D1 (GPIO5) → Relais IN (HW-803 actif LOW)
//   D2 (GPIO4) → DS3231 SDA  (optionnel)
//   D3 (GPIO0) → DS3231 SCL  (optionnel)
// ================================================================

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RTClib.h>
#include <ArduinoOTA.h>
#include <time.h>        // NTP intégré ESP8266, aucune lib à installer

// ================================================================
// ⚙️  CONFIGURATION — À ADAPTER
// ================================================================

const char* WIFI_SSID     = "TP-Link";
const char* WIFI_PASSWORD = "TON_PASS_WIFI";       // ← à corriger

const char* MQTT_SERVER   = "192.168.1.XX";        // ← IP de ton HA
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "mqtt_user";
const char* MQTT_PASSWORD = "mqtt_password";
const char* MQTT_CLIENT   = "serre_arrosage";

const char* TOPIC_TEMP_SUB   = "zigbee2mqtt/NOM_CAPTEUR"; // ← à adapter
const char* TOPIC_STATUS_PUB = "serre/arrosage/status";
const char* TOPIC_RELAY_PUB  = "serre/arrosage/relay";
const char* TOPIC_CMD_SUB    = "serre/arrosage/cmd";

// Paramètres arrosage
const uint8_t  WATERING_HOUR       = 9;
const uint32_t DURATION_MORNING_S  = 15 * 60;
const uint32_t DURATION_TEMP_S     = 2  * 60;
const float    TEMP_THRESHOLD      = 30.0f;
const uint32_t TEMP_COOLDOWN_MS    = 30UL * 60 * 1000;

// NTP
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET_S = 3600;      // France UTC+1 (hiver)
const int   DST_OFFSET_S = 3600;      // +1h été (DST auto)

// Pins
#define RELAY_PIN D1
#define I2C_SDA   D2
#define I2C_SCL   D3

// ================================================================
// Variables
// ================================================================

RTC_DS3231   rtc;
bool         rtcPresent      = false;
bool         ntpSynced       = false;

WiFiClient   espClient;
PubSubClient mqtt(espClient);

bool     relayActive       = false;
uint32_t relayStartMs      = 0;
uint32_t relayDurationMs   = 0;
char     relayReason[32]   = "";

bool morningDone    = false;
int  lastDayChecked = -1;

bool     tempCooldownActive = false;
uint32_t tempCooldownStart  = 0;
float    currentTemp        = -99.0f;

uint32_t lastStatusMs = 0;
const uint32_t STATUS_INTERVAL_MS = 30000;

// ================================================================
// Obtenir l'heure courante (NTP prioritaire, DS3231 fallback)
// ================================================================

struct TimeInfo {
  int hour, minute, second, day, month;
  int year;
  bool valid;
};

TimeInfo getTime() {
  TimeInfo t = {0, 0, 0, 1, 1, 2024, false};

  // --- Priorité 1 : NTP ---
  if (ntpSynced) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (ti->tm_year > 100) {  // > 2000 = heure valide
      t.hour   = ti->tm_hour;
      t.minute = ti->tm_min;
      t.second = ti->tm_sec;
      t.day    = ti->tm_mday;
      t.month  = ti->tm_mon + 1;
      t.year   = ti->tm_year + 1900;
      t.valid  = true;
      return t;
    }
  }

  // --- Fallback : DS3231 ---
  if (rtcPresent) {
    DateTime now = rtc.now();
    if (now.year() > 2020 && now.year() < 2100) {
      t.hour   = now.hour();
      t.minute = now.minute();
      t.second = now.second();
      t.day    = now.day();
      t.month  = now.month();
      t.year   = now.year();
      t.valid  = true;
    }
  }
  return t;
}

// ================================================================
// RELAY
// ================================================================

void relayON(uint32_t durationMs, const char* reason) {
  if (relayActive) return;
  digitalWrite(RELAY_PIN, HIGH);  // HIGH = relais activé (arrosage)
  relayActive     = true;
  relayStartMs    = millis();
  relayDurationMs = durationMs;
  strncpy(relayReason, reason, sizeof(relayReason) - 1);

  Serial.printf("[RELAY] ON — %s — %lu s\n", reason, durationMs / 1000);

  if (mqtt.connected()) {
    mqtt.publish(TOPIC_RELAY_PUB, "ON", true);
    char msg[128];
    snprintf(msg, sizeof(msg),
      "{\"relay\":\"ON\",\"duration_s\":%lu,\"reason\":\"%s\"}",
      durationMs / 1000, reason);
    mqtt.publish(TOPIC_STATUS_PUB, msg, true);
  }
}

void relayOFF() {
  digitalWrite(RELAY_PIN, LOW);   // LOW = relais OFF (repos)
  relayActive = false;
  Serial.println("[RELAY] OFF");
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_RELAY_PUB, "OFF", true);
    mqtt.publish(TOPIC_STATUS_PUB, "{\"relay\":\"OFF\"}", true);
  }
}

// ================================================================
// WiFi
// ================================================================

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WIFI] Connexion à %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connecté — IP : %s\n",
      WiFi.localIP().toString().c_str());

    // Synchronisation NTP
    configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
    Serial.print("[NTP]  Synchronisation");
    uint32_t t2 = millis();
    while (time(nullptr) < 1000000000UL && millis() - t2 < 10000) {
      delay(500);
      Serial.print(".");
    }
    if (time(nullptr) > 1000000000UL) {
      ntpSynced = true;
      time_t now = time(nullptr);
      Serial.printf("\n[NTP]  OK — %s", ctime(&now));

      // Sync DS3231 depuis NTP si présent
      if (rtcPresent) {
        rtc.adjust(DateTime((uint32_t)now));
        Serial.println("[RTC]  DS3231 mis à l'heure via NTP");
      }
    } else {
      Serial.println("\n[NTP]  Échec — fallback RTC");
    }
  } else {
    Serial.println("\n[WIFI] Échec — mode autonome (RTC requis pour arrosage matin)");
  }
}

// ================================================================
// MQTT Callback
// ================================================================

void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] ← %s : %s\n", topic, msg.c_str());

  if (String(topic) == TOPIC_TEMP_SUB) {
    int idx = msg.indexOf("\"temperature\":");
    if (idx != -1) {
      currentTemp = msg.substring(idx + 14).toFloat();
      Serial.printf("[TEMP] %.1f°C\n", currentTemp);
    }
    return;
  }

  if (String(topic) == TOPIC_CMD_SUB) {
    if      (msg == "ON_TEMP")       relayON(DURATION_TEMP_S    * 1000UL, "manual_short");
    else if (msg == "ON_FULL")       relayON(DURATION_MORNING_S * 1000UL, "manual_long");
    else if (msg == "OFF")           relayOFF();
    else if (msg == "RESET_MORNING") { morningDone = false; Serial.println("[CMD] Reset matin"); }
  }
}

// ================================================================
// MQTT Connect
// ================================================================

void connectMQTT() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  Serial.print("[MQTT] Connexion...");
  bool ok = mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASSWORD,
    TOPIC_STATUS_PUB, 1, true, "{\"status\":\"offline\"}");
  if (ok) {
    Serial.println(" OK");
    mqtt.publish(TOPIC_STATUS_PUB, "{\"status\":\"online\"}", true);
    mqtt.subscribe(TOPIC_TEMP_SUB);
    mqtt.subscribe(TOPIC_CMD_SUB);
  } else {
    Serial.printf(" FAILED rc=%d\n", mqtt.state());
  }
}

// ================================================================
// Status MQTT
// ================================================================

void publishStatus(const TimeInfo& t) {
  if (!mqtt.connected()) return;
  float rtcTemp = rtcPresent ? rtc.getTemperature() : -99.0f;
  char msg[380];
  snprintf(msg, sizeof(msg),
    "{"
      "\"status\":\"online\","
      "\"time\":\"%02d:%02d:%02d\","
      "\"date\":\"%04d-%02d-%02d\","
      "\"time_source\":\"%s\","
      "\"temp_c\":%.1f,\"rtc_temp_c\":%.1f,"
      "\"relay\":\"%s\","
      "\"relay_reason\":\"%s\","
      "\"morning_done\":%s,"
      "\"temp_cooldown\":%s,"
      "\"ip\":\"%s\""
    "}",
    t.hour, t.minute, t.second,
    t.year, t.month, t.day,
    ntpSynced ? "NTP" : (rtcPresent ? "RTC" : "none"),
    currentTemp,
    rtcTemp,
    relayActive ? "ON" : "OFF",
    relayReason,
    morningDone        ? "true" : "false",
    tempCooldownActive ? "true" : "false",
    WiFi.localIP().toString().c_str()
  );
  mqtt.publish(TOPIC_STATUS_PUB, msg, true);
}

// ================================================================
// SETUP
// ================================================================

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=============================");
  Serial.println("  SERRE ARROSAGE v1.1");
  Serial.println("=============================");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);   // LOW = relais OFF au démarrage (sécurité)

  // DS3231 optionnel
  Wire.begin(I2C_SDA, I2C_SCL);
  if (rtc.begin()) {
    rtcPresent = true;
    Serial.println("[RTC]  DS3231 détecté ✓");
    if (rtc.lostPower()) {
      Serial.println("[RTC]  Heure perdue — sera resynchronisée via NTP");
    }
  } else {
    Serial.println("[RTC]  DS3231 absent — heure via NTP uniquement");
  }

  connectWiFi();

  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  connectMQTT();

  ArduinoOTA.setHostname("serre-arrosage");
  ArduinoOTA.onStart([]() { Serial.println("[OTA] Démarrage..."); });
  ArduinoOTA.onEnd([]()   { Serial.println("[OTA] Terminé"); });
  ArduinoOTA.begin();

  Serial.println("[SETUP] Démarrage OK\n");
}

// ================================================================
// LOOP
// ================================================================

void loop() {
  ArduinoOTA.handle();

  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (!mqtt.connected())             connectMQTT();
  mqtt.loop();

  TimeInfo now = getTime();
  uint32_t ms  = millis();

  // 1. Fin de cycle relais
  if (relayActive && (ms - relayStartMs >= relayDurationMs)) {
    relayOFF();
  }

  // 2. Reset flag matin à chaque nouveau jour
  if (now.valid && now.day != lastDayChecked) {
    lastDayChecked = now.day;
    morningDone    = false;
    Serial.printf("[TIME] Nouveau jour : %02d/%02d/%04d\n",
      now.day, now.month, now.year);
  }

  // 3. Arrosage matin 09h00
  if (now.valid && !morningDone && !relayActive &&
      now.hour == WATERING_HOUR && now.minute == 0) {
    morningDone = true;
    relayON(DURATION_MORNING_S * 1000UL, "morning_9h");
  }

  // 4. Fin cooldown température
  if (tempCooldownActive && (ms - tempCooldownStart >= TEMP_COOLDOWN_MS)) {
    tempCooldownActive = false;
    Serial.println("[TEMP] Cooldown terminé");
  }

  // 5. Arrosage température > 30°C
  if (!relayActive && !tempCooldownActive && currentTemp >= TEMP_THRESHOLD) {
    tempCooldownActive = true;
    tempCooldownStart  = ms;
    relayON(DURATION_TEMP_S * 1000UL, "temp_30deg");
  }

  // 6. Status périodique
  if (ms - lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = ms;
    publishStatus(now);
  }

  delay(1000);
}
