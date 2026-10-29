#!/usr/bin/env python3
# Patch the TGL VA driver (sha256 d265f203...) for Tahoe's video context ABI.
# usage: patch-vadriver.py <in> <out>
import hashlib, sys

src, dst = sys.argv[1:3]
data = bytearray(open(src, 'rb').read())
if not hashlib.sha256(data).hexdigest().startswith('d265f2038135c2c5'):
    sys.exit('unexpected driver build')

patches = [
    # The video context status block shrank by one dword on Tahoe:
    # 0x34b dwords (0xd2c bytes, 0xd30 with header) -> 0x34a.
    *[(off, b'\x4b', b'\x4a') for off in
      (0x5a7cc, 0x5a7d7, 0x5a835, 0x5a985, 0x5a990, 0x5a9d9, 0x5ad7d, 0x5aeaf, 0x5afef, 0x5b01c)],
    *[(off, b'\x2c', b'\x28') for off in (0x5a807, 0x5a9b8)],
    (0x5ad12, b'\x30', b'\x2c'),
    # Always run the post-submit step at 0x640b0.
    (0x64a15, bytes.fromhex('0f8412000000'), bytes.fromhex('660f1f440000')),
]
for off, old, new in patches:
    if data[off:off + len(old)] != old:
        sys.exit(f'{off:#x}: {data[off:off + len(old)].hex()} != {old.hex()}')
    data[off:off + len(new)] = new
open(dst, 'wb').write(data)
