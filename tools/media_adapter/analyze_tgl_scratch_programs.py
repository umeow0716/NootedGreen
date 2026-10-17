#!/usr/bin/env python3
"""Fixed-function, read-only extraction of exact native scratch source writes.

No process/GPU memory access and no replay. Outputs are STATIC source copies,
not proof that the same bytes were fetched by a GPU.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import struct
import subprocess
import tempfile

START = 0x3162e
SIZE = 0x4d0
DIGEST = '56e4b5c4a05923dcc67668b603722bf5aa8a15759a99b5bad6efbe15bbcedc18'
UUID = bytes.fromhex('ba3aa1c0fe6b33b39d8573f848394e3d')
IGA_COMMIT = 'c36a504a4e6c68030c3302646aa396b8b59b22cb'
BLEND_SYMBOLS = {'__ZL17DefaultBlendState','__ZL22DisableAlphaBlendState',
                 '__ZL28DisableAlphaD32FS8BlendState','__ZL19AlphaOnlyBlendState',
                 '__ZL25AlphaOnlyD32FS8BlendState'}


def read_at(stream, offset, count):
    if offset < 0 or not 0 < count <= 65536:
        raise ValueError('fixed read exceeds bounds')
    stream.seek(offset)
    result = stream.read(count)
    if len(result) != count:
        raise ValueError('short fixed read')
    return result


def copies(body):
    if len(body) != SIZE or hashlib.sha256(body).hexdigest() != DIGEST:
        raise ValueError('initializer identity mismatch')
    first = re.match(rb'\x48\x8d\x35(.{4})\xba(.{4})\x48\x89\xc7\xe8\0\0\0\0', body[0x24:], re.S)
    if first is None:
        raise ValueError('initial native memcpy shape mismatch')
    result = [dict(destination=0, source=START+0x24+7+struct.unpack('<i',first[1])[0],
                   bytes=struct.unpack('<I', first[2])[0], call_operand=0x31662)]
    pattern = rb'\x48\x8d\xbb(.{4})\x48\x8d\x35(.{4})\xba(.{4})\xe8\0\0\0\0'
    for match in re.finditer(pattern, body, re.S):
        result.append(dict(destination=struct.unpack('<I',match[1])[0],
                           source=START+match.start()+14+struct.unpack('<i',match[2])[0],
                           bytes=struct.unpack('<I',match[3])[0], call_operand=START+match.start()+20))
    if len(result) != 40:
        raise ValueError('initializer copy coverage changed')
    ranges = []
    for item in result:
        first, size = item['destination'], item['bytes']
        if not 0 < size <= 65536 or first+size > 65536:
            raise ValueError('scratch program extent exceeds fixed allocation bound')
        if any(first < end and begin < first+size for begin,end in ranges):
            raise ValueError('overlapping native program copies')
        ranges.append((first,first+size))
    return result


def metadata(stream, programs):
    header = struct.unpack('<8I', read_at(stream,0,32))
    if header[0] != 0xfeedfacf or header[4] > 128:
        raise ValueError('invalid Mach-O metadata')
    at, sym, dysym, uuid, segments = 32, None, None, None, []
    for _ in range(header[4]):
        kind, length = struct.unpack('<II', read_at(stream,at,8))
        if not 8 <= length <= 4096:
            raise ValueError('invalid load command bound')
        command = read_at(stream,at,length)
        if kind == 2: sym = struct.unpack_from('<4I',command,8)
        if kind == 0xb: dysym = struct.unpack_from('<18I',command,8)
        if kind == 0x1b: uuid = command[8:24]
        if kind == 0x19:
            segments.append(struct.unpack_from('<16s4Q4I',command,8))
        at += length
    if uuid != UUID or sym is None or dysym is None:
        raise ValueError('wrong native image identity')
    symbols, count, strings, string_bytes = sym
    relocation_offset, relocation_count = dysym[14:16]
    if count > 32768 or relocation_count > 32768:
        raise ValueError('metadata table exceeds bound')
    by_operand = {p['call_operand']: p for p in programs}
    found = set()
    def symbol_name(index):
        if index >= count: raise ValueError('bad symbol index')
        name_index = struct.unpack_from('<I',read_at(stream,symbols+16*index,16))[0]
        if name_index >= string_bytes: raise ValueError('bad string index')
        raw = read_at(stream,strings+name_index,min(256,string_bytes-name_index))
        return raw[:raw.index(b'\0')].decode('ascii')
    for i in range(relocation_count):
        address, info = struct.unpack('<iI', read_at(stream,relocation_offset+8*i,8))
        if address not in by_operand: continue
        if info >> 24 != 0x2d or symbol_name(info & 0xffffff) != '_memcpy' or address in found:
            raise ValueError('source copy is not the unique native memcpy relocation')
        found.add(address)
    if found != set(by_operand): raise ValueError('missing native memcpy relocation')
    targets = {p['source']: p for p in programs}
    for i in range(count):
        name, _, _, _, value = struct.unpack('<IBBHQ',read_at(stream,symbols+16*i,16))
        if value in targets:
            targets[value]['symbol'] = symbol_name(i)
    for item in programs:
        matches = [s for s in segments if s[1] <= item['source'] and
                   item['source']-s[1]+item['bytes'] <= s[4]]
        if len(matches) != 1 or matches[0][0].rstrip(b'\0') != b'__TEXT' or matches[0][6] & 2:
            raise ValueError('program source not in unique readonly native text segment')
        segment = matches[0]
        item['file_offset'] = segment[3]+item['source']-segment[1]
        name = item.get('symbol','')
        if name.startswith('__ZL') and 'IGBlit3DKernel' in name:
            item['kind'] = 'EU_KERNEL'
        elif name in BLEND_SYMBOLS:
            item['kind'] = 'BLEND_STATE'
        else:
            raise ValueError('program source symbol identity missing')
    return segments


def tail_writes(body, stream, segments):
    if body[0x3e0:0x3e2] != b'\x31\xc0':
        raise ValueError('zero-state initialization register mismatch')
    writes = []
    zeros = body[0x3e2:0x467]
    pattern = rb'\x48\x89\x83(.{4})'
    matches = list(re.finditer(pattern,zeros,re.S))
    if len(matches)!=19 or b''.join(m[0] for m in matches)!=zeros:
        raise ValueError('zero-state initialization coverage mismatch')
    for match in matches:
        writes.append(dict(destination=struct.unpack('<I',match[1])[0],bytes=8,
                           kind='ZERO_STATE',hex='00'*8))
    sampler = body[0x467:0x4bb]
    matches = list(re.finditer(rb'\x48\x8b\x05(.{4})\x48\x89\x83(.{4})',sampler,re.S))
    if len(matches)!=6 or b''.join(m[0] for m in matches)!=sampler:
        raise ValueError('sampler initialization coverage mismatch')
    for match in matches:
        source = START+0x467+match.start()+7+struct.unpack('<i',match[1])[0]
        candidates = [s for s in segments if s[1]<=source and source-s[1]+8<=s[4] and not s[6]&2]
        if len(candidates)!=1:
            raise ValueError('sampler constant source not in unique readonly segment')
        segment=candidates[0]
        raw=read_at(stream,segment[3]+source-segment[1],8)
        writes.append(dict(destination=struct.unpack('<I',match[2])[0],source=source,bytes=8,
                           kind='SAMPLER_STATE',hex=raw.hex()))
    return writes


def child_bound():
    resource.setrlimit(resource.RLIMIT_FSIZE,(524288,524288))


def code_coverage(assembly, diagnostics, raw):
    instructions = []
    with assembly.open() as source:
        for line in source:
            if len(line) > 4096 or len(instructions) > 8192:
                raise ValueError('disassembly exceeds line/instruction bound')
            match = re.match(r'/\* \[([0-9a-fA-F]+)\]\s*\*/(.*)',line)
            if match:
                instructions.append((int(match[1],16),match[2]))
    eots = [(pc,text) for pc,text in instructions if re.search(r'\bEOT\b',text)]
    if not eots:
        return dict(eot_present=False,active_encoding_admitted=False)
    pc,text = eots[-1]
    end = pc+(8 if 'Compacted' in text else 16)
    expected, contiguous = 0, True
    for at,text in instructions:
        if at>pc: break
        length = 8 if 'Compacted' in text else 16
        contiguous = contiguous and at==expected and at+length<=len(raw)
        if at+4<=len(raw):
            contiguous = contiguous and bool(struct.unpack_from('<I',raw,at)[0] & (1<<29)) == (length==8)
        expected = at+length
    invalid = sum('illegal' in text for at,text in instructions if at<=pc)
    post = sum('illegal' not in text for at,text in instructions if at>pc)
    zero_tail = end<=len(raw) and not any(raw[end:])
    known_warnings = True
    with diagnostics.open() as source:
        for line in source:
            if len(line)>4096: raise ValueError('diagnostic line exceeds bound')
            if not re.fullmatch(r'byte offset 0x[0-9a-fA-F]+: warning: unexpected padding at end of kernel\n?',line):
                known_warnings = False
    return dict(eot_present=True,last_eot_end=end,illegal_before_last_eot=invalid,
                active_decode_contiguous=contiguous,
                post_eot_nonpadding_instructions=post,zero_tail=zero_tail,
                only_known_padding_warnings=known_warnings,
                active_encoding_admitted=contiguous and not invalid and not post and zero_tail and known_warnings)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--iga',type=Path)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--self-test',action='store_true')
    args = parser.parse_args()
    with args.image.open('rb') as stream:
        body = read_at(stream,START,SIZE)
        programs = copies(body)
        segments = metadata(stream,programs)
        writes = tail_writes(body,stream,segments)
        # A single identity-byte mutation must reject, not fall back to search.
        for index in (0, SIZE-1, 0x24, 0x43):
            bad = bytearray(body); bad[index] ^= 1
            try: copies(bad)
            except ValueError: pass
            else: raise AssertionError('initializer mutation admitted')
        if args.self_test:
            if len(writes)!=25 or sum(p['kind']=='EU_KERNEL' for p in programs)!=35:
                raise AssertionError('native scratch source coverage changed')
            with tempfile.TemporaryDirectory(prefix='ngreen-scratch-coverage-') as directory:
                assembly,log = Path(directory)/'owned.asm',Path(directory)/'owned.log'
                log.write_text('')
                assembly.write_text('/* [0000] */ send.rc {EOT}\n/* [0010] */ illegal\n')
                assert code_coverage(assembly,log,b'\x01'*16+b'\0'*16)['active_encoding_admitted']
                assert not code_coverage(assembly,log,b'\x01'*32)['active_encoding_admitted']
                log.write_text('unreviewed warning\n')
                assert not code_coverage(assembly,log,b'\x01'*16+b'\0'*16)['active_encoding_admitted']
                log.write_text('')
                assembly.write_text('/* [0000] */ illegal\n/* [0010] */ send.rc {EOT}\n')
                assert not code_coverage(assembly,log,b'\x01'*32)['active_encoding_admitted']
                assembly.write_text('/* [0000] */ mov r0 r1\n')
                assert not code_coverage(assembly,log,b'\x01'*16)['active_encoding_admitted']
                assembly.write_text('/* [0010] */ send.rc {EOT}\n')
                assert not code_coverage(assembly,log,b'\x01'*32)['active_encoding_admitted']
                assembly.write_text('/* [0000] */ send.rc {EOT,Compacted}\n')
                assert not code_coverage(assembly,log,b'\x01'*8+b'\0'*8)['active_encoding_admitted']
            print('PASS bounded native scratch 40 copies/25 state writes/UUID/relocations; EOT/padding refusal fixtures; no GPU')
            return
        if args.iga is None or args.output is None:
            parser.error('--iga and --output are required outside --self-test')
        args.output.mkdir(mode=0o700)
        initialized, known = bytearray(65536), bytearray(65536)
        def install(destination, raw):
            if not 0<=destination<=65536-len(raw) or any(known[destination:destination+len(raw)]):
                raise ValueError('overlapping/outsize initialized state')
            initialized[destination:destination+len(raw)] = raw
            known[destination:destination+len(raw)] = b'\x01'*len(raw)
        for write in writes:
            install(write['destination'],bytes.fromhex(write['hex']))
        version = subprocess.run([str(args.iga),'--help'],check=True,capture_output=True,timeout=10)
        if IGA_COMMIT[:7].encode() not in version.stdout+version.stderr:
            raise ValueError('unexpected IGA build identity')
        for index, item in enumerate(programs):
            binary = args.output/f'program-{index:02}.bin'
            raw = read_at(stream,item['file_offset'],item['bytes'])
            install(item['destination'],raw)
            with binary.open('xb') as output: output.write(raw)
            item['sha256'] = hashlib.sha256(raw).hexdigest()
            item['decodes'] = {}
            if item['kind'] != 'EU_KERNEL': continue
            for platform in ('11','12p1'):
                asm = args.output/f'program-{index:02}-{platform}.asm'
                log = args.output/f'program-{index:02}-{platform}.diagnostics'
                with log.open('xb') as errors:
                    process = subprocess.run([str(args.iga),'-d','-p='+platform,'-Xprint-pc','--output-on-fail',
                                              str(binary),'-o',str(asm)],stdout=errors,stderr=errors,
                                             timeout=15,preexec_fn=child_bound)
                item['decodes'][platform] = dict(exit=process.returncode,
                    assembly_bytes=asm.stat().st_size if asm.exists() else 0,
                    diagnostic_bytes=log.stat().st_size)
                if platform == '12p1' and asm.exists():
                    item['decodes'][platform]['coverage'] = code_coverage(asm,log,raw)
                    if process.returncode:
                        item['decodes'][platform]['coverage']['active_encoding_admitted'] = False
    for filename, raw in (('source-expected.bin',initialized),('source-known-mask.bin',known)):
        with (args.output/filename).open('xb') as output: output.write(raw)
    report = dict(image_uuid=UUID.hex(),initializer_sha256=DIGEST,iga_source_commit=IGA_COMMIT,
                  programs=programs,tail_writes=writes,known_bytes=sum(known),
                  unknown_bytes=len(known)-sum(known),unknown_zero_bytes_are_not_source_values=True,
                  expected_image_requires_known_mask=True,static_source_only=True,gpu_fetch_proven=False,
                  hang_cause_proven=False,source_kernel_bytes_modified=False)
    with (args.output/'report.json').open('x') as output: json.dump(report,output,separators=(',',':'))
    print(json.dumps(dict(programs=len(programs),total_program_bytes=sum(p['bytes'] for p in programs),
        eu_kernels=sum(p['kind']=='EU_KERNEL' for p in programs),
        known_initializer_bytes=sum(known),unknown_initializer_bytes=len(known)-sum(known),
        gen11_decode_success=sum(p['decodes'].get('11',{}).get('exit')==0 for p in programs),
        gen12p1_decode_success=sum(p['decodes'].get('12p1',{}).get('exit')==0 for p in programs),
        gen12p1_active_encoding_admitted=sum(p['decodes'].get('12p1',{}).get('coverage',{}).get('active_encoding_admitted',False) for p in programs),
        gpu_fetch_proven=False,hang_cause_proven=False)))


if __name__ == '__main__': main()
