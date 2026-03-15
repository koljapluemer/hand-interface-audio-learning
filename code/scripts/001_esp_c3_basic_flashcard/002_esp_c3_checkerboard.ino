#include <U8g2lib.h>
#include <Wire.h>

// Use U8g2 library to drive the integrated OLED on SDA (GPIO5) and SCL (GPIO6)
// The 72x40 region is mapped in the middle of the 132x64 buffer.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);
u8g2.setFont(u8g2_font_unifont_t_symbols); 

int width = 72;
int height = 40;
int xOffset = 30; // centers the region in a 132-pixel wide buffer
int yOffset = 12; // centers the region in a 64-pixel high buffer

// Define pins
#define BUTTON_PIN 4  // Button on GPIO4 (wired to GND, using INPUT_PULLUP)
#define LED_PIN    8  // LED on GPIO8

// Game state: state 0 = showing city prompt; state 1 = showing country.
uint8_t state = 0;

// Structure holding a city–country pair.
struct CityCountry {
  const char* city;
  const char* country;
};

// List of city–country pairs (only pairs with short names).
CityCountry pairs[] = {
  { "Ouani", "Comoros" },
  { "Mandza", "Comoros" },
  { "Fomboni", "Comoros" },
  { "Encamp", "Andorra" },
  { "La Massana", "Andorra" },
  { "Canillo", "Andorra" },
  { "Ordino", "Andorra" },
  { "Nuuk", "Greenland" },
  { "Sisimiut", "Greenland" },
  { "Ilulissat", "Greenland" },
  { "Aasiaat", "Greenland" },
};

const int numPairs = sizeof(pairs) / sizeof(pairs[0]);
int currentIndex = 0;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("City-Country Learning Game starting...");

  // Initialize the OLED display.
  u8g2.begin();
  u8g2.setContrast(255);         // Maximum contrast.
  u8g2.setBusClock(400000);       // 400 kHz I2C clock.
  u8g2.setFont(u8g2_font_6x10_tf);
  // Rotate text 180° by setting font direction to 2.
  u8g2.setFontDirection(2);
  Serial.println("OLED initialized (text rotated 180°)");

  // Configure the button with internal pull-up.
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.print("Button set on pin: ");
  Serial.println(BUTTON_PIN);

  // Configure the LED as output.
  pinMode(LED_PIN, OUTPUT);
  Serial.print("LED set on pin: ");
  Serial.println(LED_PIN);

  // Seed the random number generator.
  randomSeed(analogRead(0));

  // Pick an initial random pair and show the city prompt.
  currentIndex = random(0, numPairs);
  state = 0; // Start by showing the city prompt.
  displayCity();
}

void loop() {
  // Check if the button is pressed (active LOW with INPUT_PULLUP).
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);  // debounce delay
    if (digitalRead(BUTTON_PIN) == LOW) {  // confirm still pressed
      // Flash the LED.
      digitalWrite(LED_PIN, HIGH);
      delay(200);
      digitalWrite(LED_PIN, LOW);

      // Cycle states: if state 0 (city prompt) then display country; else, pick a new random pair.
      if (state == 0) {
        state = 1;
        displayCountry();
      } else {
        state = 0;
        currentIndex = random(0, numPairs);
        displayCity();
      }
      
      // Wait until the button is released.
      while (digitalRead(BUTTON_PIN) == LOW) {
        delay(10);
      }
      Serial.println("Button released.");
    }
  }
  delay(50);
}

void displayCity() {
  // Display the city prompt:
  // First line: "is in?"  
  // Second line: the city name.
  u8g2.clearBuffer();
  u8g2.drawFrame(xOffset, yOffset, width, height);
  // For font direction 2, the cursor sets the upper-right corner.
  // So position near the right edge of the frame.
  u8g2.setCursor(xOffset + width - 5, yOffset + 20);
  u8g2.print(pairs[currentIndex].city);
  u8g2.setCursor(xOffset + width - 5, yOffset + 35);
  u8g2.print("is in?");
  u8g2.sendBuffer();
  Serial.print("Displaying city: ");
  Serial.println(pairs[currentIndex].city);
}

void displayCountry() {
  // Display the country centered in the frame.
  u8g2.clearBuffer();
  u8g2.drawFrame(xOffset, yOffset, width, height);
  u8g2.setCursor(xOffset + width - 5, yOffset + 25);
  u8g2.print(pairs[currentIndex].country);
  u8g2.sendBuffer();
  Serial.print("Displaying country: ");
  Serial.println(pairs[currentIndex].country);
}
