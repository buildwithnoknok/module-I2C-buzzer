# Buzzer Module (I²C)

A compact I²C‑controlled buzzer module for the noknok ecosystem.  
Designed for audio feedback, alerts, melodies, and UI interaction in modular builds.

![Buzzer Module Front](hardware/module-I2C-buzzer-front.png)
![Buzzer Module Back](hardware/module-I2C-buzzer-back.png)

> **Visual concept:** open [`docs/noknok-buzzer-concept.html`](docs/noknok-buzzer-concept.html) in a browser for a full interactive diagram of the protocol and architecture.

---

## Overview

The Buzzer Module uses a **CH32V003J4M6** microcontroller to drive an **MLT‑8530** magnetic buzzer via PWM through an MMBT3904 NPN transistor. It connects to the Raspberry Pi Pico (Conductor) over the standard noknok **JST SH 4‑pin I²C connector**.

The Conductor sends a short command — a frequency, duration, and volume, or just a tune ID — and the module handles everything else independently. The Pico is free to do other things while the buzzer plays.

---

## Features

- Dynamic I²C address via noknok enumeration protocol (no hardcoded address)
- CH32V003J4M6 microcontroller (RISC‑V, 48 MHz, 16 KB flash)
- Drives MLT‑8530 magnetic buzzer via PWM
- **Fire and forget** — all timing is handled on‑module
- **Volume control** (0–100%, maps to PWM duty cycle)
- **5 preloaded tunes** stored in flash (Nokia, Happy Birthday, Beep OK/Error, Startup Chime)
- Immediate interrupt — any new command stops whatever is playing
- 3.3V operation via noknok I²C connector
- Compact 20×20 mm PCB

---

## How It Works

```
Your Python code (Pico)          JST SH 4-pin           Buzzer Module (CH32V003)
────────────────────────         ────────────           ────────────────────────
                                  GND ────────
buzzer.play(440, 2.0)  ──────►   3.3V ───────  ──────►  receives 5 bytes
                                  SDA ─ data ─          sets PWM to 440 Hz
                                  SCL ─ clk  ─          starts 2s countdown
                                                         ← ACK (Pico moves on)
                                                         ... 2 seconds pass ...
                                                         timer fires → silence
```

The Pico sends **3–5 bytes** and returns immediately. The CH32V003 handles all timing internally using a hardware timer. No blocking, no waiting.

---

## I²C Protocol

**I²C address:** assigned dynamically at boot (see [Enumeration](#enumeration) below).

### Commands (Pico → Buzzer)

| Bytes | Command | Description |
|-------|---------|-------------|
| `0x00` | **STOP** | Silence immediately |
| `0x01` `fH` `fL` `dur` `vol` | **PLAY NOTE** | Play a tone |
| `0x02` `id` | **PLAY TUNE** | Play a preloaded tune |

**PLAY NOTE fields:**

| Field | Size | Description |
|-------|------|-------------|
| `fH` + `fL` | 2 bytes | Frequency in Hz, big‑endian (e.g. 440 Hz = `0x01 0xB8`) |
| `dur` | 1 byte | Duration in 100 ms units (1 = 100 ms, 10 = 1 s, 0 = play forever) |
| `vol` | 1 byte | Volume 0–100 |

### Status read (Pico ← Buzzer)

Read 1 byte from the module address:

| Value | Meaning |
|-------|---------|
| `0x01` | Currently playing |
| `0x00` | Idle |

---

## Preloaded Tunes

Tunes are stored as `const` arrays in the CH32V003's flash memory. Zero RAM cost.

| ID | Name | Notes |
|----|------|-------|
| `0x01` | Nokia Tune | 13 notes |
| `0x02` | Happy Birthday | 28 notes |
| `0x03` | Beep OK | Rising double beep |
| `0x04` | Beep Error | Low double buzz |
| `0x05` | Startup Chime | Rising 4‑note arpeggio (plays on every boot) |

---

## Enumeration

The buzzer uses the standard **noknok dynamic enumeration protocol**. There is no hardcoded I²C address.

At boot, each module:
1. Keeps I²C **off** and plays the startup chime
2. Calculates a unique backoff delay from its hardware UID (FNV‑1a hash, 300–2800 ms)
3. Enables I²C at the staging address **`0x7F`**
4. Sends a 10‑byte UID response when the Conductor reads it
5. Switches to the Conductor‑assigned address and operates normally

This allows **multiple identical buzzer modules** on the same bus — each gets a unique runtime address automatically.

See [Ecosystem / Software Guidelines](https://github.com/buildwithnoknok/Ecosystem/blob/main/software/readme.md) for the full enumeration spec.

---

## Python API

Use the `noknok.py` library (in `/firmware/bin/`) on the Pico.

```python
from noknok import Conductor

c = Conductor()        # GP8 = SDA, GP9 = SCL
c.enumerate()          # discover all modules (takes ~3 s)

# Single note — fire and forget
c.buzzer[0].play(440, 2000)             # 440 Hz for 2 seconds
c.buzzer[0].play(440, 1000, volume=50)  # half volume

# Predefined tune — fire and forget
c.buzzer[0].tune(c.buzzer[0].NOKIA)
c.buzzer[0].tune(c.buzzer[0].HAPPY_BIRTHDAY)
c.buzzer[0].tune(c.buzzer[0].BEEP_OK)

# Note in a melody — plays and waits
c.buzzer[0].note(440, 500)   # plays 440 Hz for 500 ms, then returns

# Stop
c.buzzer[0].stop()

# Status
c.buzzer[0].is_playing()     # True or False
c.buzzer[0].wait()           # block until idle

# Multiple buzzers
c.buzzer[1].play(880, 500)   # second buzzer (if present)
```

---

## Files on the Pico

The following files need to be present on the Pico's CIRCUITPY drive:

| File | Purpose |
|------|---------|
| `noknok.py` | noknok library — required by all scripts |
| `noknok_state.json` | Auto-created by `enumerate()` — stores module addresses so re-runs don't need to re-enumerate |
| `noknok_roles.json` | Created by `noknok_setup_roles.py` — maps role names to UIDs |

> **Filesystem write access required.** `noknok_state.json` and `noknok_roles.json` are written automatically by the library. The Pico filesystem must be writable from code. In CircuitPython, this requires remounting the filesystem — see the [CircuitPython filesystem docs](https://docs.circuitpython.org/en/latest/docs/library/storage.html).

---

## Hardware

| Spec | Value |
|------|-------|
| PCB size | 20 × 20 mm |
| MCU | CH32V003J4M6 (SOP‑8, RISC‑V, 48 MHz) |
| Buzzer | MLT‑8530 (magnetic, 3.3V) |
| Driver | MMBT3904 NPN transistor |
| Connector | JST SH 4‑pin (Qwiic / Stemma QT compatible) |
| Supply voltage | 3.3V via I²C connector |
| PWM pin | PA1 (TIM1 CH2) |
| I²C SDA | PC1 |
| I²C SCL | PC2 |
| Flash header | J4 — 5‑pin (GND, SWIO, RST, VCC) |

> **Known hardware note:** Remove the 10Ω series resistor on VDD (R1). It causes a brownout reset when the buzzer fires at high volume.

---

## Firmware

Source is in `/firmware/src/`. Build and flash from the RPi4:

```bash
cd /home/noknok/dev/ch32fun/noknok_buzzer
make        # compile
make flash  # compile + flash via WCH Link-E
```

| Metric | Value |
|--------|-------|
| Firmware version | v3.1 |
| Flash used | 2756 B of 16 KB (17%) |
| RAM used | 76 B of 2 KB (4%) |

### Files

| File | Description |
|------|-------------|
| `firmware/src/buzzer_firmware.c` | CH32V003 firmware source |
| `firmware/src/Makefile` | Build configuration |
| `firmware/src/funconfig.h` | ch32v003fun config |
| `firmware/bin/noknok.py` | CircuitPython library (Conductor + NoknokBuzzer) |
| `firmware/bin/noknok_buzzer_test.py` | Test script — all 6 tests |
| `firmware/bin/noknok_enum_test.py` | Enumeration test — multiple buzzers |
| `firmware/bin/Buzzer_OdeToTheJoy_Tune.py` | Example melody |
| `docs/noknok-buzzer-concept.html` | Interactive protocol diagram |

---

## Status

| Area | Status |
|------|--------|
| Hardware | v1.0 |
| Firmware | **v3.0 — complete** |
| Python library | **complete** |
| Documentation | **complete** |

---

## License

TBD — to be added when the repository becomes public.
