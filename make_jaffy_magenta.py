"""Put the rebuilt magenta palette into the verified menu-0.1.2 image."""
from pathlib import Path
import hashlib

here = Path(__file__).resolve().parent
directory = here / 'build-v080'
baseline = directory / 'onerom-fire-24-e-0.8.0-rgb-menu0.1.2-candidate.bin'
image = baseline.read_bytes()
assert hashlib.sha256(image).hexdigest() == 'f8b062ea70a461ba0eaaf1d61ac03ee5adcce0ebe07f8fa7e18c32fb06e0703d', 'Baseline changed'
plugin = bytearray((here.parent / 'one-rom-v0.8.0/plugins/user/host-control/build/host_control_plugin.bin').read_bytes())
assert len(plugin) == 65536
old_plugin = image[0x20000:0x30000]
changes = [(i, old_plugin[i], plugin[i]) for i in range(0xf000) if old_plugin[i] != plugin[i]]
assert changes == [(21814, 255, 0), (21815, 0, 255)], changes
plugin[0xf000:] = old_plugin[0xf000:]
updated = image[:0x20000] + plugin + image[0x30000:]
assert len(updated) == len(image)
assert sum(a != b for a, b in zip(image, updated)) == 2
destination = directory / 'onerom-fire-24-e-0.8.0-menu0.1.2-jaffy-magenta.bin'
destination.write_bytes(updated)
print('Verified: only two JaffyDOS palette bytes changed from the working yellow image.')
print('SHA256:', hashlib.sha256(updated).hexdigest())
print(destination)
