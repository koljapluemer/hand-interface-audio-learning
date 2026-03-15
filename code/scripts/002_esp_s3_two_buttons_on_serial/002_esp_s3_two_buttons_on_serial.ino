#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLE2902.h>

// UUID definitions for the BLE service and characteristic.
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// Define your button pins (active LOW)
const int buttonPin1 = 2;  // Button 1 on GPIO1
const int buttonPin2 = 5;  // Button 2 on GPIO5

BLECharacteristic *pCharacteristic;
bool deviceConnected = false;

// Custom BLE Server Callbacks to track connection status.
class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
    Serial.println("BLE Client Connected");
  };

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    Serial.println("BLE Client Disconnected");
  }
};

void setup() {
  Serial.begin(9600);
  Serial.println("Hello!");
  // Configure button pins as inputs with internal pull-ups.
  pinMode(buttonPin1, INPUT_PULLUP);
  pinMode(buttonPin2, INPUT_PULLUP);

  // Initialize BLE device and create a server.
  BLEDevice::init("ESP32_S3_Button_Device");
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  // Create a BLE service.
  BLEService *pService = pServer->createService(SERVICE_UUID);

  // Create a BLE characteristic with read, write, and notify properties.
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ   |
                      BLECharacteristic::PROPERTY_WRITE  |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );
  // Add descriptor to allow notifications.
  pCharacteristic->addDescriptor(new BLE2902());

  // Set an initial value.
  pCharacteristic->setValue("No Button Pressed");

  pService->start();

  // Start advertising the service.
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("BLE advertising started. Waiting for client connection...");
}

void loop() {
  // Read the button states (active LOW means pressed equals LOW)
  int state1 = digitalRead(buttonPin1);
  int state2 = digitalRead(buttonPin2);

  String buttonState;
  if (state1 == LOW && state2 == LOW) {
    buttonState = "Both Pressed";
  } else if (state1 == LOW) {
    buttonState = "Button 1 Pressed";
  } else if (state2 == LOW) {
    buttonState = "Button 2 Pressed";
  } else {
    buttonState = "No Button Pressed";
  }

  // Log the state to the serial monitor.
  Serial.println(buttonState);

  // If a BLE client is connected, update the characteristic and notify.
  if (deviceConnected) {
    pCharacteristic->setValue(buttonState.c_str());
    pCharacteristic->notify();
  }

  // Small delay to help with button debounce and avoid flooding notifications.
  delay(100);
}
