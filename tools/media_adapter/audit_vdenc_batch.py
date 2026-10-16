#!/usr/bin/env python3
"""Offline packet/rebase audit; NEVER emits a GPU-submittable command buffer.

Input is the saved pre-relocation AVC command buffer, not a live mapping.
Gen12 sizes are pinned to intel/media-driver 2a32c7fafd8fa169112e962587777969cc8c654f.
Size differences describe producer ABI, not hardware validity or a hang verdict.
"""
import argparse
import json
import struct
from pathlib import Path

# Fixed-size native packets observed in the saved frame. PAK_INSERT is variable.
SIZES = {0x13000000: 5, 0x10000000: 5, 0x12000000: 4,
         0x70000000: 5, 0x70010000: 6, 0x70020000: 68,
         0x70030000: 26, 0x70040000: 10, 0x70070000: 18,
         0x70080000: 34, 0x71000000: 21, 0x71030000: 11,
         0x70800000: 2, 0x70810000: 6, 0x70820000: 6,
         0x70830000: 6, 0x70840000: 37, 0x70850000: 35,
         0x70860000: 61, 0x70870000: 4, 0x77800000: 2}
EXPANSIONS = {0x70800000: 6, 0x70830000: 10,
              0x70840000: 71, 0x70870000: 27}

def audit(data):
    if not data or len(data) > 65536 or len(data) % 4:
        raise ValueError('bounded nonempty dword-aligned batch required')
    words = struct.unpack('<%dI' % (len(data)//4), data)
    packets, mapping = [], {}
    old = new = 0
    while old < len(words):
        header = words[old]
        opcode = header & 0xffff0000
        if header == 0:
            size = 1
        elif opcode == 0x70480000:
            size = (header & 0xfff) + 2
        elif opcode in SIZES:
            size = SIZES[opcode]
            # MI_FLUSH_DW bit7 is a control flag, not a length bit.
            mask = 0x3f if opcode == 0x13000000 else 0xfff
            if (header & mask) + 2 != size:
                raise ValueError('unexpected packet shape at %#x' % (old*4))
        else:
            raise ValueError('unreviewed opcode at %#x' % (old*4))
        if old + size > len(words):
            raise ValueError('truncated packet at %#x' % (old*4))
        # Hypothetical size-only map includes one WAIT on each side of MFX mode.
        # It intentionally does not supply defaults, headers, or resource fields.
        before = int(opcode == 0x70000000)
        new += before
        proposed = EXPANSIONS.get(opcode, size)
        for index in range(size):
            mapping[old+index] = new+index
        packets.append({'offset': old*4, 'header': '%08x' % header,
                        'native_dwords': size, 'producer_dwords': proposed,
                        'proposed_offset': new*4})
        old += size
        new += proposed + before
    return {'status': 'AUDIT_ONLY_NOT_SUBMITTABLE', 'native_dwords': len(words),
            'proposed_dwords': new, 'packets': packets}, mapping

def selftest():
    data = struct.pack('<7I', 0x70000003, 0, 0, 0, 0, 0x70800000, 0)
    report, mapping = audit(data)
    assert report['proposed_dwords'] == 13
    assert mapping[0] == 1 and mapping[5] == 7 and mapping[6] == 8
    for bad in (b'', b'1', struct.pack('<I', 0xffffffff), data[:-4]):
        try:
            audit(bad)
        except ValueError:
            continue
        raise AssertionError('bad packet accepted')
    print('VDENC_BATCH_AUDIT_SELFTEST_OK')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('batch', nargs='?', type=Path)
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        selftest()
    elif args.batch:
        if args.batch.stat().st_size > 65536:
            parser.error('batch exceeds 64KiB')
        report, mapping = audit(args.batch.read_bytes())
        # Known sideband dword offsets are reported, never rewritten.
        report['proposed_rebase'] = {str(x): mapping.get(x) for x in (15,139,519,646,677)}
        print(json.dumps(report, separators=(',', ':')))
    else:
        parser.error('batch or --self-test required')
