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
    # Tahoe requests the H.264 encoder by its 0x20/0x4000 description entry;
    # create the driver's VDEnc encoder (requested as 0x400 before) for it...
    (0x14382, bytes.fromhex('483d00040000'), bytes.fromhex('483d00400000')),
    # ...and route its parameters to it (in place of the unused 0x2000 entry).
    (0x15e72, bytes.fromhex('4881fa00200000'), bytes.fromhex('4881fa00400000')),
    # The VDEnc encoder's SetParameter accepts entries of its own codec value.
    (0x27214, bytes.fromhex('48817e0800040000'), bytes.fromhex('48817e0800400000')),
    # VDEnc starts with MB statistics stream-out on, writing into a pool that
    # only a separate stream-out accelerator would allocate; start it off.
    (0x26cda, bytes.fromhex('c786906a000001000001'), bytes.fromhex('c786906a000000000001')),
    # Encoder teardown releases a CF object that may never have been created.
    (0x63d15, bytes.fromhex('488b45f0488b40584889c7e85ba31d00488b45f04889c7be48010000e8fafafdff'),
     bytes.fromhex('488b45f0488b78584885ff7405e859a31d00488b7df0be48010000e8fbfafdff90')),
    # 1080p encode goes through the renderer, whose absent VEBOX stage
    # (+0x910[idx]) is passed to this setter: return when it is NULL.
    (0x126524, bytes.fromhex('48897df8488975f0488955e8488b45f8488b4de8'),
     bytes.fromhex('488975f0488955e84889f84889d14885c0745890')),
    # The VEBOX render pass has no VEBOX stage here (+0x910 is NULL); make it
    # a no-op so 1080p encode input goes on to the composite stage.
    (0x131d42, bytes.fromhex('0f85bc000000'), bytes.fromhex('90e9bc000000')),
    # The video processor cannot render here (no VEBOX); ngva does the
    # scaler's work on the CPU, so complete the job with its stamp anyway.
    (0x235e37, bytes.fromhex('0f840c000000'), bytes.fromhex('90e90c000000')),
    # H.264 encode: the first two of the three encoder stages at +0x910 are
    # not created on this hardware; skip their setup call when absent.
    (0x132818, bytes.fromhex('488b88100900' '00488b75f0488b90c8090000488b394889bd48ffffff4889cf'
                             '488b8d48ffffffff51108945dc837ddc000f8405000000e959010000'),
     bytes.fromhex('488b88100900004885c9742f488b75f0488b90c80900004889cf488b09ff5110'
                   '8945dc85c00f8569010000' + '90' * 16)),
    (0x13285a, bytes.fromhex('488b88180900' '00488b75f0488b90c8090000488b394889bd40ffffff4889cf'
                             '488b8d40ffffffff51108945dc837ddc000f8405000000e917010000'),
     bytes.fromhex('488b88180900004885c9742f488b75f0488b90c80900004889cf488b09ff5110'
                   '8945dc85c00f8527010000' + '90' * 16)),
    # H.264 decode (codechal) submit hook: 0x64df0 jumps to a trampoline in the
    # padding at 0x2c131 that calls the pointer at 0x75f000 (zero __DATA_CONST
    # slack, set by ngva) when set, else runs the original prologue (ngva calls
    # 0x2c13f for it) and continues at 0x64df8.
    (0x2c131, bytes.fromhex('662e0f1f8400000000000f1f440000909090909090909090909090'),
     bytes.fromhex('488b05c82e73004885c07402ffe0554889e54883ec50e9ac8c0300')),
    (0x64df0, bytes.fromhex('554889e54883ec50'), bytes.fromhex('e93c73fcff0f1f00')),
]
for off, old, new in patches:
    if data[off:off + len(old)] != old:
        sys.exit(f'{off:#x}: {data[off:off + len(old)].hex()} != {old.hex()}')
    data[off:off + len(new)] = new
open(dst, 'wb').write(data)
