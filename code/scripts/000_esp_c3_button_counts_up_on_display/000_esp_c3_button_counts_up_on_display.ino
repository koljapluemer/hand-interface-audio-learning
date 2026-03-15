#include <U8g2lib.h>
#include <Wire.h>

// The 72x40 screen is mapped in the middle of the 132x64 pixel buffer.
// OLED is connected via I2C with SDA on pin 5 and SCL on pin 6.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 6, 5);

int width = 72;
int height = 40;
int xOffset = 30; // (132 - 72) / 2
int yOffset = 12; // (64 - 40) / 2

// Define the button pin (for touch) and LED pin.
#define TOUCH_PIN 4  // Button wired to GPIO4 and GND.
#define LED_PIN   8  // LED connected to GPIO8.

unsigned int counter = 0;

void setup(void)
{
  // Start Serial for debugging.
  Serial.begin(115200);
  delay(1000);
  Serial.println("Starting ESP32-C3 OLED test with button & LED...");

  // Initialize display.
  u8g2.begin();
  u8g2.setContrast(255);         // maximum contrast
  u8g2.setBusClock(400000);       // 400 kHz I2C clock
  u8g2.setFont(u8g2_font_ncenB10_tr);
  Serial.println("OLED initialized");

  // Configure button pin with internal pull-up.
  pinMode(TOUCH_PIN, INPUT_PULLUP);
  Serial.print("Button (TOUCH_PIN) configured on pin: ");
  Serial.println(TOUCH_PIN);

  // Configure LED pin as output.
  pinMode(LED_PIN, OUTPUT);
  Serial.print("LED configured on pin: ");
  Serial.println(LED_PIN);
}

void loop(void)
{
  // Check the button state (active LOW when pressed).
  int buttonState = digitalRead(TOUCH_PIN);
  Serial.print("Button state: ");
  Serial.println(buttonState);

  if (buttonState == LOW) {
    delay(50);  // debounce delay
    if (digitalRead(TOUCH_PIN) == LOW) {  // confirm still pressed
      counter++;  // increment counter
      Serial.print("Button pressed! Counter incremented to: ");
      Serial.println(counter);

      // Flash the LED.
      Serial.println("Flashing LED...");
      digitalWrite(LED_PIN, HIGH);
      delay(200);
      digitalWrite(LED_PIN, LOW);

      // Wait until the button is released to avoid multiple counts per press.
      while (digitalRead(TOUCH_PIN) == LOW) {
        delay(10);
      }
      Serial.println("Button released.");
    }
  }

  // Update the display.
  u8g2.clearBuffer();                              // clear internal memory
  u8g2.drawFrame(xOffset, yOffset, width, height);   // draw a frame
  u8g2.setCursor(xOffset + 5, yOffset + 25);
  u8g2.printf("Count: %u", counter);               // display the counter value
  u8g2.sendBuffer();                               // transfer internal memory to the display

  // Short delay for stability.
  delay(50);
}
