# sinowealth-mouse-macos (`nos-m700`)

**Turn off the RGB / DPI-indicator / scroll-wheel LED on Sinowealth-based gaming mice from
macOS.** It's a small command-line tool with no Windows software, no kernel extension and no
background app. Built and verified on the **NOS M-700 RGB**. The same controller and
protocol are used by the Glorious Model O/D and many budget "Model O clones" (list below).

It's plain userspace C against IOKit's IOHIDManager: no kernel driver, no
background daemon and no vendor software. The setting is stored in the mouse's own flash, so
it survives re-plugging and carries over to other computers.

```bash
sudo nos-m700 light off     # scroll-wheel LED off (scrolling is unaffected)
sudo nos-m700 light on      # colours back
sudo nos-m700 status        # active DPI stage, LED state
```

`wheel off|on` is an alias of `light off|on`. **Status: working, confirmed on hardware
2026-09-29.**

---

## TL;DR: how it works

- The mouse uses a **Sinowealth** controller (`258a:0029`), the same family as the Glorious
  Model O. Its settings live in a **131-byte config block**, read and written with HID
  *feature reports* 4 and 5.
- The scroll-wheel LED isn't an independent light. It's the **DPI-stage indicator**: it
  shows the colour stored for the currently active DPI stage.
- There's no "LED off" flag, but colour **`000000` = off**. `light off` writes black into
  all 8 DPI-stage colour slots. DPI switching still works; the indicator just shows
  "black" for every stage.

---

## Which mice may this work with?

Anything with **USB vendor ID `258a` (SINOWEALTH)** that uses the Glorious-Model-O-style
report 4 / report 5 config protocol. To check your mouse on macOS:

```bash
ioreg -r -c IOHIDDevice -l | grep -E '"(Product|VendorID|ProductID)"'
```

VendorID `9610` = `0x258a`. Convert the ProductID to hex and pass it with `--pid`, then start
with the **read-only** commands:

```bash
nos-m700 --pid 0036 info
```

```bash
sudo nos-m700 --pid 0036 read-config mymouse.bin
```

If the config reads back as 123–167 bytes starting `04 11`, `light off` will very likely work
too. Keep your dump so you can `restore` it.

| Brand / model | USB ID | Status | Source |
|---|---|---|---|
| **NOS M-700 RGB** (Nordic brand sold by Gigantti / Elkjøp) | 258a:0029 | ✅ **verified** | this repo |
| Everest GT-100 RGB | 258a:0029 | same ID, should work | OpenRGB [2] |
| Machenike M620 | 258a:0029 | same ID, should work | libratbag [9][11] |
| Mad Dog GM905 | 258a:0029 | same ID, should work | libratbag [9] |
| Glorious Model O / O- | 258a:0036 | same protocol, try `--pid 0036` | [1][2][8] |
| Glorious Model D / D- | 258a:0033 | same protocol, try `--pid 0033` | [2][8] |
| Glorious Model O (old firmware) | 258a:0027 | same protocol | libratbag |
| Genesis Xenon 770 | 258a:0027 | same protocol | libratbag |
| DreamMachines DM5 Blink | 258a:0027 | same protocol | libratbag |
| G-Wolves Hati HT-M (wired) | 258a:0027 | no LEDs | libratbag |
| Inphic PG2 | 258a:0028 | related | libratbag [12] |
| T-Dagger Imperial T-TGM310 | 258a:0051 | related | libratbag |
| Marvo Scorpion G961 | 258a:1007 | related, different controller variant | libratbag |
| Genesis Xenon 200, ZET Fury Pro | 258a:1007 | **different protocol** (OpenRGB has separate drivers) | OpenRGB [2] |
| Glorious Model O / D **Wireless** | 258a:2011 / 2012 / 2022 / 2023 | **different protocol** | OpenRGB [2] |

Many more unbranded or rebranded 258a mice from AliExpress and Amazon (often sold as
"Model O clones", "honeycomb ultralight") use the same firmware. **Sinowealth keyboards
(`258a:0016`, `0090`, `010c`, …) use a different protocol**, so don't use this tool on them.

### Who benefits

- Mac users with a Sinowealth mouse whose vendor software is **Windows-only**
- People who want the **DPI / wheel light off** at night, for streaming or to save power,
  which even the official software often can't do (it only offers per-stage colours)
- Linux / OpenRGB / libratbag developers: this documents that the DPI indicator = the
  per-stage colour table at `0x1d–0x34`, where black = off, and that `0x0b`'s high nibble is
  the active stage

---

## 1. How we got there (step by step)

### Step 1: Identify the device
`ioreg -r -c IOHIDDevice -l` showed the mouse as **`SINOWEALTH` / `Wired Gaming Mouse`,
VID `0x258A`, PID `0x0029`**, with two HID interfaces. One of them has
`MaxFeatureReportSize = 520`, a strong sign of a vendor configuration channel.

### Step 2: Dump and parse the HID report descriptors
The `descriptors` command parses the raw descriptors and computes the report sizes. The
second interface contains **vendor page 0xFF00** with:
- **Report 0x04**, feature, 519 B (+1 ID byte = 520): a large data block
- **Report 0x05**, feature, 5 B (+1 = 6): a small command channel
- **Report 0x07**, input, 7 B: change notifications

This "report 5 = command, report 4 = data block" layout is the signature of Sinowealth
gaming-mouse firmware.

### Step 3: Recognise the chipset family from open-source projects
- **OpenRGB** already lists `258a:0029` as the "Everest GT-100 RGB" [2].
- **libratbag** lists `258a:0029` as the Machenike M620 and Mad Dog GM905 [9][11].
- **gloriousctl**, **OpenRGB** and **libratbag** all document the same config protocol for
  the Glorious Model O family [1][3][8].

So the NOS M-700 is a rebadged Sinowealth reference design, and the existing
reverse-engineering applies.

### Step 4: Get past macOS permissions
Interface 1 also declares a **keyboard** collection (for button→key macros). macOS
therefore gates `IOHIDDeviceOpen()` behind **Input Monitoring** (TCC) and returns
`0xe00002e2 kIOReturnNotPermitted`. Feature reports can't be sent without an open
(`0xe00002cd kIOReturnNotOpen`). Fix: run with `sudo`, or grant Terminal Input Monitoring.

### Step 5: Read the config block
Following the known protocol: `SET_FEATURE 05 11 00 00 00 00` ("read config"), then
`GET_FEATURE` report 4. The mouse returned **131 bytes**, exactly the size gloriousctl and
OpenRGB expect (`CONFIG_SIZE_USED = 131`). Decoding it against their struct matched field
for field: 6 DPI stages, a disabled-stage mask, and six DPI-indicator colours (red, green,
blue, yellow, cyan, magenta).

### Step 6: The obvious approach doesn't apply
The main RGB effect byte (offset `0x35`) was **already `0x00` = "off"**, yet the wheel was
lit. OpenRGB's own source comments that mode 0x00 "does nothing" [4]. The user then
confirmed that **the wheel colour changes with the DPI button**. So the wheel LED is the DPI
indicator, which the effect byte doesn't control.

### Step 7: Research
No one online documents turning off the DPI indicator on this family. The Glorious software
only offers per-stage colours; libratbag, OpenRGB and gloriousctl only implement "off" as
effect 0x00. We built write support with a safety net: `poke` backs up, patches, writes,
reads back and verifies, and `restore` undoes it.

### Step 8: A failed experiment, and why it failed
We blacked out **stage 5's** colour (believing "cyan" = stage 5). The write was accepted and
read back correctly, but the wheel didn't change. Red instead of black didn't change it
either.

To rule out profiles, `probe` asked for the active profile (`05 02`) → **profile 1**, the
one we edited. It also read the **button map** (`05 12`) and found button 9 mapped to
`50 07` ("cycle LED modes").

### Step 9: The breakthrough, diffing before and after one DPI press
The user read the config, switched from "cyan fast" to "red slow", and read it again.
`diff` showed a single byte: **`[0x0b] 66 → 16`**. The high nibble of `0x0b` is the
**active DPI stage (1-based)**, and it's saved to flash on every press. So "cyan fast" had
been **stage 6**, and we'd poked the wrong stage.

### Step 10: The correct experiment
On stage 1 (red), `poke 0x1d 0 0x1e 0 0x1f 0` → **the wheel LED went dark.** ✅

### Step 11: Turn it into a tool
`light off` blacks out all 8 stage colours (so it stays dark on every stage), saves the
previous colours for `light on`, writes, then verifies by reading back.

---

## 2. Device facts

| Field | Value |
|---|---|
| Retail name | NOS M-700 RGB (Gigantti / Elkjøp house brand) [10] |
| USB strings | `SINOWEALTH` / `Wired Gaming Mouse`, no serial |
| VID:PID | **`258a:0029`** |
| bcdDevice | 1.00 |
| Firmware (`05 01`) | ASCII `"2616"` |
| Sensor | PixArt PMW3325, 5000 DPI max [10]; config sensor byte `0x0b` |
| Report interval | 1 ms (1000 Hz) |
| Same PID elsewhere | Everest GT-100 RGB [2], Machenike M620 (fw V127), Mad Dog GM905 (fw V287) [9] |

### HID interfaces

| # | Primary usage | Usage pairs | Descriptor | Max in / out / feature |
|---|---|---|---|---|
| 0 | 0001:0002 Mouse | 0001:0002, 0001:0001 | 71 B | 7 / 0 / 0 |
| 1 | 0001:0006 Keyboard | 0001:0006, 000c:0001, **ff00:0001** | 213 B | 8 / 1 / **520** |

**Interface 0 (no report ID):** 5 buttons + 3 pad bits, X/Y 16-bit relative
(−32768..32767), 8-bit wheel, 8-bit AC Pan (Consumer 0x0238).

**Interface 1:**

| Report ID | Usage page | Type | Size (without ID byte) | Purpose |
|---|---|---|---|---|
| 0x01 | 0x07 Keyboard | Input | 7 B | modifiers + 6 keys (button→key macros) |
| 0x02 | 0x0C Consumer | Input | 3 B | media keys |
| **0x04** | 0xFF00 | **Feature** | **519 B** | **config / button data block** |
| **0x05** | 0xFF00 | **Feature** | **5 B** | **command channel** |
| 0x07 | 0xFF00 | Input | 7 B | change notifications |

---

## 3. Protocol

All traffic goes over **interface 1** with HID class control transfers
(`SET_REPORT` / `GET_REPORT`, report type *Feature*).

### Command channel (report 5, 6 bytes)

| Command | Bytes | Then | Result |
|---|---|---|---|
| Firmware version | `05 01 00 00 00 00` | GET report 5 | bytes 2–5 ASCII → `"2616"` |
| Active profile | `05 02 00 00 00 00` | GET report 5 | byte 2 = profile (1-based) → `1` |
| Set profile | `05 02 NN 00 00 00` | none | (not used) |
| Read config, profile 1 | `05 11 00 00 00 00` | GET report 4 | 131 bytes |
| Read buttons, profile 1 | `05 12 00 00 00 00` | GET report 4 | 88 bytes |
| Read config, profile 2 / 3 | `05 21` / `05 31` | GET report 4 | M700: profile 2 all zeros, profile 3 fails (`0xe0005000`) |

Command IDs are from libratbag [8].

### Writing the config (confirmed on the M700)

1. Read the current block (`05 11` → report 4, 131 bytes).
2. Modify the fields.
3. Set byte `0x03` = **`0x7B`** (= 131 − 8, the "write" marker; `0x00` in reads).
4. `SET_FEATURE` report 4, **padded with zeros to 520 bytes**.
5. The mouse commits to flash. Reading back returns the new values.

Always write back the **whole** block you read; there's no partial write.

### Config block map (131 bytes)

Values are this mouse's factory dump (`config-baseline.bin`).

| Offset | Factory | Field | Source |
|---|---|---|---|
| 0x00 | `04` | report ID | [1][3] |
| 0x01 | `11` | command ID (profile 1) | [1][8] |
| 0x03 | `00` | `00` read / `7B` write | [1][3] |
| 0x04–0x08 | `00 00 00 00 64` | unknown | [8] |
| 0x09 | `0b` | sensor ID (not in libratbag's list; probably PMW3325) | [8] |
| 0x0a | `04` | low nibble report-rate index (4 = 1000 Hz), high nibble flags (8 = XY-independent DPI) | [8] |
| **0x0b** | `66` | **low nibble = DPI stage count, high nibble = active stage (1-based)**, saved on every DPI press | [8], verified |
| 0x0c | `c0` | DPI stage disable mask (bit set = disabled: stages 7, 8) | [1] |
| 0x0d–0x1c | `07 0f 17 23 31 3b 00…` | DPI value per stage (encoding for the PMW3325 unverified) | [1][8] |
| **0x1d–0x34** | ff0000 00ff00 0000ff ffff00 00ffff ff00ff 000000 000000 | **DPI-indicator (= wheel LED) colour per stage, 8 × R,G,B** | [1], verified |
| 0x35 | `00` | main RGB effect (see below) | [1][3] |
| 0x36 / 0x37 | `42` / `00` | rainbow mode byte / direction | [1][3] |
| 0x38 / 0x39–0x3b | `40` / `ff0000` | static: mode / colour | [1][3] |
| 0x3c / 0x3d / 0x3e–0x52 | `02` / `07` / 7 colours | spectrum breathing: mode / count / colours | [1][3] |
| 0x53 | `42` | tail mode | [1][3] |
| 0x54 | `02` | breathing / spectrum-cycle mode | [3][8] |
| 0x55 / 0x56–0x67 | `00` / 6 colours | constant (per-LED) mode / colours | [8] |
| 0x68–0x73 | … | unknown | [8] |
| 0x74 / 0x75–0x7a | `42` / 2 colours | rave mode / colours | [3] |
| 0x7c | `42` | wave mode | [1][3] |
| 0x7d / 0x7e–0x80 | `02` / `ff0000` | breathing (1 colour) mode / colour | [1][3] |
| 0x81 | `01` | unknown; OpenRGB writes `00` for OFF (*"either 0x00 or 0x03"*) | [3] |
| 0x82 | `00` | lift-off distance | [1] |

**Mode byte** (0x36, 0x38, …): high nibble brightness (1 / 2 / 4), low nibble speed (1–3) [3].
**Colour order:** R,G,B on this firmware. libratbag marks both known `0029` firmwares
`LedType=RGB` [9], while the Glorious Model O uses R,B,G [1][3].

### RGB effect IDs (0x35)

| ID | OpenRGB [4] | gloriousctl [1] / libratbag [8] |
|---|---|---|
| 0x00 | OFF ("does nothing") | OFF |
| 0x01 | RAINBOW | GLORIOUS |
| 0x02 | STATIC | SINGLE |
| 0x03 | SPECTRUM_BREATHING | BREATHING7 |
| 0x04 | TAIL | TAIL |
| 0x05 | SPECTRUM_CYCLE | BREATHING |
| 0x06 | none | CONSTANT (per-LED) |
| 0x07 | RAVE | RAVE |
| 0x08 | EPILEPSY | RANDOM |
| 0x09 | WAVE | WAVE |
| 0x0A | BREATHING | BREATHING1 |

### Button map (`05 12` → report 4, 88 bytes)

Four bytes per button, starting at offset 0x08. Type codes are from libratbag [8].

| Slot | Bytes | Meaning |
|---|---|---|
| 0 | `11 01` | left |
| 1 | `11 02` | right |
| 2 | `11 04` | middle |
| 3 | `11 08` | back |
| 4 | `11 10` | forward |
| 5 | `41 00` | DPI cycle up |
| 6 / 7 | `11 10` / `11 08` | second pair of magnetic side buttons |
| 8 | `50 07` | special: cycle LED modes (no effect on the LEDs observed) |
| 9+ | `50 01` | none |

---

## 4. The light-off mechanism

```
config[0x0b] >> 4          → active stage s (1..6)
config[0x1d + 3*(s-1) ..]  → R, G, B shown on the wheel LED
```

`light off`:
1. Read the config (`05 11` → report 4).
2. Save the 24 colour bytes (and the effect byte) to `~/.nos-m700-state.bin`. Under sudo
   it uses `SUDO_USER`'s home and `chown`s the file back to them.
3. Write `00` to all 24 bytes at `0x1d–0x34`, so every stage is dark and DPI cycling can't
   bring the light back.
4. Set `0x03 = 0x7B`, pad to 520 bytes, `SET_FEATURE` report 4.
5. Wait 100 ms, read again and compare bytes 4–130. Warn if they differ.

`light on` writes the saved colours back. Any stage that's black, and all of them if there's
no save file, gets the factory palette. This matters because the manual stage-1 test left a
black stage behind.

`side off|on` / `all off|on` switch the main effect byte `0x35`. It's already `0x00` from
the factory, and on this mouse the wheel/DPI LED is the only light we've observed, so in
practice `all off` = `light off`.

---

## 5. macOS implementation notes

- **API:** `IOHIDManagerCreate` → match `{VendorID: 0x258A, ProductID: 0x0029}` →
  `IOHIDManagerCopyDevices`. The manager itself is never opened (that would open the mouse
  interface too). The vendor interface is chosen as the one with
  `MaxFeatureReportSize ≥ 520`.
- **I/O:** `IOHIDDeviceOpen` → `IOHIDDeviceSetReport(kIOHIDReportTypeFeature, id, buf, len)`
  / `IOHIDDeviceGetReport(...)`. `buf[0]` holds the report ID, as the device expects
  (hidapi-style).
- **Short reads are normal:** GET report 4 with a 520-byte buffer returns 131 bytes (config)
  or 88 bytes (buttons).
- **Permissions:** the keyboard collection on interface 1 triggers the Input Monitoring TCC
  check. The tool calls `IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)` so macOS shows the
  prompt. Otherwise use `sudo`.
- **Build:** `clang -framework IOKit -framework CoreFoundation`; one C file, no dependencies.

---

## 6. Lessons learned

1. **"Off" in the effect enum isn't "all LEDs off."** On Sinowealth mice the DPI indicator
   is a separate path, driven only by the per-stage colour table.
2. **Find out which state is active before testing.** A single-press `diff` showed that
   `0x0b`'s high nibble is the active stage. Before that we had tested a stage that wasn't
   selected.
3. **Diffing dumps beats guessing.** Read → change one thing → read → `diff` isolated the
   byte immediately.
4. **Always read-modify-write the whole block** and verify by reading back. The mouse stores
   everything in one 131-byte flash record.
5. **Colour order varies by firmware.** Check it against a known palette rather than trusting
   one source.

## 7. Open questions

- Stage 6 is stored as `ff00ff` (magenta), but the user saw it as cyan. Possibly a channel
  mapping quirk, or just how the LED renders.
- DPI value encoding for the PMW3325 (offset 0x0d…).
- Meaning of `0x04–0x08`, `0x68–0x73`, `0x81`.
- Button 9's `50 07` "cycle LED modes" had no observed effect.

---

## Build and install

```bash
make
```

```bash
sudo make install
```

Installs to `/usr/local/bin/nos-m700`. Needs the Xcode command-line tools.

To run without sudo, add your terminal app under System Settings › Privacy & Security ›
**Input Monitoring** and restart it.

## Commands

| Command | Writes to mouse? | Description |
|---|---|---|
| `light off\|on` | **yes** | wheel LED off (all stage colours black) / restore colours |
| `wheel off\|on` | **yes** | alias of `light` |
| `side off\|on` | **yes** | main RGB effect byte 0x35 off / restore |
| `all off\|on` | **yes** | `light` + `side` |
| `status` | read command only | active DPI stage, wheel LED state, effect |
| `info` | no | device + HID interface summary |
| `descriptors [--raw]` | no | hex dump + parsed report descriptors + report sizes |
| `probe` | read commands only | firmware, active profile, all profile configs, button map |
| `read-config [file]` | read command only | read, dump, decode, optionally save the config block |
| `decode <file>` | no device needed | decode a saved dump |
| `diff <a> <b>` | no device needed | byte diff of two dumps |
| `monitor [s]` | no | print vendor input reports (report 7) |
| `get-feature <id> [len]` | no | raw GET_FEATURE |
| `poke <off> <val> …` | **yes** | back up → patch bytes → write → verify |
| `restore <file>` | **yes** | write a saved dump back |

Options: `-v` prints every SET_FEATURE payload; `-p N` selects profile 1–3 for
`read-config` / `poke` / `restore`; `--vid` / `--pid` (hex) target another Sinowealth mouse
(default `258a:0029`).

To get back to the exact factory state, restore the factory dump. Note that this also sets the
active DPI stage back to 6:

```bash
sudo nos-m700 restore config-baseline.bin
```

## Files

| Path | Purpose |
|---|---|
| `src/main.c` | the CLI |
| `Makefile` | build / install |
| `config-baseline.bin` | factory config dump, 131 bytes (2026-09-29) |
| `dumps/config-p1.bin`, `dumps/config-p2.bin`, `dumps/buttons-p1.bin` | `probe` output |
| `dumps/m0.bin`, `dumps/m1.bin` | before/after the DPI press that revealed byte 0x0b |
| `~/.nos-m700-state.bin` | colours saved by `light off` for `light on` |

## Optional: USB captures from the Windows software

These weren't needed, but they'd resolve the open questions. Use USBPcap [6] + Wireshark and
change one setting per capture. Filter:
`usb.device_address == N && (usb.setup.bRequest == 0x09 || usb.setup.bRequest == 0x01)`.
The official software is `NOS-M700-mouse-2014-12-25.zip` on <https://nosgg.com/software>
(listed there for the *M700 WL Ultralight Spider*, so it may be a different mouse).

## Sources

1. **gloriousctl**: Glorious Model O config tool; `struct config`, `CONFIG_SIZE_USED 131`,
   `config_write = CONFIG_SIZE_USED - 8`, effect enum.
   <https://github.com/enkore/gloriousctl> ·
   [gloriousctl.c](https://github.com/enkore/gloriousctl/blob/master/gloriousctl.c)
2. **OpenRGB, Sinowealth detector**: `SINOWEALTH_VID 0x258A`, `Everest_GT100_PID 0x0029`,
   read command `05 11` + 520-byte report 4.
   <https://gitlab.com/CalcProgrammer1/OpenRGB/-/blob/master/Controllers/SinowealthController/SinowealthControllerDetect.cpp>
3. **OpenRGB, SinowealthController.cpp**: `SetMode()` offsets, write flag `0x7B`, byte 0x81,
   firmware query `05 01`.
   <https://gitlab.com/CalcProgrammer1/OpenRGB/-/blob/master/Controllers/SinowealthController/SinowealthController/SinowealthController.cpp>
4. **OpenRGB, SinowealthController.h**: `GLORIOUS_MODE_*` (OFF "does nothing"),
   speed/brightness enums, sizes 131 / 167 / 520.
   <https://gitlab.com/CalcProgrammer1/OpenRGB/-/blob/master/Controllers/SinowealthController/SinowealthController/SinowealthController.h>
5. **MacRazer**: macOS IOHIDManager feature-report tool for Razer mice; same Input
   Monitoring restriction. <https://github.com/SorcRR/MacRazer>
6. **USBPcap**: <https://desowin.org/usbpcap/>
7. **Apple IOHIDManager reference**:
   <https://developer.apple.com/documentation/iokit/iohidmanager_h>
8. **libratbag, driver-sinowealth.c**: most complete struct; command IDs (firmware, profile,
   config ×3, buttons), active-stage nibble, sensor byte, report rate, button types.
   <https://github.com/libratbag/libratbag/blob/master/src/driver-sinowealth.c>
9. **libratbag, sinowealth-0029.device**: `258a:0029` firmwares V127 / V287, `LedType=RGB`.
   <https://github.com/libratbag/libratbag/blob/master/data/devices/sinowealth-0029.device>
10. **Gigantti, NOS M-700 RGB**: PMW3325, 5000 DPI, two shells.
    <https://www.gigantti.fi/product/tietokoneet-ja-toimistotarvikkeet/tietokonetarvikkeet/hiiret-ja-nappaimistot/tietokoneen-hiiret/nos-m-700-rgb-pelihiiri/200751>
11. **libratbag issue #1361 / PR #1362**: Machenike M620 on `258a:0029`.
    <https://github.com/libratbag/libratbag/issues/1361> ·
    <https://github.com/libratbag/libratbag/pull/1362>
12. **libratbag issue #1411**: Inphic PG2, multiple LED zones, LED control ineffective.
    <https://github.com/libratbag/libratbag/issues/1411>
13. Mad Dog GM905 product page.
    <https://www.mediaexpert.pl/komputery-i-tablety/myszy-komputerowe/myszki-uniwersalne/mysz-mad-dog-gm905-1>
14. Glorious Model O wired product guide (per-stage DPI indicator colours).
    <https://www.gloriousgaming.com/pages/guide-model-o-wired>
15. Delux M700 manual (button-combo lighting control).
    <https://manuals.plus/delux/m700-pro-gaming-mouse-manual>
16. linux-hardware.org, `258a:0029`. <https://linux-hardware.org/?id=usb:258a-0029>
17. NOS software downloads. <https://nosgg.com/software>

## License

MIT. See [LICENSE](LICENSE). Not affiliated with NOS, Sinowealth, Glorious or any brand named
here. Writing to your mouse's flash is at your own risk; keep a `read-config` dump so you can
`restore`.
