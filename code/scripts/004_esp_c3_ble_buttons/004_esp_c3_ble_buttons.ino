/*
 * 004_esp_c3_ble_buttons
 * ----------------------
 * Turns the three-button ESP32-C3 remote from 003 into a BLE keypad for the
 * "Sentences" Flutter app. Same buttons on GPIO 3, 4 and 5 (INPUT_PULLUP, wire
 * each button between its GPIO and GND), same debounce logic as 003, but instead
 * of printing to serial/OLED it exposes a Nordic UART Service (NUS) and, on each
 * confirmed press, notifies a single ASCII digit ('3', '4' or '5') followed by
 * '\n' on the TX characteristic.
 *
 * No display here, so the GPIO5 / OLED-SDA conflict from 003 does not apply.
 *
 * BLE contract (must match lib/services/ble_button_service.dart):
 *   Device name : "SentenceRemote"
 *   Service     : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E   (Nordic UART Service)
 *   TX (notify) : 6E400003-B5A3-F393-E0A9-E50E24DCCA9E   device -> app, button digit
 *   RX (write)  : 6E400002-B5A3-F393-E0A9-E50E24DCCA9E   app -> device, unused for now
 *
 * Build: Arduino IDE with the esp32 board package (BLE is bundled, no extra
 * library install). Select an ESP32-C3 board, 115200 baud serial for the log.
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define DEVICE_NAME "SentenceRemote"
#define NUS_SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

// --- Buttons (same as 003) -----------------------------------------------------
const uint8_t  BTN_PINS[]  = { 3, 4, 5 };
const uint8_t  NUM_BTNS    = sizeof(BTN_PINS) / sizeof(BTN_PINS[0]);
const uint16_t DEBOUNCE_MS = 25;

uint8_t  stableState[NUM_BTNS];
uint8_t  lastReading[NUM_BTNS];
uint32_t lastChangeMs[NUM_BTNS];

// --- BLE ---------------------------------------------------------------------
BLECharacteristic *txChar = nullptr;
bool clientConnected = false;

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    clientConnected = true;
    Serial.println("BLE client connected");
  }
  void onDisconnect(BLEServer *server) override {
    clientConnected = false;
    Serial.println("BLE client disconnected, re-advertising");
    server->startAdvertising();
  }
};

void notifyButton(uint8_t pin) {
  Serial.printf("PRESS P%u -> notify '%c'\n", pin, char('0' + pin));
  if (!clientConnected || txChar == nullptr) return;
  uint8_t msg[2] = { uint8_t('0' + pin), uint8_t('\n') };
  txChar->setValue(msg, sizeof(msg));
  txChar->notify();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("004_esp_c3_ble_buttons: NUS button remote on pins 3, 4, 5");

  for (uint8_t i = 0; i < NUM_BTNS; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    stableState[i]  = HIGH;
    lastReading[i]  = HIGH;
    lastChangeMs[i] = 0;
  }

  BLEDevice::init(DEVICE_NAME);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService *service = server->createService(NUS_SERVICE_UUID);

  txChar = service->createCharacteristic(
      NUS_TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());

  // RX is declared for a standard-looking NUS profile; nothing reads it yet.
  service->createCharacteristic(
      NUS_RX_UUID, BLECharacteristic::PROPERTY_WRITE);

  service->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("Advertising as \"" DEVICE_NAME "\"");
}

void loop() {
  const uint32_t now = millis();

  for (uint8_t i = 0; i < NUM_BTNS; i++) {
    const uint8_t raw = digitalRead(BTN_PINS[i]);

    if (raw != lastReading[i]) {
      lastReading[i]  = raw;
      lastChangeMs[i] = now;
    }

    if ((now - lastChangeMs[i]) >= DEBOUNCE_MS && raw != stableState[i]) {
      stableState[i] = raw;
      if (raw == LOW) {          // HIGH -> LOW = a fresh press
        notifyButton(BTN_PINS[i]);
      }
    }
  }

  delay(5);
}
