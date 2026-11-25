# MIDI Button

One button on an RP2350 board. Press it and it sends a MIDI message over USB: a note,
a control change or a program change. You set what it sends from a small web page that
talks to the button over the same USB MIDI port, so there is no driver, app or serial port
to install.

**Site, firmware downloads and configurator: https://davidwnek.github.io/midi-pedal/**

```
firmware/MidiButton/MidiButton.ino   the firmware (Arduino, earlephilhower RP2040/RP2350 core)
firmware/build.ps1                   builds build/midi-button-<board>.uf2 on Windows
site/index.html                      the landing page
site/configure/index.html            the configurator: one static file, no build step
.github/workflows/pages.yml          builds the firmware and deploys site/ to GitHub Pages
```

A just-for-fun open-source project, MIT licensed. Issues and pull requests welcome.

## Parts

| Qty | Part | Notes |
|---|---|---|
| 1 | [Waveshare RP2350-Zero](https://www.amazon.com/dp/B0H9RB7T4F) | A Raspberry Pi Pico 2 also works |
| 1 | [SPST momentary footswitch, normally open, 2 pin](https://www.amazon.com/dp/B08TBTWDYV) | Any momentary switch works |
| ~3" | 24 AWG wire | Cut in two, one per lug |
| 1 | USB-C data cable | Charge-only cables won't show up as MIDI |

A 3D-printable case is coming; the STL files will go in this repo.

## Wiring

A momentary switch between **GP9** and **GP13**, either way round. GP9 is driven low and
acts as the return, so no GND wire is needed. On the Waveshare RP2350-Zero both are on the
bottom edge (GP13 is pad 10, GP9 is pad 14). On a Pico 2 they are pins 12 (GP9) and 17 (GP13).

To wire the switch to GND instead, set `BUTTON_RETURN_PIN` to `-1` and `BUTTON_PIN` to the
input pin at the top of the sketch.

The board's **BOOT** button also works as the button, so you can try it before wiring
anything. Set `USE_BOOTSEL_AS_BUTTON` to `0` to turn that off.

The LED shows the state. On the RP2350-Zero's RGB LED the colour shows the message type
(blue note, green CC, amber program change): dim when idle, bright while the button is
held or latched, white for a moment after a save. On a plain LED it is on while active.

## Build and flash

Needs the Arduino **Raspberry Pi Pico/RP2040/RP2350** core by Earle Philhower (6.x) and,
for boards with an RGB LED, the **Adafruit NeoPixel** library. The script uses the
arduino-cli that comes with Arduino IDE 2.

```powershell
.\firmware\build.ps1                      # Waveshare RP2350-Zero
.\firmware\build.ps1 -Board rpipico2      # Raspberry Pi Pico 2
```

Or skip building: the site has ready-made `.uf2` files, built by CI from `main`.

To flash, hold BOOT while plugging the board in, then copy `build\midi-button-<board>.uf2`
onto the `RP2350` drive that appears. After that, `.\firmware\build.ps1 -Upload -Port COMx`
works too (the board also has a serial port, which arduino-cli uses to reset it).

It appears as a USB MIDI device called **MIDI Button**. It is class compliant, so it works
with Windows, macOS, Linux, iPad (with a USB adapter) and Android without drivers.

## Configure

Open the [configurator](https://davidwnek.github.io/midi-pedal/configure/) in Chrome or
Edge (desktop or Android), click **Connect**, allow MIDI
access, and change the settings. Changes take effect on the button right away so you can try
them. **Save to button** writes them to flash so they survive unplugging.

| Setting | Note | Control change | Program change |
|---|---|---|---|
| Number | note 0-127 | controller 0-127 | program 1-128 |
| On press | note on, velocity | value when pressed | program change |
| On release | note off | value when released | nothing |
| Toggle mode | 1st press on, 2nd press off | alternates the two values | n/a |

**Long press** (optional) is a second note. Tick *Long press plays a second note*, set how
long to hold (0.1 to 5 seconds) and which note and velocity to send. The note plays while
you keep holding and stops when you let go. A press shorter than the hold time does the
short press as usual. Because the button can't tell a short press from a long one until it's
released, the short press fires on release when long press is on, so it can't start
sooner than that. With long press off, nothing changes.

Safari (macOS and iOS) has no Web MIDI, so it can't configure the button. The button
still works there as a MIDI device. Firefox can, after it asks to install its site
permission add-on.

### Hosting

Every push to `main` runs `.github/workflows/pages.yml`: it compiles the firmware for each
board, copies the `.uf2` files and a `version.json` into `site/firmware/`, and deploys `site/`
to GitHub Pages. In a fork, turn it on once under Settings > Pages > Source: **GitHub Actions**.

The pages are plain HTML with no build step and no external requests, so `site/` works on
any static host that serves HTTPS. Web MIDI only works in a secure context, so plain
`http://` does not work except on `localhost`. To try it locally:

```bash
python -m http.server 8000 --directory site
```

and open http://localhost:8000. Copy `build\*.uf2` into `site\firmware\` first if you
want the download links to work locally.

## Protocol

Configuration is SysEx on the button's own MIDI port. Messages use the non-commercial
manufacturer ID `7D` followed by `4D 42` ("MB"):

```
F0 7D 4D 42 <cmd> <payload...> F7
```

| Cmd | Host sends | Button replies |
|---|---|---|
| `01` HELLO | `<nonce>` | `01 <nonce> <protocol> <major> <minor> <patch> <16 nibbles of chip ID>` |
| `02` GET | nothing | `02 <config> <dirty>` |
| `03` SET | `<config>` | `02 <config> <dirty>`. Applied live, not saved. |
| `04` SAVE | nothing | `02 <config> 00` once written to flash |
| `05` DEFAULTS | nothing | `02 <config> <dirty>`. Defaults applied live, not saved. |
| `7F` ERROR | | `7F <cmd> <code>`: 1 bad length, 2 value out of range, 3 flash write failed, 4 unknown command |

`<config>` is eleven bytes: `type` (0 note, 1 CC, 2 program change), `channel` (0-15),
`number`, `onValue`, `offValue`, `mode` (0 momentary, 1 toggle), `longOn` (0/1),
`longNote` (0-127), `longVelocity` (1-127), then `holdMs` as two 7-bit bytes, low then high
(100-5000). `dirty` is 1 when the live config differs from what is saved. Defaults are note
60 (C4), velocity 100, channel 1, momentary, long press off (note 64, velocity 100, hold 500 ms).

Protocol 2 (firmware 1.1) added the long-press fields. Protocol 1 (firmware 1.0) used six
bytes; the button reads those settings from flash after an update and fills in the long-press
defaults.

The page finds the button by sending HELLO with a different nonce to every output port
named "MIDI Button". The input that answers with a given nonce belongs to the same device,
which pairs the ports correctly when several buttons are plugged in.

When the config changes while a note is held or latched, the button sends the note off
(or the CC's released value) first, so nothing is left hanging.

## Not done yet

- USB VID/PID is the core's default Raspberry Pi ID. A product needs its own.
- Tested on a Waveshare RP2350-Zero with a wired switch on GP9/GP13: USB MIDI, every
  SysEx command, save across reboot, clean press/release and long holds, and the web page in
  Chrome setting note, CC and program change, checked in another MIDI app. Not yet tried on
  a Pico 2, macOS, Linux or Android.
