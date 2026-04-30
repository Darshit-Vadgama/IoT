/*
 * ESP32 BLE Wi-Fi Provisioning + MQTT Cloud Control
 * ─────────────────────────────────────────────────────────────────
 * Full flow:
 *   1. Boot → derive unique name from MAC  (e.g. IoT-BE0911)
 *   2. Try saved Wi-Fi from NVS → if connected, jump straight to MQTT
 *   3. Otherwise → BLE provisioning via phone / QR code
 *   4. After Wi-Fi connects → subscribe to HiveMQ, publish status every 5 s
 *   5. Incoming MQTT commands (relay on/off) handled in callback
 *
 * BLE Security:
 *   BLE_PIN (defined below) is a 6-digit passkey.
 *   When a phone tries to connect it will be prompted to enter this PIN.
 *   Only after correct PIN entry will the phone be able to read/write
 *   the provisioning characteristics.
 *
 * Pin roles:
 *   SENSOR_PIN (GPIO34) — digital input only, read-only
 *                          reported as "duty" field (0 or 1) in the payload
 *                          No DB changes needed — duty column already exists.
 *
 *   LED_PIN    (GPIO2)  — built-in LED on most ESP32 dev boards
 *                          read + write: dashboard sends relay "on"/"off",
 *                          LED reflects that command, and status reports it back.
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
#include <BLESecurity.h>       // ← NEW: needed for BLE PIN security
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

// ── BLE Security PIN ───────────────────────────────────────────────────
// Change this to any 6-digit number (000000 – 999999).
// The connecting phone will be prompted: "Enter PIN: 123456"
#define BLE_PIN  123456

// ── Provisioning page (unchanged) ─────────────────────────────────────
#define PAGE_BASE_URL "https://darshit-vadgama.github.io/IoT/"

// ── MQTT broker ────────────────────────────────────────────────────────
#define MQTT_HOST  "39d1e721ad77428ea3f6d3040ae82386.s1.eu.hivemq.cloud"
#define MQTT_PORT  8883
#define MQTT_USER  "hivemq.webclient.1776147043732"
#define MQTT_PASS  "6W5bCDhKm@q<!u14Y:kL"

// ── Hardware pins ──────────────────────────────────────────────────────
#define SENSOR_PIN  34   // Read-only digital input (GPIO34 is input-only on ESP32)
#define LED_PIN      2   // Built-in LED on most ESP32 dev boards (GPIO2)

// ── Timing ────────────────────────────────────────────────────────────
#define STATUS_INTERVAL_MS  5000   // publish status every 5 s
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

// ── Device state ──────────────────────────────────────────────────────
bool relayState = false;

unsigned long lastPublishMs   = 0;
unsigned long lastReconnectMs = 0;

Preferences preferences;

// ─────────────────────────────────────────────────────────────────────
// NEW: BLE Security callbacks
// Handles the passkey display / confirmation flow.
// ─────────────────────────────────────────────────────────────────────
class MySecurityCallbacks : public BLESecurityCallbacks {
  // Called when the stack needs to display a passkey to the user.
  // We print it on Serial — you could also show it on a display.
  void onPassKeyNotify(uint32_t pass_key) override {
    Serial.printf("[BLE-SEC] Passkey Notify: %06d\n", pass_key);
  }

  // Called when the stack generates a passkey (static PIN mode).
  // Return BLE_PIN so the stack uses our fixed value.
  uint32_t onPassKeyRequest() override {
    Serial.printf("[BLE-SEC] Passkey Request → returning %06d\n", BLE_PIN);
    return BLE_PIN;
  }

  // Called to confirm the passkey on both sides (numeric comparison).
  // For static PIN we always return true.
  bool onConfirmPIN(uint32_t pin) override {
    Serial.printf("[BLE-SEC] Confirm PIN: %06d\n", pin);
    return true;
  }

  // Called when authentication completes.
  void onAuthenticationComplete(esp_ble_auth_cmpl_t cmpl) override {
    if (cmpl.success) {
      Serial.println("[BLE-SEC] Authentication successful ✓");
    } else {
      Serial.printf("[BLE-SEC] Authentication FAILED — reason: 0x%02X\n", cmpl.fail_reason);
    }
  }

  // Required override — return true if pre-authentication is needed.
  bool onSecurityRequest() override {
    Serial.println("[BLE-SEC] Security request received");
    return true;
  }
};

// ─────────────────────────────────────────────────────────────────────
// MQTT: incoming command handler
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
    digitalWrite(LED_PIN, relayState ? HIGH : LOW);
    Serial.printf("[CMD]  LED (relay) → %s\n", relayState ? "ON" : "OFF");
  }
}

// ─────────────────────────────────────────────────────────────────────
// MQTT: publish current device state
// ─────────────────────────────────────────────────────────────────────
void publishStatus() {
  if (!mqtt.connected()) return;

  int sensorState = digitalRead(SENSOR_PIN);

  StaticJsonDocument<128> doc;
  doc["relay"]  = relayState ? "on" : "off";
  doc["duty"]   = sensorState;
  doc["uptime"] = millis() / 1000;
  doc["ip"]     = WiFi.localIP().toString();

  char buf[128];
  serializeJson(doc, buf);
  mqtt.publish(topicStatus, buf, true);
  Serial.printf("[MQTT] Published: %s\n", buf);
}

// ─────────────────────────────────────────────────────────────────────
// MQTT: connect / reconnect
// ─────────────────────────────────────────────────────────────────────
void connectMQTT() {
  wifiSecureClient.setInsecure();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(10);

  Serial.printf("[MQTT] Connecting to %s...\n", MQTT_HOST);
  if (mqtt.connect(deviceName, MQTT_USER, MQTT_PASS)) {
    Serial.println("[MQTT] Connected!");
    mqtt.subscribe(topicCommands);
    Serial.printf("[MQTT] Subscribed to %s\n", topicCommands);
    publishStatus();
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
// BLE helpers
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

  // ── NEW: Configure BLE security with a static PIN ─────────────────
  BLEDevice::setSecurityCallbacks(new MySecurityCallbacks());

  BLESecurity* pSecurity = new BLESecurity();
  pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_MITM_BOND);
  //   SC   = Secure Connections (BLE 4.2+)
  //   MITM = Man-in-the-middle protection  ← this enforces the PIN
  //   BOND = remember the paired device

  pSecurity->setCapability(ESP_IO_CAP_OUT);
  //   IO capability "Display Only" → ESP32 shows the PIN on Serial.
  //   The phone generates a random key and prompts the user to confirm
  //   it matches what the device displays.
  //   To use a FIXED PIN instead, keep setStaticPIN() below.

  pSecurity->setStaticPIN(BLE_PIN);
  //   Forces the passkey to always be BLE_PIN (123456 by default).
  //   Remove this line if you prefer a random PIN each pairing.

  pSecurity->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  Serial.printf("[BLE-SEC] PIN security enabled — passkey: %06d\n", BLE_PIN);
  // ──────────────────────────────────────────────────────────────────

  BLEDevice::setMTU(185);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // ── NEW: Require encryption on SSID & Password characteristics ────
  // A phone must complete PIN pairing before it can write these.
  BLECharacteristic* pSSIDChar = pService->createCharacteristic(
    SSID_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  pSSIDChar->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);  // ← encrypted write only
  pSSIDChar->setCallbacks(new SSIDCallbacks());

  BLECharacteristic* pPassChar = pService->createCharacteristic(
    PASS_CHAR_UUID, BLECharacteristic::PROPERTY_WRITE);
  pPassChar->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);  // ← encrypted write only
  pPassChar->setCallbacks(new PassCallbacks());

  // Status characteristic — notify only, no PIN needed to receive updates
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

  pinMode(SENSOR_PIN, INPUT);
  pinMode(LED_PIN,    OUTPUT);
  digitalWrite(LED_PIN, LOW);

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(deviceName, sizeof(deviceName), "IoT-%02X%02X%02X", mac[3], mac[4], mac[5]);

  snprintf(topicStatus,   sizeof(topicStatus),   "iot/%s/status",   deviceName);
  snprintf(topicCommands, sizeof(topicCommands),  "iot/%s/commands", deviceName);

  Serial.printf("[ID]   Device name : %s\n", deviceName);
  Serial.printf("[ID]   Full MAC    : %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("[QR]   Generate QR : python generate_qr.py %s\n", deviceName);
  Serial.printf("[QR]   Or URL      : %s?device=%s\n\n", PAGE_BASE_URL, deviceName);

  preferences.begin("wifi-creds", true);
  String savedSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("pass", "");
  preferences.end();

  if (savedSSID.length() > 0) {
    Serial.println("[WiFi] Found saved credentials, trying...");
    if (connectWiFi(savedSSID, savedPass)) {
      Serial.println("[WiFi] Connected!  IP: " + WiFi.localIP().toString());
      connectMQTT();
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

  // ── 1. Handle Wi-Fi credentials received via BLE ──────────────────
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
      delay(3000);

      connectMQTT();
    } else {
      Serial.println("[WiFi] Connection failed");
      sendBLEStatus("failed");
    }
  }

  // ── 2. MQTT maintenance ───────────────────────────────────────────
  if (WiFi.status() == WL_CONNECTED) {

    if (!mqtt.connected()) {
      unsigned long now = millis();
      if (now - lastReconnectMs >= MQTT_RECONNECT_MS) {
        lastReconnectMs = now;
        Serial.println("[MQTT] Disconnected — attempting reconnect...");
        connectMQTT();
      }
    } else {
      mqtt.loop();

      if (millis() - lastPublishMs >= STATUS_INTERVAL_MS) {
        publishStatus();
        lastPublishMs = millis();
      }
    }
  }

  // ── 3. Your IoT application logic goes here ───────────────────────

  delay(10);
}
