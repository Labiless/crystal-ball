// Crystal Ball – prints a random fortune on a BLE ESC/POS thermal printer when the button is pressed,
// and forwards print jobs from the webapp (esp32.html) to the same printer.
// Requires the "NimBLE-Arduino" (version 2.x), "Adafruit GFX Library" and "Adafruit NeoPixel" libraries.

#include <NimBLEDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <Fonts/FreeSerifBold18pt7b.h>
#include <vector>
#include <atomic>
#include "decoration.h"

// Part of the printer's Bluetooth name (case-sensitive). Leave empty to only list nearby devices on the Serial Monitor.
const char* PRINTER_NAME = "MPT-II";

// Bluetooth name the webapp sees. The UUIDs must match the ones in esp32.html.
const char* BRIDGE_NAME = "Crystal Ball";
const char* BRIDGE_SERVICE_UUID = "6e7c0001-8f1a-4c7e-9b1d-2a9c8b7f5e01";
const char* JOB_CHAR_UUID       = "6e7c0002-8f1a-4c7e-9b1d-2a9c8b7f5e01";
const char* STATUS_CHAR_UUID    = "6e7c0003-8f1a-4c7e-9b1d-2a9c8b7f5e01";
const size_t MAX_JOB_SIZE = 100 * 1024;

const int BUTTON_PIN = 12;
const unsigned long LONG_PRESS_MS = 2000;      // hold the button this long to print
const unsigned long ANIMATION_RAMP_MS = 7000;   // slow-to-fast build-up; the button waits this long before printing
const unsigned long ANIMATION_FADE_IN_MS = 400;
const unsigned long ANIMATION_FADE_OUT_MS = 2000;

const int STRIP_PIN = 13;              // WS2811 strip Din (through a 330 Ω resistor)
const int STRIP_PIXELS = 9;            // each WS2811 pixel drives 3 LEDs
const int PIXELS_PER_TURN = 9;         // pixels in one full wrap around the cylinder
const int ROTATION_DIRECTION = 1;      // 1 or -1 to reverse the spin
const uint8_t STRIP_BRIGHTNESS = 200;   // 0–255
const int PAPER_WIDTH = 384;     // 58 mm paper at 203 dpi

#ifndef RGB_BUILTIN
const int LED_PIN = 2;           // single-color onboard LED on classic ESP32 DevKit boards
#endif

const char* PHRASES[] = {
  "Vedo nel tuo futuro un grande viaggio.",
  "Qualcuno pensa a te più di quanto immagini.",
  "Una sorpresa inaspettata busserà alla tua porta.",
  "Le stelle dicono: stasera non bere l'ultimo drink.",
  "Il tuo prossimo errore sarà il tuo più grande successo.",
  "Una persona con un costume strano cambierà la tua serata.",
  "La fortuna ti sorride, ma solo dopo mezzanotte.",
  "Presto riceverai un messaggio che aspettavi da tempo.",
  "Diffida di chi ti offre caramelle stanotte.",
  "Il destino ha grandi piani per te. Ma prima, balla.",
};
const int PHRASE_COUNT = sizeof(PHRASES) / sizeof(PHRASES[0]);

NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic* printerChar = nullptr;
int lastPhrase = -1;

// ---------- Bluetooth ----------

bool connectToPrinter() {
  Serial.println("Scanning for 5 seconds...");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(5000);

  const NimBLEAdvertisedDevice* target = nullptr;
  for (int i = 0; i < results.getCount(); i++) {
    const NimBLEAdvertisedDevice* dev = results.getDevice(i);
    std::string name = dev->getName();
    if (name.empty()) continue;
    Serial.printf("  found: \"%s\" (%s)\n", name.c_str(), dev->getAddress().toString().c_str());
    if (strlen(PRINTER_NAME) > 0 && name.find(PRINTER_NAME) != std::string::npos) target = dev;
  }
  if (!target) {
    Serial.println("Printer not found.");
    return false;
  }

  if (!client) client = NimBLEDevice::createClient();
  Serial.printf("Connecting to %s...\n", target->getName().c_str());
  if (!client->connect(target)) {
    Serial.println("Connection failed.");
    return false;
  }

  printerChar = nullptr;
  for (NimBLERemoteService* service : client->getServices(true)) {
    for (NimBLERemoteCharacteristic* chr : service->getCharacteristics(true)) {
      if (chr->canWriteNoResponse() || chr->canWrite()) {
        printerChar = chr;
        Serial.printf("Using service %s, characteristic %s\n",
                      service->getUUID().toString().c_str(), chr->getUUID().toString().c_str());
        break;
      }
    }
    if (printerChar) break;
  }
  if (!printerChar) {
    Serial.println("No writable characteristic found.");
    client->disconnect();
    return false;
  }
  Serial.printf("Printer connected! (MTU %d)\n", client->getMTU());
  return true;
}

bool isConnected() {
  return client && client->isConnected() && printerChar;
}

bool sendBytes(const uint8_t* data, size_t len) {
  bool withResponse = !printerChar->canWriteNoResponse();
  size_t chunk = min((size_t)client->getMTU() - 3, (size_t)180);
  for (size_t i = 0; i < len; i += chunk) {
    size_t n = min(chunk, len - i);
    int attempts = 0;
    while (!printerChar->writeValue(data + i, n, withResponse)) {
      if (++attempts > 10 || !isConnected()) {
        Serial.println("Write failed, print aborted.");
        return false;
      }
      delay(50);
    }
    if (!withResponse) delay(20);
  }
  return true;
}

// ---------- Printing ----------

// The font only has plain ASCII characters, so Italian accents become "e'" etc.
String toPrinterText(String s) {
  const char* from[] = {"à", "è", "é", "ì", "ò", "ù", "À", "È", "É", "Ì", "Ò", "Ù", "’", "‘", "“", "”", "…"};
  const char* to[]   = {"a'", "e'", "e'", "i'", "o'", "u'", "A'", "E'", "E'", "I'", "O'", "U'", "'", "'", "\"", "\"", "..."};
  for (int i = 0; i < 17; i++) s.replace(from[i], to[i]);
  return s;
}

int textWidth(GFXcanvas1& canvas, const String& text, int16_t* offsetX = nullptr) {
  int16_t x1, y1;
  uint16_t w, h;
  canvas.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  if (offsetX) *offsetX = x1;
  return w;
}

std::vector<String> wrapText(GFXcanvas1& canvas, const String& text, int maxWidth) {
  std::vector<String> lines;
  String line;
  int start = 0;
  while (start <= (int)text.length()) {
    int end = text.indexOf(' ', start);
    if (end < 0) end = text.length();
    String word = text.substring(start, end);
    String test = line.length() ? line + " " + word : word;
    if (line.length() && textWidth(canvas, test) > maxWidth) {
      lines.push_back(line);
      line = word;
    } else {
      line = test;
    }
    start = end + 1;
  }
  lines.push_back(line);
  return lines;
}

uint8_t reverseBits(uint8_t b) {
  b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
  b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
  return (b & 0xAA) >> 1 | (b & 0x55) << 1;
}

// Sends the canvas as ESC/POS "GS v 0" raster bands, rotated 180° so the receipt reads correctly
// from the side it comes out of. Same commands the webapp uses.
void printCanvas(GFXcanvas1& canvas) {
  const int bytesPerRow = PAPER_WIDTH / 8;
  const int height = canvas.height();
  const uint8_t* pixels = canvas.getBuffer();
  const int BAND = 128;
  static uint8_t band[8 + (PAPER_WIDTH / 8) * BAND];

  const uint8_t init[] = {0x1B, 0x40};        // ESC @   reset
  const uint8_t feed[] = {0x1B, 0x64, 0x04};  // ESC d 4 feed 4 lines
  if (!sendBytes(init, sizeof(init))) return;

  for (int y0 = 0; y0 < height; y0 += BAND) {
    int rows = min(BAND, height - y0);
    const uint8_t header[] = {0x1D, 0x76, 0x30, 0x00,
                              (uint8_t)(bytesPerRow & 0xFF), (uint8_t)(bytesPerRow >> 8),
                              (uint8_t)(rows & 0xFF), (uint8_t)(rows >> 8)};
    memcpy(band, header, sizeof(header));
    for (int y = 0; y < rows; y++) {
      const uint8_t* src = pixels + (height - 1 - (y0 + y)) * bytesPerRow;
      uint8_t* dst = band + 8 + y * bytesPerRow;
      for (int b = 0; b < bytesPerRow; b++) dst[b] = reverseBits(src[bytesPerRow - 1 - b]);
    }
    if (!sendBytes(band, 8 + rows * bytesPerRow)) return;
  }
  sendBytes(feed, sizeof(feed));
}

void printFortune() {
  int i;
  do { i = random(PHRASE_COUNT); } while (i == lastPhrase && PHRASE_COUNT > 1);
  lastPhrase = i;
  Serial.printf("Printing: %s\n", PHRASES[i]);

  const int padding = 8;
  const int gap = 8;
  const int lineHeight = FreeSerifBold18pt7b.yAdvance;

  GFXcanvas1 measure(1, 1);
  measure.setFont(&FreeSerifBold18pt7b);
  measure.setTextWrap(false);
  std::vector<String> lines = wrapText(measure, toPrinterText(PHRASES[i]), PAPER_WIDTH - padding * 2);

  int16_t x1, y1;
  uint16_t w, h;
  measure.getTextBounds("Ay", 0, 0, &x1, &y1, &w, &h);
  const int ascent = -y1;
  const int glyphHeight = h;

  const int textTop = DECORATION_HEIGHT + gap;
  const int height = textTop + lines.size() * lineHeight + padding * 2 + gap + DECORATION_HEIGHT;
  GFXcanvas1 canvas(PAPER_WIDTH, height);
  if (!canvas.getBuffer()) {
    Serial.println("Not enough memory for the print image.");
    return;
  }
  canvas.fillScreen(0);
  canvas.setFont(&FreeSerifBold18pt7b);
  canvas.setTextWrap(false);
  canvas.setTextColor(1);

  canvas.drawBitmap(0, 0, DECORATION, DECORATION_WIDTH, DECORATION_HEIGHT, 1);
  const int decoBytesPerRow = (DECORATION_WIDTH + 7) / 8;
  for (int y = 0; y < DECORATION_HEIGHT; y++) {
    canvas.drawBitmap(0, height - 1 - y, DECORATION + y * decoBytesPerRow, DECORATION_WIDTH, 1, 1);
  }

  for (size_t l = 0; l < lines.size(); l++) {
    int16_t offsetX;
    int width = textWidth(canvas, lines[l], &offsetX);
    int baseline = textTop + padding + l * lineHeight + (lineHeight - glyphHeight) / 2 + ascent;
    canvas.setCursor((PAPER_WIDTH - width) / 2 - offsetX, baseline);
    canvas.print(lines[l]);
  }

  printCanvas(canvas);
}

// ---------- LED strip ----------
// The animation runs in its own task so it keeps moving while the main loop is blocked printing.

Adafruit_NeoPixel strip(STRIP_PIXELS, STRIP_PIN, NEO_RGB + NEO_KHZ800);
std::atomic<bool> buttonAnimation{false};
std::atomic<bool> jobAnimation{false};

// A yellow comet with a fading tail spinning around the cylinder over a dim purple glow. It starts slow,
// speeds up over ANIMATION_RAMP_MS, keeps its top speed while printing, and fades out once the print is over.
void stripTask(void*) {
  const float PURPLE[] = {140, 0, 255};
  const float YELLOW[] = {255, 170, 0};
  const float BACKGROUND = 0.25f;  // purple glow level away from the comet
  const float MIN_SPEED = 0.2f;    // turns per second
  const float MAX_SPEED = 2.5f;

  float position = 0, speed = MIN_SPEED, level = 0;
  bool wasActive = false, lit = false;
  unsigned long startedAt = 0, last = millis();

  for (;;) {
    unsigned long now = millis();
    float dt = (now - last) / 1000.0f;
    last = now;

    bool active = buttonAnimation || jobAnimation;
    if (active && !wasActive) startedAt = now;
    wasActive = active;

    if (active) {
      float progress = min(1.0f, (now - startedAt) / (float)ANIMATION_RAMP_MS);
      speed = MIN_SPEED + (MAX_SPEED - MIN_SPEED) * progress * progress;
      level = min(1.0f, level + dt * 1000.0f / ANIMATION_FADE_IN_MS);
    } else {
      level = max(0.0f, level - dt * 1000.0f / ANIMATION_FADE_OUT_MS);
    }

    if (level > 0 || lit) {
      position = fmodf(position + speed * dt, 1.0f);
      for (int i = 0; i < STRIP_PIXELS; i++) {
        float angle = (float)(i % PIXELS_PER_TURN) / PIXELS_PER_TURN;
        if (ROTATION_DIRECTION < 0) angle = 1.0f - angle;
        float behind = fmodf(position - angle + 1.0f, 1.0f);   // 0 = at the comet head, near 1 = just ahead of it
        float tail = powf(1.0f - behind, 3.0f);
        float ahead = max(0.0f, 1.0f - (1.0f - behind) * PIXELS_PER_TURN);  // smooths the head between pixels
        float comet = max(tail, ahead);
        uint8_t c[3];
        for (int k = 0; k < 3; k++) {
          float purple = PURPLE[k] * BACKGROUND * (1.0f - comet);
          c[k] = (purple + YELLOW[k] * comet) * level;
        }
        strip.setPixelColor(i, c[0], c[1], c[2]);
      }
      strip.show();
      lit = level > 0;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void startStrip() {
  strip.begin();
  strip.setBrightness(STRIP_BRIGHTNESS);
  strip.clear();
  strip.show();
  xTaskCreatePinnedToCore(stripTask, "strip", 4096, nullptr, 1, nullptr, 1);
}

// ---------- Webapp bridge ----------
// The webapp sends a complete ESC/POS job on the job characteristic:
//   [0x01, size as uint32 little-endian]  starts a job
//   [0x02, data...]                       appends data; the job is printed once `size` bytes arrived
// The status characteristic notifies: "ready", "offline", "printing", "done", "busy", "error".

NimBLECharacteristic* statusChar = nullptr;
uint8_t* jobBuffer = nullptr;
size_t jobSize = 0;
size_t jobReceived = 0;
std::atomic<bool> jobReady{false};

void notifyStatus(const char* status) {
  Serial.printf("Bridge status: %s\n", status);
  statusChar->setValue(status);
  statusChar->notify();
}

void discardJob() {
  free(jobBuffer);
  jobBuffer = nullptr;
  jobSize = jobReceived = 0;
}

class JobCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override {
    const NimBLEAttValue& value = chr->getValue();
    const uint8_t* data = value.data();
    size_t len = value.length();
    if (len == 0) return;
    if (jobReady) {
      notifyStatus("busy");
      return;
    }
    if (data[0] == 0x01 && len >= 5) {
      uint32_t size = data[1] | data[2] << 8 | data[3] << 16 | (uint32_t)data[4] << 24;
      discardJob();
      if (size == 0 || size > MAX_JOB_SIZE || !(jobBuffer = (uint8_t*)malloc(size))) {
        notifyStatus("error");
        return;
      }
      jobSize = size;
      jobAnimation = true;
    } else if (data[0] == 0x02 && jobBuffer) {
      size_t n = min(len - 1, jobSize - jobReceived);
      memcpy(jobBuffer + jobReceived, data + 1, n);
      jobReceived += n;
      if (jobReceived == jobSize) jobReady = true;
    }
  }
};

class BridgeServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    Serial.println("Webapp connected.");
  }
  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    Serial.println("Webapp disconnected.");
    if (!jobReady) {
      discardJob();
      jobAnimation = false;
    }
  }
};

void startBridge() {
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new BridgeServerCallbacks());
  server->advertiseOnDisconnect(true);

  NimBLEService* service = server->createService(BRIDGE_SERVICE_UUID);
  NimBLECharacteristic* jobChar = service->createCharacteristic(
      JOB_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  jobChar->setCallbacks(new JobCallbacks());
  statusChar = service->createCharacteristic(STATUS_CHAR_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  statusChar->setValue("offline");
  server->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->enableScanResponse(true);
  adv->setName(BRIDGE_NAME);
  adv->addServiceUUID(BRIDGE_SERVICE_UUID);
  adv->start();
  Serial.printf("Advertising as \"%s\" for the webapp.\n", BRIDGE_NAME);
}

void printPendingJob() {
  if (!jobReady) return;
  if (isConnected()) {
    notifyStatus("printing");
    notifyStatus(sendBytes(jobBuffer, jobSize) ? "done" : "error");
  } else {
    notifyStatus("error");
  }
  discardJob();
  jobReady = false;
  jobAnimation = false;
}

// ---------- LED ----------

// Boards with an onboard RGB LED (ESP32-S3/C3/C6) cycle through colors; classic ESP32 boards toggle the LED.
void nextLedColor() {
  static int step = 0;
  step++;
#ifdef RGB_BUILTIN
  const uint8_t colors[][3] = {{64, 0, 64}, {0, 0, 64}, {0, 64, 0}, {64, 32, 0}, {64, 0, 0}};
  const uint8_t* c = colors[step % 5];
  rgbLedWrite(RGB_BUILTIN, c[0], c[1], c[2]);
#else
  digitalWrite(LED_PIN, step % 2);
#endif
}

// ---------- Main ----------

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#ifndef RGB_BUILTIN
  pinMode(LED_PIN, OUTPUT);
#endif
  NimBLEDevice::init(BRIDGE_NAME);
  NimBLEDevice::setMTU(517);
  startStrip();
  startBridge();
  connectToPrinter();
}

void loop() {
  static unsigned long lastRetry = 0;
  static bool lastPressed = false;
  static unsigned long lastChange = 0;
  static unsigned long pressedSince = 0;
  static bool longPressHandled = false;
  static int lastPrinterState = -1;

  int printerState = isConnected();
  if (printerState != lastPrinterState) {
    lastPrinterState = printerState;
    notifyStatus(printerState ? "ready" : "offline");
  }

  printPendingJob();

  if (!isConnected() && millis() - lastRetry > 10000) {
    lastRetry = millis();
    connectToPrinter();
  }

  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  if (pressed != lastPressed && millis() - lastChange > 50) {
    lastChange = millis();
    lastPressed = pressed;
    if (pressed) {
      Serial.println("Button pressed, hold to print...");
      nextLedColor();
      pressedSince = millis();
      longPressHandled = false;
    }
  }

  if (lastPressed && !longPressHandled && millis() - pressedSince >= LONG_PRESS_MS) {
    longPressHandled = true;
    if (isConnected()) {
      buttonAnimation = true;
      delay(ANIMATION_RAMP_MS);
      printFortune();
      buttonAnimation = false;
    } else {
      Serial.println("Long press, but the printer is not connected.");
    }
  }
}
