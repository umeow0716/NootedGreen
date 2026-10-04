#!/usr/bin/env python3
"""Offline Unicorn execution of pinned native constructor; no hardware access."""
import hashlib
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *


def run(image, success, candidate=False):
    uc = Uc(UC_ARCH_X86, UC_MODE_64)
    uc.mem_map(0, (len(image) + 0xfff) & ~0xfff)
    uc.mem_write(0, image)
    uc.mem_map(0x300000, 0x310000)
    context, task, accel, channel, params = (0x410000, 0x400000,
                                            0x420000, 0x430000, 0x440000)
    pool, vt, meta, mvt = 0x450000, 0x460000, 0x470000, 0x480000
    def put(a, v): uc.mem_write(a, struct.pack('<Q', v))
    def get(a): return struct.unpack('<Q', uc.mem_read(a, 8))[0]
    put(context, 0x490000)
    put(0x490000 + 0x130, 0x600030)
    put(0xc80d8, 0x4a0000)
    put(0x4a0000, meta)
    put(meta, mvt)
    put(mvt + 0x88, 0x600010)
    put(pool, vt)
    put(vt + 0x118, 0x600020)
    put(pool + 0x1860, 0xa5a5a5a5a5a5a5a5)
    def jump(source, target):
        return b'\xe9' + struct.pack('<i', target - source - 5)
    if candidate:
        # Offline caves only; no deployable patch or executable-cave claim.
        # Preserve the displaced stack cleanup and backing-size load.
        assert image[0x7cf55:0x7cf5e] == bytes.fromhex('48 83 c4 20 49 8b 74 24 10')
        uc.mem_write(0x7cf55, jump(0x7cf55, 0x600100) + b'\x90' * 4)
        cave = bytes.fromhex('48 83 c4 20 84 c0 0f 84')
        cave += struct.pack('<i', 0x7cfa3 - (0x600100 + 12))
        cave += bytes.fromhex('49 8b 74 24 10')
        cave += jump(0x600100 + len(cave), 0x7cf5e)
        uc.mem_write(0x600100, cave)
        # Model a future init wrapper's explicit record initialization in x86,
        # then tail to the same mocked callback. It does not change init ABI.
        shim = bytes.fromhex('48 c7 87 60 18 00 00 00 00 00 00')
        uc.mem_write(0x600020, shim + jump(0x600020 + len(shim), 0x600050))
    for offset, value in ((0x10, 0xd240), (0x18, 256),
                          (0x20, 65536), (0x28, 64), (0x30, 8)):
        put(params + offset, value)
    events = []
    def ret(value):
        sp = uc.reg_read(UC_X86_REG_RSP)
        uc.reg_write(UC_X86_REG_RAX, value)
        uc.reg_write(UC_X86_REG_RIP, get(sp))
        uc.reg_write(UC_X86_REG_RSP, sp + 8)
    def hook(machine, address, size, data):
        if address == 0x7b9d4:
            put(context + 0x50, accel)
            put(context + 0xb8, channel)
            ret(1)
        elif address == 0x600010:
            events.append('allocate')
            ret(pool)
        elif address == (0x600050 if candidate else 0x600020):
            args = [uc.reg_read(r) for r in (UC_X86_REG_RDI,
                    UC_X86_REG_RSI, UC_X86_REG_RDX, UC_X86_REG_RCX,
                    UC_X86_REG_R8, UC_X86_REG_R9)]
            sp = uc.reg_read(UC_X86_REG_RSP)
            args += [get(sp + 8 + i * 8) for i in range(4)]
            assert args == [pool, accel, channel, task, 256, 65536,
                            0x300, 1, 64, 8], args
            assert get(pool + 0x1860) == (0 if candidate else 0xa5a5a5a5a5a5a5a5)
            events.append('init')
            ret(int(success))
        elif address == 0x10924:
            assert uc.reg_read(UC_X86_REG_RDI) == task
            assert uc.reg_read(UC_X86_REG_RSI) == 0xd240
            events.append('backing')
            ret(0x4b0000)
        elif address == 0x600030:
            events.append('setup')
            ret(0)
        elif address == 0x600040:
            uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook)
    sp = 0x5ff008
    put(sp, 0x600040)
    for r, v in ((UC_X86_REG_RSP, sp), (UC_X86_REG_RDI, context),
                 (UC_X86_REG_RSI, task), (UC_X86_REG_RDX, params)):
        uc.reg_write(r, v)
    saved = {r: 0x123000 + i * 16 for i, r in enumerate((
        UC_X86_REG_RBX, UC_X86_REG_RBP, UC_X86_REG_R12,
        UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15))}
    for r, v in saved.items():
        uc.reg_write(r, v)
    uc.emu_start(0x7cebc, 0x600041, count=1000)
    assert uc.reg_read(UC_X86_REG_RIP) == 0x600040
    expected_ok = not candidate or success
    assert uc.reg_read(UC_X86_REG_RAX) & 0xff == int(expected_ok)
    assert events == (['allocate', 'init', 'backing', 'setup']
                      if expected_ok else ['allocate', 'init']), events
    assert get(context + 0xe0) == pool
    assert get(context + 0xd8) == (0x4b0000 if expected_ok else 0)
    assert uc.reg_read(UC_X86_REG_RSP) == sp + 8
    assert all(uc.reg_read(r) == v for r, v in saved.items())


image = Path(sys.argv[1]).read_bytes()
assert hashlib.sha256(image[0x7cebc:0x7cfb0]).hexdigest() == \
    '67dfb7530b4142f6a2186b617e396df18dcb46517e45ab2ce540a38c1cc20fd1'
for candidate in (False, True):
    for result in (False, True):
        run(image, result, candidate)
print('PASS native constructor ABI and offline candidate failure branch;'
      ' original ignores false, candidate skips backing/setup;'
      ' no production cave, outer cleanup or GPU proof')
