#!/usr/bin/env python3
"""Stream private native runtime evidence; never load a process dump into RAM."""
import argparse
import json
import os
import struct
import tempfile

HEADER = struct.Struct("<4I")
PACKET = struct.Struct("<IIQQ")
FOOTER = struct.Struct("<5Q")


def inspect(path, address=None, size=0):
    if address is not None and (address < 0 or not 0 < size <= 65536 or address + size > 2**64):
        raise ValueError("extraction must be one explicit range of 1..65536 bytes")
    extracted = bytearray(size)
    coverage = bytearray(size)
    counts = dict(interrupted=0, threads=0, thread_failures=0, regions=0, read_failures=0, memory_bytes=0, critical_bytes=0, critical_failures=0)
    terminal = None
    critical_terminal = False
    critical_stack_truncated = True
    timestamps = {}
    with open(path, "rb") as source:
        prefix = source.read(HEADER.size)
        if len(prefix) != HEADER.size:
            raise ValueError("missing format header")
        magic, version, packet_size, state_size = HEADER.unpack(prefix)
        if (magic, version, packet_size) != (0x4E47534E, 1, PACKET.size) or not 0 < state_size <= 1024:
            raise ValueError("unknown snapshot format")
        extent = os.fstat(source.fileno()).st_size
        while source.tell() < extent:
            raw = source.read(PACKET.size)
            if len(raw) != PACKET.size:
                raise ValueError("truncated packet header")
            kind, length, start, detail = PACKET.unpack(raw)
            if kind not in range(1, 11) or length > 65536 or source.tell() + length > extent:
                raise ValueError("invalid/truncated packet")
            if kind == 1:
                if length != state_size:
                    raise ValueError("invalid interrupted state")
                counts["interrupted"] += 1
            elif kind == 2:
                if length not in (0, state_size):
                    raise ValueError("invalid thread state")
                counts["threads"] += 1
                counts["thread_failures"] += int(detail != 0)
            elif kind == 3:
                if not length or not detail or start + detail > 2**64:
                    raise ValueError("invalid region descriptor")
                counts["regions"] += 1
            elif kind in (4, 8):
                if not length or start + length > 2**64:
                    raise ValueError("invalid memory span")
                counts["memory_bytes" if kind == 4 else "critical_bytes"] += length
                if address is not None:
                    left, right = max(start, address), min(start + length, address + size)
                    if left < right:
                        source.seek(left - start, os.SEEK_CUR)
                        value = source.read(right - left)
                        offset = left - address
                        for index, byte in enumerate(value, offset):
                            priority = 2 if kind == 8 else 1
                            if coverage[index] == priority:
                                raise ValueError("overlapping same-phase memory records")
                            if coverage[index] < priority:
                                extracted[index] = byte
                                coverage[index] = priority
                        source.seek(start + length - right, os.SEEK_CUR)
                        continue
            elif kind == 5:
                if length != 8:
                    raise ValueError("invalid read-failure record")
                counts["read_failures"] += 1
            elif kind == 6:
                if length != FOOTER.size or terminal is not None:
                    raise ValueError("invalid terminal record")
                reason, copied, failures, regions, seconds = FOOTER.unpack(source.read(length))
                if reason not in (0, 1, 2, 3):
                    raise ValueError("unknown terminal reason")
                if copied != counts["memory_bytes"] or failures != counts["read_failures"] or regions != counts["regions"]:
                    raise ValueError("terminal counts disagree with payload")
                if source.tell() != extent:
                    raise ValueError("trailing data after terminal record")
                terminal = dict(reason=reason, seconds=seconds)
                break
            elif kind == 7:
                counts["thread_failures"] += 1
            elif kind == 9:
                if length == 24 and start == 0 and detail == 0:
                    copied, failures, truncated = struct.unpack("<3Q", source.read(length))
                    if critical_terminal or copied != counts["critical_bytes"] or failures != counts["critical_failures"]:
                        raise ValueError("critical phase counts disagree")
                    if truncated not in (0, 1):
                        raise ValueError("invalid critical stack flag")
                    critical_stack_truncated = bool(truncated)
                    critical_terminal = True
                    continue
                if length or not detail:
                    raise ValueError("invalid critical failure")
                counts["critical_failures"] += 1
            elif kind == 10:
                if length != 32 or start or detail not in (0, 1) or detail in timestamps:
                    raise ValueError("invalid phase timestamp")
                wall, mono, pid, phase = struct.unpack("<4Q", source.read(length))
                if not wall or not mono or not pid or phase != detail:
                    raise ValueError("invalid timestamp values")
                timestamps[phase] = mono
                continue
            source.seek(length, os.SEEK_CUR)
    complete = set(timestamps) == {0, 1} and critical_terminal and not critical_stack_truncated and not counts["critical_failures"] and terminal is not None and terminal["reason"] == 0 and not counts["read_failures"] and not counts["thread_failures"] and counts["threads"] > 0 and counts["interrupted"] == 1
    if address is not None and not all(coverage):
        raise ValueError("requested range was not fully captured; no fabricated zero-fill")
    pause = timestamps[1]-timestamps[0] if set(timestamps) == {0, 1} else None
    if pause is not None and pause < 0:
        raise ValueError("phase timestamps reversed")
    return dict(**counts, terminal=terminal, critical_terminal=critical_terminal, critical_stack_truncated=critical_stack_truncated, critical_pause_ns=pause, complete=complete, other_threads_atomic=False), extracted


def self_test():
    def packet(kind, data=b"", start=0, detail=0):
        return PACKET.pack(kind, len(data), start, detail) + data
    prefix = HEADER.pack(0x4E47534E, 1, PACKET.size, 168)
    events = (packet(1, bytes(168)) + packet(10, struct.pack("<4Q", 100, 100, 1, 0)) +
              packet(8, b"abcdefgh", 0x1000) + packet(9, struct.pack("<3Q", 8, 0, 0)) +
              packet(10, struct.pack("<4Q", 200, 200, 1, 1), detail=1) +
              packet(2, bytes(168)) + packet(3, bytes(80), 0x1000, 8))
    memory = packet(4, b"abcdefgh", 0x1000)
    footer = packet(6, FOOTER.pack(0, 8, 0, 1, 1))
    valid = prefix + events + memory + footer
    with tempfile.TemporaryDirectory(prefix="ngreen-snapshot-test-") as directory:
        path = os.path.join(directory, "synthetic.bin")
        def put(data):
            with open(path, "wb") as target:
                target.write(data)
        put(valid)
        report, data = inspect(path, 0x1002, 4)
        assert report["complete"] and data == b"cdef" and not report["other_threads_atomic"]
        invalid = [b"", valid[:-1], valid + b"x", prefix + packet(4, bytes(65537)),
                   prefix + events + memory + packet(6, FOOTER.pack(0, 9, 0, 1, 1))]
        for value in invalid:
            put(value)
            try:
                inspect(path)
            except ValueError:
                pass
            else:
                raise AssertionError("malformed snapshot accepted")
        for reason in (1, 2, 3):
            put(prefix + events + memory + packet(6, FOOTER.pack(reason, 8, 0, 1, 1)))
            assert not inspect(path)[0]["complete"]
        put(prefix + events + memory)
        assert not inspect(path)[0]["complete"]
        put(prefix + events + packet(7, detail=5) + memory + footer)
        assert not inspect(path)[0]["complete"]
        put(valid)
        for address, size in ((0x0FFF, 8), (0x1000, 65537), (-1, 1), (0x1000, 0)):
            try:
                inspect(path, address, size)
            except ValueError:
                pass
            else:
                raise AssertionError("invalid/uncovered extraction accepted")
    print("PASS runtime snapshot decoder: streaming, bounded extraction, malformed/truncated/limited/denied rejection")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", nargs="?")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--address", type=lambda value: int(value, 0))
    parser.add_argument("--size", type=lambda value: int(value, 0), default=0)
    parser.add_argument("--output")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.snapshot:
        parser.error("snapshot is required")
    if (args.address is None) != (args.output is None) or (args.address is None and args.size):
        parser.error("extraction requires --address, --size and a new --output")
    report, data = inspect(args.snapshot, args.address, args.size)
    if args.output:
        fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as target:
            target.write(data)
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
