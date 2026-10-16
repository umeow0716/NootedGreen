#!/usr/bin/env python3
"""Offline packet/rebase audit; NEVER emits a GPU-submittable command buffer.

Input is the saved pre-relocation AVC command buffer, not a live mapping.
Gen12 sizes are pinned to intel/media-driver 2a32c7fafd8fa169112e962587777969cc8c654f.
Size differences describe producer ABI, not hardware validity or a hang verdict.
"""
import argparse
import base64
import json
import re
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
MFC_REGISTERS = {0x128a0: 0x1c08a0, 0x128a4: 0x1c08a4,
                 0x128b4: 0x1c08b4, 0x128b8: 0x1c08b8}

def audit(data):
    if not data or len(data) > 65536 or len(data) % 4:
        raise ValueError('bounded nonempty dword-aligned batch required')
    words = struct.unpack('<%dI' % (len(data)//4), data)
    packets, mapping, registers, controls, surfaces = [], {}, [], [], []
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
        if opcode in (0x70810000,0x70820000,0x70830000):
            field = words[old+3]
            surfaces.append({'offset':old*4, 'old_format':field>>28,
                             'old_interleave':(field>>27)&1,
                             'gen12_if_blind_copied_format':field>>27,
                             'nv12_reencoded_control':(field&0xfffff)|(4<<27)
                             if field>>28==4 and field&(1<<27) else None})
        if opcode == 0x12000000:
            reg = words[old+1]
            expected = MFC_REGISTERS.get(reg)
            registers.append({'offset': old*4, 'native_register': reg,
                              'gen12_absolute': expected,
                              'gen12_vcs_relative': None if expected is None else expected & 0x3fff,
                              'native_add_cs_mmio': bool(header & (1 << 19)),
                              'gen12_add_cs_mmio': True if expected is not None else None})
        if opcode == 0x70840000:
            # Compiled offsetof checks against pinned Gen9/Gen12 headers prove
            # words7..9 changed from DsBwdRef0 to RESERVED. Address zeros before
            # relocation are not a resource absence proof; report, don't clear.
            controls.append({'offset': old*4,
                             'old_ds_bwd_becomes_reserved': list(words[old+7:old+10]),
                             'reserved_triplet_already_zero': not any(words[old+7:old+10]),
                             'compression_enabled_slots': [i for i in range(3,37,3)
                                                           if words[old+i] & (1 << 9)],
                             'cache_indices': [words[old+i] & 0x7f for i in range(3,37,3)]})
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
    # process_token_VDENC uses source-root+0x48 for PIPE_BUF, not an offset
    # supplied for that packet. Preserve this constraint in every proposal.
    roots = [p for p in packets if p['header'] == '70810004']
    pipes = [p for p in packets if p['header'] == '70840023']
    coupling = []
    for root in roots:
        following = [p for p in pipes if p['offset'] > root['offset']]
        if following:
            pipe = following[0]
            delta = pipe['proposed_offset'] - root['proposed_offset']
            coupling.append({'native_root': root['offset'], 'native_fixed_delta': 0x48,
                             'observed_native_delta': pipe['offset']-root['offset'],
                             'proposed_delta': delta,
                             'naive_pre_relocation_compatible': delta == 0x48})
    return {'status': 'AUDIT_ONLY_NOT_SUBMITTABLE', 'native_dwords': len(words),
            'proposed_dwords': new, 'packets': packets,
            'registers': registers, 'buffer_controls': controls,
            'surface_controls': surfaces, 'relocation_coupling': coupling}, mapping

def publication(lines, published_only=False):
    counts = None
    streams = [[], []]
    terminal = False
    for line in lines:
        if len(line) > 4096:
            raise ValueError('publication line exceeds bound')
        if 'V372 AVC_CPU_' not in line:
            continue
        if terminal:
            raise ValueError('duplicate publication')
        match = re.search(r'V372 AVC_CPU_BEGIN source=(\d+) published=(\d+) offset=(\d+) used=(\d+) bytes=(\d+) mapFlags=([0-9a-f]+)', line)
        if match:
            if counts is not None:
                raise ValueError('duplicate begin')
            counts = [int(match[1]), int(match[2])]
            offset, used, size = map(int, match.group(3, 4, 5))
            if not (2 <= counts[0] <= 1024 and counts[0] < counts[1] <= 1536 and
                    offset <= used <= size and counts[1]*4 <= used-offset):
                raise ValueError('invalid publication extent')
            continue
        match = re.search(r'V372 AVC_CPU_BLOCK kind=(\d+) first=(\d+) words=(\d+) data=([A-Za-z0-9+/=]+)', line)
        if match and counts:
            kind, first, valid = map(int, match.group(1, 2, 3))
            if kind > 1 or valid != min(48, counts[kind]-first) or valid <= 0 or first % 48:
                raise ValueError('invalid chunk order/extent')
            raw = base64.b64decode(match[4], validate=True)
            if len(raw) != 192 or any(raw[valid*4:]):
                raise ValueError('invalid chunk padding')
            if published_only and kind == 0:
                continue
            if first != len(streams[kind]):
                raise ValueError('invalid chunk order/extent')
            streams[kind].extend(struct.unpack('<%dI' % valid, raw[:valid*4]))
            continue
        match = re.search(r'V372 AVC_CPU_END source=(\d+) published=(\d+)', line)
        if match and counts == [int(match[1]), int(match[2])] and len(streams[1]) == counts[1] and (published_only or len(streams[0]) == counts[0]):
            terminal = True
            continue
        raise ValueError('malformed/incomplete publication')
    if not terminal:
        raise ValueError('missing publication terminal')
    return {'status': 'CPU_PUBLICATION_ONLY_NOT_GPU_EXECUTION',
            'source_status': 'NOT_DECODED' if published_only else 'COMPLETE',
            'source': None if published_only else streams[0], 'published': streams[1]}

def native_batch(lines):
    """One terminal, stable, native-record-bound CPU span; not GPU proof."""
    metadata = None
    words = []
    terminal = False
    for line in lines:
        if len(line) > 4096:
            raise ValueError('native batch line exceeds bound')
        if 'V377 NATIVE_BATCH_' not in line:
            continue
        if terminal:
            raise ValueError('duplicate native batch')
        match = re.search(r'V377 NATIVE_BATCH_BEGIN words=(\d+) offset=(\d+) mapFlags=([0-9a-f]+) map-task-match=([01]) media-task-match=([01]) va=([0-9a-f]{8}):([0-9a-f]{8}) atomic=0 native-count=(\d+) memory-self=([01]) memory-backing=([01])', line)
        if match:
            count, offset = int(match[1]), int(match[2])
            if metadata is not None or not 2 <= count <= 1536 or int(match[8])+2 != count or offset + count*4 > 65536 or offset % 4:
                raise ValueError('invalid native batch begin')
            metadata = {'count': count, 'offset': offset, 'map_flags': int(match[3], 16),
                        'map_task_match': int(match[4]), 'media_task_match': int(match[5]),
                        'gpu_va': int(match[6] + match[7], 16), 'native_count': int(match[8]),
                        'memory_self': int(match[9]), 'memory_backing': int(match[10])}
            continue
        match = re.search(r'V377 NATIVE_BATCH_BLOCK first=(\d+) words=(\d+) data=([A-Za-z0-9+/=]+)', line)
        if match and metadata:
            first, valid = int(match[1]), int(match[2])
            if first != len(words) or first % 48 or valid != min(48, metadata['count']-first) or valid <= 0:
                raise ValueError('invalid native batch chunk extent/order')
            raw = base64.b64decode(match[3], validate=True)
            if len(raw) != 192 or any(raw[valid*4:]):
                raise ValueError('invalid native batch chunk padding')
            words.extend(struct.unpack('<%dI' % valid, raw[:valid*4]))
            continue
        match = re.search(r'V377 NATIVE_BATCH_END words=(\d+) stable=1 atomic=0', line)
        if match and metadata and int(match[1]) == len(words) == metadata['count']:
            terminal = True
            continue
        raise ValueError('malformed/incomplete/unstable native batch')
    if not terminal:
        raise ValueError('missing native batch terminal')
    return {'status': 'CPU_NATIVE_BATCH_ONLY_NOT_GPU_EXECUTION', 'atomic': False,
            'metadata': metadata, 'words': words}


def selftest():
    data = struct.pack('<7I', 0x70000003, 0, 0, 0, 0, 0x70800000, 0)
    report, mapping = audit(data)
    assert report['proposed_dwords'] == 13
    assert mapping[0] == 1 and mapping[5] == 7 and mapping[6] == 8
    shapes = [(0x70810004,6),(0x70820004,6),(0x70830004,6),(0x70840023,37)]
    data = b''.join(struct.pack('<%dI' % n, h, *([0]*(n-1))) for h,n in shapes)
    report, _ = audit(data)
    assert report['relocation_coupling'] == [{'native_root':0,'native_fixed_delta':72,
        'observed_native_delta':72,'proposed_delta':88,'naive_pre_relocation_compatible':False}]
    assert report['buffer_controls'][0]['reserved_triplet_already_zero']
    packet = [0]*37
    packet[0], packet[9], packet[12] = 0x70840023, 14, (1 << 9) | 42
    report, _ = audit(struct.pack('<37I', *packet))
    assert not report['buffer_controls'][0]['reserved_triplet_already_zero']
    assert report['buffer_controls'][0]['compression_enabled_slots'] == [12]
    assert report['buffer_controls'][0]['cache_indices'][3] == 42
    report, _ = audit(struct.pack('<6I',0x70810004,0,0,0x48000003,0,0))
    assert report['surface_controls'][0]['old_format']==4
    assert report['surface_controls'][0]['gen12_if_blind_copied_format']==9
    assert report['surface_controls'][0]['nv12_reencoded_control']==0x20000003
    for reg, absolute in MFC_REGISTERS.items():
        report, _ = audit(struct.pack('<4I',0x12000002,reg,0,0))
        assert report['registers'][0]['gen12_vcs_relative'] == absolute & 0x3fff
        assert not report['registers'][0]['native_add_cs_mmio']
    data = struct.pack('<7I', 0x70000003, 0, 0, 0, 0, 0x70800000, 0)
    for bad in (b'', b'1', struct.pack('<I', 0xffffffff), data[:-4]):
        try:
            audit(bad)
        except ValueError:
            continue
        raise AssertionError('bad packet accepted')
    print('VDENC_BATCH_AUDIT_SELFTEST_OK')
    chunks = ['V372 AVC_CPU_BEGIN source=2 published=3 offset=0 used=12 bytes=64 mapFlags=0']
    for kind, count in enumerate((2, 3)):
        encoded = base64.b64encode(struct.pack('<48I', *range(count), *([0]*(48-count)))).decode()
        chunks.append(f'V372 AVC_CPU_BLOCK kind={kind} first=0 words={count} data={encoded}')
    chunks.append('V372 AVC_CPU_END source=2 published=3')
    assert publication(chunks)['published'] == [0, 1, 2]
    assert publication([chunks[0], *chunks[2:]], True)['source'] is None
    assert publication([chunks[0], *chunks[2:]], True)['published'] == [0, 1, 2]
    for bad in (chunks[:-1], chunks+chunks, chunks[:2]+chunks[1:],
                [x.replace('first=0', 'first=48') for x in chunks],
                [x.replace('used=12', 'used=8') for x in chunks],
                [x.replace('published=3', 'published=1537') for x in chunks],
                [x.replace('kind=1', 'kind=2') for x in chunks],
                [x.replace('words=3', 'words=48') for x in chunks],
                [x.replace('published=3', 'published=4') if 'END' in x else x for x in chunks],
                [x.replace('data=', 'data=!') for x in chunks]):
        try:
            publication(bad)
        except (ValueError, base64.binascii.Error):
            continue
        raise AssertionError('bad publication accepted')
    print('AVC_CPU_PUBLICATION_DECODER_SELFTEST_OK')
    chunks = ['V377 NATIVE_BATCH_BEGIN words=55 offset=16 mapFlags=201 map-task-match=1 media-task-match=1 va=00000000:40010010 atomic=0 native-count=53 memory-self=0 memory-backing=1']
    for first in (0, 48):
        valid = min(48, 55-first)
        raw = struct.pack('<48I', *list(range(first, first+valid)), *([0]*(48-valid)))
        chunks.append(f'V377 NATIVE_BATCH_BLOCK first={first} words={valid} data={base64.b64encode(raw).decode()}')
    chunks.append('V377 NATIVE_BATCH_END words=55 stable=1 atomic=0')
    assert native_batch(chunks)['words'] == list(range(55))
    for bad in [chunks[1:], chunks[:-1], [chunks[0], *chunks[2:]],
                [chunks[0], chunks[1], chunks[1], *chunks[2:]],
                [chunks[0], chunks[2], chunks[1], chunks[3]], [*chunks, chunks[-1]],
                [chunks[0].replace('words=55', 'words=16385'), *chunks[1:]],
                [chunks[0].replace('offset=16', 'offset=65536'), *chunks[1:]],
                [chunks[0].replace('native-count=53', 'native-count=55'), *chunks[1:]],
                [*chunks[:-1], chunks[-1].replace('stable=1', 'stable=0')],
                [*chunks[:2], chunks[2].replace('data=', 'data=AAAA'), chunks[-1]]]:
        try:
            native_batch(bad)
        except (ValueError, base64.binascii.Error):
            continue
        raise AssertionError('bad native batch accepted')
    print('NATIVE_BATCH_CPU_DECODER_SELFTEST_OK')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('batch', nargs='?', type=Path)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--publication-log', type=Path)
    parser.add_argument('--native-batch-log', type=Path)
    parser.add_argument('--published-only', action='store_true', help='require complete published stream; never claim source completeness')
    args = parser.parse_args()
    if args.self_test:
        selftest()
    elif args.native_batch_log:
        if args.publication_log or args.published_only or args.native_batch_log.stat().st_size > 2097152:
            parser.error('native batch log exceeds 2MiB or conflicting mode')
        with args.native_batch_log.open() as source:
            print(json.dumps(native_batch(source), separators=(',', ':')))
    elif args.publication_log:
        if args.publication_log.stat().st_size > 2097152:
            parser.error('publication log exceeds 2MiB')
        with args.publication_log.open() as source:
            print(json.dumps(publication(source, args.published_only), separators=(',', ':')))
    elif args.batch:
        if args.batch.stat().st_size > 65536:
            parser.error('batch exceeds 64KiB')
        report, mapping = audit(args.batch.read_bytes())
        # Known sideband dword offsets are reported, never rewritten.
        report['proposed_rebase'] = {str(x): mapping.get(x) for x in (15,139,519,646,677)}
        print(json.dumps(report, separators=(',', ':')))
    else:
        parser.error('batch or --self-test required')
