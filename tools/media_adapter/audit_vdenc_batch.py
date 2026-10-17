#!/usr/bin/env python3
"""Offline packet/rebase audit; NEVER emits a GPU-submittable command buffer.

Input is a saved pre-relocation AVC buffer, publication log, or explicitly
decoded publication artifact, never a live mapping. Missing source/BEGIN
labels are preserved; no mode reconstructs missing command bytes.
Gen12 sizes are pinned to intel/media-driver 2a32c7fafd8fa169112e962587777969cc8c654f.
Size differences describe producer ABI, not hardware validity or a hang verdict.
"""
import argparse
import base64
import hashlib
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

def mode_template_fields(words):
    """Bounded Gen12 field interpretation, not codec legality or GPU proof.

    Accept reviewed legacy/expanded lengths for inventory only. An unknown
    shape stops the walk; never scan payload DWORDs for apparent opcodes.
    Gen12's AVC walker producer leaves FirstSuperSlice at constructor zero;
    its HEVC/VP9 branches setting that bit must not become an AVC requirement.
    """
    result = {'status': 'GEN12_FIELDS_CPU_ONLY_NOT_ADMISSION',
              'complete_packet_walk': False, 'mfx_modes': [], 'pak_headers': [], 'walkers': [],
              'pipeline_flushes': [], 'mi_flushes': [], 'mi_operands': [], 'mi_profile_issues': [],
              'vdenc_pipe_resources': [], 'mfx_pipe_resources': [],
              'mfx_indirect_resources': [], 'mfx_bsp_resources': [], 'indirect_start_fields': [],
              'surface_layouts': [],
              'resource_coverage': 'EXPLICIT_BASE_FIELDS_ONLY_NOT_EXTENTS_OR_ALL_INDIRECT_ACCESSES'}
    if not words or len(words) > 1536 or any(type(x) is not int or not 0 <= x <= 0xffffffff for x in words):
        result['refusal'] = 'word-extent-or-value'
        return result
    at = 0
    def mfx_resource(collection, index, name, shift=6, control_index=None,
                     cache_select=False, upper_index=None):
        low, high = words[at+index:at+index+2]
        control = words[at+control_index] if control_index is not None else None
        cache = (control >> 12) & 1 if cache_select else None
        item = dict(packet_word=at, address_word=at+index, role=name,
                    raw=[f'{low:08x}', f'{high:08x}'],
                    address_hex=f'{((high & 0xffff) << 32) | (low & ~((1 << shift)-1)):016x}',
                    address_reserved_bits=bool(low & ((1 << shift)-1) or high & 0xffff0000),
                    control_raw=None if control is None else f'{control:08x}', cache_select=cache,
                    address_domain='INTERNAL_MEDIA_STORAGE' if cache else 'BUFFER_BASE_ADDRESS',
                    extent_bytes=None, mapping_proven=False, gpu_fetch_proven=False)
        if upper_index is not None:
            end_low, end_high = words[at+upper_index:at+upper_index+2]
            item.update(upper_bound_raw=[f'{end_low:08x}', f'{end_high:08x}'],
                        upper_bound_page_hex=f'{((end_high & 0xffff) << 32) | (end_low & ~4095):016x}',
                        upper_bound_reserved_bits=bool(end_low & 4095 or end_high & 0xffff0000))
        result[collection].append(item)
    while at < len(words):
        header = words[at]
        op = header & (0xff800000 if header < 0x60000000 else 0xffff0000)
        if header in (0, 0x68000100):
            count = 1
        elif header == 0x05000000 and at+2 == len(words) and words[at+1] == 0:
            count = 1  # Exact native final END/NOOP pair, never scan beyond END.
        elif op == 0x70480000:
            count = (header & 0xfff) + 2
        elif op in SIZES:
            length_mask = {0x13000000: 0x3f, 0x10000000: 0x3ff, 0x12000000: 0xff}.get(op, 0xfff)
            count = (header & length_mask) + 2
            if count not in (SIZES[op], EXPANSIONS.get(op, SIZES[op])):
                result.update(refusal='packet-shape', stopped_word=at)
                return result
        else:
            result.update(refusal='unreviewed-opcode', stopped_word=at)
            return result
        if count > len(words) - at:
            result.update(refusal='truncated-packet', stopped_word=at)
            return result
        if op in (0x70010000, 0x70810000, 0x70820000, 0x70830000):
            # Gen12 MFX DW2..5 / VDENC common surface fields DW2..5.
            # Format moved from MFX bit28 to VDENC bit27; do not unify it.
            geometry, control, u, v = words[at+2:at+6]
            result['surface_layouts'].append(dict(word=at, opcode=f'{op:08x}',
                surface_id=words[at+1]&15 if op==0x70010000 else None,
                width=((geometry>>4)&0x3fff)+1, height=(geometry>>18)+1,
                pitch=((control>>3)&0x1ffff)+1, tiled=(control>>1)&1,
                tile_walk=control&1, half_pitch=(control>>2)&1,
                format=control>>(28 if op==0x70010000 else 27),
                interleave=(control>>27)&1 if op==0x70010000 else None,
                u_y=u&0x7fff, u_x=(u>>16)&0x7fff,
                v_y=v&0xffff, v_x=(v>>16)&0x1fff,
                allocation_extent_proven=False, gpu_fetch_proven=False))
        elif op == 0x70000000:
            control = words[at+1]
            result['mfx_modes'].append(dict(word=at, control=control, standard=control & 15,
                encode=(control >> 4) & 1, vdenc=(control >> 13) & 1, standalone=(control >> 14) & 1))
        elif op == 0x70480000:
            control = words[at+1]
            result['pak_headers'].append(dict(word=at, control=control, template=(control >> 14) & 1))
        elif op == 0x70870000:
            result['walkers'].append(dict(word=at, start_y=words[at+1] & 0x1ff,
                end_y=words[at+2] & 0x3ff, first_super_slice=(words[at+1] >> 28) & 1))
        elif op == 0x77800000:
            control = words[at+1]
            result['pipeline_flushes'].append(dict(word=at, control=control,
                vdenc_done=(control >> 1) & 1, mfx_done=(control >> 3) & 1,
                parser_done=(control >> 4) & 1, vdenc_flush=(control >> 17) & 1))
        elif op == 0x13000000:
            result['mi_flushes'].append(dict(word=at, header=header))
        elif op == 0x70020000 and count == 68:
            # Pinned MFX_PIPE_BUF_ADDR_STATE: six address/control triples,
            # sixteen 2DW references sharing DW51 control, then five triples.
            slots = ((1, 'pre_deblock'), (4, 'post_deblock'), (7, 'source'),
                     (10, 'streamout'), (13, 'intra_rowstore'), (16, 'deblock_rowstore'),
                     (52, 'macroblock_or_error'), (55, 'ildb'), (58, 'ildb_second'),
                     (62, 'scaled_reference'), (65, 'slice_size_streamout'))
            for index, name in slots:
                mfx_resource('mfx_pipe_resources', index, name, control_index=index+2,
                             cache_select=index in (13, 16))
            for index in range(16):
                mfx_resource('mfx_pipe_resources', 19+2*index, f'reference{index}', control_index=51)
        elif op == 0x70030000 and count == 26:
            # Each entry is base-low/high, attributes, access-upper-low/high.
            # The upper bound is an encoded page, NOT an allocation-size proof.
            for index, name in ((1, 'bitstream_decoder_stitch'), (6, 'mv'),
                                (11, 'it_coeff_decoder'), (16, 'it_deblock_decoder'),
                                (21, 'pak_bse_encoder')):
                mfx_resource('mfx_indirect_resources', index, name, shift=12,
                             control_index=index+2, upper_index=index+3)
        elif op == 0x70040000 and count == 10:
            for index, name in ((1, 'bsd_mpc_rowstore'), (4, 'mpr_rowstore_decoder'), (7, 'bitplane')):
                mfx_resource('mfx_bsp_resources', index, name, control_index=index+2,
                             cache_select=index in (1, 4))
        elif op == 0x71030000 and count == 11:
            result['indirect_start_fields'].append(dict(packet_word=at, word=at+7,
                role='indirect_pak_bse_start', raw=f'{words[at+7]:08x}',
                encoded_value=words[at+7] & 0x1fffffff, reserved_bits=bool(words[at+7] & 0xe0000000),
                base_resolution='NOT_PERFORMED', gpu_fetch_proven=False))
        elif op == 0x70840000 and count == 71:
            # Pinned Gen12 hwcmd: 3DW address/control records. DW7..9 is
            # reserved and DW61 is a histogram OFFSET, neither is an address.
            # Addresses are observations, NOT resource extents/ownership or
            # proof that a disabled mode will actually access the operand.
            slots = ((1, 'ds_fwd0'), (4, 'ds_fwd1_or_bwd0'), (10, 'source'),
                     (13, 'streamin'), (16, 'rowstore'), (19, 'colocated_mv'),
                     (22, 'fwd0'), (25, 'fwd1'), (28, 'fwd2'), (31, 'bwd0'),
                     (34, 'statistics'), (37, 'ds_fwd0_4x'), (40, 'ds_fwd1_or_bwd0_4x'),
                     (43, 'cu_record_out'), (46, 'lcu_pak'), (49, 'scaled8x'),
                     (52, 'scaled4x'), (55, 'vp9_map_in'), (58, 'vp9_map_out'),
                     (62, 'tile_rowstore'), (65, 'cumulative_cu'), (68, 'palette'))
            for index, name in slots:
                low, high, control = words[at+index:at+index+3]
                result['vdenc_pipe_resources'].append(dict(
                    packet_word=at, address_word=at+index, role=name,
                    raw=[f'{value:08x}' for value in (low, high, control)],
                    address_hex=f'{((high & 0xffff) << 32) | (low & ~63):016x}',
                    cache_select=(control >> 12) & 1,
                    address_domain='INTERNAL_MEDIA_STORAGE' if control & 0x1000 else 'LLC_BUFFER_ADDRESS',
                    reserved_bits=bool(low & 63 or high & 0xffff0000 or control & 0xffff8800),
                    extent_bytes=None, mapping_proven=False, gpu_fetch_proven=False))
        if op in (0x10000000, 0x12000000, 0x13000000):
            # Packet-shape walk only. Never dereference GPU addresses or treat
            # a clean field report as ownership, cache visibility or admission.
            low, high = words[at+1:at+3] if op != 0x12000000 else words[at+2:at+4]
            operand = dict(word=at, opcode=f'{op:08x}', header=f'{header:08x}',
                           address_words=[f'{low:08x}', f'{high:08x}'])
            if op != 0x12000000:
                operand['immediate_words'] = [f'{value:08x}' for value in words[at+3:at+5]]
            issues = []
            if op == 0x13000000:
                operand.update(address_hex=f'{((high & 0xffff) << 32) | (low & ~7):016x}',
                               ggtt=(low >> 2) & 1, hws_index=(header >> 21) & 1,
                               post_sync=(header >> 14) & 3)
                if header & 0x005b3c40: issues.append('flush-header-reserved')
                if low & 3 or high & 0xffff0000: issues.append('flush-address-reserved')
                if operand['ggtt'] or operand['hws_index']: issues.append('flush-address-space')
                if operand['post_sync'] == 2: issues.append('flush-postsync')
            else:
                operand.update(address_hex=f'{(high << 32) | (low & ~3):016x}', ggtt=(header >> 22) & 1)
                if op == 0x10000000:
                    operand.update(core_mode=low & 1, store_qword=(header >> 21) & 1,
                                   inline_mocs=(header >> 20) & 1, mocs=(header >> 13) & 0x7f)
                    if header & 0x1c00: issues.append('store-header-reserved')
                    if low & 2: issues.append('store-address-reserved')
                    if operand['ggtt']: issues.append('store-global-gtt')
                else:
                    operand.update(register_hex=f'{words[at+1]:08x}', cs_offset=(header >> 19) & 1,
                                   mmio_remap=(header >> 17) & 1)
                    if header & 0x0035ff00: issues.append('srm-header-reserved')
                    if low & 3: issues.append('srm-address-reserved')
                    if operand['ggtt']: issues.append('srm-global-gtt')
            result['mi_operands'].append(operand)
            result['mi_profile_issues'].extend(dict(word=at, reason=reason) for reason in issues)
        at += count
    result['complete_packet_walk'] = True
    return result

def decoded_publication_fields(data):
    """Inspect an existing decoded artifact without upgrading its provenance."""
    if not data or len(data) > 32768:
        raise ValueError('decoded publication requires 1..32768 bytes')
    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('duplicate decoded publication key')
            result[key] = value
        return result
    decoded = json.loads(data, object_pairs_hook=unique_pairs)
    if not isinstance(decoded, dict) or not isinstance(decoded.get('published'), list):
        raise ValueError('decoded published word array required')
    words = decoded['published']
    fields = mode_template_fields(words)
    if fields.get('refusal') == 'word-extent-or-value':
        raise ValueError('invalid decoded publication words')
    metadata = {}
    for name in ('status', 'source_status'):
        value = decoded.get(name, 'NOT_PROVIDED')
        if not isinstance(value, str) or len(value) > 128:
            raise ValueError('invalid decoded publication provenance label')
        metadata[name] = value
    return dict(status='DECODED_CPU_PUBLICATION_FIELDS_ONLY_NOT_ADMISSION',
                input_status=metadata['status'], source_status=metadata['source_status'],
                source_decoded_by_this_mode=False,
                artifact_sha256=hashlib.sha256(data).hexdigest(),
                capture_authenticity='NOT_VERIFIED_BY_THIS_TOOL', published_words=len(words),
                gen12_mode_template_fields=fields)

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
            'surface_controls': surfaces, 'relocation_coupling': coupling,
            'gen12_mode_template_fields': mode_template_fields(list(words))}, mapping

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
            'source': None if published_only else streams[0], 'published': streams[1],
            'gen12_mode_template_fields': mode_template_fields(streams[1])}

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


def timeout_ring_packets(words):
    return cpu_ring_packets(words, maximum_words=48, allow_pipe_control=False)


def cpu_ring_packets(words, maximum_words=524288, allow_pipe_control=True):
    """Inventory the exact short native ring grammar; do not execute/simulate it.

    Field positions: pinned Intel gen12 mhw_mi_hwcmd_g12_X.h. Unknown or
    truncated packets terminate the walk; never resynchronize inside payload.
    Stores/waits below are encoded intent, NOT sampled memory or GPU progress.
    """
    if len(words) > maximum_words or any(type(w) is not int or not 0 <= w <= 0xffffffff for w in words):
        raise ValueError('invalid bounded ring words')
    packets, at = [], 0
    while at < len(words):
        h = words[at]
        packet = {'offset': at * 4, 'header': f'{h:08x}'}
        if h in (0, 0x03800000):
            size = 1
            packet['opcode'] = 'MI_NOOP' if h == 0 else 'MI_USER_INTERRUPT'
        elif h == 0x11000001:
            size = 3
            packet['opcode'] = 'MI_LOAD_REGISTER_IMM'
        elif h == 0x0e009002:
            size = 4
            packet.update(opcode='MI_SEMAPHORE_WAIT', comparison='memory_unsigned_greater_equal',
                          polling=True, address_space='PPGTT')
        elif h == 0x18800101:
            size = 3
            packet.update(opcode='MI_BATCH_BUFFER_START', address_space='PPGTT')
        elif h & 0xff800000 == 0x13000000 and h & 0x3f == 3:
            size = 5
            packet.update(opcode='MI_FLUSH_DW', post_sync=(h >> 14) & 3,
                          tlb_invalidate=bool(h & (1 << 18)),
                          hws_index=bool(h & (1 << 21)), notify=bool(h & 0x100))
        elif allow_pipe_control and h == 0x7a000004:
            size = 6
            packet['opcode'] = 'PIPE_CONTROL'
        else:
            return {'complete': False, 'stopped_offset': at * 4, 'reason': 'unknown_header', 'packets': packets}
        if size > len(words) - at:
            return {'complete': False, 'stopped_offset': at * 4, 'reason': 'truncated_packet', 'packets': packets}
        if packet['opcode'] == 'PIPE_CONTROL':
            control = words[at+1]
            # Contrast the older media header with Intel compute-runtime
            # 413bd626955908f8a599e56a70808677d5d66634 gen12lp PIPE_CONTROL.
            # DW1 bit30 is L3FabricFlush: the old schema alone is insufficient
            # to call this invalid, much less the cause of a hardware hang.
            packet.update(control=f'{control:08x}', post_sync=(control >> 14) & 3,
                          cs_stall=bool(control & (1 << 20)), tlb_invalidate=bool(control & (1 << 18)),
                          pinned_gen12_reserved_dw1=f'{control & 0xc8400040:08x}',
                          intel_gen12lp_l3_fabric_flush=bool(control & (1 << 30)),
                          intel_gen12lp_reserved_dw1=f'{control & 0x80400000:08x}',
                          legacy_only_schema_bits=f'{control & 0x48000040:08x}',
                          address=f'{(words[at+2] & ~3) | words[at+3] << 32:016x}',
                          address_space='GGTT' if control & (1 << 24) else 'PPGTT',
                          immediate=words[at+4] | words[at+5] << 32)
        elif packet['opcode'] == 'MI_LOAD_REGISTER_IMM':
            packet.update(register=f'{words[at+1]:08x}', value=f'{words[at+2]:08x}')
        elif packet['opcode'] == 'MI_SEMAPHORE_WAIT':
            packet.update(address=f'{words[at+2] | words[at+3] << 32:016x}', threshold=words[at+1])
        elif packet['opcode'] == 'MI_BATCH_BUFFER_START':
            packet['address'] = f'{words[at+1] | words[at+2] << 32:016x}'
        elif packet['opcode'] == 'MI_FLUSH_DW':
            packet.update(address=f'{(words[at+1] & ~7) | words[at+2] << 32:016x}',
                          address_space='GGTT' if words[at+1] & 4 else 'PPGTT',
                          immediate=words[at+3] | words[at+4] << 32)
        packet['bytes'] = size * 4
        packets.append(packet)
        at += size
    return {'complete': True, 'packets': packets, 'execution_observed': False}


def timeout_ring(text):
    """V379 selected retained CPU ring; never a GPU PC or completion proof."""
    text = text.replace('\\/', '/').replace('\\n', '\n')
    channels = re.findall(r'V267: refusing VF event timeout recovery channel=(-?[0-9]{1,10}) V364:', text)
    windows = re.findall(r'V379: E=([0-9a-f]{1,8}) V366: P=([0-9a-f]{1,8})/([0-9a-f]{1,8})/([A-Za-z0-9+/=]+)', text)
    if len(channels) != 1 or len(windows) != 1:
        raise ValueError('missing or ambiguous selected timeout prefix')
    engine, flags, size = (int(value, 16) for value in windows[0][:3])
    channel = int(channels[0])
    if channel not in (0, 3) or engine != channel or flags != 1:
        raise ValueError('unsupported channel or mismatched prefix identity')
    frames = re.findall(rf' T{engine}=([0-9a-f]{{1,8}}(?:/[0-9a-f]{{1,8}}){{8}})(?=\s)', text)
    if len(frames) != 1:
        raise ValueError('missing or ambiguous selected frame')
    frame = [int(value, 16) for value in frames[0].split('/')]
    head, tail = frame[7:9]
    if frame[1] & 15 != 15 or head != 0 or tail < 32 or tail > 192 or tail & 7:
        raise ValueError('unqualified selected CPU frame or invalid span')
    if size < 256 or size > 0x200000 or size & (size - 1) or tail >= size:
        raise ValueError('invalid ring allocation bound')
    encoded = windows[0][3]
    raw = base64.b64decode(encoded, validate=True)
    if len(encoded) != 256 or len(raw) != 192 or base64.b64encode(raw).decode() != encoded or any(raw[tail:]):
        raise ValueError('truncated, noncanonical, or nonzero padded prefix')
    words = list(struct.unpack(f'<{tail // 4}I', raw[:tail]))
    return {'engine_index': engine, 'native_channel': channel, 'ring_allocation_bytes': size,
            'head': head, 'tail': tail, 'words': words, 'packet_inventory': timeout_ring_packets(words),
            'cpu_snapshot_only': True, 'live_instruction_pointer': False, 'completion_proof': False}


def selftest():
    packets = timeout_ring_packets([0x0e009002, 2, 0x40001000, 0,
                                   0x18800101, 0x40010000, 0,
                                   0x13004103, 0x400010c0, 0, 1, 0, 0x03800000])
    assert packets['complete'] and not packets['execution_observed']
    wait, batch, store, interrupt = packets['packets']
    assert wait['threshold'] == 2 and wait['comparison'] == 'memory_unsigned_greater_equal'
    assert wait['address'] == '0000000040001000' and wait['polling']
    assert batch['address'] == '0000000040010000' and batch['offset'] == 16
    assert store['immediate'] == 1 and store['post_sync'] == 1 and store['notify']
    assert interrupt['opcode'] == 'MI_USER_INTERRUPT'
    assert not timeout_ring_packets([0xffffffff, 0x18800101, 8, 0])['complete']
    assert timeout_ring_packets([0xffffffff, 0x18800101, 8, 0])['packets'] == []
    for length in range(1, 5):
        assert not timeout_ring_packets([0x13004003, 8, 0, 1, 0][:length])['complete']
    ggtt = timeout_ring_packets([0x13004003, 12, 0, 1, 0])['packets'][0]
    assert ggtt['address_space'] == 'GGTT' and ggtt['address'] == '0000000000000008'
    rcs = [0x7a000004, 0x50105021, 0x40000000, 0, 0, 0]
    pipe = cpu_ring_packets(rcs)['packets'][0]
    assert pipe['pinned_gen12_reserved_dw1'] == '40000000'
    assert pipe['intel_gen12lp_l3_fabric_flush']
    assert pipe['intel_gen12lp_reserved_dw1'] == '00000000'
    assert pipe['legacy_only_schema_bits'] == '40000000'
    reserved = cpu_ring_packets([0x7a000004, 0x80000000, 0, 0, 0, 0])['packets'][0]
    assert reserved['intel_gen12lp_reserved_dw1'] == '80000000'
    assert reserved['legacy_only_schema_bits'] == '00000000'
    assert pipe['cs_stall'] and pipe['post_sync'] == 1 and pipe['address_space'] == 'PPGTT'
    assert not timeout_ring_packets(rcs)['complete']  # Old selected-prefix grammar unchanged.
    for length in range(1, 6):
        assert not cpu_ring_packets(rcs[:length])['complete']
    assert cpu_ring_packets(rcs+[0x03800000])['complete']
    assert cpu_ring_packets([0xffffffff]+rcs)['packets'] == []
    for bad in ([0] * 49, [-1], [1 << 32], [True]):
        try:
            timeout_ring_packets(bad)
        except ValueError:
            continue
        raise AssertionError('invalid bounded ring accepted')
    print('TIMEOUT_RING_PACKET_INVENTORY_SELFTEST_OK')
    ring = [0x13000003] + [0xffffffff] * 33 + [0] * 14
    encoded = base64.b64encode(struct.pack('<48I', *ring)).decode()
    saved = ('V267: refusing VF event timeout recovery channel=0 V364: capture=1 '
             'T0=1bc1/f/5/1/0/88/0/0/88 V379: E=0 V366: P=1/1000/' + encoded)
    assert timeout_ring(saved)['words'] == ring[:34]
    assert timeout_ring(saved.replace('/', '\\/'))['engine_index'] == 0
    assert timeout_ring(saved.replace('channel=0','channel=3').replace('E=0','E=3').replace('T0=','T3='))['engine_index'] == 3
    for bad in (saved[:-1], saved + '\n' + saved, saved.replace('E=0', 'E=3'),
                saved.replace('P=1/', 'P=0/'), saved.replace('/0/0/88 ', '/0/8/88 '),
                saved.replace('/1000/', '/80/'), saved.replace('V379: E=0 ', ''),
                saved.replace('1bc1/', '1bc112345/'),
                saved.replace(encoded, base64.b64encode(b'\xff' * 192).decode())):
        try:
            timeout_ring(bad)
        except (ValueError, base64.binascii.Error):
            continue
        raise AssertionError('bad selected timeout CPU prefix accepted')
    print('SELECTED_TIMEOUT_CPU_RING_SELFTEST_OK')
    fields = mode_template_fields([0x68000100, 0x70000003, 0x626d2, 0, 0, 0,
                                  0x70480001, 0x4e4c, 0,
                                  0x70870002, 0, 17, 0, 0x12080002, 0x8a0, 0, 0])
    assert fields['complete_packet_walk'] and fields['mfx_modes'][0]['vdenc'] == 1
    assert fields['mfx_modes'][0]['standalone'] == 0 and fields['pak_headers'][0]['template'] == 1
    assert fields['walkers'][0]['first_super_slice'] == 0
    flush = mode_template_fields([0x77800000, 0x2001a, 0x13000083, 0, 0, 0, 0])
    assert flush['complete_packet_walk'] and flush['mi_flushes'][0]['word'] == 2
    assert all(flush['pipeline_flushes'][0][name] == 1 for name in
               ('vdenc_done', 'mfx_done', 'parser_done', 'vdenc_flush'))
    mi = mode_template_fields([0x10600003, 0x2000, 0x80000000, 1, 0,
                              0x1320c003, 0x1004, 0, 1, 0,
                              0x12480002, 0x8a0, 0x3000, 0x80000000])
    assert mi['complete_packet_walk'] and len(mi['mi_operands']) == 3
    assert mi['mi_operands'][0]['address_hex'] == '8000000000002000'
    assert mi['mi_operands'][1]['post_sync'] == 3 and mi['mi_operands'][1]['hws_index'] == 1
    assert mi['mi_operands'][2]['cs_offset'] == 1 and mi['mi_operands'][2]['ggtt'] == 1
    assert [issue['reason'] for issue in mi['mi_profile_issues']] == [
        'store-global-gtt', 'flush-address-space', 'srm-global-gtt']
    invalid_flush = mode_template_fields([0x13008003, 3, 0x10000, 0, 0])
    assert [issue['reason'] for issue in invalid_flush['mi_profile_issues']] == [
        'flush-address-reserved', 'flush-postsync']
    pipe = [0]*71
    pipe[0] = 0x70840045
    pipe[7:10] = [0xffffffff]*3  # Reserved, never enumerate as a resource.
    pipe[10:13] = [0x40002000, 0x1234, 2]
    pipe[16:19] = [0x4000, 0, 0x1000]
    pipe[61] = 0xffffffff  # Histogram offset, not a pointer.
    resources = mode_template_fields(pipe)
    assert resources['complete_packet_walk'] and len(resources['vdenc_pipe_resources']) == 22
    by_name = {item['role']: item for item in resources['vdenc_pipe_resources']}
    assert by_name['source']['address_hex'] == '0000123440002000'
    assert by_name['rowstore']['address_domain'] == 'INTERNAL_MEDIA_STORAGE'
    assert not by_name['source']['reserved_bits'] and by_name['source']['extent_bytes'] is None
    assert all(item['address_word'] not in (7, 8, 9, 61) and not item['mapping_proven']
               for item in resources['vdenc_pipe_resources'])
    pipe[10] |= 1
    assert mode_template_fields(pipe)['vdenc_pipe_resources'][2]['reserved_bits']
    mfx = [0]*68
    mfx[0] = 0x70020042
    mfx[7:9] = [0x4075e000, 0]
    mfx[13:16] = [0x4000, 0, 0x1000]
    mfx[19:21] = [0x40c00000, 0x1234]
    mfx[51] = 0x80  # Shared reference attributes, not an address.
    mfx[61] = 0xffffffff  # Per-reference compression control, not an address.
    mf = mode_template_fields(mfx)
    assert mf['complete_packet_walk'] and len(mf['mfx_pipe_resources']) == 27
    names = {item['role']: item for item in mf['mfx_pipe_resources']}
    assert names['source']['address_hex'] == '000000004075e000'
    assert names['reference0']['address_hex'] == '0000123440c00000'
    assert names['reference15']['control_raw'] == '00000080'
    assert names['intra_rowstore']['address_domain'] == 'INTERNAL_MEDIA_STORAGE'
    assert all(item['address_word'] not in (51, 61) for item in mf['mfx_pipe_resources'])
    indirect = [0]*26
    indirect[0] = 0x70030018
    indirect[21:26] = [0x40500000, 0, 2, 0x405ff000, 0]
    item = mode_template_fields(indirect)['mfx_indirect_resources'][-1]
    assert item['address_hex'] == '0000000040500000' and item['extent_bytes'] is None
    assert item['upper_bound_page_hex'] == '00000000405ff000' and not item['upper_bound_reserved_bits']
    indirect[22] |= 0x10000
    assert mode_template_fields(indirect)['mfx_indirect_resources'][-1]['address_reserved_bits']
    bsp = [0x70040008]+[0]*9
    bsp[1:4] = [0x10000, 0, 0x1000]
    assert len(mode_template_fields(bsp)['mfx_bsp_resources']) == 3
    assert mode_template_fields(bsp)['mfx_bsp_resources'][0]['cache_select'] == 1
    slice_state = [0x71030009]+[0]*10
    slice_state[7] = 0x12345
    offsets = mode_template_fields(slice_state)['indirect_start_fields']
    assert len(offsets) == 1 and offsets[0]['encoded_value'] == 0x12345
    assert offsets[0]['base_resolution'] == 'NOT_PERFORMED'
    assert not mode_template_fields(mfx[:-1])['complete_packet_walk']
    assert not mode_template_fields(mfx[:-1])['mfx_pipe_resources']
    mfx_surface=[0x70010004,4,0x10fc77f0,0x48003bfb,0x440,0x440]
    vdenc_surface=[0x70810004,0,0x10fc77f0,0x20003bfb,0x440,0x440]
    for packet in (mfx_surface,vdenc_surface):
        layout=mode_template_fields(packet)['surface_layouts'][0]
        assert (layout['width'],layout['height'],layout['pitch'])==(1920,1088,1920)
        assert layout['format']==4 and layout['u_y']==1088 and layout['tiled']==1
        assert not layout['allocation_extent_proven'] and not layout['gpu_fetch_proven']
    assert mode_template_fields(mfx_surface)['surface_layouts'][0]['interleave']==1
    assert mode_template_fields(vdenc_surface)['surface_layouts'][0]['interleave'] is None
    assert not mode_template_fields(vdenc_surface[:-1])['surface_layouts']
    # Distinguish the two format layouts and exercise every field boundary.
    edge=[0x70810004,0,0xffffffff,0xffffffff,0xffffffff,0xffffffff]
    layout=mode_template_fields(edge)['surface_layouts'][0]
    assert (layout['width'],layout['height'],layout['pitch'])==(16384,16384,131072)
    assert (layout['format'],layout['u_x'],layout['u_y'],layout['v_x'],layout['v_y'])==(
        31,32767,32767,8191,65535)
    edge[0]=0x70010004
    layout=mode_template_fields(edge)['surface_layouts'][0]
    assert layout['format']==15 and layout['surface_id']==0
    assert mode_template_fields([0x05000000, 0])['complete_packet_walk']
    assert not mode_template_fields([0x05000000, 0, 0])['complete_packet_walk']
    assert not mode_template_fields([0x05000000, 1])['complete_packet_walk']
    for header, packet, mask, reason in (
            (0x10200003, [0x2000, 0, 1, 0], 0x1c00, 'store-header-reserved'),
            (0x13004003, [0x1000, 0, 1, 0], 0x005b3c40, 'flush-header-reserved'),
            (0x12080002, [0x8a0, 0x3000, 0], 0x0035ff00, 'srm-header-reserved')):
        for bit in range(32):
            if mask & (1 << bit):
                fields = mode_template_fields([header | (1 << bit)] + packet)
                assert fields['complete_packet_walk']
                assert reason in [issue['reason'] for issue in fields['mi_profile_issues']]
    decoded = decoded_publication_fields(json.dumps(dict(status='BEGIN_MISSING', source_status='NOT_DECODED',
        published=[0x13000003, 0, 0, 0, 0])).encode())
    assert decoded['input_status'] == 'BEGIN_MISSING' and decoded['source_status'] == 'NOT_DECODED'
    assert decoded['capture_authenticity'] == 'NOT_VERIFIED_BY_THIS_TOOL' and decoded['published_words'] == 5
    assert not decoded['source_decoded_by_this_mode']
    unknown = decoded_publication_fields(b'{"published":[1]}')
    assert not unknown['gen12_mode_template_fields']['complete_packet_walk']
    assert unknown['gen12_mode_template_fields']['refusal'] == 'unreviewed-opcode'
    for words in ([-1], [0x100000000], [0]*1537):
        try:
            decoded_publication_fields(json.dumps(dict(published=words)).encode())
        except ValueError:
            pass
        else:
            raise AssertionError('invalid decoded word extent/value accepted')
    for invalid in (b'', b'{}', b'[]', b'{"published":[true]}', b' ' * 32769,
                    b'{"published":[0],"source_status":false}',
                    json.dumps(dict(published=[0], status='x'*129)).encode(),
                    b'{"published":[0],"published":[0]}'):
        try:
            decoded_publication_fields(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError('invalid decoded publication accepted')
    for invalid in ([], [True], [0x100000000], [0xffffffff], [0x70480001, 0x4e4c],
                    [0x70870003, 0, 0, 0, 0], [0]*1537):
        assert not mode_template_fields(invalid)['complete_packet_walk']
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
    parser.add_argument('--timeout-panic', type=Path, help='selected V379 retained CPU ring only; not a GPU snapshot')
    parser.add_argument('--published-only', action='store_true', help='require complete published stream; never claim source completeness')
    parser.add_argument('--decoded-publication', type=Path, help='bounded existing JSON artifact; preserves missing-source/provenance labels')
    args = parser.parse_args()
    if args.timeout_panic and (args.self_test or args.batch or args.publication_log or args.native_batch_log or args.published_only or args.decoded_publication):
        parser.error('timeout panic conflicts with other modes')
    if args.decoded_publication and (args.self_test or args.batch or args.publication_log or args.native_batch_log or args.published_only):
        parser.error('decoded publication conflicts with other input modes')
    if args.timeout_panic:
        if args.timeout_panic.stat().st_size > 1048576:
            parser.error('panic report exceeds 1MiB')
        chunks = []
        with args.timeout_panic.open('rb') as source:
            for _ in range(257):
                part = source.read(4096)
                if not part:
                    break
                chunks.append(part)
        data = b''.join(chunks)
        if len(data) > 1048576:
            parser.error('panic report grew beyond 1MiB')
        print(json.dumps(timeout_ring(data.decode('utf-8')), separators=(',', ':')))
    elif args.self_test:
        selftest()
    elif args.decoded_publication:
        with args.decoded_publication.open('rb') as source:
            chunks, total = [], 0
            while total < 32769:
                part = source.read(min(4096, 32769-total))
                if not part:
                    break
                chunks.append(part)
                total += len(part)
            data = b''.join(chunks)
        print(json.dumps(decoded_publication_fields(data), separators=(',', ':')))
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
