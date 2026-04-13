/*
 * ESP32 BLE Wi-Fi Provisioning
 * ─────────────────────────────────────────────────────────────────
 * Flow:
 *   1. On boot → try saved Wi-Fi credentials (NVS/Preferences)
 *   2. If no saved creds (or connection fails) → start BLE advertising
 *   3. Phone connects via Web Bluetooth (QR code → browser page)
 *   4. Phone writes SSID, then Password via BLE GATT
 *   5. ESP32 connects to Wi-Fi, saves creds, notifies status back
 *   6. BLE stays alive so phone can see result ("connected" / "failed")
 *
 * Required Libraries (install via Arduino Library Manager):
 *   - ESP32 BLE Arduino  (built-in with ESP32 board package)
 *   - Preferences        (built-in with ESP32 board package)
 *
 * Board: ESP32 Dev Module (or your specific ESP32 board)
 * ─────────────────────────────────────────────────────────────────
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <WiFi.h>
#include <Preferences.h>

// ── Device Identity ──────────────────────────────────────────────
// This MUST match the name in the HTML provisioning page's filter.
#define DEVICE_NAME "IoT-Setup"

// ── GATT UUIDs ───────────────────────────────────────────────────
// These must also match the UUIDs in wifi_provisioning.html
#define SERVICE_UUID     "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define SSID_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define PASS_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define STATUS_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"

// ── Globals ──────────────────────────────────────────────────────
BLEServer*         pServer     = nullptr;
BLECharacteristic* pStatusChar = nullptr;

bool   deviceConnected  = false;
bool   ssidReceived     = false;
bool   passReceived     = false;
bool   triggerWiFi      = false;
String pendingSSID      = "";
String pendingPass      = "";

Preferences preferences;

// ─────────────────────────────────────────────────────────────────
// BLE Server Callbacks
// ─────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────
// SSID Characteristic Write Callback
// ─────────────────────────────────────────────────────────────────
class SSIDCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() <= 32) {
      pendingSSID  = String(val.c_str());
      ssidReceived = true;
      Serial.println("[BLE] SSID received: " + pendingSSID);
    } else {
      Serial.println("[BLE] Invalid SSID length");
    }
  }
};

// ─────────────────────────────────────────────────────────────────
// Password Characteristic Write Callback
// Receiving the password triggers the Wi-Fi connection attempt.
// ─────────────────────────────────────────────────────────────────
class PassCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pChar) override {
    std::string val = pChar->getValue();
    if (val.length() > 0 && val.length() <= 64) {
      pendingPass  = String(val.c_str());
      passReceived = true;
      Serial.println("[BLE] Password received (hidden for security)");
      if (ssidReceived) {
        triggerWiFi = true;   // handled safely in loop()
      } else {
        Serial.println("[BLE] Warning: password received before SSID");
      }
    } else {
      Serial.println("[BLE] Invalid password length");
    }
  }
};

// ─────────────────────────────────────────────────────────────────
// Helper: send a status string back to the phone via BLE notify
// ─────────────────────────────────────────────────────────────────
void sendStatus(const String& status) {
  if (pStatusChar && deviceConnected) {
    pStatusChar->setValue(status.c_str());
    pStatusChar->notify();
    Serial.println("[BLE] Status sent: " + status);
  }
}

// ─────────────────────────────────────────────────────────────────
// Initialize and start BLE advertising
// ─────────────────────────────────────────────────────────────────
void startBLE() {
  Serial.println("[BLE] Initialising...");
  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setMTU(185);   // allow longer writes (SSID+Pass)

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // SSID characteristic — WRITE only
  BLECharacteristic* pSSIDChar = pService->createCharacteristic(
    SSID_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pSSIDChar->setCallbacks(new SSIDCallbacks());

  // Password characteristic — WRITE only
  BLECharacteristic* pPassChar = pService->createCharacteristic(
    PASS_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pPassChar->setCallbacks(new PassCallbacks());

  // Status characteristic — NOTIFY only (phone subscribes for updates)
  pStatusChar = pService->createCharacteristic(
    STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pStatusChar->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->addServiceUUID(SERVICE_UUID);
  pAdv->setScanResponse(true);
  pAdv->setMinPreferred(0x06);  // helps iPhone connection
  pAdv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising as \"" DEVICE_NAME "\"");
}

// ─────────────────────────────────────────────────────────────────
// Attempt Wi-Fi connection with given credentials
// ─────────────────────────────────────────────────────────────────
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

// ─────────────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n=== ESP32 BLE Provisioning ===");

  // ── Try saved credentials first ──────────────────────────────
  preferences.begin("wifi-creds", true);   // read-only namespace
  String savedSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("pass", "");
  preferences.end();

  if (savedSSID.length() > 0) {
    Serial.println("[WiFi] Found saved credentials, trying...");
    if (connectWiFi(savedSSID, savedPass)) {
      Serial.println("[WiFi] Connected!  IP: " + WiFi.localIP().toString());

      // ── YOUR IoT CODE STARTS HERE when already provisioned ────
      // e.g., begin MQTT, start sensors, etc.
      return;  // skip BLE entirely if already provisioned
    }
    Serial.println("[WiFi] Saved credentials failed — falling back to BLE provisioning");
  } else {
    Serial.println("[WiFi] No saved credentials found");
  }

  // ── No valid Wi-Fi → start BLE provisioning ──────────────────
  startBLE();
}

// ─────────────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────────────
void loop() {

  // ── Handle incoming Wi-Fi credentials from phone ─────────────
  if (triggerWiFi) {
    triggerWiFi = false;

    sendStatus("connecting");

    if (connectWiFi(pendingSSID, pendingPass)) {
      String ip = WiFi.localIP().toString();
      Serial.println("[WiFi] Connected!  IP: " + ip);

      // Save credentials to NVS for future boots
      preferences.begin("wifi-creds", false);  // read-write
      preferences.putString("ssid", pendingSSID);
      preferences.putString("pass", pendingPass);
      preferences.end();
      Serial.println("[NVS] Credentials saved");

      sendStatus("connected:" + ip);

      // Give the phone 3 s to receive the notification before we
      // potentially stop BLE (optional — remove if you keep BLE up)
      delay(3000);

      // ── YOUR IoT CODE STARTS HERE after fresh provisioning ────
      // e.g., begin MQTT, start sensors, etc.

    } else {
      Serial.println("[WiFi] Connection failed");
      sendStatus("failed");
      // Credentials are NOT saved — phone can retry
    }
  }

  // ── Add your recurring IoT logic below ───────────────────────
  // e.g., read sensors, publish MQTT, etc.

  delay(10);
}
