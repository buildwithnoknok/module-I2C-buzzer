import time
import board
import busio
from buzzer import NoknokBuzzer

i2c = busio.I2C(board.GP9, board.GP8, frequency=100000)

while not i2c.try_lock():
    pass
found = 0x45 in i2c.scan()
i2c.unlock()
if not found:
    raise SystemExit("Buzzer not found on I2C bus")

b = NoknokBuzzer(i2c)

# ── Note frequencies (Hz) ────────────────────────────────────────────────
C4 = 262
D4 = 294
E4 = 330
F4 = 349
G4 = 392

# ── Note durations (ms) ─────────────────────────────────────────────────
q  = 400   # quarter note
e  = 200   # eighth note
dq = 600   # dotted quarter (q + e)
h  = 800   # half note

GAP = 0.05  # brief silence between notes so repeated notes are distinct

def note(freq, ms):
    b.beep(freq, ms)
    time.sleep(ms / 1000 + GAP)

# ── Ode to Joy — full chorus ─────────────────────────────────────────────
chorus = [
    # Line 1
    (E4,q),(E4,q),(F4,q),(G4,q),
    (G4,q),(F4,q),(E4,q),(D4,q),
    (C4,q),(C4,q),(D4,q),(E4,q),
    (E4,dq),(D4,e),(D4,h),
    # Line 2
    (E4,q),(E4,q),(F4,q),(G4,q),
    (G4,q),(F4,q),(E4,q),(D4,q),
    (C4,q),(C4,q),(D4,q),(E4,q),
    (D4,dq),(C4,e),(C4,h),
    # Bridge
    (D4,q),(D4,q),(E4,q),(C4,q),
    (D4,q),(E4,e),(F4,e),(E4,q),(C4,q),
    (D4,q),(E4,e),(F4,e),(E4,q),(D4,q),
    (C4,q),(D4,q),(G4,h),
    # Line 4 (same ending as Line 2)
    (E4,q),(E4,q),(F4,q),(G4,q),
    (G4,q),(F4,q),(E4,q),(D4,q),
    (C4,q),(C4,q),(D4,q),(E4,q),
    (D4,dq),(C4,e),(C4,h),
]

print("Playing Ode to Joy — press Ctrl+C to stop.")
while True:
    for freq, ms in chorus:
        note(freq, ms)
    time.sleep(1)   # short pause before repeating