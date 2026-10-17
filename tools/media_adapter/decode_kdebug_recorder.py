#!/usr/bin/env python3
"""Stream the owned kdebug archive; never interpret integrity as GPU success."""
import argparse
import base64
import io
import json
import os
from pathlib import Path
import re
import sqlite3
import struct
import tempfile
import zlib

HEADER = struct.Struct('<IIIIQ')
EVENT = struct.Struct('<Q5QIIQ')
INFO = struct.Struct('<iiIii')
CLOCK = struct.Struct('<QQQII')
PLATFORM = struct.Struct('<32s64s')
MAGIC = 0x444b474e
FLIGHT = 0x4e47464c00000000
CALIBRATION_ID = 0x06fefff0
CALIBRATION_MAGIC = 0x4e474b4443414c31
MASK64 = (1 << 64) - 1
KINDS = {1: 'scope_begin', 2: 'source_post_relocation', 3: 'published_commands',
         4: 'native_batch', 5: 'ring_prefix', 6: 'h2g_cpu_readback',
         7: 'submit_identity', 8: 'g2h_raw', 9: 'publication_mapping',
         10: 'timeout', 11: 'scope_end', 12: 'native_mapping_before_copy',
         13: 'ring_operand_leaf', 14: 'batch_leaf',
         15: 'retained_ring_metadata', 16: 'retained_ring_chunk',
         17: 'mapper_backing_page', 18: 'cpu_alias_ram_page',
         19: 'synchronous_batch_metadata', 20: 'synchronous_batch_chunk',
         21: 'resource_base_metadata', 22: 'resource_base_leaf',
         23: 'cpu_map_header_pair', 24: 'cpu_map_list_boundary',
         25: 'native_render_surface_output'}
RESOURCE_SLOTS = {
    1: (1, 4, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46, 49, 52, 55, 58, 62, 65, 68),
    2: (1, 4, 7, 10, 13, 16, 52, 55, 58, 62, 65)+tuple(range(19, 50, 2)),
    3: (1, 6, 11, 16, 21), 4: (1, 4, 7)}


def validate_leaf(kind, words):
    if kind == 25:
        if (len(words) != 52 or words[0] != 1 or words[1] > 2 or
                words[2] & ~31 or words[3] > 48 or words[4] > 48 or words[5] > 64 or words[11]):
            raise ValueError('invalid native surface output extent/flags')
        for bit, count, full, raw in ((1, words[3], 48, words[12:24]),
                                     (2, words[4], 48, words[24:36]),
                                     (4, words[5], 64, words[36:52])):
            if bool(words[2] & bit) != (count == full):
                raise ValueError('native surface copy status mismatch')
            if any(struct.pack(f'<{len(raw)}I', *raw)[count:]):
                raise ValueError('native surface short-copy padding')
        stable = words[3] == words[4] == 48 and words[12:24] == words[24:36]
        if bool(words[2] & 8) != stable:
            raise ValueError('native surface parameter stability mismatch')
        return
    if kind in (23, 24):
        version=words[0] if words else 0
        extended=kind==23 and version==2
        expected = (366 if extended else 226) if kind == 23 else 24
        if len(words) != expected or version not in ((1,2) if kind==23 else (1,)) or words[1] > 2 or words[2] > 1:
            raise ValueError('invalid CPU map snapshot extent/role')
        def zero_padding(raw, copied):
            return not any(struct.pack(f'<{len(raw)}I', *raw)[copied:])
        def value(raw, offset):
            return raw[offset//4] | raw[offset//4+1] << 32
        if kind == 24:
            if (words[3] not in (0, 1) or words[4] > 6 or words[5] > 256 or
                    words[7] > 20 or words[8] > 20 or words[9] != 256 or words[10] or words[11] or
                    not zero_padding(words[12:18], words[7]) or not zero_padding(words[18:24], words[8])):
                raise ValueError('invalid CPU map list boundary flags/padding')
            if words[3] == 0 and any(words[i] for i in (4, 5, 6, 7, 8)):
                raise ValueError('CPU map list begin is not empty')
            if words[3] == 1:
                if words[7] == 20 and words[16] != words[6]:
                    raise ValueError('CPU map list declared count mismatch')
                if words[4] == 0 and (words[7:9] != [20, 20] or words[5] != words[6] or words[12:17] != words[18:23]):
                    raise ValueError('CPU map list completion without equal full heads')
                if words[4] == 0:
                    head, tail = value(words[12:18], 0), value(words[12:18], 8)
                    if (words[6] == 0 and (head or tail)) or (words[6] and
                            (head >> 47 != 0x1ffff or tail >> 47 != 0x1ffff or (head | tail) & 7)):
                        raise ValueError('CPU map completed topology has invalid endpoints')
                if words[4] == 2 and (words[5] != 256 or words[6] < 256):
                    raise ValueError('CPU map list limit inconsistent')
            return
        map_size,memory_size=(296,408) if extended else (208,216)
        map_words,memory_words=map_size//4,memory_size//4
        if (words[3] >= 256 or words[6] > map_size or words[7] > map_size or words[8] > memory_size or words[9] > memory_size or
                words[10] & ~255 or words[11] or words[12] or words[13] != 2*(map_words+memory_words)):
            raise ValueError('invalid CPU map header counts/flags')
        address = words[4] | words[5] << 32
        if address >> 47 != 0x1ffff or address & 7:
            raise ValueError('noncanonical CPU map node')
        raw=words[14:]
        before,after=raw[:map_words],raw[map_words:2*map_words]
        memory,memory_after=raw[2*map_words:2*map_words+memory_words],raw[2*map_words+memory_words:]
        for raw, copied in ((before, words[6]), (after, words[7]), (memory, words[8]), (memory_after, words[9])):
            if not zero_padding(raw, copied):
                raise ValueError('CPU map uncopied bytes are nonzero')
        flags = words[10]
        for bit, copied, size in ((1, words[6], map_size), (2, words[7], map_size), (4, words[8], memory_size), (8, words[9], memory_size)):
            if bool(flags & bit) != (copied == size):
                raise ValueError('CPU map full-copy flag mismatch')
        if flags & 0xb0 and flags & 3 != 3:
            raise ValueError('CPU map identity without full headers')
        if flags & 16 and (not value(before, 0x88) or not value(before, 0x90) or
                           any(value(before, offset) != value(after, offset) for offset in (0x88, 0x90))):
            raise ValueError('CPU map owner comparison mismatch')
        if flags & 32 and (not value(before, 0) or value(before, 0) != value(after, 0)):
            raise ValueError('CPU map class comparison mismatch')
        if flags & 64 and (flags & 0x3f != 0x3f or not value(memory, 0) or not value(memory, 0x28) or
                           any(value(memory, offset) != value(memory_after, offset) for offset in (0, 0x28, 0x40, 0xd0))):
            raise ValueError('CPU backing header comparison mismatch')
        if flags & 128 and (before[4] != after[4] or any(value(before, offset) != value(after, offset)
                           for offset in (0, 0x18, 0x28, 0x30, 0x88, 0x90, 0x98, 0xc8))):
            raise ValueError('CPU map stable-header comparison mismatch')
        if extended and flags&128 and (before[0x114//4]!=after[0x114//4] or
                (flags&64 and memory[0xd8//4]!=memory_after[0xd8//4])):
            raise ValueError('CPU native extension false stability')
        return
    if kind == 21:
        if (len(words) != 8 or words[0] != 1 or words[1] > 64 or
                words[2] not in (0, 1) or not 1 <= words[4] <= 1536 or
                words[3] > words[4] or (words[2] and words[3] != words[4]) or
                words[5] not in (0, 1) or words[6] not in (0, 1) or words[7]):
            raise ValueError('invalid resource base inventory metadata')
        return
    if kind == 22:
        if (len(words) != 18 or words[0] != 1 or not 1 <= words[2] <= 64 or words[1] >= words[2] or
                words[3] not in RESOURCE_SLOTS or words[5] not in RESOURCE_SLOTS[words[3]] or
                words[4] >= 1536 or words[6] not in (0, 1, 2) or words[7] not in (0, 1) or
                any(words[i] not in (0, 1) for i in (8, 9, 17)) or words[16] or
                (words[9] and not words[8])):
            raise ValueError('invalid resource base leaf schema')
        gpu = words[10] | words[11] << 32
        shift = 12 if words[3] == 3 else 6
        internal_allowed = (words[3] == 1 or (words[3] == 2 and words[5] in (13, 16)) or
                            (words[3] == 4 and words[5] in (1, 4)))
        if (gpu >> 48 or gpu & ((1 << shift)-1) or
                (words[6] == 0 and not gpu) or (words[6] == 2 and gpu) or
                (words[6] == 1 and not internal_allowed) or
                (not words[8] and any(words[i] for i in (12, 13, 14, 15))) or
                (words[6] and any(words[i] for i in (8, 9, 12, 13, 14, 15)))):
            raise ValueError('invalid resource address domain or skipped-leaf payload')
        return
    if kind == 19:
        if (len(words) != 13 or words[0] != 1 or not 1 <= words[1] <= 8 or
                not 1 <= words[4] <= 16384 or words[5] != words[4]*4 or
                words[6] > words[5] or words[7] & ~7 or
                (not words[7] & 1 and (words[6] or words[7])) or
                bool(words[7] & 2) != (words[6] == words[5]) or
                words[9] not in (0, 1) or words[10] not in (0, 1) or
                words[11] >= 4096 or words[11] & 3 or words[12]):
            raise ValueError('invalid synchronous CPU batch metadata')
        gpu = words[2] | words[3] << 32
        if not gpu or gpu & 3 or gpu > MASK64-words[5]:
            raise ValueError('invalid synchronous CPU batch address extent')
        return
    if kind == 20:
        if (len(words) < 7 or words[0] != 1 or not 1 <= words[2] <= 16384 or
                words[3] > words[2]*4 or not 1 <= words[4] <= 512 or
                words[4] != len(words)-6 or words[1]+words[4] > words[3]//4 or
                words[5] & ~7 or words[5] & 5 != 5 or
                bool(words[5] & 2) != (words[3] == words[2]*4)):
            raise ValueError('invalid synchronous CPU batch chunk')
        return
    if kind in (17, 18):
        if (len(words) != 20 or words[0] != 1 or not 1 <= words[2] <= 16 or
                words[1] >= words[2] or words[3] & ~15 or words[16] or
                (kind == 17 and (words[17] or words[19])) or
                (kind == 18 and (words[17] not in (0, 1) or words[19] not in (0, 1))) or
                not 1 <= words[14] <= 65536 or words[2] != (words[14]+4095)//4096 or
                words[15] != min(4096, words[14]-4096*words[1])):
            raise ValueError('invalid mapper backing page schema')
        gpu, offset, physical, length, map_offset = (
            words[i] | words[i+1] << 32 for i in (4, 6, 8, 10, 12))
        if (gpu & 4095 or map_offset & 4095 or offset != map_offset + words[1]*4096 or
                offset > MASK64-words[15] or gpu > MASK64-words[15] or
                (words[3] & 1 and (not words[3] & 2 or (kind == 17 and not words[18] & 0x800))) or
                (words[3] & 4 and words[3] & 3 != 3) or
                (words[3] & 8 and (not words[3] & 1 or not physical or physical & 4095 or
                                    physical > MASK64-words[15] or length < words[15])) or
                (not words[3] & 1 and (physical or length))):
            raise ValueError('invalid mapper backing page identity/extent')
        return
    if kind == 15:
        if (len(words) != 13 or words[0] != 1 or words[6] < 4096 or words[6] > 2097152 or
                words[6] & (words[6]-1) or words[7] >= words[6] or words[9] not in (0, 1) or words[10]):
            raise ValueError('invalid retained ring metadata')
        return
    if kind == 16:
        if (len(words) < 7 or words[0] != 1 or words[1] % 4 or words[2] < 4096 or
                words[2] > 2097152 or words[2] & (words[2]-1) or words[3] >= words[2] or
                words[4] != len(words)-6 or words[1]+words[4]*4 > words[2] or words[5]):
            raise ValueError('invalid retained ring chunk extent')
        return
    if kind == 10:
        if (len(words) != 16 or words[0] != 1 or words[2] & ~7 or
                words[3] >= 6 or words[4] > 6 or words[15] > 192):
            raise ValueError('invalid timeout CPU frame schema')
        return
    if kind not in (13, 14):
        return
    expected = 16 if kind == 13 else 14
    if len(words) != expected or words[0] != 1 or words[-1] != 0:
        raise ValueError('invalid leaf schema/extent/atomicity')
    if kind == 13:
        index, operand, offset, role, exact_table, read = words[1:7]
        booleans = (role, exact_table, read, words[13], words[14])
        if index >= 8 or operand not in (1, 2, 3) or offset % 4:
            raise ValueError('invalid operand leaf identity')
        if words[14] and (not read or not words[13]):
            raise ValueError('unqualified dummy match')
    else:
        index, offset, role, exact_table, read = words[1:6]
        booleans = (role, exact_table, read, words[12])
        if index >= 2 or offset % 4:
            raise ValueError('invalid batch leaf identity')
    if any(value not in (0, 1) for value in booleans) or (read and not exact_table):
        raise ValueError('invalid leaf qualification flags')


def exact(source, size):
    data = bytearray()
    while len(data) < size:
        chunk = source.read(size - len(data))
        if not chunk:
            raise ValueError('truncated native packet')
        data.extend(chunk)
    return bytes(data)


def binary_packets(source):
    while True:
        first = source.read(1)
        if not first:
            return
        header = first + exact(source, HEADER.size - 1)
        length = HEADER.unpack(header)[2]
        if length > 65536:
            raise ValueError('native packet exceeds 64KiB')
        yield header, exact(source, length)


def text_packets(source):
    while True:
        line = source.readline(90000)
        if not line:
            return
        if len(line) >= 90000:
            raise ValueError('text channel line exceeds bound')
        if not line.startswith(b'NGKD'):
            continue  # Root PTY prompt/control text, not native event evidence.
        match = re.fullmatch(rb'NGKD H=([A-Za-z0-9+/=]{32}) P=([A-Za-z0-9+/=]{0,87384})', line.rstrip(b'\r\n'))
        if not match:
            raise ValueError('malformed native text frame')
        values = []
        for encoded in match.groups():
            raw = base64.b64decode(encoded, validate=True)
            if base64.b64encode(raw) != encoded:
                raise ValueError('noncanonical base64')
            values.append(raw)
        yield tuple(values)


class Decoder:
    def __init__(self, save, sideband=None):
        self.save = save
        self.sideband = sideband
        self.unified_bytes = 0
        self.sequence = self.complete_records = self.events = 0
        self.kind_counts = {name: 0 for name in KINDS.values()}
        self.active = {}
        self.info = self.clock = self.terminal = None
        self.origin = None
        self.platform = None
        self.calibration = None
        self.calibration_seen = False
        self.loss = False

    def event(self, raw):
        stamp, a, b, c, d, thread, debugid, cpu, unused = EVENT.unpack(raw)
        self.events += 1
        if debugid == CALIBRATION_ID:
            if self.calibration is None or raw != self.calibration or self.calibration_seen:
                raise ValueError('missing/mismatched/duplicate calibration event')
            self.calibration_seen = True
            return
        if debugid & ~3 == 0x07020008:
            self.loss = True
            return
        if debugid >> 16 != 0x06fe:
            return  # Preserve native sideband in the source archive.
        if debugid == 0x06fe0000:
            record_id, kind, count = a & 0xffffffff, c >> 32, c & 0xffffffff
            if (a & ~0xffffffff != FLIGHT or not record_id or record_id in self.active or
                    kind not in KINDS or count > 1536 or b > 0xffffffff or not thread or
                    len(self.active) >= 64):
                raise ValueError('invalid/duplicate/over-capacity flight begin')
            self.active[record_id] = {'record_id': record_id, 'kind': kind, 'group': b,
                                     'identity': f'{d:016x}', 'word_count': count,
                                     'thread': thread, 'begin_ticks': stamp,
                                     'words': [], 'hash': 14695981039346656037}
        elif debugid == 0x06fe0004:
            record_id, first = a >> 32, a & 0xffffffff
            record = self.active.get(record_id)
            if record is None or thread != record['thread'] or first != len(record['words']) or first % 6:
                raise ValueError('missing/duplicate/out-of-order flight chunk')
            valid = min(6, record['word_count'] - first)
            if valid <= 0:
                raise ValueError('flight chunk exceeds declared extent')
            words = [value for pair in (b, c, d) for value in (pair & 0xffffffff, pair >> 32)]
            if any(words[valid:]):
                raise ValueError('nonzero chunk padding')
            for word in words[:valid]:
                record['hash'] = ((record['hash'] ^ word) * 1099511628211) & MASK64
                record['words'].append(word)
        elif debugid == 0x06fe0008:
            record_id = a & 0xffffffff
            record = self.active.get(record_id)
            if (a & ~0xffffffff != FLIGHT or record is None or b != record['group'] or
                    thread != record['thread'] or c != record['word_count'] or
                    c != len(record['words']) or d != record['hash'] or stamp < record['begin_ticks']):
                raise ValueError('incomplete/mismatched flight end')
            validate_leaf(record['kind'], record['words'])
            record.update(end_ticks=stamp, kind_name=KINDS[record['kind']],
                          cpu_snapshot_only=True, atomic_snapshot=False, gpu_execution_proof=False,
                          group_associated=bool(record['group']), producer_declared_origin=self.origin,
                          source_authenticity='NOT_VERIFIED_BY_DECODER')
            self.save(record)  # Disk writer rejects IDs already completed; no unbounded seen-ID set.
            del self.active[record_id]
            self.complete_records += 1
            self.kind_counts[record['kind_name']] += 1
        else:
            raise ValueError('unknown owned trace event')

    def packet(self, header, payload):
        magic, kind, length, crc, sequence = HEADER.unpack(header)
        if magic != MAGIC or length != len(payload) or length > 65536 or sequence != self.sequence or self.terminal:
            raise ValueError('invalid native packet extent/sequence/terminal')
        zero = HEADER.pack(magic, kind, length, 0, sequence)
        if zlib.crc32(payload, zlib.crc32(zero)) != crc:
            raise ValueError('native packet checksum mismatch')
        if sequence == 0:
            if kind != 1 or length != INFO.size:
                raise ValueError('missing native ABI metadata')
            events, disabled, flags, threads, owner = INFO.unpack(payload)
            if (not 0 < events <= 524288 or disabled or flags & (8|2|0x10|0x40|0x800|0x400000) or
                    not flags & 0x100 or owner <= 0):
                raise ValueError('unqualified native trace state')
            self.info = {'events': events, 'flags': flags, 'owner': owner}
        elif kind == 5:
            if self.clock is not None or length != CLOCK.size or self.events:
                raise ValueError('invalid/duplicate clock anchor')
            wall, absolute, continuous, numer, denom = CLOCK.unpack(payload)
            if not wall or not numer or not denom:
                raise ValueError('invalid timebase')
            self.clock = {'wall_ns': wall, 'absolute_ticks': absolute,
                          'continuous_ticks': continuous, 'numer': numer, 'denom': denom}
        elif kind == 2:
            if self.clock is None or self.origin is None or self.platform is None or not length or length % EVENT.size:
                raise ValueError('missing clock or malformed native events')
            for offset in range(0, length, EVENT.size):
                self.event(payload[offset:offset+EVENT.size])
        elif kind in (3, 4):
            if self.clock is None or self.origin is None or self.platform is None or length != INFO.size:
                raise ValueError('invalid native terminal')
            events, disabled, flags, threads, owner = INFO.unpack(payload)
            self.loss |= bool(flags & 8)
            if kind == 3 and (owner != self.info['owner'] or not disabled or not flags & 0x100):
                raise ValueError('unqualified final native state')
            if kind == 3 and not self.calibration_seen:
                raise ValueError('uncalibrated native terminal')
            self.terminal = 'collector_complete' if kind == 3 else 'collector_failed'
        elif kind == 6:
            if (self.clock is None or self.origin is not None or self.events or
                    payload not in (b'NATIVE_ROOT_TRACE', b'OWNED_CPU_FIXTURE')):
                raise ValueError('missing/ambiguous producer origin label')
            self.origin = payload.decode('ascii')
        elif kind == 7:
            if self.origin is None or self.platform is not None or self.events or length != PLATFORM.size:
                raise ValueError('missing/duplicate platform provenance')
            values = []
            for raw in PLATFORM.unpack(payload):
                first, marker, rest = raw.partition(b'\0')
                if not marker or any(rest):
                    raise ValueError('invalid platform string extent/padding')
                values.append(first.decode('ascii'))
            build, boot = values
            if not re.fullmatch(r'[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}', boot):
                raise ValueError('invalid source boot UUID')
            expected = '25G229' if self.origin == 'NATIVE_ROOT_TRACE' else 'OWNED-CPU-FIXTURE'
            if build != expected or (self.origin == 'NATIVE_ROOT_TRACE' and not set(boot)-{'0', '-'}):
                raise ValueError('native platform/fixture mismatch')
            self.platform = {'build': build, 'boot_uuid': boot}
        elif kind == 9:
            if self.platform is None or not self.calibration_seen or not length or self.sideband is None:
                raise ValueError('unqualified/unhandled unified log sideband')
            self.sideband(payload)
            self.unified_bytes += length
        elif kind == 8:
            if self.platform is None or self.calibration is not None or length != EVENT.size:
                raise ValueError('missing/duplicate native ABI calibration')
            stamp, magic, nonce, size, owner, thread, code, cpu, unused = EVENT.unpack(payload)
            if (code != CALIBRATION_ID or magic != CALIBRATION_MAGIC or not stamp or not nonce or
                    size != EVENT.size or owner != self.info['owner'] or not thread):
                raise ValueError('native ABI calibration mismatch')
            self.calibration = payload
        else:
            raise ValueError('unknown/duplicate native metadata')
        self.sequence += 1

    def report(self):
        return {'complete_records': self.complete_records, 'pending_records': sorted(self.active),
                'kind_counts': self.kind_counts,
                'producer_declared_origin': self.origin, 'source_authenticity': 'NOT_VERIFIED_BY_DECODER',
                'clock_anchor': self.clock, 'native_state': self.info,
                'platform': self.platform,
                'abi_calibration_event_seen': self.calibration_seen,
                'native_events': self.events, 'terminal': self.terminal or 'MISSING',
                'unified_log_bytes': self.unified_bytes,
                'unified_log_complete': False,
                'native_loss_observed': self.loss,
                'capture_transport_complete': self.terminal == 'collector_complete' and not self.active and not self.loss,
                'upstream_complete': False, 'gpu_execution_proof': False}


class DiskSink:
    """Disk-backed ID uniqueness/index; no growing seen-ID set or inode flood."""
    def __init__(self, directory):
        directory.mkdir(mode=0o700)
        self.path = directory/'records.sqlite3'
        fd = os.open(self.path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        os.close(fd)
        self.db = sqlite3.connect(self.path)
        self.db.execute('PRAGMA journal_mode=WAL')
        self.db.execute('PRAGMA synchronous=FULL')
        self.db.execute('PRAGMA cache_size=-2048')
        self.db.execute('PRAGMA mmap_size=0')
        self.db.execute('PRAGMA temp_store=FILE')
        self.db.execute('PRAGMA wal_autocheckpoint=128')
        self.db.execute('CREATE TABLE records (id INTEGER PRIMARY KEY, group_id INTEGER, kind INTEGER, metadata TEXT, payload BLOB) WITHOUT ROWID')
        self.db.execute('CREATE INDEX by_group_kind ON records(group_id,kind)')
        self.db.execute('CREATE TABLE rings (identity TEXT PRIMARY KEY, group_id INTEGER, bytes INTEGER, tail INTEGER, admitted INTEGER, captured INTEGER) WITHOUT ROWID')
        self.db.execute('CREATE TABLE backings (identity TEXT, group_id INTEGER, pages INTEGER, captured INTEGER, valid INTEGER, bytes INTEGER, map_offset TEXT, options INTEGER, PRIMARY KEY(identity,group_id)) WITHOUT ROWID')
        self.db.execute('CREATE TABLE cpu_backings (identity TEXT, group_id INTEGER, pages INTEGER, captured INTEGER, valid INTEGER, bytes INTEGER, map_offset TEXT, options INTEGER, PRIMARY KEY(identity,group_id)) WITHOUT ROWID')
        self.db.execute('CREATE TABLE synchronous_batches (identity TEXT PRIMARY KEY, group_id INTEGER, gpu TEXT, words INTEGER, copied INTEGER, flags INTEGER, captured INTEGER) WITHOUT ROWID')
        self.db.execute('CREATE TABLE resource_batches (identity TEXT, group_id INTEGER, fields INTEGER, words INTEGER, admitted INTEGER, captured INTEGER, stable INTEGER, last_field TEXT, PRIMARY KEY(identity,group_id)) WITHOUT ROWID')
        self.db.execute('CREATE TABLE map_lists (identity TEXT, group_id INTEGER, role INTEGER, list INTEGER, attempts INTEGER, good INTEGER, known INTEGER, ended INTEGER, status INTEGER, walked INTEGER, declared INTEGER, PRIMARY KEY(identity,group_id,role,list)) WITHOUT ROWID')
        self.db.commit()
        self.pending = 0
        self.sideband_file = None

    def sideband(self, data):
        if self.sideband_file is None:
            fd = os.open(self.path.parent/'unified.log', os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            self.sideband_file = os.fdopen(fd, 'wb')
        space = os.statvfs(self.path)
        if space.f_bavail*space.f_frsize < 1024**3:
            raise ValueError('decoder sideband disk free floor reached')
        self.sideband_file.write(data)

    def __call__(self, record):
        if not self.db.in_transaction:
            self.db.execute('BEGIN')
        self.db.execute('SAVEPOINT flight_record')
        try:
            self._save_record(record)
        except Exception:
            self.db.execute('ROLLBACK TO flight_record')
            self.db.execute('RELEASE flight_record')
            raise
        self.db.execute('RELEASE flight_record')
        self.pending += 1
        if self.pending >= 256:
            space = os.statvfs(self.path)
            if space.f_bavail*space.f_frsize < 1024**3:
                raise ValueError('decoder disk free floor reached')
            self.db.commit()
            self.pending = 0

    def _save_record(self, record):
        words = record['words']
        validate_leaf(record['kind'], words)
        if self.db.execute('SELECT 1 FROM records WHERE id=?', (record['record_id'],)).fetchone():
            raise ValueError('duplicate completed flight record ID')
        if record['kind'] == 25 and (not record['group'] or int(record['identity'], 16)):
            raise ValueError('surface output requires owning flight group and no pointer identity')
        if record['kind'] in (23, 24):
            key = (record['identity'], record['group'], words[1], words[2])
            if not int(record['identity'], 16) or not record['group']:
                raise ValueError('CPU map snapshot parent missing')
            if record['kind'] == 24 and words[3] == 0:
                try:
                    self.db.execute('INSERT INTO map_lists VALUES (?,?,?,?,0,0,0,0,NULL,NULL,NULL)', key)
                except sqlite3.IntegrityError as exc:
                    raise ValueError('duplicate CPU map list begin') from exc
            else:
                current = self.db.execute('SELECT attempts,good,ended FROM map_lists WHERE identity=? AND group_id=? AND role=? AND list=?', key).fetchone()
                if current is None or current[2]:
                    raise ValueError('CPU map node/end before begin or after end')
                if record['kind'] == 23:
                    if current[0] != words[3]:
                        raise ValueError('CPU map node duplicate/out of order')
                    self.db.execute('UPDATE map_lists SET attempts=attempts+1,good=good+?,known=known+? WHERE identity=? AND group_id=? AND role=? AND list=?',
                                    (int(words[10] & 0x93 == 0x93), int(words[10] & 0x60 == 0x60))+key)
                else:
                    if current[0] not in (words[5], words[5]+1):
                        raise ValueError('CPU map attempted/walked count mismatch')
                    if words[4] == 0 and (current[0] != words[5] or current[1] != current[0]):
                        raise ValueError('CPU map completion missing qualified headers')
                    self.db.execute('UPDATE map_lists SET ended=1,status=?,walked=?,declared=? WHERE identity=? AND group_id=? AND role=? AND list=?',
                                    (words[4], words[5], words[6])+key)
        elif record['kind'] == 21:
            if not int(record['identity'], 16) or not record['group']:
                raise ValueError('resource inventory batch identity/group missing')
            try:
                self.db.execute('INSERT INTO resource_batches VALUES (?,?,?,?,?,0,0,NULL)',
                                (record['identity'], record['group'], words[1], words[4], int(words[2] and words[5])))
            except sqlite3.IntegrityError as exc:
                raise ValueError('duplicate resource inventory metadata') from exc
        elif record['kind'] == 22:
            key = (record['identity'], record['group'])
            current = self.db.execute('SELECT fields,words,admitted,captured,last_field FROM resource_batches WHERE identity=? AND group_id=?', key).fetchone()
            if (current is None or current[0] != words[2] or words[4]+1 >= current[1] or
                    current[2] != 1 or current[3] != words[1]*2+words[7]):
                raise ValueError('missing/denied/duplicate/out-of-order resource leaf')
            signature = json.dumps(words[3:7]+words[10:12], separators=(',', ':'))
            if words[7] and current[4] != signature:
                raise ValueError('private/bootstrap resource field mismatch')
            self.db.execute('UPDATE resource_batches SET captured=captured+1,stable=stable+?,last_field=? WHERE identity=? AND group_id=?',
                            (words[17], signature)+key)
        elif record['kind'] == 19:
            if int(record['identity'], 16) != words[1] or not record['group']:
                raise ValueError('synchronous CPU batch identity/group mismatch')
            try:
                self.db.execute('INSERT INTO synchronous_batches VALUES (?,?,?,?,?,?,0)',
                                (record['identity'], record['group'], f'{words[3]:08x}{words[2]:08x}',
                                 words[4], words[6], words[7]))
            except sqlite3.IntegrityError as exc:
                raise ValueError('duplicate synchronous CPU batch identity') from exc
        elif record['kind'] == 20:
            current = self.db.execute('SELECT group_id,words,copied,flags,captured FROM synchronous_batches WHERE identity=?',
                                      (record['identity'],)).fetchone()
            if current != (record['group'], words[2], words[3], words[5], words[1]):
                raise ValueError('missing/duplicate/out-of-order/mismatched synchronous CPU batch chunk')
            self.db.execute('UPDATE synchronous_batches SET captured=captured+? WHERE identity=?',
                            (words[4], record['identity']))
        elif record['kind'] in (17, 18):
            table = 'backings' if record['kind'] == 17 else 'cpu_backings'
            gpu = words[4] | words[5] << 32
            if gpu != int(record['identity'], 16)+words[1]*4096:
                raise ValueError('mapper backing GPU identity mismatch')
            key = (record['identity'], record['group'])
            config = (words[2], words[14], f'{words[13]:08x}{words[12]:08x}', words[18])
            if words[1] == 0:
                try:
                    self.db.execute(f'INSERT INTO {table} VALUES (?,?,?,0,0,?,?,?)',
                                    key+(config[0],)+config[1:])
                except sqlite3.IntegrityError as exc:
                    raise ValueError('duplicate mapper backing capture') from exc
            current = self.db.execute(f'SELECT pages,bytes,map_offset,options,captured FROM {table} WHERE identity=? AND group_id=?', key).fetchone()
            if current != config+(words[1],):
                raise ValueError('missing/duplicate/out-of-order/mismatched mapper backing page')
            self.db.execute(f'UPDATE {table} SET captured=captured+1,valid=valid+? WHERE identity=? AND group_id=?',
                            (int(words[3] == 15),)+key)
        elif record['kind'] == 15:
            try:
                self.db.execute('INSERT INTO rings VALUES (?,?,?,?,?,0)',
                                (record['identity'], record['group'], words[6], words[7], words[9]))
            except sqlite3.IntegrityError as exc:
                raise ValueError('duplicate ring capture identity') from exc
        elif record['kind'] == 16:
            ring = self.db.execute('SELECT group_id,bytes,tail,admitted,captured FROM rings WHERE identity=?',
                                   (record['identity'],)).fetchone()
            if ring is None or ring != (record['group'], words[2], words[3], 1, words[1]):
                raise ValueError('missing/duplicate/out-of-order/unqualified ring chunk')
            self.db.execute('UPDATE rings SET captured=captured+? WHERE identity=?',
                            (words[4]*4, record['identity']))
        metadata = {key: value for key, value in record.items() if key != 'words'}
        raw = struct.pack(f"<{len(record['words'])}I", *record['words'])
        try:
            self.db.execute('INSERT INTO records VALUES (?,?,?,?,?)',
                            (record['record_id'], record['group'], record['kind'],
                             json.dumps(metadata, separators=(',', ':')), raw))
        except sqlite3.IntegrityError as exc:
            raise ValueError('duplicate completed flight record ID') from exc

    def backing_report(self, cpu_alias=False):
        table = 'cpu_backings' if cpu_alias else 'backings'
        total, covered, valid = self.db.execute(f'SELECT COUNT(*),COALESCE(SUM(captured=pages),0),COALESCE(SUM(captured=pages AND valid=pages),0) FROM {table}').fetchone()
        return {'captures': total, 'complete_page_coverage': covered,
                'all_pages_stable_valid': valid, 'gpu_execution_proven': False}

    def ring_report(self):
        total, complete = self.db.execute('SELECT COUNT(*),COALESCE(SUM(admitted=1 AND captured=bytes),0) FROM rings').fetchone()
        missing = [dict(zip(('identity', 'bytes', 'captured', 'admitted'), row)) for row in
                   self.db.execute('SELECT identity,bytes,captured,admitted FROM rings WHERE admitted!=1 OR captured!=bytes LIMIT 64')]
        return {'captures': total, 'complete_cpu_copies': complete, 'incomplete': total-complete,
                'incomplete_examples': missing, 'gpu_execution_proof': False}

    def synchronous_batch_report(self):
        total, complete = self.db.execute('SELECT COUNT(*),COALESCE(SUM(flags=7 AND copied=words*4 AND captured=words),0) FROM synchronous_batches').fetchone()
        return {'captures': total, 'complete_cpu_copies': complete, 'incomplete': total-complete,
                'maximum_captures': 8, 'all_submissions_captured': False,
                'optional_terminal_extent_included': False,
                'atomic_snapshot': False, 'gpu_execution_proof': False}

    def resource_report(self):
        total, complete, stable = self.db.execute('SELECT COUNT(*),COALESCE(SUM(admitted=1 AND captured=fields*2),0),COALESCE(SUM(admitted=1 AND captured=fields*2 AND stable=captured),0) FROM resource_batches').fetchone()
        return {'captures': total, 'complete_field_coverage': complete, 'stable_field_coverage': stable,
                'stability_scope': 'SOURCE_IDENTITY_ONLY_NOT_TABLE_ATOMICITY',
                'only_base_first_pages': True, 'allocation_extents_proven': False,
                'gpu_execution_proof': False}

    def map_list_report(self):
        total, complete, unknown = self.db.execute('SELECT COUNT(*),COALESCE(SUM(ended=1 AND status=0),0),COALESCE(SUM(attempts-known),0) FROM map_lists').fetchone()
        return {'lists': total, 'observed_complete_topologies': complete, 'partial_or_missing_end': total-complete,
                'unknown_or_partial_backing_nodes': unknown, 'native_object_lease_proven': False,
                'allocation_extents_proven': False, 'gpu_execution_proof': False}

    def close(self):
        if self.sideband_file is not None:
            self.sideband_file.flush()
            os.fsync(self.sideband_file.fileno())
            self.sideband_file.close()
            os.chmod(self.path.parent/'unified.log', 0o400)
        self.db.commit()  # Preserve completed records even if a later frame is corrupt.
        self.db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
        self.db.close()
        os.chmod(self.path, 0o400)


def self_test():
    backing = [1, 0, 16, 15, 0x40010000, 0, 0, 0, 0x12300000, 3,
               65536, 0, 0, 0, 65536, 4096, 0, 0, 0x888, 0]
    validate_leaf(17, backing)
    cpu_alias = list(backing)
    cpu_alias[18] = 0
    validate_leaf(18, cpu_alias)  # CPU RAM, not the native mapper-domain getter.
    for index in (17, 19):
        bad = list(cpu_alias)
        bad[index] = 2
        try:
            validate_leaf(18, bad)
        except ValueError:
            continue
        raise AssertionError('invalid CPU mapper-presence observation accepted')
    for index, value in ((0, 2), (1, 16), (2, 17), (3, 16), (4, 1), (6, 4096),
                         (8, 1), (10, 4095), (12, 1), (14, 65537), (15, 1),
                         (16, 1), (17, 1), (18, 0), (19, 1)):
        bad = list(backing)
        bad[index] = value
        try:
            validate_leaf(17, bad)
        except ValueError:
            continue
        raise AssertionError('invalid mapper backing schema accepted')
    for size in (0, 19, 21):
        try:
            validate_leaf(17, (backing+[0])[:size])
        except ValueError:
            continue
        raise AssertionError('invalid mapper backing record length accepted')
    denied = list(backing)
    denied[3], denied[8], denied[9], denied[10] = 0, 0, 0, 0
    validate_leaf(17, denied)  # Explicit absence, never physical validity.
    timeout = [1, 3, 1, 3, 3, 42, 31, 2, 1, 0, 0xb8, 0x64, 0, 0xb8, 1, 184]
    validate_leaf(10, timeout)
    for index, value in ((0, 2), (2, 8), (3, 6), (4, 7), (15, 193)):
        bad = list(timeout)
        bad[index] = value
        try:
            validate_leaf(10, bad)
        except ValueError:
            continue
        raise AssertionError('invalid timeout schema accepted')
    leaf = [1, 0, 1, 0x3c, 0, 1, 1, 0x40010000, 0, 0x12345000, 3, 0x89, 0, 1, 0, 0]
    validate_leaf(13, leaf)
    batch = [1, 0, 0x3c, 0, 1, 1, 0x40010000, 0, 0x12345000, 3, 0x89, 0, 0, 0]
    validate_leaf(14, batch)
    for kind, good, changes in ((13, leaf, ((0, 2), (1, 8), (2, 4), (3, 3), (4, 2), (5, 0), (15, 1))),
                                (14, batch, ((0, 0), (1, 2), (2, 1), (3, 2), (4, 0), (13, 1)))):
        for index, value in changes:
            bad = list(good)
            bad[index] = value
            try:
                validate_leaf(kind, bad)
            except ValueError:
                continue
            raise AssertionError('invalid leaf schema accepted')
    def packet(kind, seq, data):
        zero = HEADER.pack(MAGIC, kind, len(data), 0, seq)
        return HEADER.pack(MAGIC, kind, len(data), zlib.crc32(data, zlib.crc32(zero)), seq), data
    def event(code, a, b=0, c=0, d=0, thread=77):
        return EVENT.pack(123, a, b, c, d, thread, code, 0, 0)
    words = [0xffffffff, 0x12345678, 3, 4, 5, 6, 7]
    hashed = 14695981039346656037
    for word in words:
        hashed = ((hashed ^ word)*1099511628211) & MASK64
    records = [event(0x06fe0000, FLIGHT|1, 42, (3 << 32)|7, 0x40010000)]
    for first in (0, 6):
        block = words[first:first+6]+[0]*max(0, first+6-len(words))
        pairs = [block[i]|(block[i+1]<<32) for i in (0, 2, 4)]
        records.append(event(0x06fe0004, (1 << 32)|first, *pairs))
    records.append(event(0x06fe0008, FLIGHT|1, 42, 7, hashed))
    beacon = EVENT.pack(99, CALIBRATION_MAGIC, 55, EVENT.size, 123, 88, CALIBRATION_ID, 0, 0)
    archive = [packet(1, 0, INFO.pack(262144, 0, 0x100, 2, 123)),
               packet(5, 1, CLOCK.pack(100, 10, 10, 1, 1)),
               packet(6, 2, b'OWNED_CPU_FIXTURE'),
               packet(7, 3, PLATFORM.pack(b'OWNED-CPU-FIXTURE', b'00000000-0000-0000-0000-000000000000')),
               packet(8, 4, beacon), packet(2, 5, beacon+b''.join(records)),
               packet(3, 6, INFO.pack(262144, 1, 0x100, 2, 123))]
    saved = []
    decoder = Decoder(saved.append)
    for item in archive:
        decoder.packet(*item)
    assert saved[0]['words'] == words and decoder.report()['capture_transport_complete']
    assert not decoder.report()['upstream_complete'] and not decoder.report()['gpu_execution_proof']
    binary = b''.join(header+data for header, data in archive)
    assert list(binary_packets(io.BytesIO(binary))) == archive
    text = b''.join(b'NGKD H='+base64.b64encode(header)+b' P='+base64.b64encode(data)+b'\r\n' for header,data in archive)
    assert list(text_packets(io.BytesIO(b'Password:\r\n'+text))) == archive
    def rejected(items):
        candidate = Decoder(lambda _: None)
        try:
            for item in items:
                candidate.packet(*item)
        except (ValueError, struct.error):
            assert not candidate.report()['capture_transport_complete']
            return
        raise AssertionError('invalid native envelope accepted')
    damaged = bytearray(archive[5][0])
    damaged[12] ^= 1
    rejected(archive[:5]+[(bytes(damaged), archive[5][1])])
    rejected(archive[:5]+[packet(2, 6, archive[5][1])])
    rejected(archive[:2]+[packet(6, 2, b'UNKNOWN')])
    rejected(archive[:3]+[packet(7, 3, PLATFORM.pack(b'25G229', b'00000000-0000-0000-0000-000000000000'))])
    rejected(archive[:4]+[packet(8, 4, event(CALIBRATION_ID, CALIBRATION_MAGIC, 55, EVENT.size, 999))])
    rejected(archive[:4]+[packet(3, 4, INFO.pack(262144, 1, 0x100, 2, 123))])
    rejected(archive[:6]+[packet(3, 6, INFO.pack(262144, 0, 0x100, 2, 123))])
    for terminal_kind, loss in ((4, False), (3, True)):
        candidate = Decoder(lambda _: None)
        for item in archive[:6]:
            candidate.packet(*item)
        candidate.packet(*packet(terminal_kind, 6, INFO.pack(262144, 1, 0x108 if loss else 0x100, 2, 123)))
        assert not candidate.report()['capture_transport_complete']
    for bad in (records[1:], records[:-1]+[records[-1][:-16]],
                records[:2]+records[1:], records[:2]+[event(0x06fe0004, (1<<32)|6, 7, thread=78)]+records[3:],
                records[:-1]+[event(0x06fe0008, FLIGHT|1, 42, 7, hashed^1)]):
        try:
            d = Decoder(lambda _: None)
            for header,data in archive[:5]:
                d.packet(header,data)
            d.packet(*packet(2, 5, beacon+b''.join(bad)))
        except (ValueError, struct.error):
            continue
        raise AssertionError('bad flight event accepted')
    d = Decoder(lambda _: None)
    for item in archive[:5]:
        d.packet(*item)
    d.packet(*packet(2, 5, beacon+records[0]))
    assert d.report()['pending_records'] == [1] and not d.report()['capture_transport_complete']
    with tempfile.TemporaryDirectory(prefix='ngreen-kdebug-decode-') as tmp:
        save = DiskSink(Path(tmp)/'records')
        save(saved[0])
        try:
            save(saved[0])
        except ValueError:
            pass
        else:
            raise AssertionError('record overwritten')
        ring_record = dict(saved[0], record_id=100, kind=15, identity='0000000000001234',
                           words=[1, 0x100319, 0x20000020, 3, 3, 1, 4096, 184, 0, 1, 0, 7, 0])
        save(ring_record)
        assert save.ring_report()['incomplete'] == 1
        save.db.execute("CREATE TEMP TRIGGER owned_failure BEFORE INSERT ON records WHEN NEW.id=101 BEGIN SELECT RAISE(ABORT,'owned injected write failure'); END")
        failed_part = dict(ring_record, record_id=101, kind=16,
                           words=[1, 0, 4096, 184, 512, 0]+[0]*512)
        try:
            save(failed_part)
        except ValueError:
            pass
        else:
            raise AssertionError('injected record write failure accepted')
        assert save.ring_report()['incomplete_examples'][0]['captured'] == 0
        assert save.db.execute('SELECT COUNT(*) FROM records WHERE id=101').fetchone()[0] == 0
        save.db.execute('DROP TRIGGER owned_failure')
        for offset in (0, 2048):
            part = dict(ring_record, record_id=101+offset//2048, kind=16,
                        words=[1, offset, 4096, 184, 512, 0]+[0]*512)
            save(part)
        assert save.ring_report()['complete_cpu_copies'] == 1
        try:
            save(dict(part, record_id=103))
        except ValueError:
            pass
        else:
            raise AssertionError('duplicate ring range accepted')
        assert save.ring_report()['complete_cpu_copies'] == 1
        backing_record = dict(saved[0], record_id=200, kind=17, identity='0000000040010000', words=backing)
        save(backing_record)
        assert save.backing_report()['complete_page_coverage'] == 0
        try:
            save(dict(backing_record, record_id=201))
        except ValueError:
            pass
        else:
            raise AssertionError('duplicate mapper backing accepted')
        assert save.backing_report()['complete_page_coverage'] == 0
        next_page = list(backing)
        next_page[1], next_page[4], next_page[6], next_page[8], next_page[10] = 1, 0x40011000, 4096, 0x12301000, 61440
        save.db.execute("CREATE TEMP TRIGGER backing_failure BEFORE INSERT ON records WHEN NEW.id=201 BEGIN SELECT RAISE(ABORT,'owned backing injected write failure'); END")
        try:
            save(dict(backing_record, record_id=201, words=next_page))
        except ValueError:
            pass
        else:
            raise AssertionError('injected backing write failure accepted')
        assert save.db.execute('SELECT captured FROM backings').fetchone()[0] == 1
        save.db.execute('DROP TRIGGER backing_failure')
        for i in range(1, 16):
            page = list(backing)
            page[1], page[4], page[6], page[8], page[10] = i, 0x40010000+i*4096, i*4096, 0x12300000+i*4096, 65536-i*4096
            save(dict(backing_record, record_id=201+i, words=page))
        assert save.backing_report() == {'captures': 1, 'complete_page_coverage': 1,
                                          'all_pages_stable_valid': 1, 'gpu_execution_proven': False}
        assert save.backing_report(cpu_alias=True)['captures'] == 0
        save(dict(backing_record, record_id=250, kind=18, words=cpu_alias))
        assert save.backing_report(cpu_alias=True)['complete_page_coverage'] == 0
        assert save.backing_report()['complete_page_coverage'] == 1
        for i in range(1, 16):
            page = list(cpu_alias)
            page[1], page[4], page[6], page[8], page[10] = i, 0x40010000+i*4096, i*4096, 0x12300000+i*4096, 65536-i*4096
            save(dict(backing_record, record_id=250+i, kind=18, words=page))
        assert save.backing_report(cpu_alias=True)['complete_page_coverage'] == 1
        try:
            save(dict(backing_record, record_id=280, kind=18, words=cpu_alias))
        except ValueError:
            pass
        else:
            raise AssertionError('duplicate CPU alias capture accepted')
        batch = dict(saved[0], record_id=300, group=3, kind=19, identity='0000000000000001',
                     words=[1, 1, 0x41d34000, 0, 1024, 4096, 4096, 7, 2, 0, 1, 0, 0])
        save(batch)
        chunk = dict(batch, record_id=301, kind=20, words=[1, 0, 1024, 4096, 512, 7]+[0]*512)
        save(chunk)
        assert save.synchronous_batch_report()['incomplete'] == 1
        for bad in (dict(chunk, record_id=302), dict(chunk, record_id=302, group=4),
                    dict(chunk, record_id=302, identity='0000000000000002')):
            try:
                save(bad)
            except ValueError:
                pass
            else:
                raise AssertionError('duplicate/mismatched synchronous chunk accepted')
        assert save.db.execute('SELECT captured FROM synchronous_batches').fetchone()[0] == 512
        second = list(chunk['words']); second[1] = 512
        save(dict(chunk, record_id=302, words=second))
        assert save.synchronous_batch_report()['complete_cpu_copies'] == 1
        short = list(batch['words']); short[1], short[6], short[7] = 2, 3, 5
        save(dict(batch, record_id=303, identity='0000000000000002', words=short))
        assert save.synchronous_batch_report()['incomplete'] == 1
        refused = list(batch['words']); refused[1], refused[6], refused[7] = 3, 0, 0
        save.db.execute("CREATE TEMP TRIGGER sync_failure BEFORE INSERT ON records WHEN NEW.id=304 BEGIN SELECT RAISE(ABORT,'owned sync injected write failure'); END")
        try:
            save(dict(batch, record_id=304, identity='0000000000000003', words=refused))
        except ValueError:
            pass
        else:
            raise AssertionError('injected synchronous metadata write failure accepted')
        assert save.synchronous_batch_report()['captures'] == 2
        save.db.execute('DROP TRIGGER sync_failure')
        save(dict(batch, record_id=304, identity='0000000000000003', words=refused))
        assert save.synchronous_batch_report()['incomplete'] == 2
        assert not save.synchronous_batch_report()['all_submissions_captured']
        resource = dict(saved[0], record_id=400, group=3, kind=21, identity='0000000040010000',
                        words=[1, 2, 1, 71, 71, 1, 1, 0])
        save(resource)
        leaf = dict(resource, record_id=401, kind=22,
                    words=[1, 0, 2, 1, 10, 10, 0, 0, 1, 1, 0x4075e000, 0, 0x12340000, 0, 0x8b, 0, 0, 1])
        save(leaf)
        assert save.resource_report()['complete_field_coverage'] == 0
        for index, value in ((2, 3), (7, 2), (8, 0), (10, 0x4075e001), (16, 1), (17, 2)):
            bad = list(leaf['words']); bad[index] = value
            try:
                save(dict(leaf, record_id=402, words=bad))
            except ValueError:
                pass
            else:
                raise AssertionError('invalid/out-of-order resource leaf accepted')
        pair = list(leaf['words']); pair[7] = 1
        wrong = list(pair); wrong[10] += 4096
        try:
            save(dict(leaf, record_id=402, words=wrong))
        except ValueError:
            pass
        else:
            raise AssertionError('cross-table resource identity mismatch accepted')
        save.db.execute("CREATE TEMP TRIGGER resource_failure BEFORE INSERT ON records WHEN NEW.id=402 BEGIN SELECT RAISE(ABORT,'owned resource injected write failure'); END")
        try:
            save(dict(leaf, record_id=402, words=pair))
        except ValueError:
            pass
        else:
            raise AssertionError('injected resource leaf write failure accepted')
        assert save.db.execute('SELECT captured FROM resource_batches').fetchone()[0] == 1
        save.db.execute('DROP TRIGGER resource_failure')
        save(dict(leaf, record_id=402, words=pair))
        internal = [1, 1, 2, 1, 16, 16, 1, 0, 0, 0, 0x4000, 0, 0, 0, 0, 0, 0, 1]
        save(dict(leaf, record_id=403, words=internal))
        internal[7] = 1
        save(dict(leaf, record_id=404, words=internal))
        assert save.resource_report()['complete_field_coverage'] == 1
        assert save.resource_report()['stable_field_coverage'] == 1
        denied = list(resource['words']); denied[2], denied[3] = 0, 20
        save(dict(resource, record_id=405, identity='0000000040020000', words=denied))
        try:
            save(dict(leaf, record_id=406, identity='0000000040020000'))
        except ValueError:
            pass
        else:
            raise AssertionError('refused resource inventory admitted a leaf')
        boundary = dict(saved[0], record_id=700, group=3, kind=24, identity='0000000040010000',
                        words=[1, 0, 0, 0, 0, 0, 0, 0, 0, 256, 0, 0]+[0]*12)
        save(boundary)
        def put64(raw, offset, value):
            raw[offset//4:offset//4+2] = [value & 0xffffffff, value >> 32]
        node, memory = [0]*52, [0]*54
        put64(node, 0, 0xffffff804010a000)
        put64(node, 0x18, 0xffffff8041004000)
        put64(node, 0x88, 0xffffff8040200000)
        put64(node, 0x90, 0xffffff8041000000)
        put64(node, 0x98, 0x40010000)
        put64(node, 0xc8, 131072); node[4] = 0x4001
        put64(memory, 0, 0xffffff804010b000)
        put64(memory, 0x28, 0xffffff8040200000)
        put64(memory, 0x40, 65536)
        put64(memory, 0xd0, 0xffffff8041006000)
        map_words = [1, 0, 0, 0, 0x41002000, 0xffffff80, 208, 208, 216, 216, 255, 0, 0, 212]+node+node+memory+memory
        snapshot = dict(boundary, record_id=701, kind=23, words=map_words)
        native_node=node+[0]*22
        native_memory=memory+[0]*48
        native_node[0x114//4]=1
        native_memory[0xd8//4]=0x300
        native_words=[2,0,0,0,0x41002000,0xffffff80,296,296,408,408,255,0,0,352]+native_node*2+native_memory*2
        validate_leaf(23,native_words)
        for index,val in ((0,3),(6,297),(8,409),(13,351),
                          (14+74+0x114//4,2),(14+148+102+0xd8//4,0x400)):
            bad=list(native_words);bad[index]=val
            try:validate_leaf(23,bad)
            except ValueError:pass
            else:raise AssertionError('invalid native object extent/version/cache stability accepted')
        for index, val in ((6, 209), (10, 254), (11, 1), (12, 1), (13, 213), (14+104+16, 0)):
            bad = list(map_words); bad[index] = val
            try:
                validate_leaf(23, bad)
            except ValueError:
                pass
            else:
                raise AssertionError('invalid/false-leased CPU map header accepted')
        save.db.execute("CREATE TEMP TRIGGER map_failure BEFORE INSERT ON records WHEN NEW.id=701 BEGIN SELECT RAISE(ABORT,'owned map injected write failure'); END")
        try:
            save(snapshot)
        except ValueError:
            pass
        else:
            raise AssertionError('injected CPU map header write failure accepted')
        assert save.db.execute('SELECT attempts FROM map_lists').fetchone()[0] == 0
        save.db.execute('DROP TRIGGER map_failure')
        save(snapshot)
        assert save.map_list_report()['observed_complete_topologies'] == 0
        heads = [0x41002000, 0xffffff80, 0x41002000, 0xffffff80, 1, 0]
        ended = dict(boundary, record_id=702, words=[1, 0, 0, 1, 0, 1, 1, 20, 20, 256, 0, 0]+heads+heads)
        save(ended)
        assert save.map_list_report()['observed_complete_topologies'] == 1
        assert not save.map_list_report()['native_object_lease_proven']
        try:
            save(dict(snapshot, record_id=703))
        except ValueError:
            pass
        else:
            raise AssertionError('CPU map header after END accepted')
        for index, value in ((5, 1), (6, 4097), (7, 5), (9, 2), (12, 1)):
            bad = list(batch['words']); bad[index] = value
            try:
                validate_leaf(19, bad)
            except ValueError:
                pass
            else:
                raise AssertionError('invalid synchronous batch metadata accepted')
        sideband = Decoder(lambda _: None, save.sideband)
        for item in archive[:6]:
            sideband.packet(*item)
        sideband.packet(*packet(9, 6, b'owned unified fixture\n'))
        sideband.packet(*packet(3, 7, INFO.pack(262144, 1, 0x100, 2, 123)))
        assert sideband.report()['unified_log_bytes'] == 22
        save.close()
        assert (Path(tmp)/'records'/'unified.log').read_bytes() == b'owned unified fixture\n'
    print('KDEBUG_STREAM_DECODER_SELFTEST_OK owned-fixtures-only')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--input', type=Path)
    parser.add_argument('--format', choices=('binary', 'text'), default='binary')
    parser.add_argument('--output-dir', type=Path)
    args = parser.parse_args()
    if args.self_test:
        if args.input or args.output_dir:
            parser.error('self-test cannot consume runtime evidence')
        self_test()
    else:
        if not args.input or not args.output_dir:
            parser.error('--input and fresh --output-dir required')
        sink = DiskSink(args.output_dir)
        decoder = Decoder(sink, sink.sideband)
        error = None
        try:
            with args.input.open('rb') as source:
                packets = binary_packets(source) if args.format == 'binary' else text_packets(source)
                for header, data in packets:
                    decoder.packet(header, data)
        except (ValueError, OSError, struct.error, sqlite3.Error) as exc:
            error = str(exc)
        ring_report = sink.ring_report()
        backing_report = sink.backing_report()
        cpu_alias_report = sink.backing_report(cpu_alias=True)
        synchronous_batch_report = sink.synchronous_batch_report()
        resource_report = sink.resource_report()
        map_list_report = sink.map_list_report()
        try:
            sink.close()
        except (OSError, sqlite3.Error) as exc:
            error = str(exc)
        report = decoder.report()
        report['rings'] = ring_report
        report['backings'] = backing_report
        report['cpu_alias_backings'] = cpu_alias_report
        report['synchronous_batches'] = synchronous_batch_report
        report['resource_bases'] = resource_report
        report['cpu_map_lists'] = map_list_report
        if error:
            report['error'] = error
            report['capture_transport_complete'] = False
        with (args.output_dir/'report.json').open('x') as output:
            json.dump(report, output, separators=(',', ':'))
        print(json.dumps(report, separators=(',', ':')))
        raise SystemExit(0 if report['capture_transport_complete'] else 1)
