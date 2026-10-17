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
         13: 'ring_operand_leaf', 14: 'batch_leaf'}


def validate_leaf(kind, words):
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
        metadata = {key: value for key, value in record.items() if key != 'words'}
        raw = struct.pack(f"<{len(record['words'])}I", *record['words'])
        try:
            self.db.execute('INSERT INTO records VALUES (?,?,?,?,?)',
                            (record['record_id'], record['group'], record['kind'],
                             json.dumps(metadata, separators=(',', ':')), raw))
        except sqlite3.IntegrityError as exc:
            raise ValueError('duplicate completed flight record ID') from exc
        self.pending += 1
        if self.pending >= 256:
            space = os.statvfs(self.path)
            if space.f_bavail*space.f_frsize < 1024**3:
                raise ValueError('decoder disk free floor reached')
            self.db.commit()
            self.pending = 0

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
        try:
            sink.close()
        except (OSError, sqlite3.Error) as exc:
            error = str(exc)
        report = decoder.report()
        if error:
            report['error'] = error
            report['capture_transport_complete'] = False
        with (args.output_dir/'report.json').open('x') as output:
            json.dump(report, output, separators=(',', ':'))
        print(json.dumps(report, separators=(',', ':')))
        raise SystemExit(0 if report['capture_transport_complete'] else 1)
