"""Rebuild the owned checksum ISA from the pinned native Gen11 instruction stream.

Only fixed Mach-O metadata and the 1480-byte program are read. IGA is an
explicit local tool, never downloaded or executed by the portable self-test.
CPU instruction-footprint tests do not prove GPU execution or PF safety.
"""
import argparse
import hashlib
import json
import sqlite3
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
    heap_decoder_self_test()
    print('CHECKSUM_ISA_FOOTPRINT_OK native-Gen11 old-loop-limit=0 fixed=240 '
          '5-legacy/20-independent-negatives no-GPU-proof')


def decode_heap_lines(lines):
    """Strict owned CPU-source pairs; never GPU/completion attestation."""
    totals={1:4096,2:4096,3:4096,4:312,5:144};groups={}
    identity=re.compile(r'VTEncoderXPCService\[([0-9]+):([0-9a-fA-F]+)\].*?(NGRN_CHECKSUM_HEAP.*)')
    for ordinal,line in enumerate(lines):
        if ordinal>=65536:raise ValueError("heap log line-count bound")
        if len(line)>4096:raise ValueError('heap log line bound')
        if 'NGRN_CHECKSUM_HEAP' not in line:continue
        match=identity.search(line)
        if not match:raise ValueError('missing native process/thread identity')
        pid,thread,text=match.groups();text=text.strip()
        begin=re.fullmatch(r'NGRN_CHECKSUM_HEAPS_BEGIN seq=([12]) schema=1',text)
        data=re.fullmatch(r'NGRN_CHECKSUM_HEAP_CPU seq=([12]) role=([1-5]) copy=([01]) offset=([0-9]+) bytes=([0-9]+) total=([0-9]+) hex=([0-9a-f]+)',text)
        end=re.fullmatch(r'NGRN_CHECKSUM_HEAPS_END seq=([12]) status=([0-9]+) atomic=0 source_only=1',text)
        if not (begin or data or end):raise ValueError('malformed owned heap record')
        key=(pid,thread,(begin or data or end).group(1))
        if key not in groups:
            if len(groups)>=8:raise ValueError('heap group budget')
            groups[key]={'begin':False,'end':None,'blocks':{}}
        g=groups[key]
        if begin:
            if g['begin'] or g['blocks'] or g['end'] is not None:raise ValueError('duplicate/late heap begin')
            g['begin']=True
        elif end:
            if not g['begin'] or g['end'] is not None:raise ValueError('orphan/duplicate heap end')
            g['end']=int(end.group(2))
        else:
            if not g['begin'] or g['end'] is not None:raise ValueError('heap data outside source interval')
            _,role,copy,offset,count,total,encoded=data.groups()
            role,copy,offset,count,total=map(int,(role,copy,offset,count,total))
            if (total!=totals[role] or offset%128 or offset>=total or
                    count!=min(128,total-offset) or len(encoded)!=count*2):raise ValueError('heap block extent')
            slot=(role,copy,offset)
            if slot in g['blocks']:raise ValueError('duplicate heap block')
            g['blocks'][slot]=bytes.fromhex(encoded)
    if not groups:raise ValueError('no owned heap records')
    result=[]
    for (pid,thread,sequence),g in groups.items():
        if not g['begin'] or g['end']!=0:raise ValueError('heap group incomplete/refused')
        heaps={}
        for role,total in totals.items():
            copies=[]
            for copy in (0,1):
                chunks=[g['blocks'].get((role,copy,offset)) for offset in range(0,total,128)]
                if any(x is None for x in chunks):raise ValueError('missing heap block')
                copies.append(b''.join(chunks))
            if copies[0]!=copies[1]:raise ValueError('unstable heap pair')
            heaps[role]=copies[0]
        result.append((pid,thread,int(sequence),heaps))
    return result


def heap_decoder_self_test():
    prefix='VTEncoderXPCService[111:abc] (OwnedFixture) '
    lines=[prefix+'NGRN_CHECKSUM_HEAPS_BEGIN seq=1 schema=1']
    for role,total in {1:4096,2:4096,3:4096,4:312,5:144}.items():
        data=bytes((i+role)&255 for i in range(total))
        for copy in (0,1):
            for offset in range(0,total,128):
                block=data[offset:offset+128]
                lines.append(prefix+f'NGRN_CHECKSUM_HEAP_CPU seq=1 role={role} copy={copy} offset={offset} bytes={len(block)} total={total} hex={block.hex()}')
    lines.append(prefix+'NGRN_CHECKSUM_HEAPS_END seq=1 status=0 atomic=0 source_only=1')
    groups=decode_heap_lines(lines)
    if len(groups)!=1 or len(groups[0][3])!=5:raise ValueError('heap fixture lost domains')
    cases=[lines[1:],lines[:-1],lines[:2]+lines[1:],lines[:-1]+[lines[-1].replace('status=0','status=5')],
           lines[:2]+lines[3:],lines+[lines[-1]],
           [x.replace('offset=0 bytes=128','offset=1 bytes=128') for x in lines],
           [x.replace('total=4096','total=4095') for x in lines],
           [x.replace('copy=1 offset=0 bytes=128 total=4096 hex=01','copy=1 offset=0 bytes=128 total=4096 hex=ff') for x in lines],
           [x.replace('VTEncoderXPCService[111:abc]','unknown-process') for x in lines],
           [x.replace('seq=1','seq=3') for x in lines],
           [x.replace('atomic=0','atomic=1') for x in lines],
           [x.replace('bytes=128','bytes=129') for x in lines]]
    for case in cases:
        try:decode_heap_lines(case)
        except ValueError:pass
        else:raise ValueError('heap negative accepted')
    page=bytes(range(256))*16
    headers=[[2,4096,4096,4096,15,0,1,0],
             [1,1,0,4096,4096,15,0,0], [1,1,1,4096,4096,15,0,0]]
    rows=[]
    for index,head in enumerate(headers):
        kind=30 if index==0 else 31
        payload=struct.pack('<'+str(len(head))+'I',*head)+(b'' if index==0 else page)
        meta={'kind':kind,'word_count':len(payload)//4,'group':4,'identity':'0000000040040000','thread':2748}
        rows.append((kind,json.dumps(meta),payload))
    variants=[rows,rows[1:],rows[:2],rows+[rows[1]],
              rows[:2]+[(31,rows[2][1],rows[2][2][:-1]+bytes([rows[2][2][-1]^1]))],
              [(30,rows[0][1],struct.pack('<8I',2,4096,4096,4096,7,0,1,0)),*rows[1:]],
              rows[:2]+[(31,rows[2][1].replace('2748','2749'),rows[2][2])],
              [(30,rows[0][1].replace('"word_count": 8','"word_count": 9'),rows[0][2]),*rows[1:]],
              rows[:2]+[(31,rows[2][1].replace('40040000','40041000'),rows[2][2])],
              [(30,'not-json',rows[0][2]),*rows[1:]],rows*3,
              rows[:2]+[(31,rows[2][1],bytes(6148))],
              rows[:2]+[(31,rows[2][1],rows[2][2][:-4])]]
    with tempfile.TemporaryDirectory(prefix='ngreen-checksum-page-wire-') as directory:
        for mode,variant in enumerate(variants):
            path=Path(directory)/f'page{mode}.sqlite3';database=sqlite3.connect(path)
            database.execute('CREATE TABLE records(id INTEGER PRIMARY KEY,kind INTEGER,metadata TEXT,payload BLOB)')
            database.executemany('INSERT INTO records(kind,metadata,payload) VALUES(?,?,?)',variant);database.commit();database.close()
            try:decoded=kernel_ssh_pages(path)
            except ValueError:
                if mode==0:raise
            else:
                if mode or len(decoded)!=1 or decoded[0]['data']!=page:raise ValueError('kernel page negative accepted')
    # Independent native store-set check: table and unused tail cannot mutate.
    before=bytearray(4096);struct.pack_into('<I',before,64,0x25017000);struct.pack_into('<I',before,72,479)
    after=bytearray(before);struct.pack_into('<I',after,96,0x40020000)
    if relocation_difference(before,after)['outside_native_store_set']:raise ValueError('native address store rejected')
    pitch=bytearray(before);struct.pack_into('<I',pitch,76,1<<18)
    if relocation_difference(before,pitch)['outside_native_store_set']!=[19]:raise ValueError('unexpected pitch high bit hidden')
    after[4000]=1
    if relocation_difference(before,after)['outside_native_store_set']!=[1000]:raise ValueError('unexpected tail store hidden')
    print('CHECKSUM_CPU_HEAP_DECODER_OK five-domains/13-log/12-page-negatives/store-set CPU-only')


def kernel_ssh_pages(path):
    from decode_kdebug_recorder import validate_leaf
    if not path.is_file() or path.stat().st_size>64*1024*1024:raise ValueError('kernel CPU database bound')
    connection=sqlite3.connect(path.resolve().as_uri()+'?mode=ro',uri=True)
    try:
        rows=connection.execute('SELECT kind,substr(metadata,1,4097),length(metadata),substr(payload,1,6145),length(payload) FROM records WHERE kind IN (30,31) ORDER BY id LIMIT 7').fetchall()
    except sqlite3.Error as error:
        raise ValueError('invalid kernel CPU database') from error
    finally:connection.close()
    if not rows or len(rows)>6:raise ValueError('kernel SSH record budget/missing data')
    groups={}
    for kind,encoded,meta_bytes,payload,payload_bytes in rows:
        if (not isinstance(encoded,str) or not isinstance(payload,bytes) or
                type(meta_bytes) is not int or type(payload_bytes) is not int or
                meta_bytes>4096 or payload_bytes>6144 or payload_bytes%4):raise ValueError('kernel SSH record extent')
        metadata=json.loads(encoded)
        if not isinstance(metadata,dict):raise ValueError('kernel SSH metadata shape')
        words=list(struct.unpack('<'+str(payload_bytes//4)+'I',payload))
        validate_leaf(kind,words)
        if (metadata.get('kind')!=kind or metadata.get('word_count')!=len(words) or
                type(metadata.get('group')) is not int or metadata['group']<=0 or
                type(metadata.get('thread')) is not int or metadata['thread']<=0 or
                not re.fullmatch('[0-9a-f]{16}',metadata.get('identity',''))):raise ValueError('kernel SSH identity')
        if kind==30:
            if len(words)!=8 or words[0]!=2:raise ValueError('legacy partial SSH is not a full page')
            key=(metadata['group'],metadata['identity'],words[6])
            if key in groups:raise ValueError('duplicate kernel SSH metadata')
            groups[key]={'metadata':metadata,'header':words,'copies':{}}
        else:
            key=(metadata['group'],metadata['identity'],words[1]);g=groups.get(key)
            if not g or words[2] in g['copies']:raise ValueError('orphan/duplicate kernel SSH copy')
            if metadata['thread']!=g['metadata']['thread'] or words[5]!=g['header'][4]:raise ValueError('kernel SSH source changed')
            g['copies'][words[2]]=payload[32:]
    result=[]
    for (group,identity,ticket),g in groups.items():
        if (g['header'][2:5]!=[4096,4096,15] or set(g['copies'])!={0,1} or
                g['copies'][0]!=g['copies'][1]):raise ValueError('kernel SSH incomplete/unstable pair')
        result.append({'group':group,'identity':identity,'ticket':ticket,'thread':g['metadata']['thread'],'data':g['copies'][0]})
    return result


def relocation_difference(source,actual):
    before=struct.unpack('<1024I',source);after=struct.unpack('<1024I',actual)
    allowed={};active=[]
    for state in range(8):
        base=16+state*16
        if before[base+2]&0x3fff:
            active.append(state);allowed[base+8]=allowed[base+9]=0xffffffff
            if before[base]&0xe0000000==0x20000000:allowed[base+3]=0x3ffff
    changed=[i for i in range(1024) if before[i]!=after[i]]
    return {'active_source_states':active,'changed_dwords':changed,
            'outside_native_store_set':[i for i in changed if (before[i]^after[i])&~allowed.get(i,0)],
            'scope':'STATIC_NATIVE_MAIN_STORE_SET_ONLY_NOT_BACKING_PROOF'}


def audit_heap_log(path,output,kernel_records=None):
    output.mkdir(mode=0o700)
    try:
        if not path.is_file() or path.stat().st_size>128*1024*1024:raise ValueError("heap log file bound")
        with path.open() as stream:groups=decode_heap_lines(iter(lambda:stream.readline(4097),''))
        report={'scope':'OWNED_PRE_KMD_CPU_ONLY','source_authenticity':'NOT_VERIFIED_BY_THIS_AUDITOR','gpu_execution_proof':False,'atomic':False,'groups':[]}
        for pid,thread,seq,heaps in groups:
            item={'pid':int(pid),'thread':thread,'sequence':seq,'heaps':{},
                  'owned_isa_match':heaps[2][1536:3112]==header_binary(HEADER.read_text()),
                  'original_isa_match':hashlib.sha256(heaps[2][:1480]).hexdigest()==NATIVE_SHA,
                  'idd0':list(struct.unpack_from('<8I',heaps[1],0)),
                  'idd1':list(struct.unpack_from('<8I',heaps[1],64)),
                  'curbe':list(struct.unpack_from('<16I',heaps[1],1024))}
            for role,data in heaps.items():
                name=f'pid{pid}-thread{thread}-seq{seq}-role{role}.bin'
                (output/name).write_bytes(data);(output/name).chmod(0o400)
                item['heaps'][str(role)]={'file':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()}
            report['groups'].append(item)
        if kernel_records:
            report['kernel_ssh_pages']=[]
            for page in kernel_ssh_pages(kernel_records):
                data=page.pop('data');name=f"kernel-group{page['group']}-ticket{page['ticket']}-ssh.bin"
                (output/name).write_bytes(data);(output/name).chmod(0o400);page['file']=name;page['sha256']=hashlib.sha256(data).hexdigest()
                page['source_comparisons']=[]
                for pid,thread,seq,heaps in groups:
                    difference=relocation_difference(heaps[3],data)
                    page['source_comparisons'].append(dict(pid=int(pid),thread=thread,sequence=seq,
                        same_native_thread=int(thread,16)==page['thread'],**difference))
                page['association']='CANDIDATES_ONLY_NOT_UNIQUE_FRAME_OR_BACKING_PROOF'
                report['kernel_ssh_pages'].append(page)
        (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    except ValueError as error:
        (output/'rejected.json').write_text(json.dumps({'complete':False,'reason':str(error),'gpu_execution_proof':False})+'\n')
        raise
    print('CHECKSUM_CPU_HEAPS_COMPLETE groups='+str(len(groups))+' GPU=NOT_PROVEN')


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
    parser.add_argument('--heap-log', type=Path)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--kernel-records', type=Path)
    arguments = parser.parse_args()
    if arguments.heap_log:
        if not arguments.output_dir or arguments.self_test or arguments.iga or arguments.image or arguments.write:
            parser.error('heap-log requires a fresh output-dir and isolated analysis mode')
        audit_heap_log(arguments.heap_log,arguments.output_dir,arguments.kernel_records)
    elif arguments.kernel_records or arguments.output_dir:
        parser.error("kernel-records requires isolated heap-log mode")
    elif arguments.self_test:
        self_test()
    else:
        if not arguments.iga or not arguments.image:
            parser.error('--iga and --image required for the explicit local rebuild/check')
        rebuild(arguments.iga.resolve(), arguments.image.resolve(), arguments.write)
