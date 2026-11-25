// MIDI Button: one button on an RP2350 that sends MIDI messages over USB.
//
// The device is a class-compliant USB MIDI port named "MIDI Button". It is configured
// over the same port with SysEx, so the web configurator (web/index.html) only needs
// Web MIDI. The protocol is described in the README.
//
// Wiring: a momentary switch between GP9 and GP13, either way round. GP9 is driven low
// and acts as the return, GP13 reads the switch through the internal pull-up, so no GND
// wire is needed. To wire to GND instead, set BUTTON_RETURN_PIN to -1.
// The board's BOOT button also works as the button, so you can test with no wiring.

#include <Arduino.h>
#include <MIDIUSB.h>
#include <USB.h>
#include <EEPROM.h>
#include <pico/unique_id.h>
#if defined(PIN_NEOPIXEL)
#include <Adafruit_NeoPixel.h>
#endif

// ---------------------------------------------------------------- settings

static const int BUTTON_PIN = 13;         // switch input (pull-up)
static const int BUTTON_RETURN_PIN = 9;   // other side of the switch, held low; -1 = wired to GND
#define USE_BOOTSEL_AS_BUTTON 1           // BOOT button acts as the button too
static const uint8_t PRESS_SAMPLES = 2;   // 1 ms samples that must read closed to fire
static const uint8_t RELEASE_MS = 15;     // open this long before a release counts
static const uint32_t LOCKOUT_MS = 30;    // ignore the switch this long after any change
static const char *DEVICE_NAME = "MIDI Button";

#define FW_MAJOR 1
#define FW_MINOR 1
#define FW_PATCH 0
#define PROTOCOL_VERSION 2

// ---------------------------------------------------------------- config

enum : uint8_t { TYPE_NOTE = 0, TYPE_CC = 1, TYPE_PC = 2, TYPE_COUNT };
enum : uint8_t { MODE_MOMENTARY = 0, MODE_TOGGLE = 1, MODE_COUNT };

static const uint16_t HOLD_MIN_MS = 100;  // shortest long-press hold
static const uint16_t HOLD_MAX_MS = 5000; // longest long-press hold

struct Config {
  uint8_t type;        // TYPE_*
  uint8_t channel;     // 0-15 (shown to users as 1-16)
  uint8_t number;      // note, CC number or program, 0-127
  uint8_t onValue;     // note velocity or CC value sent on press, 0-127
  uint8_t offValue;    // CC value sent on release (or second toggle press), 0-127
  uint8_t mode;        // MODE_*
  uint8_t longOn;      // 1 = holding past holdMs plays a second note
  uint8_t longNote;    // that note, 0-127 (same channel)
  uint8_t longVelocity;// its velocity, 1-127
  uint16_t holdMs;     // how long to hold before the long press fires
};

// Config as it travels over SysEx and sits in flash: holdMs is split into two 7-bit bytes.
static const size_t WIRE_BYTES = 11;
static const size_t WIRE_BYTES_V1 = 6;   // firmware 1.0 layout, migrated on load

static const Config DEFAULT_CONFIG = {TYPE_NOTE, 0, 60, 100, 0, MODE_MOMENTARY, 0, 64, 100, 500};

static bool configValid(const Config &c) {
  return c.type < TYPE_COUNT && c.channel < 16 && c.number < 128 && c.onValue < 128 &&
         c.offValue < 128 && c.mode < MODE_COUNT && c.longOn < 2 && c.longNote < 128 &&
         c.longVelocity >= 1 && c.longVelocity < 128 &&
         c.holdMs >= HOLD_MIN_MS && c.holdMs <= HOLD_MAX_MS;
}

static void encodeConfig(const Config &c, uint8_t *o) {
  o[0] = c.type;
  o[1] = c.channel;
  o[2] = c.number;
  o[3] = c.onValue;
  o[4] = c.offValue;
  o[5] = c.mode;
  o[6] = c.longOn;
  o[7] = c.longNote;
  o[8] = c.longVelocity;
  o[9] = c.holdMs & 0x7F;
  o[10] = (c.holdMs >> 7) & 0x7F;
}

// Returns false (and leaves the caller to reject it) when the values are out of range.
static bool decodeConfig(const uint8_t *i, Config &c) {
  c.type = i[0];
  c.channel = i[1];
  c.number = i[2];
  c.onValue = i[3];
  c.offValue = i[4];
  c.mode = i[5];
  c.longOn = i[6];
  c.longNote = i[7];
  c.longVelocity = i[8];
  c.holdMs = (uint16_t)(i[9] | (i[10] << 7));
  return configValid(c);
}

static uint8_t checksum(const uint8_t *p, size_t n) {
  uint8_t sum = 0xA5;
  for (size_t i = 0; i < n; i++) sum = (uint8_t)((sum << 1 | sum >> 7) ^ p[i]);
  return sum;
}

static bool sameConfig(const Config &a, const Config &b) {
  uint8_t wa[WIRE_BYTES], wb[WIRE_BYTES];
  encodeConfig(a, wa);
  encodeConfig(b, wb);
  return memcmp(wa, wb, WIRE_BYTES) == 0;
}

static Config cfg;    // live config
static Config saved;  // what is in flash

// Flash layout: magic (4, little-endian) | version (1) | config (11) | checksum (1)
static const uint32_t STORE_MAGIC = 0x4D424331;  // "MBC1"
static const uint8_t STORE_VERSION = 2;
static const uint8_t STORE_VERSION_1 = 1;        // magic | version | 6 bytes | checksum
static const size_t STORE_BYTES = 5 + WIRE_BYTES + 1;

static void loadConfig() {
  uint8_t b[STORE_BYTES];
  for (size_t i = 0; i < STORE_BYTES; i++) b[i] = EEPROM.read(i);
  uint32_t magic = b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);

  cfg = DEFAULT_CONFIG;
  if (magic == STORE_MAGIC && b[4] == STORE_VERSION &&
      checksum(b + 5, WIRE_BYTES) == b[5 + WIRE_BYTES]) {
    Config c;
    if (decodeConfig(b + 5, c)) cfg = c;
  } else if (magic == STORE_MAGIC && b[4] == STORE_VERSION_1 &&
             checksum(b + 5, WIRE_BYTES_V1) == b[5 + WIRE_BYTES_V1]) {
    // Firmware 1.0 saved only the six basic fields; the long press takes its defaults.
    uint8_t w[WIRE_BYTES];
    encodeConfig(DEFAULT_CONFIG, w);
    memcpy(w, b + 5, WIRE_BYTES_V1);
    Config c;
    if (decodeConfig(w, c)) cfg = c;
  }
  saved = cfg;
}

static bool saveConfig() {
  uint8_t b[STORE_BYTES];
  b[0] = STORE_MAGIC & 0xFF;
  b[1] = (STORE_MAGIC >> 8) & 0xFF;
  b[2] = (STORE_MAGIC >> 16) & 0xFF;
  b[3] = (STORE_MAGIC >> 24) & 0xFF;
  b[4] = STORE_VERSION;
  encodeConfig(cfg, b + 5);
  b[5 + WIRE_BYTES] = checksum(b + 5, WIRE_BYTES);
  for (size_t i = 0; i < STORE_BYTES; i++) EEPROM.write(i, b[i]);
  bool ok = EEPROM.commit();
  if (ok) saved = cfg;
  return ok;
}

// ---------------------------------------------------------------- LED

#if defined(PIN_NEOPIXEL)
static Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
#endif
static uint32_t flashUntil = 0;
static bool flashWhite = false;

static void ledBegin() {
#if defined(PIN_NEOPIXEL)
  pixel.begin();
  pixel.show();
#elif defined(LED_BUILTIN)
  pinMode(LED_BUILTIN, OUTPUT);
#endif
}

static void ledFlash(uint32_t ms, bool white = false) {
  flashUntil = millis() + ms;
  flashWhite = white;
}

// Lit while a message is in effect (held, latched in toggle mode, or the long-press note
// sounding), flashes on a program change or save, dim otherwise. On a NeoPixel the colour
// shows the message type.
static void ledUpdate(bool active) {
  bool flashing = (int32_t)(flashUntil - millis()) > 0;
#if defined(PIN_NEOPIXEL)
  static const uint8_t colours[TYPE_COUNT][3] = {{0, 90, 255}, {0, 255, 60}, {255, 140, 0}};
  const uint8_t *c = colours[cfg.type];
  uint8_t r, g, b;
  if (flashing && flashWhite) {
    r = g = b = 120;
  } else if (active || flashing) {
    r = c[0] / 2; g = c[1] / 2; b = c[2] / 2;
  } else {
    r = c[0] / 40; g = c[1] / 40; b = c[2] / 40;
  }
  static uint32_t last = 0xFFFFFFFF;
  uint32_t packed = pixel.Color(r, g, b);
  if (packed != last) {
    last = packed;
    pixel.setPixelColor(0, packed);
    pixel.show();
  }
#elif defined(LED_BUILTIN)
  digitalWrite(LED_BUILTIN, (active || flashing) ? HIGH : LOW);
#endif
}

// ---------------------------------------------------------------- MIDI out

static void sendPacket(uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3) {
  uint8_t p[4] = {cin, b1, b2, b3};  // cable 0
  uint32_t start = millis();
  while (!MidiUSB.writePacket(p)) {
    if (!MidiUSB.connected() || millis() - start > 20) return;  // host not reading
    delay(1);
  }
}

static bool active = false;      // the short-press message has been sent and not yet undone
static bool longActive = false;  // the long-press note is sounding
static bool pendingShort = false;  // pressed, and not yet known to be short or long
static uint32_t pressedAt = 0;

static void sendOn() {
  uint8_t ch = cfg.channel & 0x0F;
  switch (cfg.type) {
    case TYPE_NOTE: sendPacket(0x09, 0x90 | ch, cfg.number, cfg.onValue ? cfg.onValue : 1); break;
    case TYPE_CC:   sendPacket(0x0B, 0xB0 | ch, cfg.number, cfg.onValue); break;
    case TYPE_PC:   sendPacket(0x0C, 0xC0 | ch, cfg.number, 0); break;
  }
}

static void sendOff() {
  uint8_t ch = cfg.channel & 0x0F;
  switch (cfg.type) {
    case TYPE_NOTE: sendPacket(0x08, 0x80 | ch, cfg.number, 0); break;
    case TYPE_CC:   sendPacket(0x0B, 0xB0 | ch, cfg.number, cfg.offValue); break;
    case TYPE_PC:   break;
  }
}

static void sendLongOn() {
  sendPacket(0x09, 0x90 | (cfg.channel & 0x0F), cfg.longNote, cfg.longVelocity);
}

static void sendLongOff() {
  sendPacket(0x08, 0x80 | (cfg.channel & 0x0F), cfg.longNote, 0);
}

// The short-press action. Momentary and toggle work as before.
static void onPress() {
  if (cfg.type == TYPE_PC) {
    sendOn();
    ledFlash(120);
  } else if (cfg.mode == MODE_TOGGLE) {
    if (active) sendOff(); else sendOn();
    active = !active;
  } else {
    sendOn();
    active = true;
  }
}

static void onRelease() {
  if (cfg.type != TYPE_PC && cfg.mode == MODE_MOMENTARY && active) {
    sendOff();
    active = false;
  }
}

// With long press on, the short action waits for the release, because only then is it
// known to be short. Without it, the action fires straight away as it always has.
static void buttonDown(uint32_t now) {
  if (cfg.longOn) {
    pendingShort = true;
    pressedAt = now;
  } else {
    onPress();
  }
}

static void buttonUp() {
  if (longActive) {
    sendLongOff();
    longActive = false;
  }
  if (pendingShort) {
    pendingShort = false;
    onPress();
  }
  onRelease();
}

// Undo a held or latched message before the config changes, so no note hangs.
static void releaseActive() {
  if (active) sendOff();
  active = false;
  if (longActive) sendLongOff();
  longActive = false;
  pendingShort = false;
}

// ---------------------------------------------------------------- SysEx config protocol
//
//   F0 7D 4D 42 <cmd> <payload...> F7      (7D = non-commercial ID, 4D 42 = "MB")
//
//   01 HELLO    host: <nonce>             device: 01 <nonce> <proto> <maj> <min> <patch> <16 id nibbles>
//   02 GET      host: -                   device: 02 <config 11 bytes> <dirty>
//   03 SET      host: <config 11 bytes>   device: 02 ...  (applied live, not saved)
//   04 SAVE     host: -                   device: 02 ...  (written to flash)
//   05 DEFAULTS host: -                   device: 02 ...  (applied live, not saved)
//   7F ERROR                              device: 7F <cmd> <code>  1=bad length 2=bad value 3=flash 4=unknown

enum : uint8_t {
  CMD_HELLO = 0x01, CMD_GET = 0x02, CMD_SET = 0x03, CMD_SAVE = 0x04, CMD_DEFAULTS = 0x05,
  CMD_ERROR = 0x7F
};
static const uint8_t SYSEX_ID[3] = {0x7D, 0x4D, 0x42};

static void sendSysex(uint8_t cmd, const uint8_t *payload, size_t n) {
  uint8_t buf[48];
  size_t len = 0;
  buf[len++] = 0xF0;
  for (uint8_t b : SYSEX_ID) buf[len++] = b;
  buf[len++] = cmd;
  for (size_t i = 0; i < n && len < sizeof(buf) - 1; i++) buf[len++] = payload[i] & 0x7F;
  buf[len++] = 0xF7;

  // USB-MIDI framing: CIN 4 = three bytes of an unfinished SysEx, CIN 5/6/7 = it ends
  // after 1/2/3 bytes.
  size_t i = 0;
  while (len - i > 3) {
    sendPacket(0x04, buf[i], buf[i + 1], buf[i + 2]);
    i += 3;
  }
  size_t rest = len - i;
  sendPacket((uint8_t)(0x04 + rest), buf[i], rest > 1 ? buf[i + 1] : 0, rest > 2 ? buf[i + 2] : 0);
}

static void sendError(uint8_t cmd, uint8_t code) {
  uint8_t p[2] = {cmd, code};
  sendSysex(CMD_ERROR, p, 2);
}

static void sendConfig() {
  uint8_t p[WIRE_BYTES + 1];
  encodeConfig(cfg, p);
  p[WIRE_BYTES] = sameConfig(cfg, saved) ? 0 : 1;
  sendSysex(CMD_GET, p, sizeof(p));
}

static void sendHello(uint8_t nonce) {
  pico_unique_board_id_t id;
  pico_get_unique_board_id(&id);
  uint8_t p[5 + 2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES];
  size_t n = 0;
  p[n++] = nonce;
  p[n++] = PROTOCOL_VERSION;
  p[n++] = FW_MAJOR;
  p[n++] = FW_MINOR;
  p[n++] = FW_PATCH;
  for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
    p[n++] = id.id[i] >> 4;
    p[n++] = id.id[i] & 0x0F;
  }
  sendSysex(CMD_HELLO, p, n);
}

static void handleSysex(const uint8_t *d, size_t len) {
  if (len < 4 || memcmp(d, SYSEX_ID, 3) != 0) return;  // not for us
  uint8_t cmd = d[3];
  const uint8_t *p = d + 4;
  size_t n = len - 4;

  switch (cmd) {
    case CMD_HELLO:
      sendHello(n > 0 ? p[0] : 0);
      break;
    case CMD_GET:
      sendConfig();
      break;
    case CMD_SET: {
      if (n != WIRE_BYTES) { sendError(cmd, 1); break; }
      Config c;
      if (!decodeConfig(p, c)) { sendError(cmd, 2); break; }
      if (!sameConfig(c, cfg)) {
        releaseActive();
        cfg = c;
      }
      sendConfig();
      break;
    }
    case CMD_SAVE:
      if (saveConfig()) {
        ledFlash(200, true);
        sendConfig();
      } else {
        sendError(cmd, 3);
      }
      break;
    case CMD_DEFAULTS:
      releaseActive();
      cfg = DEFAULT_CONFIG;
      sendConfig();
      break;
    default:
      sendError(cmd, 4);
  }
}

static uint8_t sysexBuf[40];
static size_t sysexLen = 0;
static bool inSysex = false;

static void feedByte(uint8_t b) {
  if (b == 0xF0) {
    inSysex = true;
    sysexLen = 0;
  } else if (b == 0xF7) {
    if (inSysex) handleSysex(sysexBuf, sysexLen);
    inSysex = false;
  } else if (b & 0x80) {
    if (b < 0xF8) inSysex = false;  // real-time bytes may interleave; anything else aborts
  } else if (inSysex) {
    if (sysexLen < sizeof(sysexBuf)) sysexBuf[sysexLen++] = b;
    else inSysex = false;  // too long to be ours
  }
}

static void pollMidiIn() {
  uint8_t p[4];
  while (MidiUSB.readPacket(p)) {
    uint8_t cin = p[0] & 0x0F;
    size_t count = 0;
    if (cin == 0x04 || cin == 0x07) count = 3;
    else if (cin == 0x06) count = 2;
    else if (cin == 0x05) count = 1;
    for (size_t i = 0; i < count; i++) feedByte(p[1 + i]);
  }
}

// ---------------------------------------------------------------- button

static bool readRaw() {
  bool down = digitalRead(BUTTON_PIN) == LOW;
#if USE_BOOTSEL_AS_BUTTON
  down = down || BOOTSEL;
#endif
  return down;
}

// Footswitches can bounce for 10-20 ms and long leads pick up noise, so:
//  - a press fires as soon as two 1 ms samples in a row read closed (no added latency
//    beyond 1 ms, but a single noise spike is ignored),
//  - a release needs the switch open for RELEASE_MS,
//  - after either, the switch is ignored for LOCKOUT_MS.
// With long press on, a press that is still held after holdMs plays the long-press note
// instead of the short press.
static bool stableDown = false;
static uint8_t downRun = 0;      // consecutive closed samples
static uint8_t upRun = 0;        // consecutive open samples
static uint32_t lastEdge = 0;
static uint32_t lastSample = 0;

static void pollButton() {
  uint32_t now = millis();
  if (now == lastSample) return;  // sample once per millisecond
  lastSample = now;

  bool raw = readRaw();
  if (raw) { if (downRun < 255) downRun++; upRun = 0; }
  else     { if (upRun < 255) upRun++; downRun = 0; }

  if (now - lastEdge < LOCKOUT_MS) return;
  if (!stableDown && downRun >= PRESS_SAMPLES) {
    stableDown = true;
    lastEdge = now;
    buttonDown(now);
  } else if (stableDown && upRun >= RELEASE_MS) {
    stableDown = false;
    lastEdge = now;
    buttonUp();
  }

  if (stableDown && pendingShort && now - pressedAt >= cfg.holdMs) {
    pendingShort = false;
    longActive = true;
    sendLongOn();
  }
}

// ---------------------------------------------------------------- main

// MIDI_ only lets the jack name be set after begin(), when the descriptor is already
// built, so the port would be called "virtual-cable" on some hosts. Set it beforehand
// through a pointer to the protected member.
struct MidiNameAccess : MIDI_ {
  static void setJackName(MIDI_ &m, const char *name) {
    (m.*(&MidiNameAccess::_cableStrID))[0] = USB.registerString(name);
  }
};

void setup() {
  if (BUTTON_RETURN_PIN >= 0) {
    pinMode(BUTTON_RETURN_PIN, OUTPUT);
    digitalWrite(BUTTON_RETURN_PIN, LOW);
  }
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delayMicroseconds(50);  // let the pull-up settle before the first read
  ledBegin();

  EEPROM.begin(256);
  loadConfig();

  USB.setProduct(DEVICE_NAME);
  MidiUSB.setName(DEVICE_NAME);
  MidiNameAccess::setJackName(MidiUSB, DEVICE_NAME);
  MidiUSB.begin();

  stableDown = readRaw();  // a button held at power-up does not fire
}

void loop() {
  pollMidiIn();
  pollButton();
  ledUpdate(active || longActive);
}
