# noknok.py
# CircuitPython library for the noknok modular ecosystem
# Raspberry Pi Pico — I2C master ("Conductor")
#
# Quick start:
#   from noknok import Conductor
#   c = Conductor()
#   c.enumerate()                          # discover all modules (~3 s)
#   c.load_roles()                         # load noknok_roles.json (optional)
#   c.role["volume_knob"].value            # access by role name
#   c.buzzer[0].play(440, 500)             # or by type + index

import busio
import board
import time
import json


# ── CRC8 (polynomial 0x07) — matches firmware ────────────────────────────────
def _crc8(data):
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


# ═════════════════════════════════════════════════════════════════════════════
class Conductor:
    """
    I2C master for the noknok ecosystem.
    Discovers all connected modules and assigns each a unique address.

    Typical usage:
        c = Conductor()
        c.enumerate()           # ~3 s, discovers all modules
        c.load_roles()          # load noknok_roles.json if it exists

        # Access by role (stable — same physical module every boot):
        c.role["volume_knob"].value
        c.role["alert_buzzer"].play(880, 200)

        # Access by type + index (order = discovery order, use when roles don't matter):
        c.buzzer[0].play(440, 500)
        c.knob[0].value
    """

    ENUM_ADDR  = 0x7F
    ASSIGN_REG = 0x1D

    TYPE_BUZZER   = 0x01
    TYPE_KNOB     = 0x02
    TYPE_KEYBOARD = 0x03
    TYPE_LED      = 0x04

    def __init__(self, sda=board.GP8, scl=board.GP9, frequency=100_000):
        self.i2c      = busio.I2C(scl, sda, frequency=frequency)
        self.buzzer   = []    # NoknokBuzzer instances, indexed by discovery order
        self.knob     = []    # NoknokKnob instances (future)
        self.keyboard = []    # NoknokKeyboard instances (future)
        self.role     = {}    # role_name → module object, populated by load_roles()
        self._registry = {}   # uid_hex → module object

    # ── Low-level I2C ─────────────────────────────────────────────────────────

    def _read(self, addr, n):
        buf = bytearray(n)
        while not self.i2c.try_lock():
            pass
        try:
            self.i2c.readfrom_into(addr, buf)
            return buf
        except OSError:
            return None
        finally:
            self.i2c.unlock()

    def _write(self, addr, data):
        while not self.i2c.try_lock():
            pass
        try:
            self.i2c.writeto(addr, bytes(data))
            return True
        except OSError:
            return False
        finally:
            self.i2c.unlock()

    # ── Enumeration ───────────────────────────────────────────────────────────

    def enumerate(self, total_timeout_sec=10):
        """
        Discover all modules on the bus.
        Polls 0x7F every 20 ms. Stops after 500 ms of no response.
        Returns total number of modules found.
        """
        print("Enumerating noknok modules...")
        self.buzzer    = []
        self.knob      = []
        self.keyboard  = []
        self._registry = {}
        self.role      = {}

        # ── Step 1: Restore already-assigned modules ──────────────────────────
        # Ping each address in the pool. If a module is already assigned there
        # from a previous run (it keeps its address until power-cycled), restore
        # it directly without needing to go through 0x7F enumeration again.
        restored = self._restore_state()
        if restored > 0:
            print(f"  {restored} module(s) already assigned.")

        # Determine next free address (skip ones already in use)
        used = {m.address for m in self._registry.values() if m is not None}
        next_addr = 0x08
        while next_addr in used:
            next_addr += 1

        # ── Step 2: Scan 0x7F for new (unassigned) modules ───────────────────
        # Use a shorter timeout when we already have modules — if nothing new
        # appears on 0x7F within 1 s, we're done. Use 3 s when starting fresh
        # to handle re-backoffs after collisions.
        no_resp_limit = 1000 if restored > 0 else 3000
        no_resp_ms    = 0
        deadline      = time.monotonic() + total_timeout_sec
        new_found     = 0

        while no_resp_ms < no_resp_limit and time.monotonic() < deadline:

            buf = self._read(self.ENUM_ADDR, 10)

            if buf is None:
                no_resp_ms += 20
                time.sleep(0.02)
                continue

            no_resp_ms = 0

            # Verify CRC
            if _crc8(buf[:9]) != buf[9]:
                print("  CRC mismatch — possible collision, retrying...")
                time.sleep(0.05)
                continue

            uid_hex     = bytes(buf[:8]).hex()
            module_type = buf[8]
            addr        = next_addr
            next_addr  += 1

            # Assign address
            self._write(self.ENUM_ADDR, [self.ASSIGN_REG, addr])
            time.sleep(0.05)

            # Instantiate correct class and store UID on the object
            if module_type == self.TYPE_BUZZER:
                module    = NoknokBuzzer(self.i2c, address=addr)
                type_name = "Buzzer"
                self.buzzer.append(module)
            else:
                module    = None
                type_name = f"Unknown(0x{module_type:02X})"

            if module is not None:
                module._uid_hex = uid_hex

            self._registry[uid_hex] = module
            new_found += 1
            print(f"  {type_name} → 0x{addr:02X}  UID: {uid_hex}  [new]")

            time.sleep(0.02)

        # ── Step 3: Save state so next run can restore without 0x7F scan ─────
        self._save_state()

        # ── Summary ───────────────────────────────────────────────────────────
        total = sum([len(self.buzzer), len(self.knob), len(self.keyboard)])
        if new_found == 0 and restored > 0:
            print(f"No new modules. {restored} module(s) already assigned:")
            for uid, m in self._registry.items():
                if m: print(f"  {type(m).__name__} at 0x{m.address:02X}  UID: {uid}")
        else:
            print(f"Done — {total} module(s) ({restored} restored, {new_found} new).")
        return total

    # ── State persistence ────────────────────────────────────────────────────

    def _save_state(self, filename="noknok_state.json"):
        """Save current module assignments to JSON so next run can restore them."""
        data = {}
        for uid_hex, module in self._registry.items():
            if module is not None:
                t = self.TYPE_BUZZER   if isinstance(module, NoknokBuzzer) else 0
                data[uid_hex] = {"address": module.address, "type": t}
        try:
            with open(filename, "w") as f:
                json.dump(data, f)
        except OSError:
            pass   # read-only filesystem — silently skip

    def _restore_state(self, filename="noknok_state.json"):
        """
        Load saved state and ping each module at its known address.
        Modules that respond are added to the registry directly.
        Returns the number of modules successfully restored.
        """
        try:
            with open(filename, "r") as f:
                data = json.load(f)
        except OSError:
            return 0   # no saved state yet

        restored = 0
        for uid_hex, info in data.items():
            addr      = info.get("address", 0)
            type_code = info.get("type",    0)

            # Ping: try to read 1 status byte from the saved address
            if self._read(addr, 1) is None:
                continue   # module not there (disconnected or power-cycled)

            if type_code == self.TYPE_BUZZER:
                module = NoknokBuzzer(self.i2c, address=addr)
                module._uid_hex = uid_hex
                self.buzzer.append(module)
            else:
                module = None

            if module is not None:
                self._registry[uid_hex] = module
                restored += 1

        return restored

    # ── Role management ───────────────────────────────────────────────────────

    def load_roles(self, filename="noknok_roles.json"):
        """
        Load role assignments from a JSON file on the Pico's CIRCUITPY drive.

        The file maps role names to UIDs:
            {
              "volume_knob":   "e2afabcd4af0bc74",
              "menu_knob":     "e292abcd4ad3bc74",
              "alert_buzzer":  "e290abcd4ad1bc74"
            }

        After loading:
            c.role["volume_knob"].value
            c.role["alert_buzzer"].play(880, 200)

        Returns True if all roles were found, False if any are missing.
        """
        try:
            with open(filename, "r") as f:
                mapping = json.load(f)
        except OSError:
            print(f"  No roles file found at '{filename}'")
            print(f"  Run c.setup_roles() to create one.")
            return False

        print(f"Loading roles from '{filename}'...")
        self.role = {}
        missing   = []

        for role_name, uid_hex in mapping.items():
            uid_hex = uid_hex.lower().replace("-", "").replace(" ", "")
            module  = self._registry.get(uid_hex)
            if module is not None:
                self.role[role_name] = module
                type_name = type(module).__name__
                print(f"  '{role_name}' → {type_name} at 0x{module.address:02X}")
            else:
                self.role[role_name] = None
                missing.append(role_name)
                print(f"  '{role_name}' → NOT FOUND  (UID: {uid_hex})")

        if missing:
            print(f"  ⚠ {len(missing)} role(s) not found: {', '.join(missing)}")
        else:
            print(f"  All {len(self.role)} role(s) loaded.")

        return len(missing) == 0

    def save_roles(self, mapping, filename="noknok_roles.json"):
        """
        Save a role mapping dict to a JSON file.
        mapping = { "role_name": module_object, ... }

        Example:
            c.save_roles({
                "volume_knob":  c.knob[0],
                "alert_buzzer": c.buzzer[0],
            })
        """
        data = {}
        for role_name, module in mapping.items():
            if module is not None and hasattr(module, "_uid_hex"):
                data[role_name] = module._uid_hex
            else:
                print(f"  ⚠ Skipping '{role_name}' — no UID available")

        with open(filename, "w") as f:
            json.dump(data, f)

        print(f"Saved {len(data)} role(s) to '{filename}'")

    def setup_roles(self, filename="noknok_roles.json"):
        """
        Interactive role assignment wizard. Run once from the Thonny REPL.

        Walks through every discovered module, plays/activates it so you know
        which physical unit it is, then asks you to type a role name.
        Saves the result to noknok_roles.json.

        Example session:
            >>> c.enumerate()
            >>> c.setup_roles()
            Module 1/3: Buzzer at 0x08  (UID: e2afabcd4af0bc74)
            Playing a beep so you can identify it...
            Role name (Enter to skip): alert_buzzer
            → assigned as 'alert_buzzer'
            ...
            Saved 2 role(s) to 'noknok_roles.json'
        """
        all_modules = []
        for uid_hex, module in self._registry.items():
            if module is not None:
                all_modules.append((uid_hex, module))

        if not all_modules:
            print("No modules found. Run enumerate() first.")
            return

        print(f"\nRole setup wizard — {len(all_modules)} module(s) found.")
        print("For each module: identify it, then type a role name or press Enter to skip.")
        print("The role name is how you'll refer to it in your code: c.role[\"name\"]\n")

        assignment = {}

        for i, (uid_hex, module) in enumerate(all_modules):
            type_name = type(module).__name__
            print(f"Module {i+1}/{len(all_modules)}: {type_name} at 0x{module.address:02X}  (UID: {uid_hex})")

            # Identify the module — activate it so the user knows which one it is
            if isinstance(module, NoknokBuzzer):
                print("  → Playing a beep so you can identify it...")
                module.tune(module.BEEP_OK)
                time.sleep(0.5)

            role = input("  Role name (or Enter to skip): ").strip()

            if role:
                assignment[role] = module._uid_hex
                print(f"  → assigned as '{role}'\n")
            else:
                print(f"  → skipped\n")

        if assignment:
            with open(filename, "w") as f:
                json.dump(assignment, f)
            print(f"Saved {len(assignment)} role(s) to '{filename}'")
            print(f"\nIn your app code:")
            print(f"  c.enumerate()")
            print(f"  c.load_roles()")
            for role_name in assignment:
                print(f"  c.role[\"{role_name}\"]  # always this physical module")
        else:
            print("No roles assigned. File not written.")

    # ── Lookup ────────────────────────────────────────────────────────────────

    def by_uid(self, uid_hex):
        """Return a module by its UID hex string (hyphens and spaces ignored)."""
        key = uid_hex.lower().replace("-", "").replace(" ", "")
        return self._registry.get(key)


# ═════════════════════════════════════════════════════════════════════════════
class NoknokBuzzer:
    """
    Driver for the noknok Buzzer Module (CH32V003, firmware v3+).

    Normally obtained via Conductor.enumerate():
        c = Conductor()
        c.enumerate()
        b = c.buzzer[0]            # by discovery index
        b = c.role["alert_buzzer"] # by role name (after load_roles)
    """

    NOKIA           = 1
    HAPPY_BIRTHDAY  = 2
    BEEP_OK         = 3
    BEEP_ERROR      = 4
    STARTUP         = 5

    _CMD_STOP      = 0x00
    _CMD_PLAY_NOTE = 0x01
    _CMD_PLAY_TUNE = 0x02

    def __init__(self, i2c, address=0x08):
        self.i2c      = i2c
        self.address  = address
        self._uid_hex = None   # set by Conductor.enumerate()

    def _send(self, data):
        while not self.i2c.try_lock():
            pass
        try:
            self.i2c.writeto(self.address, bytes(data))
        finally:
            self.i2c.unlock()

    def _read(self, n=1):
        buf = bytearray(n)
        while not self.i2c.try_lock():
            pass
        try:
            self.i2c.readfrom_into(self.address, buf)
        finally:
            self.i2c.unlock()
        return buf

    def play(self, freq_hz, duration_ms, volume=100):
        """Play a single note. Fire and forget — returns immediately."""
        if freq_hz <= 0:
            return self.stop()
        dur     = max(1, int(duration_ms / 100))
        vol     = max(0, min(100, volume))
        freq_hi = (freq_hz >> 8) & 0xFF
        freq_lo =  freq_hz       & 0xFF
        self._send([self._CMD_PLAY_NOTE, freq_hi, freq_lo, dur, vol])

    beep = play   # backwards compatibility

    def note(self, freq_hz, duration_ms, volume=100, gap_ms=50):
        """Play a note and wait until it finishes. Use in melodies."""
        self.play(freq_hz, duration_ms, volume)
        time.sleep((duration_ms + gap_ms) / 1000)

    def tune(self, tune_id):
        """Play a preloaded tune. Fire and forget."""
        self._send([self._CMD_PLAY_TUNE, tune_id])

    def stop(self):
        """Stop playback immediately."""
        self._send([self._CMD_STOP])

    def is_playing(self):
        """Returns True if currently playing."""
        return self._read(1)[0] == 0x01

    def wait(self, timeout_sec=30):
        """Block until idle or timeout. Returns True if idle."""
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            if not self.is_playing():
                return True
            time.sleep(0.05)
        return False
