# noknok_setup_roles.py
# Run this ONCE from Thonny to assign role names to your physical modules.
# The result is saved to noknok_roles.json on the Pico's CIRCUITPY drive.
#
# After running this, all your apps can use:
#   c.enumerate()
#   c.load_roles()
#   c.role["volume_knob"].value
#   c.role["alert_buzzer"].play(880, 200)
#
# Re-run any time you want to change role assignments or add new modules.

from noknok import Conductor

c = Conductor()
c.enumerate()
c.setup_roles()
