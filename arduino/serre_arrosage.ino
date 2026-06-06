// ================================================================
//  SERRE ARROSAGE AUTONOME v1.3
//  ESP8266 + DS3231 + Relais HW-803 + Électrovanne 24VAC
//  + 2x Capteurs débit YF-B1 (maison + chevaux)
//  Heure : NTP priorité, DS3231 fallback
//  Temp  : Capteur Zigbee via HA → MQTT
// ================================================================
// Brochage :
//   D1 (GPIO5)  → Relais IN
//   D2 (GPIO4)  → DS3231 SDA
//   D3 (GPIO0)  → DS3231 SCL
//   D5 (GPIO14) → YF-B1 Maison  (via diviseur 10k+20k)
//   D6 (GPIO12) → YF-B1 Chevaux (via diviseur 10k+20k)
// ================================================================

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <RTClib.h>
#include <ArduinoOTA.h>
#include <time.h>

// ================================================================
// ⚙️  CONFIGURATION
// ================================================================
const char* WIFI_SSID     = "TP-Link_IoT_DF72";
const char* WIFI_PASSWORD = "TON_PASS_WIFI";

const char* MQTT_SERVER   = "192.168.1.160";
const int   MQTT_PORT     = 1883;
const char* MQTT_USER     = "mqtt";
const char* MQTT_PASSWORD = "jubkuh-";
const char* MQTT_CLIENT   = "serre_arrosage";

// Topics MQTT
const char* TOPIC_TEMP_SUB    = "zigbee2mqtt-edge/capteur écran";
const char* TOPIC_STATUS_PUB  = "serre/arrosage/status";
const char* TOPIC_RELAY_PUB   = "serre/arrosage/relay";
const char* TOPIC_CMD_SUB     = "serre/arrosage/cmd";
const char* TOPIC_FLOW_MAISON  = "serre/debit/maison";
const char* TOPIC_FLOW_CHEVAUX = "serre/debit/chevaux";
const char* TOPIC_FLOW_ALERT   = "serre/debit/alerte";

// Arrosage
const uint8_t  WATERING_HOUR      = 9;
const uint32_t DURATION_MORNING_S = 15 * 60;
const uint32_t DURATION_TEMP_S    = 2  * 60;
const float    TEMP_THRESHOLD     = 30.0f;
const uint32_t TEMP_COOLDOWN_MS   = 30UL * 60 * 1000;

// NTP
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET_S = 3600;
const int   DST_OFFSET_S = 3600;

// Débit YF-B1
// ⚠️ Signal 5V → diviseur de tension obligatoire (10kΩ + 20kΩ)
const float    PULSES_PER_LITER  = 450.0f; // à calibrer
const float    ALERT_LEAK_LPM    = 0.5f;   // fuite hors arrosage
const float    ALERT_HIGHFLOW_LPM = 25.0f; // débit max anormal

// Pins
#define RELAY_PIN      D1   // GPIO5
#define I2C_SDA        D2   // GPIO4
#define I2C_SCL        D3   // GPIO0
#define FLOW_PIN_MAISON  D5 // GPIO14
#define FLOW_PIN_CHEVAUX D6 // GPIO12

// ================================================================
// STRUCTURES
// ================================================================
struct TimeInfo {
  int hour, minute, second, day, month, year;
  bool valid;
};

struct FlowData {
  float    flowLpm      = 0;
  float    cycleLiters  = 0;
  float    dailyLiters  = 0;
  float    totalLiters  = 0;
  uint32_t lastPulse    = 0;
  uint32_t lastCalcMs   = 0;
};

// ================================================================
// VARIABLES GLOBALES
// ================================================================
RTC_DS3231   rtc;
bool         rtcPresent = false;
bool         ntpSynced  = false;

WiFiClient   espClient;
PubSubClient mqtt(espClient);

// Relay
bool     relayActive     = false;
uint32_t relayStartMs    = 0;
uint32_t relayDurationMs = 0;
char     relayReason[32] = "";

// Arrosage
bool morningDone    = false;
int  lastDayChecked = -1;

// Température
bool     tempCooldownActive = false;
uint32_t tempCooldownStart  = 0;
float    currentTemp        = -99.0f;

// Status
uint32_t lastStatusMs = 0;
const uint32_t STATUS_INTERVAL_MS = 30000;

// Débit — compteurs interruptions
volatile uint32_t pulseMaison  = 0;
volatile uint32_t pulseChevaux = 0;

FlowData fdMaison;
FlowData fdChevaux;

bool alertLeakMaison  = false;
bool alertLeakChevaux = false;

// ================================================================
// ISR — CAPTEURS DÉBIT (en IRAM pour fiabilité)
// ================================================================
void IRAM_ATTR isrMaison()  { pulseMaison++;  }
void IRAM_ATTR isrChevaux() { pulseChevaux++; }

// ================================================================
// HEURE (NTP priorité, DS3231 fallback)
// ================================================================
TimeInfo getTime() {
  TimeInfo t = {0, 0, 0, 1, 1, 2024, false};
  if (ntpSynced) {
    time_t now = time(nullptr);
    struct tm* ti = localtime(&now);
    if (ti->tm_year > 100) {
      t = {ti->tm_hour, ti->tm_min, ti->tm_sec,
           ti->tm_mday, ti->tm_mon+1, ti->tm_year+1900, true};
      return t;
    }
  }
  if (rtcPresent) {
    DateTime now = rtc.now();
    if (now.year() > 2020 && now.year() < 2100)
      t = {now.hour(), now.minute(), now.second(),
           now.day(), (int)now.month(), (int)now.year(), true};
  }
  return t;
}

// ================================================================
// RELAY
// ================================================================
void relayOFF();

void relayON(uint32_t durationMs, const char* reason) {
  if (relayActive) return;
  digitalWrite(RELAY_PIN, HIGH);
  relayActive     = true;
  relayStartMs    = millis();
  relayDurationMs = durationMs;
  strncpy(relayReason, reason, sizeof(relayReason)-1);
  fdMaison.cycleLiters  = 0;
  fdChevaux.cycleLiters = 0;
  Serial.printf("[RELAY] ON — %s — %lu s\n", reason, durationMs/1000);
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_RELAY_PUB, "ON", true);
    char msg[128];
    snprintf(msg, sizeof(msg),
      "{\"relay\":\"ON\",\"duration_s\":%lu,\"reason\":\"%s\"}",
      durationMs/1000, reason);
    mqtt.publish(TOPIC_STATUS_PUB, msg, true);
  }
}

void relayOFF() {
  digitalWrite(RELAY_PIN, LOW);
  relayActive = false;
  Serial.println("[RELAY] OFF");
  if (mqtt.connected()) {
    mqtt.publish(TOPIC_RELAY_PUB, "OFF", true);
    mqtt.publish(TOPIC_STATUS_PUB, "{\"relay\":\"OFF\"}", true);
  }
}

// ================================================================
// DÉBIT
// ================================================================
void computeFlow(FlowData& fd, volatile uint32_t& pulseCount) {
  uint32_t ms      = millis();
  uint32_t elapsed = ms - fd.lastCalcMs;
  if (elapsed < 1000) return;

  noInterrupts();
  uint32_t current = pulseCount;
  interrupts();

  uint32_t pulses = current - fd.lastPulse;
  fd.lastPulse    = current;

  float liters     = (float)pulses / PULSES_PER_LITER;
  fd.flowLpm       = liters * (60000.0f / (float)elapsed);
  fd.cycleLiters  += liters;
  fd.dailyLiters  += liters;
  fd.totalLiters  += liters;
  fd.lastCalcMs    = ms;
}

void publishFlow(FlowData& fd, const char* topic, const char* name) {
  if (!mqtt.connected()) return;
  char msg[200];
  snprintf(msg, sizeof(msg),
    "{\"name\":\"%s\",\"flow_lpm\":%.2f,\"cycle_l\":%.2f,"
    "\"daily_l\":%.2f,\"total_l\":%.2f}",
    name, fd.flowLpm, fd.cycleLiters, fd.dailyLiters, fd.totalLiters);
  mqtt.publish(topic, msg, true);
}

void sendFlowAlert(const char* msg) {
  Serial.printf("[DEBIT] ⚠️ %s\n", msg);
  if (mqtt.connected()) mqtt.publish(TOPIC_FLOW_ALERT, msg, false);
}

void checkFlowAlerts() {
  if (!relayActive && fdMaison.flowLpm > ALERT_LEAK_LPM && !alertLeakMaison) {
    alertLeakMaison = true;
    sendFlowAlert("FUITE_MAISON: debit detecte hors arrosage");
  } else if (fdMaison.flowLpm <= ALERT_LEAK_LPM) {
    alertLeakMaison = false;
  }
  if (fdChevaux.flowLpm > ALERT_LEAK_LPM && !alertLeakChevaux) {
    alertLeakChevaux = true;
    sendFlowAlert("FUITE_CHEVAUX: debit anormal detecte");
  } else if (fdChevaux.flowLpm <= ALERT_LEAK_LPM) {
    alertLeakChevaux = false;
  }
  if (fdMaison.flowLpm  > ALERT_HIGHFLOW_LPM)
    sendFlowAlert("DEBIT_ELEVE_MAISON: possible rupture canalisation");
  if (fdChevaux.flowLpm > ALERT_HIGHFLOW_LPM)
    sendFlowAlert("DEBIT_ELEVE_CHEVAUX: possible rupture canalisation");
}

// ================================================================
// WIFI
// ================================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WIFI] Connexion à %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] Connecté — IP : %s\n",
      WiFi.localIP().toString().c_str());
    configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
    Serial.print("[NTP]  Synchronisation");
    uint32_t t2 = millis();
    while (time(nullptr) < 1000000000UL && millis()-t2 < 10000) {
      delay(500); Serial.print(".");
    }
    if (time(nullptr) > 1000000000UL) {
      ntpSynced = true;
      time_t now = time(nullptr);
      Serial.printf("\n[NTP]  OK — %s", ctime(&now));
      if (rtcPresent) { rtc.adjust(DateTime((uint32_t)now));
        Serial.println("[RTC]  Synchronisé via NTP"); }
    } else Serial.println("\n[NTP]  Échec");
  } else Serial.println("\n[WIFI] Échec — mode autonome");
}

// ================================================================
// MQTT CALLBACK
// ================================================================
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String msg; msg.reserve(len);
  for (unsigned int i=0; i<len; i++) msg += (char)payload[i];
  Serial.printf("[MQTT] ← %s : %s\n", topic, msg.c_str());

  if (String(topic) == TOPIC_TEMP_SUB) {
    int idx = msg.indexOf("\"temperature\":");
    if (idx != -1) { currentTemp = msg.substring(idx+14).toFloat();
      Serial.printf("[TEMP] %.1f°C\n", currentTemp); }
    return;
  }
  if (String(topic) == TOPIC_CMD_SUB) {
    if      (msg == "ON_TEMP")       relayON(DURATION_TEMP_S*1000UL,    "manual_short");
    else if (msg == "ON_FULL")       relayON(DURATION_MORNING_S*1000UL, "manual_long");
    else if (msg == "OFF")           relayOFF();
    else if (msg == "RESET_MORNING") { morningDone=false;
      Serial.println("[CMD] Reset matin"); }
  }
}

// ================================================================
// MQTT CONNECT
// ================================================================
void connectMQTT() {
  if (mqtt.connected() || WiFi.status()!=WL_CONNECTED) return;
  Serial.print("[MQTT] Connexion...");
  bool ok = mqtt.connect(MQTT_CLIENT, MQTT_USER, MQTT_PASSWORD,
    TOPIC_STATUS_PUB, 1, true, "{\"status\":\"offline\"}");
  if (ok) {
    Serial.println(" OK");
    mqtt.publish(TOPIC_STATUS_PUB, "{\"status\":\"online\"}", true);
    mqtt.subscribe(TOPIC_TEMP_SUB);
    mqtt.subscribe(TOPIC_CMD_SUB);
  } else Serial.printf(" FAILED rc=%d\n", mqtt.state());
}

// ================================================================
// STATUS MQTT
// ================================================================
void publishStatus(const TimeInfo& t) {
  if (!mqtt.connected()) return;
  float rtcTemp = rtcPresent ? rtc.getTemperature() : -99.0f;
  char msg[400];
  snprintf(msg, sizeof(msg),
    "{\"status\":\"online\",\"time\":\"%02d:%02d:%02d\","
    "\"date\":\"%04d-%02d-%02d\",\"time_source\":\"%s\","
    "\"temp_c\":%.1f,\"rtc_temp_c\":%.1f,"
    "\"relay\":\"%s\",\"relay_reason\":\"%s\","
    "\"morning_done\":%s,\"temp_cooldown\":%s,\"ip\":\"%s\"}",
    t.hour, t.minute, t.second,
    t.year, t.month, t.day,
    ntpSynced ? "NTP" : (rtcPresent ? "RTC" : "none"),
    currentTemp, rtcTemp,
    relayActive ? "ON" : "OFF", relayReason,
    morningDone        ? "true" : "false",
    tempCooldownActive ? "true" : "false",
    WiFi.localIP().toString().c_str());
  mqtt.publish(TOPIC_STATUS_PUB, msg, true);
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200); delay(200);
  Serial.println("\n=============================");
  Serial.println("  SERRE ARROSAGE v1.3");
  Serial.println("=============================");

  // Relay OFF au démarrage
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  // DS3231 optionnel
  Wire.begin(I2C_SDA, I2C_SCL);
  if (rtc.begin()) {
    rtcPresent = true;
    Serial.println("[RTC]  DS3231 détecté ✓");
    if (rtc.lostPower()) Serial.println("[RTC]  Heure perdue — sync NTP en attente");
  } else Serial.println("[RTC]  DS3231 absent — NTP uniquement");

  // Capteurs débit
  pinMode(FLOW_PIN_MAISON,  INPUT_PULLUP);
  pinMode(FLOW_PIN_CHEVAUX, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_MAISON),  isrMaison,  FALLING);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN_CHEVAUX), isrChevaux, FALLING);
  fdMaison.lastCalcMs  = millis();
  fdChevaux.lastCalcMs = millis();
  Serial.println("[FLOW] Capteurs débit initialisés (D5=maison, D6=chevaux)");

  connectWiFi();
  mqtt.setServer(MQTT_SERVER, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  connectMQTT();

  ArduinoOTA.setHostname("serre-arrosage");
  ArduinoOTA.onStart([](){Serial.println("[OTA] Démarrage...");});
  ArduinoOTA.onEnd([](){Serial.println("[OTA] Terminé");});
  ArduinoOTA.begin();
  Serial.println("[SETUP] OK\n");
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
  if (relayActive && (ms-relayStartMs >= relayDurationMs)) relayOFF();

  // 2. Nouveau jour — reset flags
  if (now.valid && now.day != lastDayChecked) {
    lastDayChecked        = now.day;
    morningDone           = false;
    fdMaison.dailyLiters  = 0;
    fdChevaux.dailyLiters = 0;
    Serial.printf("[TIME] Nouveau jour : %02d/%02d/%04d\n",
      now.day, now.month, now.year);
  }

  // 3. Arrosage matin 09h00
  if (now.valid && !morningDone && !relayActive &&
      now.hour == WATERING_HOUR && now.minute == 0) {
    morningDone = true;
    relayON(DURATION_MORNING_S*1000UL, "morning_9h");
  }

  // 4. Fin cooldown température
  if (tempCooldownActive && (ms-tempCooldownStart >= TEMP_COOLDOWN_MS)) {
    tempCooldownActive = false;
    Serial.println("[TEMP] Cooldown terminé");
  }

  // 5. Arrosage température > 30°C
  if (!relayActive && !tempCooldownActive && currentTemp >= TEMP_THRESHOLD) {
    tempCooldownActive = true; tempCooldownStart = ms;
    relayON(DURATION_TEMP_S*1000UL, "temp_30deg");
  }

  // 6. Calcul débit
  computeFlow(fdMaison,  pulseMaison);
  computeFlow(fdChevaux, pulseChevaux);
  checkFlowAlerts();

  // 7. Status périodique
  if (ms-lastStatusMs >= STATUS_INTERVAL_MS) {
    lastStatusMs = ms;
    publishStatus(now);
    publishFlow(fdMaison,  TOPIC_FLOW_MAISON,  "maison");
    publishFlow(fdChevaux, TOPIC_FLOW_CHEVAUX, "chevaux");
  }

  delay(1000);
}
