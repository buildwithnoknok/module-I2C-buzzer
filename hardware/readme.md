# Hardware — noknok Buzzer

Hardware design files for the noknok Buzzer module (CH32V003J4M6 — I2C-controlled magnetic buzzer).

- KiCad project: `kicad/module-I2C-buzzer.*`
- Schematic (PDF): `module-I2C-buzzer-Schematics.pdf`
- BOM: `module-I2C-buzzer.xls`
- Board renders: `module-I2C-buzzer-front.png`, `module-I2C-buzzer-back.png` *(pre-V2 renders — pending re-export)*

Hardware is licensed CC BY-SA 4.0 (see `../LICENSE-hardware`). Connector, flashing and mounting standards follow the [noknok Ecosystem guidelines](https://github.com/buildwithnoknok/Ecosystem) (electrical + mechanical).

---

## Hardware Change Record

### v2.0 — changes from v1.0
- **Fixed R1 (VDD series) to 1 Ω** — this was previously a documented-but-unfixed known issue: the schematic still shipped with the default 10 Ω, which caused a brownout reset when the buzzer fired at high volume (the beep current spike through 10 Ω trips BOR). V1 units needed a manual bodge; V2 corrects it in the schematic itself. This is a buzzer-specific deviation — knob and LED Button stay at the 10 Ω default (no spiky VDD load).
- **Status indicator LED on PD1** (in parallel with the SWIO flashing line — zero extra GPIO). Active-low: `+3V3_PROT → R2 (2.2 kΩ) → D3 anode; D3 cathode → PD1/SWIO`. Driven by the bootloader (off = app running, slow pulse = updating/recovering, solid = error). Parts: red 0603 LED (LCSC C2286) + 2.2 kΩ 0603 (LCSC C4190).
- **Removed both castellated edge pads** — the 5-pin flashing edge (old J4) and the redundant I2C edge (old J3). I2C is via the JST-SH (Qwiic) connectors J1/J2 only.
- **New flashing interface (J3)** — noknok flash pads (`noknok_FlashPads_I2C-module_1x3_M2.5`): 3 single-side SMD pogo pads (GND / SWIO / VCC) plus an embedded M2.5 keyed mounting hole, from the noknok KiCad library in the Ecosystem repo. Pad row is offset toward the keyed hole (not centered) so a 180°-flipped clamp contact can't seat and reverse-power the board.
- **Second mounting hole (H1)** — plain `noknok_MountingHole_2.5mm_M2.5`, diagonally opposite the flash-pad hole.
- **Removed the on-board I²C pull-up resistors** (previously R2/R3) — pull-ups now live on the host (Conductor), not per module (avoids stacking on the shared bus). The host must provide the bus pull-ups (proposed 3.3 kΩ); a noknok pull-up PCB covers third-party hosts that lack them. See the [I²C Pull-up Resistor Strategy ADR](https://noknokdev.atlassian.net/wiki/spaces/SD/pages/82280449). *(`R2` is reused above for the new status-LED resistor — it is not the old pull-up.)*
- **Buzzer drive resistor renumbered R4 → R3** (still 470 Ω, unchanged value) — a side effect of the old pull-ups' designators being freed up.
- D1 (protection Zener) and D2 (1N4148W) are unchanged from v1.0.
