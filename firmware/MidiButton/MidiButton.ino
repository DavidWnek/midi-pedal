// MIDI Button: one button on an RP2350 that sends one MIDI message over USB.
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
#define FW_MINOR 0
#define FW_PATCH 0
#define PROTOCOL_VERSION 1

// ---------------------------------------------------------------- config

enum : uint8_t { TYPE_NOTE = 0, TYPE_CC = 1, TYPE_PC = 2, TYPE_COUNT };
enum : uint8_t { MODE_MOMENTARY = 0, MODE_TOGGLE = 1, MODE_COUNT };

struct Config {
  uint8_t type;      // TYPE_*
  uint8_t channel;   // 0-15 (shown to users as 1-16)
  uint8_t number;    // note, CC number or program, 0-127
  uint8_t onValue;   // note velocity or CC value sent on press, 0-127
  uint8_t offValue;  // CC value sent on release (or second toggle press), 0-127
  uint8_t mode;      // MODE_*
};
static const size_t CONFIG_BYTES = sizeof(Config);

static const Config DEFAULT_CONFIG = {TYPE_NOTE, 0, 60, 100, 0, MODE_MOMENTARY};

struct Stored {
  uint32_t magic;
  uint8_t version;
  Config cfg;
  uint8_t checksum;
};
static const uint32_t STORE_MAGIC = 0x4D424331;  // "MBC1"
static const uint8_t STORE_VERSION = 1;

static Config cfg;    // live config
static Config saved;  // what is in flash

static bool configValid(const Config &c) {
  return c.type < TYPE_COUNT && c.channel < 16 && c.number < 128 && c.onValue < 128 &&
         c.offValue < 128 && c.mode < MODE_COUNT;
}

static uint8_t checksum(const Config &c) {
  const uint8_t *p = (const uint8_t *)&c;
  uint8_t sum = 0xA5;
  for (size_t i = 0; i < CONFIG_BYTES; i++) sum = (uint8_t)((sum << 1 | sum >> 7) ^ p[i]);
  return sum;
}

static bool sameConfig(const Config &a, const Config &b) {
  return memcmp(&a, &b, CONFIG_BYTES) == 0;
}

static void loadConfig() {
  Stored s;
  EEPROM.get(0, s);
  if (s.magic == STORE_MAGIC && s.version == STORE_VERSION && configValid(s.cfg) &&
      s.checksum == checksum(s.cfg)) {
    cfg = s.cfg;
  } else {
    cfg = DEFAULT_CONFIG;
  }
  saved = cfg;
}

static bool saveConfig() {
  Stored s = {STORE_MAGIC, STORE_VERSION, cfg, checksum(cfg)};
  EEPROM.put(0, s);
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

// Lit while the "on" message is in effect (held, or latched in toggle mode), flashes on a
// program change or save, dim otherwise. On a NeoPixel the colour shows the message type.
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

static bool active = false;  // the "on" message has been sent and not yet undone

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

// Undo a held or latched message before the config changes, so no note hangs.
static void releaseActive() {
  if (active) sendOff();
  active = false;
}

// ---------------------------------------------------------------- SysEx config protocol
//
//   F0 7D 4D 42 <cmd> <payload...> F7      (7D = non-commercial ID, 4D 42 = "MB")
//
//   01 HELLO    host: <nonce>             device: 01 <nonce> <proto> <maj> <min> <patch> <16 id nibbles>
//   02 GET      host: -                   device: 02 <config 6 bytes> <dirty>
//   03 SET      host: <config 6 bytes>    device: 02 ...  (applied live, not saved)
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
  uint8_t p[CONFIG_BYTES + 1];
  memcpy(p, &cfg, CONFIG_BYTES);
  p[CONFIG_BYTES] = sameConfig(cfg, saved) ? 0 : 1;
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
      if (n != CONFIG_BYTES) { sendError(cmd, 1); break; }
      Config c;
      memcpy(&c, p, CONFIG_BYTES);
      if (!configValid(c)) { sendError(cmd, 2); break; }
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
    onPress();
  } else if (stableDown && upRun >= RELEASE_MS) {
    stableDown = false;
    lastEdge = now;
    onRelease();
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
  ledUpdate(active);
}
