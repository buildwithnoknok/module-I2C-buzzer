# Buzzer Module (I²C)

A compact I²C‑controlled buzzer module for the noknok ecosystem.  
Designed for simple audio feedback, alerts, and UI interaction in modular builds.

![USB LED Module](hardware/module-I2C-buzzer-front.png)
![USB LED Module](hardware/module-I2C-buzzer-back.png)

---

## Overview

The **Buzzer Module** uses a CH32V003 microcontroller to drive an **MLT‑8530** passive magnetic buzzer.  
It communicates via the standard noknok **I²C connector**, making it stackable and easy to integrate into any project.

Typical use cases:
- UI feedback (clicks, beeps)
- Timers and alarms
- Game sounds
- Interaction cues in kits

---

## Features

- I²C control (address configurable in firmware)
- CH32V003J4M6 microcontroller
- Drives **MLT‑8530** passive buzzer
- 3.3V operation via noknok I²C connector
- Over‑voltage protection on +3V3_PROT rail
- Compact 20×20 mm PCB
- Mounting holes for enclosure integration

---

## Firmware

Firmware is located in `/firmware`.

### Capabilities
- Play tones at arbitrary frequencies  
- Predefined beep patterns  
- Simple I²C command interface  
- Optional startup beep  

---

## Status

- Hardware: **v1.0**  
- Firmware: **in development**  
- Documentation: **in progress**

---

## License

TBD - to be added when the repository becomes public.
