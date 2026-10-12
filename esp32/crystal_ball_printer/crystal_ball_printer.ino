// Crystal Ball – prints a random fortune on a BLE ESC/POS thermal printer when the button is pressed.
// Requires the "NimBLE-Arduino" library (version 2.x).

#include <NimBLEDevice.h>

// Part of the printer's Bluetooth name (case-sensitive). Leave empty to only list nearby devices on the Serial Monitor.
const char* PRINTER_NAME = "MPT-II";

const int BUTTON_PIN = 4;
const int CHARS_PER_LINE = 32;   // 58 mm paper

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
  Serial.println("Printer connected!");
  return true;
}

bool isConnected() {
  return client && client->isConnected() && printerChar;
}

void sendBytes(const uint8_t* data, size_t len) {
  bool withResponse = !printerChar->canWriteNoResponse();
  size_t chunk = client->getMTU() - 3;
  for (size_t i = 0; i < len; i += chunk) {
    size_t n = min(chunk, len - i);
    printerChar->writeValue(data + i, n, withResponse);
    if (!withResponse) delay(20);
  }
}

void sendText(const String& text) {
  sendBytes((const uint8_t*)text.c_str(), text.length());
}

// ---------- Printing ----------

// Most cheap printers lack a reliable code page for Italian accents, so they become "e'" etc.
String toPrinterText(String s) {
  const char* from[] = {"à", "è", "é", "ì", "ò", "ù", "À", "È", "É", "Ì", "Ò", "Ù", "’", "‘", "“", "”", "…"};
  const char* to[]   = {"a'", "e'", "e'", "i'", "o'", "u'", "A'", "E'", "E'", "I'", "O'", "U'", "'", "'", "\"", "\"", "..."};
  for (int i = 0; i < 17; i++) s.replace(from[i], to[i]);
  return s;
}

String wordWrap(const String& text, int width) {
  String out, line;
  int start = 0;
  while (start <= (int)text.length()) {
    int end = text.indexOf(' ', start);
    if (end < 0) end = text.length();
    String word = text.substring(start, end);
    if (line.length() > 0 && line.length() + 1 + word.length() > (unsigned)width) {
      out += line + "\n";
      line = word;
    } else {
      line += (line.length() ? " " : "") + word;
    }
    start = end + 1;
  }
  return out + line + "\n";
}

void printFortune() {
  int i;
  do { i = random(PHRASE_COUNT); } while (i == lastPhrase && PHRASE_COUNT > 1);
  lastPhrase = i;
  Serial.printf("Printing: %s\n", PHRASES[i]);

  const uint8_t init[]       = {0x1B, 0x40};        // ESC @   reset
  const uint8_t center[]     = {0x1B, 0x61, 0x01};  // ESC a 1 center
  const uint8_t upsideDown[] = {0x1B, 0x7B, 0x01};  // ESC { 1 rotate characters 180°
  const uint8_t bigText[]    = {0x1D, 0x21, 0x11};  // GS ! 0x11 double width + height
  const uint8_t normalText[] = {0x1D, 0x21, 0x00};
  const uint8_t feed[]       = {0x1B, 0x64, 0x04};  // ESC d 4 feed 4 lines

  sendBytes(init, sizeof(init));
  sendBytes(center, sizeof(center));
  sendBytes(upsideDown, sizeof(upsideDown));

  // Upside-down mode only rotates each line, so lines are sent bottom-to-top.
  String body = "\n" + wordWrap(toPrinterText(PHRASES[i]), CHARS_PER_LINE) + "\n";
  sendText("* * *\n");
  int end = body.length();
  while (end > 0) {
    int start = end >= 2 ? body.lastIndexOf('\n', end - 2) + 1 : 0;
    sendText(body.substring(start, end));
    end = start;
  }
  sendText("* * *\n");
  sendBytes(bigText, sizeof(bigText));
  sendText("LA VEGGENTE\n");
  sendBytes(normalText, sizeof(normalText));
  sendBytes(feed, sizeof(feed));
}

// ---------- Main ----------

void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  NimBLEDevice::init("");
  connectToPrinter();
}

void loop() {
  static unsigned long lastRetry = 0;
  static bool lastPressed = false;
  static unsigned long lastChange = 0;

  if (!isConnected() && millis() - lastRetry > 10000) {
    lastRetry = millis();
    connectToPrinter();
  }

  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  if (pressed != lastPressed && millis() - lastChange > 50) {
    lastChange = millis();
    lastPressed = pressed;
    if (pressed) {
      if (isConnected()) printFortune();
      else Serial.println("Button pressed, but the printer is not connected.");
    }
  }
}
