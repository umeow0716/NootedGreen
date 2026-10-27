#!/usr/bin/env python3
"""Stream the owned kdebug archive; never interpret integrity as GPU success."""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
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
         25: 'native_render_surface_output', 26: 'native_pool_allocation_metadata',
         27: 'native_pool_allocation_chunk',28: 'native_rect_input_metadata',
         29: 'native_rect_input_chunk', 30: 'native_leased_ssh_cpu_pair', 31: 'native_leased_ssh_page_chunk',
         32: 'prepared_resource_cpu_page', 33: 'prepared_resource_task_leaf',
         34: 'prepared_resource_admission_prefix_pair',35: 'checksum_cpu_candidate_pair'}
CHECKSUM_NATIVE_SHAPE=(
    0x11000001,0,0x80000040,0x7a000004,0x001010a0,0,0,0,0,0x69041301,
    0x6101000e,1,0,0,1,0,1,0,1,0,1,0,1,0x1001,1,0x1001,
    0x70000007,0,0,0x01bf1000,0,0x007c0002,0,0,0,
    0x70010002,0,0x40,0x400,0x70020002,0,0x40,0,
    0x7103000f,0,0,0,0,0,0,0x07ff07ff,0x00020044,0,0,
    0x00010000,1,0x00020044,0,0x44,0x00020000,
    0x7a000004,0x001010a0,0,0,0,0,0x7a000004,0x0000089c,0,0,0,0)


def checksum_candidate_expected(words):
    if len(words)<16 or words[0] not in (1,2):raise ValueError('checksum candidate version')
    version=words[0];count=80 if version==1 else 86;body=count-2;start=16+count
    if (len(words)!=16+2*count or words[1] not in (15,31) or words[2] not in (1,2) or
        words[3]!=count or words[5]!=count or not count<=words[7]<=words[6]<=16384 or words[14]!=1 or
        (words[4],words[15])!=((count,0) if words[1]==31 else (0,1))):
        raise ValueError('checksum candidate header')
    heaps=[words[i]|words[i+1]<<32 for i in (8,10,12)]
    if len(set(heaps))!=3 or any(not v or v>0xfffffffff000 or v&4095 for v in heaps):
        raise ValueError('checksum candidate heap identity')
    if words[1]==15:
        if any(words[start:]):raise ValueError('rejected checksum candidate data')
        return heaps # Stable source only; never validate an absent candidate.
    source=list(CHECKSUM_NATIVE_SHAPE)
    for at,value in zip((14,16,20),heaps):source[at]=value&0xffffffff|1;source[at+1]=value>>32
    marker=[0x4e474352,version,72,body,1,1576]+[0]*(count-80)
    if words[16:88]!=source or words[88:start-2]!=marker:
        raise ValueError('checksum candidate native source/reservation')
    invalidate=[] if version==1 else [0x7a000004,0x00101cbc,0,0,0,0]
    expected=source[:26]+[0]*6+invalidate+source[26:]+[0,0]
    expected[:3]=[0,0,0];expected[10]=0x61010014;expected[11]|=6<<4
    for at in (14,16,18,20):expected[at]|=6<<4
    expected[13]=0x6c000;expected[35+len(invalidate)]|=0x40;expected[48+len(invalidate)]=0x40
    if words[start:]!=expected:raise ValueError('checksum candidate producer mismatch')
    return heaps

RESOURCE_SLOTS = {
    1: (1, 4, 10, 13, 16, 19, 22, 25, 28, 31, 34, 37, 40, 43, 46, 49, 52, 55, 58, 62, 65, 68),
    2: (1, 4, 7, 10, 13, 16, 52, 55, 58, 62, 65)+tuple(range(19, 50, 2)),
    3: (1, 6, 11, 16, 21), 4: (1, 4, 7)}


def validate_leaf(kind, words):
    if kind==35:
        checksum_candidate_expected(words)
        return
    if kind == 34:
        if (len(words)!=660 or words[0] not in (1,2) or words[1] not in (1,2) or words[2]>=6 or
                words[3]!=2640 or words[12]&~63 or words[13]>17 or any(words[14:16]) or
                words[30] not in (0,1) or bool(words[30])!=(words[13]==0) or words[31] or
                words[27] or not 0<words[26]<=4*1024*1024):
            raise ValueError('invalid prepared-resource admission prefix envelope')
        expected=0
        for index,(start,count) in enumerate(((32,36),(104,74),(252,102),(456,102))):
            for part in (0,1):
                copied=words[4+index*2+part];raw=struct.pack('<%dI'%count,*words[start+part*count:start+(part+1)*count])
                if copied>count*4 or any(raw[copied:]):raise ValueError('invalid admission prefix extent/padding')
            if words[4+index*2:6+index*2]==[count*4]*2 and words[start:start+count]==words[start+count:start+2*count]:
                expected|=4<<index
        if words[12]&60!=expected:raise ValueError('invalid admission prefix pair flags')
        for at in (16,18,20,22):
            address=words[at]|words[at+1]<<32
            if address>>47!=0x1ffff or address&7:raise ValueError('invalid admission scope identity')
        if words[13]!=1:
            resource=words[24]|words[25]<<32
            if resource>>47!=0x1ffff or resource&7:raise ValueError('invalid admitted resource identity')
        return
    if kind == 33:
        if (len(words)!=16 or words[0]!=1 or words[1] not in (1,2) or words[2]>=6 or
                words[3]>=1024 or words[5] or words[4]>=4*1024*1024 or words[7]>=4096 or
                not (words[6]|words[7]) or words[12]&~15 or not 0<words[13]<=4096 or any(words[14:]) or
                words[12]&14 and not words[12]&1 or words[12]&4 and words[12]&3!=3 or
                not words[12]&4 and any(words[8:12])):
            raise ValueError('invalid prepared-resource task leaf')
        return
    if kind == 32:
        if (len(words)!=16 or words[0]!=1 or words[1] not in (1,2) or words[2]>=6 or
                words[3]>2 or words[4]>1024 or words[6] or not 0<words[5]<=4*1024*1024 or
                words[8] or words[7]>4*1024*1024 or words[12] or words[14]>1 or words[15]>4):
            raise ValueError('invalid prepared-resource CPU page envelope')
        phase,pages,extent,offset,physical,size,flags,reason=(words[3],words[4],words[5],words[7],
            words[9]|words[10]<<32,words[11],words[13],words[15])
        if phase==0:
            valid=not pages and not physical and not size and reason in (0,1) and flags==(1 if reason==0 else 0)
        elif phase==1:
            valid=(pages<1024 and not reason and flags==15 and 0<physical<(1<<44) and
                0<size<=4096 and
                size<=4096-(physical&4095) and offset+size<=4*1024*1024)
        else:
            valid=(not physical and size<=extent and offset>=size and flags in (0,1) and
                (reason!=0 or size==extent and pages>0 and flags==1))
        if not valid:
            raise ValueError('invalid prepared-resource CPU page phase')
        return
    if kind == 30:
        if len(words)==8 and words[0]==2:
            if (words[1]!=4096 or words[2] not in (0,4096) or words[3] not in (0,4096) or
                    words[4]&~15 or words[5] or words[6] not in (1,2) or words[7] or
                    bool(words[4]&2)!=(words[2]==4096) or bool(words[4]&4)!=(words[3]==4096) or
                    words[4]&6 and not words[4]&1 or words[4]&8 and words[4]!=15):
                raise ValueError('invalid native leased SSH page metadata')
            return
        if (len(words)!=296 or words[0:2]!=[1,576] or words[4]&~15 or
                any(words[5:8]) or words[2] not in (0,576) or words[3] not in (0,576) or
                bool(words[4]&2)!=(words[2]==576) or bool(words[4]&4)!=(words[3]==576) or
                words[4]&6 and not words[4]&1 or
                words[4]&8 and (words[4]!=15 or words[8:152]!=words[152:296]) or
                not words[2] and any(words[8:152]) or not words[3] and any(words[152:296])):
            raise ValueError('invalid native leased SSH CPU pair')
        return
    if kind == 31:
        if (len(words)!=1032 or words[0]!=1 or words[1] not in (1,2) or words[2] not in (0,1) or
                words[3:5]!=[4096,4096] or words[5]&~15 or not words[5]&1 or
                not words[5]&(2<<words[2]) or words[5]&8 and words[5]!=15 or any(words[6:8])):
            raise ValueError('invalid native leased SSH page chunk')
        return
    if kind == 28:
        if (len(words)!=160 or words[0]!=1 or words[1]>272 or words[2]>272 or
                words[3]>24 or words[4]>24 or words[6]&~31 or words[7]!=4096 or words[8]!=32 or
                words[9] not in (0,1) or words[10] or words[11]):
            raise ValueError('invalid native rect metadata extent/profile')
        for raw,count in ((words[12:80],words[1]),(words[80:148],words[2]),
                          (words[148:154],words[3]),(words[154:160],words[4])):
            if any(struct.pack(f'<{len(raw)}I',*raw)[count:]):
                raise ValueError('native rect short-copy padding')
        count=words[148]|words[149]<<32;capacity=words[150]|words[151]<<32
        data=words[152]|words[153]<<32
        admitted=words[1]==272 and words[3]==24 and count<=capacity and count<=4096 and (
            not count or data>>47==0x1ffff and data<=MASK64-count*32 and (data+count*32-1)>>47==0x1ffff)
        if bool(words[9])!=admitted or (not admitted and words[5]) or admitted and words[5]>count*32:
            raise ValueError('native rect data admission mismatch')
        expected=(1 if words[1]==272 else 0)|(2 if words[3]==24 else 0)|(
            4 if admitted and words[5]==count*32 else 0)|(
            8 if words[1]==words[2]==272 and words[12:80]==words[80:148] else 0)|(
            16 if words[3]==words[4]==24 and words[148:154]==words[154:160] else 0)
        if words[6]!=expected: raise ValueError('native rect stability/copy flags mismatch')
        return
    if kind == 29:
        if (len(words)<7 or words[0]!=1 or not 0<words[2]<=32768 or words[2]%8 or
                words[3]>words[2]*4 or not 0<words[4]<=512 or words[1]+words[4]>words[3]//4 or
                len(words)!=6+words[4] or words[5]&~31):
            raise ValueError('invalid native rect input chunk')
        return
    if kind == 26:
        if (len(words)!=12 or words[0]!=1 or not 0<words[1]<=65536 or words[1]&3 or
                words[2]>words[1] or not (0<=words[3]<=words[4]<=((words[1]-words[2])&~3)) or
                words[5]>words[1] or words[6]&~7 or not words[6]&1 or
                bool(words[6]&2)!=(words[5]==words[1]) or
                not 0<=words[7]<words[9]<=words[8]<=256 or words[10] or words[11]):
            raise ValueError('invalid native pool allocation metadata')
        return
    if kind == 27:
        if (len(words)<7 or words[0]!=1 or not 0<words[2]<=16384 or words[3]>words[2]*4 or
                not 0<words[4]<=512 or words[1]+words[4]>words[3]//4 or
                len(words)!=6+words[4] or words[5]&~7 or not words[5]&1):
            raise ValueError('invalid native pool allocation chunk')
        return
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


class SourceDigest:
    """Hash exactly the bounded bytes consumed by binary OR PTY-text parsing."""
    def __init__(self, source):
        self.source=source
        self.digest=hashlib.sha256()
        self.bytes=0

    def _count(self, data):
        self.digest.update(data)
        self.bytes+=len(data)
        return data

    def read(self, size):
        return self._count(self.source.read(size))

    def readline(self, size):
        return self._count(self.source.readline(size))


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
        return {'report_schema':2,'complete_records': self.complete_records, 'pending_records': sorted(self.active),
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


def prepared_resource_coverage(rows, leaf_rows=()):
    """Strict CPU scope correlation. Never an IOVA/lifetime/completion grant."""
    scopes={};tickets={};total=0;pages={}
    for group,metadata,payload in rows:
        total+=1
        if total>12312 or len(payload)!=64:raise ValueError('prepared-resource record budget/extent')
        words=list(struct.unpack('<16I',payload));validate_leaf(32,words)
        ticket,role,phase=words[1:4];key=(group,ticket,role)
        if not group or not metadata.get('thread') or not int(metadata.get('identity','0'),16):
            raise ValueError('prepared-resource missing scope identity')
        if ticket in tickets and tickets[ticket]!=group:raise ValueError('prepared-resource ticket reused')
        tickets[ticket]=group
        if phase==0:
            if key in scopes or len(scopes)>=12:raise ValueError('prepared-resource duplicate/budget')
            scopes[key]={'thread':metadata['thread'],'identity':metadata['identity'],
                'extent':words[5],'start':words[7],'bytes':0,'pages':0,'ended':False,'reason':None}
        else:
            state=scopes.get(key)
            if not state or state['ended'] or state['thread']!=metadata['thread'] or state['identity']!=metadata['identity']:
                raise ValueError('prepared-resource missing begin/duplicate end/foreign owner')
            if words[5]!=state['extent'] or words[4]!=state['pages'] or words[7]!=state['start']+state['bytes']:
                raise ValueError('prepared-resource extent/ordinal/offset discontinuity')
            if phase==1:
                if words[11]>state['extent']-state['bytes']:raise ValueError('prepared-resource page overflow')
                pages[(*key,state['pages'])]=(metadata['thread'],metadata['identity'],state['bytes'],
                    words[9]|words[10]<<32,words[11])
                state['bytes']+=words[11];state['pages']+=1
            else:
                if words[11]!=state['bytes']:raise ValueError('prepared-resource end byte mismatch')
                state['ended']=True;state['reason']=words[15]
    if any(not state['ended'] for state in scopes.values()):raise ValueError('prepared-resource missing end')
    missing={str(ticket):sorted(set(range(6))-{key[2] for key in scopes if key[1]==ticket}) for ticket in tickets}
    if any(missing.values()):raise ValueError('prepared-resource missing resource role')
    leaves={};observed=0
    for group,metadata,payload in leaf_rows:
        if len(leaves)>=12288 or len(payload)!=64:raise ValueError('prepared-resource leaf budget/extent')
        words=list(struct.unpack('<16I',payload));validate_leaf(33,words)
        key=(group,words[1],words[2],words[3])
        if key not in pages or key in leaves:raise ValueError('prepared-resource orphan/duplicate task leaf')
        expected=pages[key]
        actual=(metadata.get('thread'),metadata.get('identity'),words[4],words[6]|words[7]<<32,words[13])
        if actual!=expected:raise ValueError('prepared-resource foreign/discontinuous task leaf')
        leaves[key]=words[12]
        observed+=words[12]==15
    if len(leaves)!=len(pages):raise ValueError('prepared-resource missing task leaf')
    return {'resources':len(scopes),'task_leaf_pages':len(leaves),'stable_native_task_leaf_reads':observed,
        'task_leaf_refused_or_unstable':len(leaves)-observed,
        'complete_cpu_page_coverage':sum(state['reason']==0 for state in scopes.values()),
        'refused_resources':sum(state['reason']!=0 for state in scopes.values()),
        'reason_by_resource':{f'{key[0]}:{key[1]}:{key[2]}':state['reason'] for key,state in scopes.items()},
        'atomic_snapshot':False,'native_object_lease_proven':False,'iova_mapping_proven':False,'gpu_execution_proof':False}


def checksum_candidate_coverage(rows):
    captures=[];seen=set()
    for group,metadata,payload in rows:
        if len(captures)>=2 or len(payload) not in (704,752):raise ValueError('checksum candidate record budget/extent')
        words=list(struct.unpack('<'+str(len(payload)//4)+'I',payload));heaps=checksum_candidate_expected(words)
        key=(group,words[2])
        identity=metadata.get('identity','')
        if (key in seen or type(group) is not int or group<=0 or type(metadata.get('thread')) is not int or
            metadata['thread']<=0 or metadata.get('group')!=group or metadata.get('kind')!=35 or
            metadata.get('word_count')!=len(words) or not re.fullmatch('[0-9a-f]{16}',identity) or
            not 0<int(identity,16)<(1<<48) or int(identity,16)&3):raise ValueError('checksum candidate scope/duplicate')
        seen.add(key)
        captures.append({'group':group,'ticket':words[2],'thread':metadata['thread'],'identity':identity,
            'source_words':words[3],'candidate_words':words[4],'heaps':[f'{v:x}' for v in heaps],
            'source_scope':'TASK_BOUND_PRE_BUILD_CPU_SOURCE_PAIR',
            'candidate_scope':'OWNED_SCRATCH_ONLY' if words[1]==31 else 'UNAVAILABLE_NOT_VALIDATED'})
    return {'captures':captures,'complete_candidates':sum(c['candidate_words']==c['source_words'] for c in captures),'scope_error':None,
        'native_command_writes':0,'native_end_called':False,'atomic_snapshot':False,
        'actual_publication_proven':False,'gpu_execution_proof':False}


def prepared_resource_admission(rows):
    stages=('accepted','scope-addresses','resource-map','resource-backing','map-class','map-accelerator',
        'map-task','map-memory-association','GPU-address-mode','rotated-map','memory-class','memory-accelerator',
        'memory-descriptor','backing-bytes','GPU-lower-bound','descriptor-offset','extent','owner-live')
    output=[];seen=set()
    for group,metadata,payload in rows:
        if len(output)>=12 or len(payload)!=2640:raise ValueError('admission snapshot budget/extent')
        w=list(struct.unpack('<660I',payload));validate_leaf(34,w)
        key=(group,w[1],w[2])
        if key in seen or not group or not metadata.get('thread'):raise ValueError('admission snapshot duplicate/owner')
        seen.add(key)
        q=lambda at:w[at]|w[at+1]<<32
        if int(metadata.get('identity','0'),16)!=q(28):raise ValueError('admission GPU identity mismatch')
        parts=[]
        for part in (0,1):
            def field(start,copied,offset,size=8):
                if copied<offset+size:return None
                raw=struct.pack('<%dI'%((offset+size+3)//4),*w[start:start+(offset+size+3)//4])
                return int.from_bytes(raw[offset:offset+size],'little')
            r,m,a,b=32+part*36,104+part*74,252+part*102,456+part*102
            counts=[w[4+part],w[6+part],w[8+part],w[10+part]]
            parts.append({'resource_type':field(r,counts[0],0x14,1),'resource_map':field(r,counts[0],0x40),'resource_backing80':field(r,counts[0],0x80),
                'resource_backing88':field(r,counts[0],0x88),'map_class':field(m,counts[1],0),
                'map_memory':field(m,counts[1],0x18),'map_prepare_value':field(m,counts[1],0x20,4),
                'map_owner':field(m,counts[1],0x88),'map_task':field(m,counts[1],0x90),
                'map_gpu':field(m,counts[1],0x98),'map_flags':field(m,counts[1],0x10,4),
                'map_rotated':field(m,counts[1],0x11c,1),
                'backing_class':field(a,counts[2],0),'backing_owner':field(a,counts[2],0x28),
                'backing_bytes':field(a,counts[2],0x40),'backing_descriptor':field(a,counts[2],0xd0),
                'map_memory_class':field(b,counts[3],0),'map_memory_owner':field(b,counts[3],0x28),
                'map_memory_bytes':field(b,counts[3],0x40),'map_memory_descriptor':field(b,counts[3],0xd0)})
        expected={'map_class':q(16),'backing_class':q(18),'map_owner':q(20),'backing_owner':q(20),'map_task':q(22)}
        if w[0]==2:
            expected={'map_class':q(16),'map_memory_class':q(18),'map_owner':q(20),'map_memory_owner':q(20),'map_task':q(22)}
        comparisons={name:(None if parts[0][name] is None else parts[0][name]==value) for name,value in expected.items()}
        output.append({'group':group,'ticket':w[1],'role':w[2],'thread':metadata['thread'],
            'resource':q(24),'gpu':q(28),'requested_bytes':q(26),'copied_prefix_bytes':w[4:12],
            'pair_flags':w[12],'admitted':bool(w[30]),'model_version':w[0],
            'stage':('resource-type' if w[0]==2 and w[13]==3 else 'native-prepare-value' if w[0]==2 and w[13]==15 else stages[w[13]]),'stage_id':w[13],
            'selected_backing_source':'map-memory' if w[0]==2 else 'legacy-resource-backing',
            'expected':expected,'expected_matches_before':comparisons,'before':parts[0],'after':parts[1]})
    return {'captures':len(output),'resources':output,'atomic_snapshot':False,
        'native_object_lease_proven':False,'gpu_execution_proof':False}


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
        self.db.execute('CREATE TABLE pool_allocations (identity TEXT PRIMARY KEY, group_id INTEGER, bytes INTEGER, copied INTEGER, flags INTEGER, captured INTEGER) WITHOUT ROWID')
        self.db.execute('CREATE TABLE rect_inputs (identity TEXT PRIMARY KEY, group_id INTEGER, words INTEGER, copied INTEGER, flags INTEGER, admitted INTEGER, captured INTEGER) WITHOUT ROWID')
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
        if record['kind'] == 28:
            if not record['group'] or not 1<=int(record['identity'],16)<=4:
                raise ValueError('missing native rect parent')
            count=words[148]|words[149]<<32
            self.db.execute('INSERT INTO rect_inputs VALUES (?,?,?,?,?,?,0)',
                            (record['identity'],record['group'],count*8 if words[9] else 0,words[5],words[6],words[9]))
        if record['kind'] == 29:
            row=self.db.execute('SELECT group_id,words,copied,flags,admitted,captured FROM rect_inputs WHERE identity=?',
                                (record['identity'],)).fetchone()
            if row!=(record['group'],words[2],words[3],words[5],1,words[1]*4):
                raise ValueError('missing/refused/out-of-order native rect chunk')
            self.db.execute('UPDATE rect_inputs SET captured=captured+? WHERE identity=?',
                            (words[4]*4,record['identity']))
        if record['kind'] == 26:
            if not record['group'] or not 1<=int(record['identity'],16)<=4:
                raise ValueError('missing native pool allocation parent')
            self.db.execute('INSERT INTO pool_allocations VALUES (?,?,?,?,?,0)',
                            (record['identity'],record['group'],words[1],words[5],words[6]))
        if record['kind'] == 27:
            row=self.db.execute('SELECT group_id,bytes,copied,flags,captured FROM pool_allocations WHERE identity=?',
                                (record['identity'],)).fetchone()
            if row != (record['group'],words[2]*4,words[3],words[5],words[1]*4):
                raise ValueError('missing/duplicate/out-of-order native pool chunk')
            self.db.execute('UPDATE pool_allocations SET captured=captured+? WHERE identity=?',
                            (words[4]*4,record['identity']))
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

    def prepared_admission_report(self):
        rows=self.db.execute('SELECT group_id,metadata,payload FROM records WHERE kind=34 ORDER BY id LIMIT 13')
        try:return prepared_resource_admission((group,json.loads(metadata),payload) for group,metadata,payload in rows)
        except (ValueError,KeyError,TypeError,struct.error) as exc:
            return {'scope_error':str(exc),'captures':0,'gpu_execution_proof':False}

    def prepared_resource_report(self):
        rows=self.db.execute('SELECT group_id,metadata,payload FROM records WHERE kind=32 ORDER BY id LIMIT 12313')
        try:
            leaves=self.db.execute('SELECT group_id,metadata,payload FROM records WHERE kind=33 ORDER BY id LIMIT 12289')
            result=prepared_resource_coverage(((group,json.loads(metadata),payload) for group,metadata,payload in rows),
                ((group,json.loads(metadata),payload) for group,metadata,payload in leaves))
            result['scope_error']=None
            return result
        except (ValueError,KeyError,TypeError,struct.error) as exc:
            return {'scope_error':str(exc),'complete_cpu_page_coverage':0,'gpu_execution_proof':False}

    def checksum_candidate_report(self):
        rows=self.db.execute('SELECT group_id,metadata,payload FROM records WHERE kind=35 ORDER BY id LIMIT 3')
        try:return checksum_candidate_coverage((group,json.loads(metadata),payload) for group,metadata,payload in rows)
        except (ValueError,KeyError,TypeError,struct.error) as exc:
            return {'scope_error':str(exc),'complete_candidates':0,'actual_publication_proven':False,'gpu_execution_proof':False}

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

    def pool_allocation_report(self):
        total, complete = self.db.execute('SELECT COUNT(*),COALESCE(SUM(flags=7 AND copied=bytes AND captured=bytes),0) FROM pool_allocations').fetchone()
        return {'captures':total,'complete_control_stable_cpu_copies':complete,'incomplete':total-complete,
                'maximum_captures':4,'all_submissions_captured':False,'atomic_snapshot':False,
                'capture_phase':'BEFORE_NATIVE_POOL_SUBMIT',
                'stability_scope':'POOL_CONTROL_FIELDS_NOT_CONTENT_ATOMICITY',
                'native_object_lease_proven':False,'gpu_execution_proof':False}

    def rect_input_report(self):
        total,complete=self.db.execute('SELECT COUNT(*),COALESCE(SUM(admitted=1 AND flags=31 AND copied=words*4 AND captured=copied),0) FROM rect_inputs').fetchone()
        return {'captures':total,'complete_control_stable_cpu_copies':complete,'incomplete':total-complete,
                'maximum_captures':4,'maximum_rectangles_per_capture':4096,'parameter_prefix_bytes':272,
                'full_parameter_extent_proven':False,'all_submissions_captured':False,'atomic_snapshot':False,
                'native_object_lease_proven':False,'gpu_execution_proof':False,'capture_phase':'BEFORE_NATIVE_RECT_CALL'}

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

    def scope_coverage_report(self):
        """Correlate actual CPU publication records; group != codec frame.

        A global histogram cannot establish that source, published and native
        copies belong together. Bound the diagnostic itself and reject ambiguous
        scopes rather than joining records across groups or threads.
        """
        order = (1, 9, 2, 3, 12, 4, 11)
        groups = self.db.execute('SELECT DISTINCT group_id FROM records WHERE group_id>0 ORDER BY group_id LIMIT 65').fetchall()
        output = []
        for (group,) in groups[:64]:
            rows = self.db.execute('SELECT id,kind,metadata,payload FROM records WHERE group_id=? AND kind IN (1,9,2,3,12,4,11) ORDER BY id LIMIT 129', (group,)).fetchall()
            by_kind = {kind: [] for kind in order}
            for row in rows:
                by_kind[row[1]].append(row)
            missing = [KINDS[kind] for kind in order if not by_kind[kind]]
            ambiguous = [KINDS[kind] for kind in order if len(by_kind[kind])>1]
            problems = []
            if missing:
                problems.append('missing-required-kind')
            if ambiguous or len(rows)>128:
                problems.append('ambiguous-or-over-budget')
            if not problems:
                selected = [by_kind[kind][0] for kind in order]
                metadata = [json.loads(row[2]) for row in selected]
                if [row[0] for row in selected] != sorted(row[0] for row in selected):
                    problems.append('native-publication-order')
                threads = {item['thread'] for item in metadata}
                if len(threads)!=1 or not next(iter(threads)):
                    problems.append('thread-mismatch')
                identities = {item['identity'] for item in metadata[1:-1]}
                if len(identities)!=1 or int(next(iter(identities)),16)==0:
                    problems.append('publication-identity-mismatch')
                if (metadata[0]['identity']!='0000000000000000' or selected[0][3] or
                        metadata[-1]['identity']!='0000000000000000' or
                        selected[-1][3] not in (struct.pack('<I',0),struct.pack('<I',1))):
                    problems.append('scope-boundary-shape')
                if any(left['end_ticks']>right['begin_ticks'] for left,right in zip(metadata,metadata[1:])):
                    problems.append('scope-clock-order')
                payloads = {row[1]:row[3] for row in selected}
                if (len(payloads[9])!=48 or len(payloads[12])!=32 or
                        any(not payloads[kind] or len(payloads[kind])%4 for kind in (2,3,4))):
                    problems.append('publication-extent-shape')
                else:
                    publication = struct.unpack('<12I',payloads[9])
                    native = struct.unpack('<8I',payloads[12])
                    address = publication[10] | publication[11]<<32
                    if (publication[0]*4!=len(payloads[2]) or publication[1]*4!=len(payloads[3]) or
                            native[0]*4!=len(payloads[4]) or native[7]!=1 or
                            address!=int(metadata[1]['identity'],16)):
                        problems.append('publication-extent-identity')
            output.append({'group':group,'publication_candidate':any(by_kind[kind] for kind in (9,2,3,12,4)),
                           'complete_cpu_publication_scope':not problems,
                           'missing':missing,'ambiguous':ambiguous,'problems':problems})
        return {'scope':'MEDIA_SUBMISSION_NOT_H264_FRAME','groups':output,
                'group_budget_exceeded':len(groups)>64,
                'complete_cpu_publication_groups':sum(item['complete_cpu_publication_scope'] for item in output),
                'full_codec_frame_proven':False,'source_writer_closed_proven':False,
                'capture_ack':False,'atomic_snapshot':False,'gpu_execution_proof':False}

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
    page=[2,4096,4096,4096,15,0,1,0]
    chunk=[1,1,0,4096,4096,15,0,0]+list(range(1024))
    validate_leaf(30,page);validate_leaf(31,chunk)
    for kind,raw,changes in ((30,page,((0,3),(1,4095),(2,4095),(3,4095),(4,31),(5,1),(6,3),(7,1))),
                             (31,chunk,((0,2),(1,3),(2,2),(3,4095),(4,4095),(5,1),(6,1),(7,1)))):
        for index,value in changes:
            bad=list(raw);bad[index]=value
            try:validate_leaf(kind,bad)
            except ValueError:pass
            else:raise AssertionError('SSH page wire negative accepted')
    ssh=[1,576,576,576,15,0,0,0]+list(range(144))*2
    validate_leaf(30,ssh)
    for index,value in ((0,2),(1,575),(2,575),(3,575),(4,31),(5,1),(6,1),(7,1),(295,999)):
        bad=list(ssh);bad[index]=value
        try:validate_leaf(30,bad)
        except ValueError:pass
        else:raise AssertionError('SSH wire negative accepted')
    refused=[1,576,0,0,0,0,0,0]+[0]*288
    validate_leaf(30,refused)
    for index in (8,151,152,295):
        bad=list(refused);bad[index]=1
        try:validate_leaf(30,bad)
        except ValueError:pass
        else:raise AssertionError('refused SSH payload accepted')
    for text in (False,True):
        data=b'owned bytes\nsecond line\n'
        hashed=SourceDigest(io.BytesIO(data))
        while (hashed.readline(7) if text else hashed.read(7)):
            pass
        assert hashed.bytes==len(data) and hashed.digest.hexdigest()==hashlib.sha256(data).hexdigest()
    # Exercise the production SQL correlation, not a synthetic histogram.
    # Packet framing/leaf admission are tested separately below.
    with tempfile.TemporaryDirectory(prefix='ngreen-scope-owned-') as directory:
        sink = DiskSink(Path(directory)/'decoded')
        kinds = (1,9,2,3,12,4,11)
        payloads = {1:b'',9:struct.pack('<12I',1,1,0,0,0,0,0,0,0,0,0x40010000,0),
                    2:bytes(4),3:bytes(4),12:struct.pack('<8I',1,0,0,1,1,1,0,1),
                    4:bytes(4),11:bytes(4)}
        def populate():
            sink.db.execute('DELETE FROM records')
            for index,kind in enumerate(kinds,1):
                metadata = {'identity':'0000000000000000' if kind in (1,11) else '0000000040010000',
                            'thread':7,'begin_ticks':index*10,'end_ticks':index*10+1}
                sink.db.execute('INSERT INTO records VALUES (?,?,?,?,?)',
                                (index,1,kind,json.dumps(metadata),payloads[kind]))
        populate()
        valid = sink.scope_coverage_report()
        assert valid['complete_cpu_publication_groups']==1 and not valid['capture_ack']
        assert not valid['full_codec_frame_proven'] and not valid['gpu_execution_proof']
        for missing in kinds:
            populate()
            sink.db.execute('DELETE FROM records WHERE kind=?',(missing,))
            assert sink.scope_coverage_report()['complete_cpu_publication_groups']==0
        for field,value in (('thread',8),('identity','0000000040020000'),('begin_ticks',1)):
            populate()
            row = sink.db.execute('SELECT metadata FROM records WHERE kind=4').fetchone()
            metadata = json.loads(row[0]);metadata[field]=value
            sink.db.execute('UPDATE records SET metadata=? WHERE kind=4',(json.dumps(metadata),))
            assert sink.scope_coverage_report()['complete_cpu_publication_groups']==0
        populate()
        sink.db.execute('UPDATE records SET group_id=2 WHERE kind=4')
        assert sink.scope_coverage_report()['complete_cpu_publication_groups']==0
        populate()
        sink.db.execute('INSERT INTO records SELECT 20,group_id,kind,metadata,payload FROM records WHERE kind=3')
        assert sink.scope_coverage_report()['complete_cpu_publication_groups']==0
        for kind,payload in ((2,b''),(9,bytes(48)),(12,bytes(32)),(11,bytes(8))):
            populate()
            sink.db.execute('UPDATE records SET payload=? WHERE kind=?',(payload,kind))
            assert sink.scope_coverage_report()['complete_cpu_publication_groups']==0
        populate()
        for group in range(2,66):
            sink.db.execute('INSERT INTO records SELECT ?,?,kind,metadata,payload FROM records WHERE id=1',
                            (100+group,group))
        assert sink.scope_coverage_report()['group_budget_exceeded']
        assert len(sink.scope_coverage_report()['groups'])==64
        sink.close()
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
        hashed_source=None
        source_complete=False
        try:
            with args.input.open('rb') as source:
                before=os.fstat(source.fileno())
                hashed_source=SourceDigest(source)
                packets = binary_packets(hashed_source) if args.format == 'binary' else text_packets(hashed_source)
                for header, data in packets:
                    decoder.packet(header, data)
                after=os.fstat(source.fileno())
                identity=lambda value:(value.st_dev,value.st_ino,value.st_size,value.st_mtime_ns,value.st_ctime_ns)
                if (not stat.S_ISREG(after.st_mode) or identity(before)!=identity(after) or
                        hashed_source.bytes!=after.st_size):
                    raise ValueError('decoder source changed or was not completely consumed')
                source_complete=True
        except (ValueError, OSError, struct.error, sqlite3.Error) as exc:
            error = str(exc)
        ring_report = sink.ring_report()
        backing_report = sink.backing_report()
        cpu_alias_report = sink.backing_report(cpu_alias=True)
        synchronous_batch_report = sink.synchronous_batch_report()
        pool_allocation_report = sink.pool_allocation_report()
        rect_input_report = sink.rect_input_report()
        resource_report = sink.resource_report()
        map_list_report = sink.map_list_report()
        prepared_resource_report = sink.prepared_resource_report()
        prepared_admission_report = sink.prepared_admission_report()
        checksum_candidate_report = sink.checksum_candidate_report()
        scope_report = sink.scope_coverage_report()
        try:
            sink.close()
        except (OSError, sqlite3.Error) as exc:
            error = str(exc)
        report = decoder.report()
        report['source_bytes']=hashed_source.bytes if hashed_source else 0
        report['source_file_complete']=source_complete
        report['source_sha256']=hashed_source.digest.hexdigest() if source_complete else None
        report['prepared_resource_cpu_pages'] = prepared_resource_report
        report['prepared_resource_admission'] = prepared_admission_report
        report['checksum_cpu_candidates'] = checksum_candidate_report
        report['rings'] = ring_report
        report['backings'] = backing_report
        report['cpu_alias_backings'] = cpu_alias_report
        report['synchronous_batches'] = synchronous_batch_report
        report['pool_allocations'] = pool_allocation_report
        report['rect_inputs'] = rect_input_report
        report['resource_bases'] = resource_report
        report['cpu_map_lists'] = map_list_report
        report['media_submission_scopes'] = scope_report
        if error is not None:
            report['error'] = error
            report['capture_transport_complete'] = False
        with (args.output_dir/'report.json').open('x') as output:
            json.dump(report, output, separators=(',', ':'))
        print(json.dumps(report, separators=(',', ':')))
        raise SystemExit(0 if report['capture_transport_complete'] else 1)
