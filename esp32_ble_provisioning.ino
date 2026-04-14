/*
 * ESP32 BLE Wi-Fi Provisioning + MQTT Cloud Control
 * ─────────────────────────────────────────────────────────────────
 * Full flow:
 *   1. Boot → derive unique name from MAC  (e.g. IoT-BE0911)
 *   2. Try saved Wi-Fi from NVS → if connected, jump straight to MQTT
 *   3. Otherwise → BLE provisioning via phone / QR code
 *   4. After Wi-Fi connects → subscribe to HiveMQ, publish status every 5 s
 *   5. Incoming MQTT commands (relay on/off, duty cycle) handled in callback
 *
 * Required libraries (Arduino Library Manager):
 *   - ESP32 BLE Arduino   (built-in with ESP32 board package)
 *   - Preferences         (built-in with ESP32 board package)
 *   - PubSubClient        by Nick O'Leary
 *   - ArduinoJson         by Benoit Blanchon
 *
 * Board: ESP32 Dev Module (or your specific variant)
 * ─────────────────────────────────────────────────────────────────
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ── BLE / provisioning page UUIDs ─────────────────────────────────────
#define SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define SSID_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define PASS_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define STATUS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"

// ── Provisioning page (unchanged) ─────────────────────────────────────
#define PAGE_BASE_URL "https://darshit-vadgama.github.io/IoT/"

// ── MQTT broker — fill in from your HiveMQ Cloud console ──────────────
//   Dashboard → Clusters → YOUR CLUSTER → Overview
#define MQTT_HOST  "YOUR-CLUSTER.s1.eu.hivemq.cloud"  // ✏️ replace
#define MQTT_PORT  8883                                // TLS
#define MQTT_USER  "YOUR_MQTT_USERNAME"                // ✏️ replace
#define MQTT_PASS  "YOUR_MQTT_PASSWORD"                // ✏️ replace

// ── Hardware pins — adjust to your wiring ─────────────────────────────
#define RELAY_PIN    26    // digital output for relay
#define PWM_PIN      25    // PWM output (motor speed, LED dimmer, etc.)
#define PWM_CHANNEL  0
#define PWM_FREQ     1000  // Hz
#define PWM_RES      8     // bits → range 0-255

// ── Timing ────────────────────────────────────────────────────────────
#define STATUS_INTERVAL_MS  5000   // publish status to cloud every 5 s
#define MQTT_RECONNECT_MS   5000   // wait between reconnect attempts

// ── BLE globals ───────────────────────────────────────────────────────
BLEServer*         pServer        = nullptr;
BLECharacteristic* pStatusChar    = nullptr;

char   deviceName[16] = "";
bool   deviceConnected = false;
bool   ssidReceived    = false;
bool   passReceived    = false;
bool   triggerWiFi     = false;
String pendingSSID     = "";
String pendingPass     = "";

// ── MQTT globals ──────────────────────────────────────────────────────
WiFiClientSecure wifiSecureClient;
PubSubClient     mqtt(wifiSecureClient);

char topicStatus[48];    // "iot/IoT-XXXXXX/status"
char topicCommands[48];  // "iot/IoT-XXXXXX/commands"

// ── Device state (updated by MQTT commands) ───────────────────────────
bool relayState          = false;
int  dutyValue           = 0;
unsigned long lastPublishMs    = 0;
unsigned long lastReconnectMs  = 0;

Preferences preferences;

// ─────────────────────────────────────────────────────────────────────
// MQTT: incoming command handler
// Expected payload: {"relay":"on"} or {"duty":128} or both
// ─────────────────────────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("[MQTT] Message on %s\n", topic);

  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("[MQTT] JSON parse failed: %s\n", err.c_str());
    return;
  }

  if (doc.containsKey("relay")) {
    relayState = strcmp(doc["relay"], "on") == 0;
    digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);
    Serial.printf("[CMD]  Relay → %s\n", relayState ? "ON" : "OFF");
  }

  if (doc.containsKey("duty")) {
    dutyValue = constrain((int)doc["duty"], 0, 255);
    ledcWrite(PWM_CHANNEL, dutyValue);
    Serial.printf("[CMD]  Duty  → %d / 255\n", dutyValue);
  }
}

// ─────────────────────────────────────────────────────────────────────
// MQTT: publish current device state
// ─────────────────────────────────────────────────────────────────────
void publishStatus() {
  if (!mqtt.connected()) return;

  StaticJsonDocument<128> doc;
  doc["relay"]  = relayState ? "on" : "off";
  doc["duty"]   = dutyValue;
  doc["uptime"] = millis() / 1000;
  doc["ip"]     = WiFi.localIP().toString();

  char buf[128];
  serializeJson(doc, buf);
  mqtt.publish(topicStatus, buf, true);  // retain=true so dashboard sees last state instantly
  Serial.printf("[MQTT] Published: %s\n", buf);
}

// ─────────────────────────────────────────────────────────────────────
// MQTT: connect / reconnect (called from setup + loop)
// ─────────────────────────────────────────────────────────────────────
void connectMQTT() {
  wifiSecureClient.setInsecure();  // skips cert check — fine for dev/testing
                                   // For production: use wifiSecureClient.setCACert(rootCA)

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(10);

  Serial.printf("[MQTT] Connecting to %s...\n", MQTT_HOST);
  if (mqtt.connect(deviceName, MQTT_USER, MQTT_PASS)) {
    Serial.println("[MQTT] Connected!");
    mqtt.subscribe(topicCommands);
    Serial.printf("[MQTT] Subscribed to %s\n", topicCommands);
    publishStatus();  // send current state immediately on connect
  } else {
    Serial.printf("[MQTT] Failed — state=%d  (will retry)\n", mqtt.state());
  }
}

// ─────────────────────────────────────────────────────────────────────
// BLE server callbacks
// ─────────────────────────────────────────────────────────────────────
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("[BLE] Phone connected");
  }
  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    ssidReceived    = false;
    passReceived    = false;
    Serial.println("[BLE] Phone disconnected — restarting advertising");
    delay(500);
    BLEDevice::startAdvertising();
  }
};

class SSIDCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String val = pChar->getValue();
    if (val.length() > 0 && val.length() <= 32) {
      pendingSSID  = val;
      ssidReceived = true;
      Serial.println("[BLE] SSID received: " + pendingSSID);
    } else {
      Serial.println("[BLE] Invalid SSID length");
    }
  }
};

class PassCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    String val = pChar->getValue();
    if (val.length() > 0 && val.length() <= 64) {
      pendingPass  = val;
      passReceived = true;
      Serial.println("[BLE] Password received");
      if (ssidReceived) triggerWiFi = true;
      else Serial.println("[BLE] Warning: password before SSID");
    } else {
      Serial.println("[BLE] Invalid password length");
    }
  }
};

// ─────────────────────────────────────────────────────────────────────
// BLE helpers (unchanged from original)
// ─────────────────────────────────────────────────────────────────────
void sendBLEStatus(const String& status) {
  if (pStatusChar && deviceConnected) {
    pStatusChar->setValue(status.c_str());
    pStatusChar->notify();
    Serial.println("[BLE] Status → " + status);
  }
}

void startBLE() {
  Serial.println("[BLE] Initialising...");
  BLEDevice::init(deviceName);
  BLEDevice::setMTU(185);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  BLECharacteristic* pSSIDChar = pService->createCharacteristic(
    SSID_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  pSSIDChar->setCallbacks(new SSIDCallbacks());

  BLECharacteristic* pPassChar = pService->createCharacteristic(
    PASS_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  pPassChar->setCallbacks(new PassCallbacks());

  pStatusChar = pService->createCharacteristic(
    STATUS_CHAR_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pStatusChar->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);
  pAdv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.printf("[BLE] Advertising as \"%s\"\n", deviceName);
}

bool connectWiFi(const String& ssid, const String& pass, int timeoutSeconds = 15) {
  Serial.printf("[WiFi] Connecting to \"%s\"...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  int elapsed = 0;
  while (WiFi.status() != WL_CONNECTED && elapsed < timeoutSeconds * 2) {
    delay(500);
    Serial.print(".");
    elapsed++;
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

// ─────────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32 BLE Provisioning + MQTT ===");

  // Hardware init
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);

  // Derive unique device name from last 3 bytes of factory MAC
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(deviceName, sizeof(deviceName), "IoT-%02X%02X%02X", mac[3], mac[4], mac[5]);

  // Build MQTT topic strings
  snprintf(topicStatus,   sizeof(topicStatus),   "iot/%s/status",   deviceName);
  snprintf(topicCommands, sizeof(topicCommands),  "iot/%s/commands", deviceName);

  Serial.printf("[ID]   Device name : %s\n", deviceName);
  Serial.printf("[ID]   Full MAC    : %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("[QR]   Generate QR : python generate_qr.py %s\n", deviceName);
  Serial.printf("[QR]   Or URL      : %s?device=%s\n\n", PAGE_BASE_URL, deviceName);

  // Try saved credentials first
  preferences.begin("wifi-creds", true);
  String savedSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("pass", "");
  preferences.end();

  if (savedSSID.length() > 0) {
    Serial.println("[WiFi] Found saved credentials, trying...");
    if (connectWiFi(savedSSID, savedPass)) {
      Serial.println("[WiFi] Connected!  IP: " + WiFi.localIP().toString());
      connectMQTT();  // ← jump straight to cloud, skip BLE entirely
      return;
    }
    Serial.println("[WiFi] Saved credentials failed — falling back to BLE provisioning");
  } else {
    Serial.println("[WiFi] No saved credentials");
  }

  startBLE();
}

// ─────────────────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────────────────
void loop() {

  // ── 1. Handle Wi-Fi credentials received via BLE ─────────────────
  if (triggerWiFi) {
    triggerWiFi = false;
    sendBLEStatus("connecting");

    if (connectWiFi(pendingSSID, pendingPass)) {
      String ip = WiFi.localIP().toString();
      Serial.println("[WiFi] Connected!  IP: " + ip);

      preferences.begin("wifi-creds", false);
      preferences.putString("ssid", pendingSSID);
      preferences.putString("pass", pendingPass);
      preferences.end();
      Serial.println("[NVS]  Credentials saved");

      sendBLEStatus("connected:" + ip);
      delay(3000);  // give phone time to read the notification

      connectMQTT();  // ← now start MQTT after fresh provisioning
    } else {
      Serial.println("[WiFi] Connection failed");
      sendBLEStatus("failed");
    }
  }

  // ── 2. MQTT maintenance (only when Wi-Fi is up) ───────────────────
  if (WiFi.status() == WL_CONNECTED) {

    if (!mqtt.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectMs >= MQTT_RECONNECT_MS) {
        lastReconnectMs = now;
        Serial.println("[MQTT] Disconnected — attempting reconnect...");
        connectMQTT();
      }
    } else {
      mqtt.loop();  // processes incoming commands from dashboard

      // Periodic status publish
      if (millis() - lastPublishMs >= STATUS_INTERVAL_MS) {
        publishStatus();
        lastPublishMs = millis();
      }
    }
  }

  // ── 3. Your IoT application logic goes here ───────────────────────
  // e.g. read sensors, run PID loop, check schedules, etc.

  delay(10);
}
