import time

class NoknokBuzzer:
    CMD_STOP = 0x00
    CMD_BEEP = 0x01

    def __init__(self, i2c, address=0x45):
        self.i2c = i2c
        self.address = address

    def beep(self, freq_hz, duration_ms):
        if freq_hz <= 0:
            return self.stop()

        dur_100ms = max(1, int(duration_ms / 100))
        freq_hi = (freq_hz >> 8) & 0xFF
        freq_lo = freq_hz & 0xFF

        data = bytes([self.CMD_BEEP, freq_hi, freq_lo, dur_100ms])

        while not self.i2c.try_lock():
            pass
        try:
            self.i2c.writeto(self.address, data)
        finally:
            self.i2c.unlock()

    def stop(self):
        while not self.i2c.try_lock():
            pass
        try:
            self.i2c.writeto(self.address, bytes([self.CMD_STOP]))
        finally:
            self.i2c.unlock()
