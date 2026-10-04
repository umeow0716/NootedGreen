#!/usr/bin/env python3
"""Execute pinned KC pool free offline; event/allocator callbacks are mocked."""
import hashlib
import struct
import sys
from pathlib import Path
from unicorn import Uc, UC_ARCH_X86, UC_MODE_64, UC_HOOK_CODE
from unicorn.x86_const import *

image = Path(sys.argv[1]).read_bytes()
assert hashlib.sha256(image[0x14b6afec:0x14b6b128]).hexdigest() == \
    '739b44800bc58c97f593876b5e102076a8b17adcdda60c512129fff840b580d3'
assert hashlib.sha256(image[0x14b82226:0x14b822c0]).hexdigest() == \
    'fcc000816e19074bcb33ab0425fb6bca1615d6b623f8aa42f2d17759e6311c81'


def run(slots, record, current=-1, linked=False):
    assert current == -1 or 0 <= current < slots
    uc = Uc(UC_ARCH_X86, UC_MODE_64)
    uc.mem_map(0x14b60000, 0xb0000)
    uc.mem_write(0x14b60000, image[0x14b60000:0x14c10000])
    uc.mem_map(0x10000, 0x1000)
    uc.mem_map(0x300000, 0x310000)
    def put(a, v): uc.mem_write(a, struct.pack('<Q', v))
    def get(a): return struct.unpack('<Q', uc.mem_read(a, 8))[0]
    pool, accel, channel, vt = 0x410000, 0x420000, 0x430000, 0x440000
    put(pool + 0x10, accel)
    put(pool + 0x20, channel)
    if linked:
        put(accel + 0xc68, pool)
        uc.mem_write(accel + 0xc70, struct.pack('<I', 2))
        put(pool + 0x18, 0x4f2000)
    uc.mem_write(pool + 0x1842, struct.pack('<h', current))
    put(pool + 0x1860, 0x480000 if record else 0)
    put(accel, vt)
    put(vt + 0x8e0, 0x600020)
    put(channel, 0x450000)
    task_cell = 0x14b6b03b + struct.unpack_from('<i', image, 0x14b6b037)[0]
    put(task_cell, 0x4f0000)
    put(0x4f0000, 0x4f1000)
    put(0x450000 + 0x28, 0x600030)
    # Base free imported vtable pointer read near the final tail jump.
    cell = 0x14b6b114 + struct.unpack_from('<i', image, 0x14b6b110)[0]
    put(cell, 0x460000)
    put(0x460000 + 0xa0, 0x600040)
    put(0x470000 + 0x28, 0x600050)
    put(0x470000 + 0x140, 0x600070)
    memories = []
    for i in range(slots):
        memory, mapping = 0x490000 + i * 0x1000, 0x4c0000 + i * 0x1000
        memories.append(memory)
        put(memory, 0x470000)
        put(mapping, 0x470000)
        for off, value in ((0, memory), (8, mapping), (16, 0x500000 + i * 4096)):
            put(pool + 0x30 + i * 24 + off, value)
    events = []
    def ret():
        sp = uc.reg_read(UC_X86_REG_RSP)
        uc.reg_write(UC_X86_REG_RAX, 0)
        uc.reg_write(UC_X86_REG_RIP, get(sp))
        uc.reg_write(UC_X86_REG_RSP, sp + 8)
    def hook(machine, address, size, data):
        labels = {0x1030c: 'unlinked-log', 0x14bb7896: 'finish-event',
                  0x14bba678: 'remove-cpu', 0x600020: 'record-release',
                  0x600030: 'channel-release', 0x600040: 'base-free',
                  0x600050: 'object-release', 0x600070: 'complete-current'}
        if address in labels:
            if address == 0x600070:
                assert uc.reg_read(UC_X86_REG_RDI) == 0x4c0000 + current * 0x1000
            events.append(labels[address])
            ret()
        elif address == 0x600060:
            uc.emu_stop()
    uc.hook_add(UC_HOOK_CODE, hook)
    sp = 0x5ff008
    put(sp, 0x600060)
    uc.reg_write(UC_X86_REG_RSP, sp)
    uc.reg_write(UC_X86_REG_RDI, pool)
    uc.emu_start(0x14b6afec, 0x600061, count=20000)
    assert uc.reg_read(UC_X86_REG_RIP) == 0x600060
    expected = [] if linked else ['unlinked-log']
    if current >= 0:
        expected += ['complete-current']
    for _ in range(slots):
        expected += ['finish-event', 'object-release', 'remove-cpu', 'object-release']
    expected += ['channel-release'] + (['record-release'] if record else []) + ['base-free']
    assert events == expected, events
    assert get(accel + 0xc68) == (0x4f2000 if linked else 0)
    assert struct.unpack('<I', uc.mem_read(accel + 0xc70, 4))[0] == int(linked)
    assert get(pool + 0x18) == 0
    assert all(get(pool + 0x30 + i * 24 + j * 8) == 0
               for i in range(256) for j in range(3))
    assert all(uc.mem_read(m + 0xc, 1)[0] & 1 for m in memories)
    assert get(pool + 0x20) == get(pool + 0x1860) == 0
    assert uc.mem_read(pool + 0x1842, 2) == b'\xfe\xff'
    assert all(get(pool + off) == 0 for off in (0x1848, 0x1850, 0x1858))
    assert uc.reg_read(UC_X86_REG_RSP) == sp + 8


cases = 0
for slots in (0, 1, 2):
    for record in (False, True):
        for current in range(-1, slots):
            for linked in (False, True):
                run(slots, record, current, linked)
                cases += 1
assert cases == 24
print('PASS 24 KC partial/current/linked-pool free fixtures; complete precedes release;'
      ' callbacks mocked, no actual event/DMA quiescence proof')
