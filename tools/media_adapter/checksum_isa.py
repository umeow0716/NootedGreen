"""Rebuild the owned checksum ISA from the pinned native Gen11 instruction stream.

Only fixed Mach-O metadata and the 1480-byte program are read. IGA is an
explicit local tool, never downloaded or executed by the portable self-test.
CPU instruction-footprint tests do not prove GPU execution or PF safety.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
NATIVE_SHA = '3f201b1caba0954e4455e88aeace016859675d4a0db40501ffff632b3781117d'
ASM = ROOT / 'tools/media_adapter/frame_checksum_gen12.asm'
HEADER = ROOT / 'tools/media_adapter/frame_checksum_gen12.hpp'
ARRAY = re.compile(r'(alignas\(64\) inline constexpr std::array<uint32_t,)\d+(> isa\{\{\n).*?\}\};', re.S)
PREAMBLE = (
    '// Gen12 Xe-LP frame checksum; owned data, no Apple text writes.\n'
    f'// Native source SHA256 {NATIVE_SHA}\n'
    '// Decode the source as Gen11: Gen9 silently changes HF/F into UQ/Q.\n'
    '// Preserve native ALU types, subregisters, regions and control flow.\n'
    '// Only SEND syntax and generated dependency barriers are adapted.\n'
    '// Assemble Intel IGA c36a504: -a -p=12p1 -Xauto-deps.\n'
)


def native_program(image):
    with image.open('rb') as stream:
        header = stream.read(32)
        magic, cpu, _, _, count, size, _, _ = struct.unpack('<8I', header)
        if magic != 0xfeedfacf or cpu != 0x01000007 or not 0 < count < 256 or not 0 < size < 65536:
            raise ValueError('bounded x86_64 Mach-O header required')
        commands = stream.read(size)
        at = 0
        matches = []
        for _ in range(count):
            command, length = struct.unpack_from('<II', commands, at)
            if length < 8 or at + length > len(commands):
                raise ValueError('invalid load command')
            if command == 0x19:
                va, _, offset, file_size = struct.unpack_from('<4Q', commands, at + 24)
                if va <= 0x76cff0 and 0x76cff0 + 1480 <= va + file_size:
                    matches.append(offset + 0x76cff0 - va)
            at += length
        if len(matches) != 1:
            raise ValueError('unique native program range required')
        stream.seek(matches[0])
        data = stream.read(1480)
    if len(data) != 1480 or hashlib.sha256(data).hexdigest() != NATIVE_SHA:
        raise ValueError('native program identity mismatch')
    return data


def convert_sends(text):
    output = []
    changes = 0
    for line in text.splitlines():
        if re.search(r'\bsends?\s', line):
            prefix = line.split('send', 1)[0]
            read = re.search(r'send\s+\(8\|M0\)\s+(r\d+)\s+(r\d+):f\s+0xC\s+a0\.0', line)
            atomic = re.search(r'sends\s+\(8\|M0\)\s+null:ud\s+(r\d+)\s+(r\d+)\s+0x4C\s+a0\.0', line)
            if read:
                line = prefix + f'send.dc1 (8|M0) {read[1]} {read[2]} null:ud 0x0 a0.0'
            elif atomic:
                line = prefix + f'send.dc1 (8|M0) null {atomic[1]} {atomic[2]}:ud 0x40 a0.0'
            elif '{EOT}' in line and 'r127:f' in line and '0x27' in line and '0x02000010' in line:
                line = prefix + 'send.ts (8|M0) null r127 null:ud 0x0 0x02000010 {EOT}'
            else:
                raise ValueError('unknown native SEND shape')
            changes += 1
        output.append(line.rstrip())
    if changes != 7:
        raise ValueError('exact native SEND inventory required')
    return '\n'.join(output) + '\n'


def normalized(text):
    labels = {}
    for line in text.splitlines():
        if re.fullmatch(r'L\d+:', line.strip()):
            labels[line.strip()[:-1]] = f'BLOCK{len(labels)}'
    output = []
    for line in text.splitlines():
        line = line.split('//', 1)[0].strip()
        if not line:
            continue
        if re.fullmatch(r'L\d+:', line):
            output.append(labels[line[:-1]] + ':')
            continue
        if re.search(r'\bsync\.all(?:rd|wr)\b', line):
            continue
        eot = re.search(r'\bEOT\b', line) is not None
        for group in re.findall(r'\{([^}]*)\}', line):
            for option in group.split(','):
                option = option.strip()
                if option not in ('EOT', 'Compacted') and not re.fullmatch(r'@[1-7]|\$[0-9]+(?:\.(?:src|dst))?', option):
                    raise ValueError('unreviewed instruction option: ' + option)
        line = re.sub(r'\{[^}]*\}', '', line)
        line = re.sub(r'\bL\d+\b', lambda match: labels[match[0]], line)
        if 'send.' in line:
            line = re.sub(r'\b(null|r\d+):ud\b', r'\1', line)
        output.append(re.sub(r'\s+', '', line) + (' EOT' if eot else ''))
    return output


def header_binary(text):
    match = ARRAY.search(text)
    if not match:
        raise ValueError('ISA array missing')
    words = re.findall(r'0x([0-9a-fA-F]{8})U', match[0])
    return b''.join(struct.pack('<I', int(word, 16)) for word in words)


def apply_immediate_move(registers, data, instruction):
    """Restricted Gen12 full MOV decoder; unknown/converting forms are denied.

    Intel GED Gen12 fields: opcode[6:0], exec-size[18:16], dst register
    [63:56], byte subregister[55:51], hstride[49:48], dst/src types in
    [39:36]/[43:40]. Only the five audited immediate MOVs are consumed.
    """
    w0, w1, w2, w3 = struct.unpack_from('<4I', data, instruction * 16)
    expected_control = {1: 0x80000061, 4: 0x80000061, 13: 0x61, 14: 0x61, 15: 0x10061}
    if w0 & ~0xff00 != expected_control[instruction] or not w1 & 0x4000:
        raise ValueError('unexpected MOV control or non-immediate source')
    kind = (w1 >> 4) & 15
    sizes = {9: 2, 10: 4, 3: 8, 7: 8}  # HF, F, UQ, Q
    if kind not in sizes or (w1 >> 8) & 15 != kind or (w1 >> 16) & 3 != 1:
        raise ValueError('unsupported MOV type, conversion or stride')
    size = sizes[kind]
    count = 1 << ((w0 >> 16) & 7)
    start = ((w1 >> 24) & 255) * 32 + ((w1 >> 19) & 31)
    immediate = (w2 << 32 | w3) if size == 8 else w3 & ((1 << (size * 8)) - 1)
    if start + count * size > len(registers):
        raise ValueError('register extent exceeded')
    for lane in range(count):
        registers[start + lane * size:start + (lane + 1) * size] = immediate.to_bytes(size, 'little')


def footprint_ok(data):
    # Native Gen11 byte footprints, independently checked against the x86
    # ad00/2c7a0 64-byte CURBE contract: DW2 is width, DW3 is height.
    # Gen9 misdecoding overwrites R1.UW8 and the R2.DW2 loop limit.
    for plane in (0, 1, 0xfff):
        registers = bytearray([0xa5] * (128 * 32))
        struct.pack_into('<H', registers, 32 + 16, plane)
        struct.pack_into('<I', registers, 64 + 8, 1920 >> 3)
        expected = bytearray(registers)
        writes = {
            1: (32 + 22, (4).to_bytes(2, 'little')),
            4: (5 * 32 + 8, (1).to_bytes(2, 'little')),
            13: (64 + 12, bytes(4)),
            14: (64 + 16, bytes(4)),
            15: (64 + 20, bytes(8)),
        }
        try:
            for instruction in (1, 4, 13, 14, 15):
                apply_immediate_move(registers, data, instruction)
                at, value = writes[instruction]
                expected[at:at + len(value)] = value
                if registers != expected:
                    return False
        except (ValueError, struct.error):
            return False
        limit = struct.unpack_from('<I', registers, 64 + 8)[0]
        if limit != 240 or not any(step == limit for step in range(1, 241)):
            return False
    return True


def self_test():
    text = HEADER.read_text()
    data = header_binary(text)
    if not footprint_ok(data):
        raise ValueError('published ISA corrupts native register footprints')
    digest = re.search(r'sha256\[\]="([0-9a-f]{64})"', text)[1]
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError('published ISA digest mismatch')
    legacy = {
        1: (0x80000061, 0x01854330, 0x00040004, 0),
        4: (0x80000061, 0x05454330, 0x00010001, 0),
        13: (0x61, 0x02454770, 0, 0),
        14: (0x61, 0x02854770, 0, 0),
        15: (0x10061, 0x02854770, 0, 0),
    }
    for instruction, words in legacy.items():
        changed = bytearray(data)
        struct.pack_into('<4I', changed, instruction * 16, *words)
        if footprint_ok(changed):
            raise ValueError('historical Gen9 misdecoding accepted')
    wrong_limit = bytearray(128 * 32)
    struct.pack_into('<I', wrong_limit, 64 + 8, 240)
    legacy_data = bytearray(data)
    struct.pack_into('<4I', legacy_data, 13 * 16, *legacy[13])
    apply_immediate_move(wrong_limit, legacy_data, 13)
    if struct.unpack_from('<I', wrong_limit, 64 + 8)[0] != 0:
        raise ValueError('historical loop-limit corruption fixture changed')
    for instruction in legacy:
        for word, bit in ((0, 24), (1, 24), (1, 19), (1, 8)):
            changed = bytearray(data)
            at = instruction * 16 + word * 4
            value = struct.unpack_from('<I', changed, at)[0]
            struct.pack_into('<I', changed, at, value ^ (1 << bit))
            if footprint_ok(changed):
                raise ValueError('wrong predicate/register/subregister/type accepted')
    correct = 'L0:\n mov (1|M0) r1.11<1>:hf 0x4:hf\n'
    wrong = 'L0:\n mov (1|M0) r1.2<1>:uq 0x4000400000000:uq\n'
    if normalized(correct) == normalized(wrong):
        raise ValueError('semantic comparison discarded type/subregister')
    if normalized('send.ts (8|M0) null r127 null:ud 0x0 0x02000010 {EOT}') != normalized(
            'send.ts (8|M0) null r127 null 0x0 0x02000010 {EOT,$0.src}'):
        raise ValueError('dependency annotations changed EOT comparison')
    if normalized('mov (1|M0) r1.0<1>:ud 0:ud') == normalized('mov (1|M0) r1.0<1>:f 0:f'):
        raise ValueError('semantic comparison discarded ALU type')
    try:
        normalized('mov (1|M0) r1.0<1>:ud 0:ud {Atomic}')
    except ValueError:
        pass
    else:
        raise ValueError('unknown semantic instruction option ignored')
    print('CHECKSUM_ISA_FOOTPRINT_OK native-Gen11 old-loop-limit=0 fixed=240 '
          '5-legacy/20-independent-negatives no-GPU-proof')


def rebuild(iga, image, write):
    with tempfile.TemporaryDirectory(prefix='ngreen-checksum-isa-') as directory:
        directory = Path(directory)
        native = directory / 'native.bin'
        native.write_bytes(native_program(image))
        source = subprocess.check_output([str(iga), '-d', '-p=11', str(native)], text=True)
        if len(source) > 65536 or 'sync.' in source:
            raise ValueError('disassembly exceeds bound')
        native_assembly = directory / 'native.asm'
        native_assembly.write_text(source)
        roundtrip = directory / 'native-roundtrip.bin'
        subprocess.run([str(iga), '-a', '-p=11', '-o', str(roundtrip), str(native_assembly)], check=True)
        if roundtrip.stat().st_size != 1480:
            raise ValueError('native Gen11 roundtrip size mismatch')
        with roundtrip.open('rb') as stream:
            if stream.read(1481) != native_program(image):
                raise ValueError('native Gen11 roundtrip bytes changed')
        assembly = PREAMBLE + convert_sends(source)
        asm_path = directory / 'owned.asm'
        asm_path.write_text(assembly)
        binary = directory / 'owned.bin'
        subprocess.run([str(iga), '-a', '-p=12p1', '-Xauto-deps', '-o', str(binary), str(asm_path)], check=True)
        if binary.stat().st_size != 1576:
            raise ValueError('new ISA size requires separate ABI review')
        with binary.open('rb') as stream:
            data = stream.read(1577)
        if len(data) != 1576 or not footprint_ok(data):
            raise ValueError('new ISA size/footprint requires separate ABI review')
        target = subprocess.check_output([str(iga), '-d', '-p=12p1', str(binary)], text=True)
        if normalized(convert_sends(source)) != normalized(target):
            raise ValueError('native Gen11 ALU/region/branch/SEND semantics changed')
        words = struct.unpack('<394I', data)
        body = ''.join('    ' + ','.join(f'0x{word:08x}U' for word in words[at:at+8]) + ',\n' for at in range(0, len(words), 8))
        header = HEADER.read_text()
        header = re.sub(r'sha256\[\]="[0-9a-f]{64}"', f'sha256[]="{hashlib.sha256(data).hexdigest()}"', header)
        header = ARRAY.sub(lambda match: match[1] + '394' + match[2] + body + '}};', header)
        if write:
            ASM.write_text(assembly)
            HEADER.write_text(header)
        elif ASM.read_text() != assembly or HEADER.read_text() != header:
            raise ValueError('committed shader assets differ from reproducible Gen11 conversion')
        print('CHECKSUM_ISA_GEN11_TO_GEN12_OK bytes=1576 sha256=' + hashlib.sha256(data).hexdigest() + ' hardware=UNPROVEN')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--self-test', action='store_true')
    parser.add_argument('--iga', type=Path)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--write', action='store_true')
    arguments = parser.parse_args()
    if arguments.self_test:
        self_test()
    else:
        if not arguments.iga or not arguments.image:
            parser.error('--iga and --image required for the explicit local rebuild/check')
        rebuild(arguments.iga.resolve(), arguments.image.resolve(), arguments.write)
