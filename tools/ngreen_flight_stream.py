#!/usr/bin/env python3
"""Bounded-memory Host stream archive. Does not acquire GPU/kernel state.

Each input channel gets its own exclusive archive. Kernel/adapter collectors
must still supply request identities and their own loss counters. A valid EOF
here proves only transport-file integrity, never upstream completeness.
"""
import argparse
import json
import os
import struct
import sys
import time
import zlib

MAGIC = b'NGFL\x01\0\0\0'
HEADER = struct.Struct('<QQIII')  # sequence, monotonic_ns, kind, bytes, CRC32
CHUNK = 65536
LIMIT = 64 * 1024**3
FREE_FLOOR = 1024**3


def write_all(fd, data):
    view = memoryview(data)
    while view:
        count = os.write(fd, view)
        if count <= 0:
            raise OSError('short archive write')
        view = view[count:]


def frame(fd, seq, kind, data):
    stamp = time.monotonic_ns()
    prefix = HEADER.pack(seq, stamp, kind, len(data), 0)[:-4]
    crc = zlib.crc32(data, zlib.crc32(prefix))
    write_all(fd, prefix + struct.pack('<I', crc))
    write_all(fd, data)


def record(source, path, channel, maximum, mirror=False):
    if not 1 <= maximum <= LIMIT or not channel or len(channel.encode()) > 128:
        raise ValueError('invalid channel/capacity')
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    seq = total = 0
    last_sync = time.monotonic()
    try:
        write_all(fd, MAGIC)
        metadata = json.dumps({'channel': channel, 'wall_ns': time.time_ns(),
                               'upstream_complete': False, 'max_bytes': maximum},
                              separators=(',', ':')).encode()
        frame(fd, seq, 1, metadata)
        seq += 1
        os.fsync(fd)
        last_space_check = 0.0
        last_space_bytes = 0
        while total < maximum:
            now = time.monotonic()
            if now - last_space_check >= 1 or total - last_space_bytes >= 8 * 1024**2:
                space = os.fstatvfs(fd)
                if space.f_bavail * space.f_frsize < FREE_FLOOR:
                    frame(fd, seq, 5, b'')
                    os.fsync(fd)
                    raise ValueError('disk free floor reached; capture incomplete')
                last_space_check = now
                last_space_bytes = total
            data = source.read(min(CHUNK, maximum - total))
            if not data:
                frame(fd, seq, 3, b'')
                os.fsync(fd)
                return total
            frame(fd, seq, 2, data)
            seq += 1
            total += len(data)
            if mirror:
                write_all(sys.stdout.fileno(), data)
            if time.monotonic() - last_sync >= 1:
                os.fsync(fd)
                last_sync = time.monotonic()
        # Do not read beyond the declared input budget to guess whether EOF
        # is next. Hitting the bound is explicitly incomplete, not success.
        frame(fd, seq, 4, b'')
        os.fsync(fd)
        raise ValueError('archive capacity reached; capture incomplete')
    finally:
        os.close(fd)


def exact(source, length):
    result = bytearray()
    while len(result) < length:
        part = source.read(length - len(result))
        if not part:
            raise ValueError('truncated archive')
        result.extend(part)
    return bytes(result)


def inspect(source):
    if exact(source, len(MAGIC)) != MAGIC:
        raise ValueError('unknown archive format')
    seq = total = 0
    terminal = None
    metadata = None
    previous_time = 0
    while True:
        first = source.read(1)
        if not first:
            break
        header = first + exact(source, HEADER.size - 1)
        number, stamp, kind, length, crc = HEADER.unpack(header)
        if number != seq or stamp < previous_time or length > CHUNK or terminal:
            raise ValueError('invalid sequence/time/record extent')
        data = exact(source, length)
        if zlib.crc32(data, zlib.crc32(header[:-4])) != crc:
            raise ValueError('record checksum mismatch')
        if seq == 0:
            if kind != 1:
                raise ValueError('missing metadata record')
            metadata = json.loads(data)
            if (metadata.get('upstream_complete') is not False or
                    type(metadata.get('max_bytes')) is not int or
                    not 1 <= metadata['max_bytes'] <= LIMIT):
                raise ValueError('invalid metadata')
        elif kind == 2 and length:
            total += length
            if total > metadata['max_bytes']:
                raise ValueError('declared input budget exceeded')
        elif kind in (3, 4, 5) and not length:
            terminal = {3: 'EOF', 4: 'LIMIT', 5: 'DISK_FLOOR'}[kind]
        else:
            raise ValueError('invalid record type/extent')
        seq += 1
        previous_time = stamp
    if metadata is None or terminal is None:
        raise ValueError('missing terminal record')
    return {'channel': metadata['channel'], 'bytes': total,
            'transport_complete': terminal == 'EOF', 'upstream_complete': False,
            'gpu_execution_proof': False, 'terminal': terminal}


def self_test():
    global FREE_FLOOR
    import io
    import tempfile
    from pathlib import Path
    with tempfile.TemporaryDirectory(prefix='ngreen-flight-') as directory:
        path = Path(directory) / 'stream.bin'
        payload = b'EOF\0LIMIT\xff' * 20000
        assert record(io.BytesIO(payload), path, 'owned-test', len(payload)+1) == len(payload)
        saved = path.read_bytes()
        result = inspect(io.BytesIO(saved))
        assert result['transport_complete'] and result['bytes'] == len(payload)
        for bad in (saved[:-1], saved[:-HEADER.size], saved[:20],
                    saved[:60]+bytes([saved[60]^1])+saved[61:],
                    saved+saved[-HEADER.size:]):
            try:
                inspect(io.BytesIO(bad))
            except (ValueError, UnicodeError):
                continue
            raise AssertionError('damaged archive accepted')
        try:
            record(io.BytesIO(payload), path, 'owned-test', len(payload)+1)
        except FileExistsError:
            pass
        else:
            raise AssertionError('existing evidence overwritten')
        limit_path = Path(directory) / 'limit.bin'
        try:
            record(io.BytesIO(payload), limit_path, 'limit', 17)
        except ValueError:
            pass
        else:
            raise AssertionError('capacity boundary reported success')
        with limit_path.open('rb') as source:
            result = inspect(source)
        assert not result['transport_complete'] and result['bytes'] == 17
        floor_path = Path(directory) / 'disk-floor.bin'
        previous_floor = FREE_FLOOR
        try:
            FREE_FLOOR = 1 << 64
            try:
                record(io.BytesIO(payload), floor_path, 'disk-floor', len(payload)+1)
            except ValueError:
                pass
            else:
                raise AssertionError('disk floor ignored')
        finally:
            FREE_FLOOR = previous_floor
        with floor_path.open('rb') as source:
            result = inspect(source)
        assert result['terminal'] == 'DISK_FLOOR' and not result['transport_complete']
    print('HOST_FLIGHT_STREAM_SELFTEST_OK')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    capture = sub.add_parser('record')
    capture.add_argument('--output', required=True)
    capture.add_argument('--channel', required=True)
    capture.add_argument('--max-bytes', type=int, default=LIMIT)
    capture.add_argument('--mirror', action='store_true')
    check = sub.add_parser('inspect')
    check.add_argument('archive')
    sub.add_parser('self-test')
    args = parser.parse_args()
    if args.command == 'record':
        record(sys.stdin.buffer.raw, args.output, args.channel, args.max_bytes, args.mirror)
    elif args.command == 'inspect':
        with open(args.archive, 'rb') as source:
            print(json.dumps(inspect(source), separators=(',', ':')))
    else:
        self_test()
