# noknok.py
# CircuitPython library for the noknok modular ecosystem
# Raspberry Pi Pico — I2C master ("Conductor")
#
# Usage:
#   from noknok import Conductor
#   c = Conductor()
#   c.enumerate()
#   c.buzzer[0].play(440, 500)
#   c.buzzer[1].tune(c.buzzer[1].NOKIA)

import busio
import board
import time


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

    Usage:
        c = Conductor()          # GP8=SDA, GP9=SCL (noknok standard)
        c.enumerate()            # discover all modules (~3 s)
        c.buzzer[0].play(440, 500)
        c.buzzer[1].tune(c.buzzer[1].NOKIA)
    """

    ENUM_ADDR  = 0x7F
    ASSIGN_REG = 0x1D

    # Module type codes — must match MODULE_TYPE in firmware
    TYPE_BUZZER   = 0x01
    TYPE_KNOB     = 0x02
    TYPE_KEYBOARD = 0x03

    def __init__(self, sda=board.GP8, scl=board.GP9, frequency=100_000):
        self.i2c    = busio.I2C(scl, sda, frequency=frequency)
        self.buzzer = []          # list of NoknokBuzzer — indexed by discovery order
        self._registry = {}       # uid_hex -> module object

    # ── Low-level I2C helpers ─────────────────────────────────────────────────

    def _read(self, addr, n):
        """Read n bytes from addr. Returns bytearray or None on NACK."""
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
        """Write bytes to addr. Returns True on success."""
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

        Polls 0x7F every 20 ms. Each responding module sends a 10-byte packet:
            [UID 8 bytes] [MODULE_TYPE] [CRC8]
        The Conductor assigns each module a unique address (0x08 upward).
        Stops after 500 ms of no response.

        Returns the total number of modules found.
        """
        print("Enumerating noknok modules...")
        self.buzzer    = []
        self._registry = {}

        next_addr    = 0x08
        no_resp_ms   = 0
        deadline     = time.monotonic() + total_timeout_sec

        while no_resp_ms < 500 and time.monotonic() < deadline:

            buf = self._read(self.ENUM_ADDR, 10)

            if buf is None:
                no_resp_ms += 20
                time.sleep(0.02)
                continue

            # Got a response — reset idle counter
            no_resp_ms = 0

            # Verify CRC
            if _crc8(buf[:9]) != buf[9]:
                print("  CRC mismatch — possible collision, retrying...")
                time.sleep(0.05)
                continue

            uid         = bytes(buf[:8])
            uid_hex     = uid.hex()
            module_type = buf[8]
            addr        = next_addr
            next_addr  += 1

            # Assign unique address
            self._write(self.ENUM_ADDR, [self.ASSIGN_REG, addr])
            time.sleep(0.05)   # give module time to switch address

            # Instantiate correct class
            if module_type == self.TYPE_BUZZER:
                module    = NoknokBuzzer(self.i2c, address=addr)
                type_name = "Buzzer"
                self.buzzer.append(module)
            else:
                module    = None
                type_name = f"Unknown(0x{module_type:02X})"

            self._registry[uid_hex] = module
            print(f"  [{len(self.buzzer) + (1 if module is None else 0) - 1}] "
                  f"{type_name} → 0x{addr:02X}  UID: {uid_hex}")

            time.sleep(0.02)

        total = len(self.buzzer)
        print(f"Done — {total} module(s) found.")
        return total

    def by_uid(self, uid_hex):
        """Return a module by its UID hex string (hyphens ignored)."""
        return self._registry.get(uid_hex.lower().replace('-', '').replace(' ', ''))


# ═════════════════════════════════════════════════════════════════════════════
class NoknokBuzzer:
    """
    Driver for the noknok Buzzer Module (CH32V003, firmware v3+).

    Normally obtained via Conductor.enumerate():
        c = Conductor()
        c.enumerate()
        b = c.buzzer[0]

    Can also be used standalone with a known address:
        b = NoknokBuzzer(i2c, address=0x08)
    """

    # ── Preloaded tune IDs ────────────────────────────────────────────────────
    NOKIA           = 1
    HAPPY_BIRTHDAY  = 2
    BEEP_OK         = 3
    BEEP_ERROR      = 4
    STARTUP         = 5

    _CMD_STOP      = 0x00
    _CMD_PLAY_NOTE = 0x01
    _CMD_PLAY_TUNE = 0x02

    def __init__(self, i2c, address=0x08):
        self.i2c     = i2c
        self.address = address

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
        """Play a single note. Fire and forget."""
        if freq_hz <= 0:
            return self.stop()
        dur     = max(1, int(duration_ms / 100))
        vol     = max(0, min(100, volume))
        freq_hi = (freq_hz >> 8) & 0xFF
        freq_lo =  freq_hz       & 0xFF
        self._send([self._CMD_PLAY_NOTE, freq_hi, freq_lo, dur, vol])

    beep = play   # backwards compatibility alias

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
