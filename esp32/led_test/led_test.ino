// LED strip test – cycles the first pixels of a WS2811 strip through red, green and blue.
// Requires the "Adafruit NeoPixel" library.

#include <Adafruit_NeoPixel.h>

const int LED_PIN = 13;           // GPIO connected to Din (through a 330 Ω resistor)
const int NUM_PIXELS = 9;         // WS2811 12 V strips drive 3 LEDs per pixel, so 3 pixels = 9 LEDs
const uint8_t BRIGHTNESS = 64;    // 0–255; kept low to limit current draw
const unsigned long STEP_MS = 1000;

// If the colors come out in the wrong order, try NEO_GRB, NEO_BRG or NEO_RBG instead of NEO_RGB.
Adafruit_NeoPixel strip(NUM_PIXELS, LED_PIN, NEO_RGB + NEO_KHZ800);

struct Step {
  const char* name;
  uint8_t r, g, b;
};

const Step STEPS[] = {
  {"RED", 255, 0, 0},
  {"GREEN", 0, 255, 0},
  {"BLUE", 0, 0, 255},
};

void setup() {
  Serial.begin(115200);
  strip.begin();
  strip.setBrightness(BRIGHTNESS);
  strip.clear();
  strip.show();
}

void loop() {
  for (const Step& step : STEPS) {
    Serial.printf("Now: %s\n", step.name);
    strip.fill(strip.Color(step.r, step.g, step.b));
    strip.show();
    delay(STEP_MS);
  }
}
