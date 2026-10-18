#!/usr/bin/env python3
"""Bounded-memory Host stream archive. Does not acquire GPU/kernel state.

Each input channel gets its own exclusive archive. Kernel/adapter collectors
must still supply request identities and their own loss counters. A valid EOF
here proves only transport-file integrity, never upstream completeness.
"""
import argparse
import hashlib
import json
import os
import re
import stat
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


def record(source, path, channel, maximum, mirror=False, durable_live=False):
    if (not 1 <= maximum <= LIMIT or not channel or len(channel.encode()) > 128 or
            type(durable_live) is not bool):
        raise ValueError('invalid channel/capacity')
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    seq = total = 0
    last_sync = time.monotonic()
    try:
        write_all(fd, MAGIC)
        metadata = json.dumps({'channel': channel, 'wall_ns': time.time_ns(),
                               'upstream_complete': False, 'max_bytes': maximum,
                               'durable_live': durable_live},
                              separators=(',', ':')).encode()
        frame(fd, seq, 1, metadata)
        seq += 1
        os.fsync(fd)
        # A synced new file alone does not make its directory entry durable.
        # Complete both barriers before consuming any upstream evidence. This
        # is still only a Host archive barrier, not a Guest/GPU completeness ACK.
        parent = os.path.dirname(os.fspath(path)) or '.'
        parent_fd = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try:
            os.fsync(parent_fd)
        finally:
            os.close(parent_fd)
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
            # Explicit live-capture policy: a received frame is synced BEFORE
            # it is mirrored or an idle upstream can block the next read.
            # This is not an upstream-completeness or GPU-release ACK.
            if durable_live:
                os.fsync(fd)
                last_sync = time.monotonic()
            if mirror:
                write_all(sys.stdout.fileno(), data)
            if not durable_live and time.monotonic() - last_sync >= 1:
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


def inspect(source, consume=None):
    if exact(source, len(MAGIC)) != MAGIC:
        raise ValueError('unknown archive format')
    seq = total = 0
    terminal = None
    metadata = None
    payload_digest = hashlib.sha256()
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
                    not 1 <= metadata['max_bytes'] <= LIMIT or
                    type(metadata.get('durable_live', False)) is not bool):
                raise ValueError('invalid metadata')
        elif kind == 2 and length:
            payload_digest.update(data)
            total += length
            if total > metadata['max_bytes']:
                raise ValueError('declared input budget exceeded')
            if consume is not None:
                consume(data)  # Only CRC/sequence-verified bytes, constant RAM.
        elif kind in (3, 4, 5) and not length:
            terminal = {3: 'EOF', 4: 'LIMIT', 5: 'DISK_FLOOR'}[kind]
        else:
            raise ValueError('invalid record type/extent')
        seq += 1
        previous_time = stamp
    if metadata is None or terminal is None:
        raise ValueError('missing terminal record')
    return {'channel': metadata['channel'], 'bytes': total,
            'payload_sha256': payload_digest.hexdigest(),
            'durable_live_policy': metadata.get('durable_live', False),
            'transport_complete': terminal == 'EOF', 'upstream_complete': False,
            'gpu_execution_proof': False, 'terminal': terminal}


def durable_live_self_test(directory):
    import io
    import stat
    import threading
    from pathlib import Path
    global write_all
    native_sync, native_write = os.fsync, write_all
    entered, mirrored = threading.Event(), threading.Event()
    counts, results, errors, mirrored_bytes = [0], [], [], bytearray()
    read_fd, write_fd = os.pipe()
    class OwnedRaw(io.FileIO):
        def read(self, length=-1):
            entered.set()
            return super().read(length)
    source = OwnedRaw(read_fd, 'rb', closefd=True)
    def checked_sync(fd):
        native_sync(fd)
        if stat.S_ISREG(os.fstat(fd).st_mode): counts[0] += 1
    def checked_write(fd, data):
        if fd == sys.stdout.fileno():
            assert counts[0] >= 2, 'mirror preceded data durability barrier'
            mirrored_bytes.extend(data); mirrored.set()
        else:
            native_write(fd, data)
    path = Path(directory)/'durable-live.bin'
    def capture():
        try: results.append(record(source, path, 'owned-live', 64, True, True))
        except BaseException as error: errors.append(error)
    thread = threading.Thread(target=capture, daemon=True)
    try:
        os.fsync, write_all = checked_sync, checked_write
        thread.start()
        assert entered.wait(5), 'raw reader did not start'
        os.write(write_fd, b'probe')
        assert mirrored.wait(5), 'live data not persisted/mirrored while source stayed open'
        assert thread.is_alive() and not results and not errors
        assert mirrored_bytes == b'probe'
    finally:
        os.close(write_fd)
        thread.join(5)
        os.fsync, write_all = native_sync, native_write
        source.close()
    assert not thread.is_alive() and not errors and results == [5]
    with path.open('rb') as saved:
        report = inspect(saved)
    assert report['durable_live_policy'] and report['transport_complete'] and not report['upstream_complete']
    # A data fsync failure must propagate BEFORE mirror, leaving no EOF ACK.
    counts[0] = 0; mirrored_bytes.clear()
    def failed_data_sync(fd):
        if stat.S_ISREG(os.fstat(fd).st_mode):
            counts[0] += 1
            if counts[0] == 2: raise OSError('owned injected data sync failure')
        native_sync(fd)
    failed = Path(directory)/'failed-live.bin'
    try:
        os.fsync, write_all = failed_data_sync, checked_write
        try: record(io.BytesIO(b'x'), failed, 'failed-live', 2, True, True)
        except OSError: pass
        else: raise AssertionError('data fsync failure ignored')
        assert not mirrored_bytes
    finally:
        os.fsync, write_all = native_sync, native_write
    with failed.open('rb') as saved:
        try: inspect(saved)
        except ValueError: pass
        else: raise AssertionError('failed data barrier became complete archive')


def seal_transport(archive, receipt, expected_channel):
    """Commit a file-integrity receipt, never an upstream/GPU-release ACK.

    Consumers must also require successful command exit: directory fsync can
    fail after the exclusive receipt was created. Retain failures as evidence.
    """
    if not isinstance(expected_channel, str) or not expected_channel:
        raise ValueError('expected channel required')
    digest = hashlib.sha256()
    class HashedReader:
        def __init__(self, source):
            self.source = source
        def read(self, length):
            data = self.source.read(length)
            digest.update(data)
            return data
    fd = os.open(archive, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    with os.fdopen(fd, 'rb', buffering=0) as source:
        before = os.fstat(source.fileno())
        if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1 or
                before.st_uid != os.geteuid() or stat.S_IMODE(before.st_mode) not in (0o400, 0o600)):
            raise ValueError('archive must be a private owned regular file')
        report = inspect(HashedReader(source))
        if (report['channel'] != expected_channel or not report['transport_complete'] or
                not report['durable_live_policy']):
            raise ValueError('archive channel/EOF/durable-live policy mismatch')
        os.fsync(source.fileno())
        after = os.fstat(source.fileno())
        identity = lambda value: (value.st_dev, value.st_ino, value.st_size,
                                  value.st_mtime_ns, value.st_ctime_ns, value.st_nlink)
        if identity(before) != identity(after):
            raise ValueError('archive changed during verification')
    def sync_parent(path):
        directory = os.open(os.path.dirname(os.path.abspath(path)), os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    sync_parent(archive)
    result = dict(report, archive_sha256=digest.hexdigest(), archive_bytes=after.st_size,
                  receipt_scope='HOST_TRANSPORT_FILE_AT_BARRIER', host_file_barrier=True,
                  requires_successful_command_exit=True, source_writer_closed_proven=False,
                  archive_immutable=False, capture_ack=False)
    payload = (json.dumps(result, separators=(',', ':'))+'\n').encode()
    fd = os.open(receipt, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        write_all(fd, payload)
        os.fsync(fd)
    finally:
        os.close(fd)
    sync_parent(receipt)
    return result


def parse_drain_receipt(line):
    if not isinstance(line,str) or not line.isascii() or len(line)>1024 or '\n' in line.strip('\r\n'):
        raise ValueError('invalid drain receipt extent')
    fields=line.split()
    if not fields or fields.pop(0)!='NGREEN_FLIGHT_DRAINED':
        raise ValueError('missing drain receipt marker')
    result={}
    for field in fields:
        pair=field.split('=')
        if len(pair)!=2 or pair[0] in result or not pair[1]:
            raise ValueError('ambiguous drain receipt')
        result[pair[0]]=pair[1]
    literals={'drain_accepted':'1','terminal':'complete','holding':'1','native_archive':'committed',
              'host_ack':'NOT_EVALUATED','gpu_completion':'NOT_EVALUATED'}
    if (set(result)!=set(literals)|{'run_id','boot_uuid','collector_pid','collector_sha256','target_uuid'} or
            any(result.get(key)!=value for key,value in literals.items()) or
            not re.fullmatch(r'[A-Za-z0-9_-]{1,80}',result['run_id']) or
            not re.fullmatch(r'[0-9a-f]{64}',result['collector_sha256']) or
            not re.fullmatch(r'[1-9][0-9]{0,9}',result['collector_pid']) or int(result['collector_pid'])<=1):
        raise ValueError('unqualified drain receipt')
    for key in ('boot_uuid','target_uuid'):
        if not re.fullmatch(r'[0-9A-Fa-f]{8}(?:-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}',result[key]):
            raise ValueError('invalid drain source identity')
    return result


def cpu_scope_receipt_admission(transport,decoded,drain_line,expected):
    """Gate caller-verified outputs, not authentication of arbitrary JSON.

    The caller must obtain drain through its authenticated action channel and
    use fresh successful seal/decoder results. This function never grants GPU
    release or whole-frame completion and does not itself emit a capture ACK.
    """
    drain=parse_drain_receipt(drain_line)
    if expected.get('macos_build')!='25G229':
        raise ValueError('unsupported native build identity')
    if (transport.get('receipt_scope')!='HOST_TRANSPORT_FILE_AT_BARRIER' or
            transport.get('host_file_barrier') is not True or
            transport.get('transport_complete') is not True or
            transport.get('durable_live_policy') is not True or
            transport.get('channel')!=expected['channel']):
        raise ValueError('host transport not committed for expected channel')
    if (decoded.get('report_schema')!=2 or decoded.get('source_file_complete') is not True or
            not re.fullmatch(r'[0-9a-f]{64}',str(decoded.get('source_sha256',''))) or
            decoded['source_sha256']!=transport.get('payload_sha256') or
            type(decoded.get('source_bytes')) is not int or decoded['source_bytes']<=0 or
            type(transport.get('bytes')) is not int or
            decoded['source_bytes']!=transport.get('bytes')):
        raise ValueError('decoder bytes are not the committed transport payload')
    for key in ('run_id','boot_uuid','collector_sha256','target_uuid'):
        if drain.get(key)!=expected[key]:
            raise ValueError('action receipt from another run/build/source')
    if (decoded.get('producer_declared_origin')!='NATIVE_ROOT_TRACE' or
            decoded.get('terminal')!='collector_complete' or
            decoded.get('capture_transport_complete') is not True or
            decoded.get('abi_calibration_event_seen') is not True or
            decoded.get('native_loss_observed') is not False or
            decoded.get('pending_records')!=[] or 'error' in decoded):
        raise ValueError('native source incomplete/uncalibrated/lost')
    platform=decoded.get('platform') or {}
    if platform.get('boot_uuid')!=expected['boot_uuid'] or platform.get('build')!=expected['macos_build']:
        raise ValueError('native platform does not match authenticated action')
    native=decoded.get('native_state') or {}
    if type(native.get('owner')) is not int or str(native['owner'])!=drain.get('collector_pid'):
        raise ValueError('native collector owner does not match action')
    clock=decoded.get('clock_anchor') or {}
    if any(type(clock.get(key)) is not int or clock[key]<=0 for key in
           ('wall_ns','absolute_ticks','continuous_ticks','numer','denom')):
        raise ValueError('native clock anchor incomplete')
    coverage=decoded.get('media_submission_scopes') or {}
    groups=coverage.get('groups',[])
    if (not isinstance(groups,list) or len(groups)>64 or
            any(not isinstance(group,dict) or type(group.get('group')) is not int or
                not 1<=group['group']<=0xffffffff for group in groups) or
            len({group['group'] for group in groups})!=len(groups)):
        raise ValueError('invalid/duplicate coverage group identities')
    candidates=[group for group in groups if group.get('publication_candidate') is True]
    if (coverage.get('scope')!='MEDIA_SUBMISSION_NOT_H264_FRAME' or
            coverage.get('group_budget_exceeded') is not False or not candidates or
            type(coverage.get('complete_cpu_publication_groups')) is not int or
            any(group.get('complete_cpu_publication_scope') is not True or
                group.get('missing')!=[] or group.get('ambiguous')!=[] or group.get('problems')!=[] for group in candidates) or
            coverage.get('complete_cpu_publication_groups')!=len(candidates)):
        raise ValueError('CPU publication scope coverage missing or ambiguous')
    return {'eligible_for_cpu_scope_receipt':True,'scope':'avc_publication_v1',
            'requires_authenticated_action_channel':True,'capture_ack':False,
            'full_codec_frame_proven':False,'gpu_execution_proof':False}


def receipt_admission_self_test():
    import copy
    expected={'channel':'owned-model','run_id':'owned-model',
              'boot_uuid':'01234567-89AB-CDEF-0123-456789ABCDEF',
              'target_uuid':'11234567-89AB-CDEF-0123-456789ABCDEF',
              'collector_sha256':'a'*64,'macos_build':'25G229'}
    line=('NGREEN_FLIGHT_DRAINED run_id={run_id} boot_uuid={boot_uuid} collector_pid=42 '
          'collector_sha256={collector_sha256} target_uuid={target_uuid} drain_accepted=1 '
          'terminal=complete holding=1 native_archive=committed host_ack=NOT_EVALUATED '
          'gpu_completion=NOT_EVALUATED').format(**expected)
    transport={'channel':'owned-model','bytes':100,'payload_sha256':'b'*64,
               'receipt_scope':'HOST_TRANSPORT_FILE_AT_BARRIER','host_file_barrier':True,
               'transport_complete':True,'durable_live_policy':True}
    decoded={'report_schema':2,'source_file_complete':True,'source_sha256':'b'*64,'source_bytes':100,
             'producer_declared_origin':'NATIVE_ROOT_TRACE','terminal':'collector_complete',
             'capture_transport_complete':True,'abi_calibration_event_seen':True,
             'native_loss_observed':False,'pending_records':[],
             'platform':{'build':'25G229','boot_uuid':expected['boot_uuid']},
             'native_state':{'owner':42},'clock_anchor':dict.fromkeys(
                 ('wall_ns','absolute_ticks','continuous_ticks','numer','denom'),1),
             'media_submission_scopes':{'scope':'MEDIA_SUBMISSION_NOT_H264_FRAME',
                 'group_budget_exceeded':False,'complete_cpu_publication_groups':1,
                 'groups':[{'group':1,'publication_candidate':True,
                            'complete_cpu_publication_scope':True,'missing':[],'ambiguous':[],'problems':[]}]}}
    admitted=cpu_scope_receipt_admission(transport,decoded,line,expected)
    assert admitted['eligible_for_cpu_scope_receipt'] and not admitted['capture_ack']
    assert not admitted['gpu_execution_proof'] and admitted['requires_authenticated_action_channel']
    mutations=[('decoded',key,value) for key,value in
        (('source_sha256','c'*64),('source_file_complete',False),('source_bytes',99),
         ('producer_declared_origin','OWNED_CPU_FIXTURE'),('terminal','MISSING'),
         ('abi_calibration_event_seen',False),('native_loss_observed',True),
         ('pending_records',[1]),('error',''))]
    mutations += [('transport',key,value) for key,value in
        (('channel','other'),('payload_sha256','c'*64),('host_file_barrier',False),
         ('transport_complete',False),('durable_live_policy',False),('bytes',99))]
    for target,key,value in mutations:
        bad_transport,bad_decoded=copy.deepcopy((transport,decoded))
        (bad_decoded if target=='decoded' else bad_transport)[key]=value
        try:
            cpu_scope_receipt_admission(bad_transport,bad_decoded,line,expected)
        except ValueError:
            pass
        else:
            raise AssertionError('incomplete/mixed source admitted')
    for bad in (line+' terminal=complete',line.replace('drain_accepted=1','drain_accepted=0'),
                line.replace('collector_pid=42','collector_pid=43'),
                line.replace('run_id=owned-model','run_id=other'),
                line.replace(expected['boot_uuid'],'21234567-89AB-CDEF-0123-456789ABCDEF')):
        try:
            cpu_scope_receipt_admission(transport,decoded,bad,expected)
        except ValueError:
            pass
        else:
            raise AssertionError('ambiguous/wrong action receipt admitted')
    for mutation in ('lost-scope','duplicate-group','wrong-clock','wrong-owner','wrong-build'):
        bad=copy.deepcopy(decoded)
        if mutation=='lost-scope':bad['media_submission_scopes']['groups'][0]['problems']=['missing']
        elif mutation=='duplicate-group':bad['media_submission_scopes']['groups']*=2
        elif mutation=='wrong-clock':bad['clock_anchor']['denom']=0
        elif mutation=='wrong-owner':bad['native_state']['owner']=43
        else:bad['platform']['build']='other'
        try:
            cpu_scope_receipt_admission(transport,bad,line,expected)
        except ValueError:
            pass
        else:
            raise AssertionError('unqualified native scope admitted')


def self_test():
    global FREE_FLOOR
    receipt_admission_self_test()
    import io
    import tempfile
    import stat
    from pathlib import Path
    with tempfile.TemporaryDirectory(prefix='ngreen-flight-') as directory:
        durable_live_self_test(directory)
        archive = Path(directory)/'receipt-source.bin'
        receipt = Path(directory)/'transport-receipt.json'
        record(io.BytesIO(b'owned transport'), archive, 'owned-receipt', 128, durable_live=True)
        sealed = seal_transport(archive, receipt, 'owned-receipt')
        assert sealed['archive_sha256'] == hashlib.sha256(archive.read_bytes()).hexdigest()
        assert sealed['host_file_barrier'] and not sealed['capture_ack']
        assert not sealed['source_writer_closed_proven'] and not sealed['gpu_execution_proof']
        assert json.loads(receipt.read_text()) == sealed
        for name, channel in (('wrong-channel.json', 'other'), ('transport-receipt.json', 'owned-receipt')):
            try:
                seal_transport(archive, Path(directory)/name, channel)
            except (ValueError, FileExistsError):
                pass
            else:
                raise AssertionError('invalid/duplicate receipt accepted')
        assert json.loads(receipt.read_text()) == sealed
        archive.chmod(0o644)
        try:
            seal_transport(archive, Path(directory)/'public.json', 'owned-receipt')
        except ValueError:
            pass
        else:
            raise AssertionError('public archive accepted')
        archive.chmod(0o600)
        alias = Path(directory)/'source-link'
        alias.symlink_to(archive)
        try:
            seal_transport(alias, Path(directory)/'link.json', 'owned-receipt')
        except OSError:
            pass
        else:
            raise AssertionError('symlink archive accepted')
        saved_sync = os.fsync
        try:
            def denied_receipt_sync(fd):
                raise OSError('owned archive barrier refusal')
            os.fsync = denied_receipt_sync
            try:
                seal_transport(archive, Path(directory)/'unsynced-receipt.json', 'owned-receipt')
            except OSError:
                pass
            else:
                raise AssertionError('failed archive barrier acknowledged')
            assert not (Path(directory)/'unsynced-receipt.json').exists()
        finally:
            os.fsync = saved_sync
        with archive.open('ab') as corrupt:
            corrupt.write(b'not-a-frame')
        try:
            seal_transport(archive, Path(directory)/'corrupt.json', 'owned-receipt')
        except ValueError:
            pass
        else:
            raise AssertionError('corrupt archive acknowledged')
        assert not (Path(directory)/'corrupt.json').exists()
        # Verify ordering and failure before the first upstream read. Keep a
        # failed archive as incomplete evidence; never remove/replace it.
        native_sync = os.fsync
        barriers = []
        class OrderedSource(io.BytesIO):
            def read(self, length=-1):
                assert barriers[:2] == ['file', 'directory']
                return super().read(length)
        def checked_sync(fd):
            barriers.append('directory' if stat.S_ISDIR(os.fstat(fd).st_mode) else 'file')
            native_sync(fd)
        try:
            os.fsync = checked_sync
            assert record(OrderedSource(b'x'), Path(directory)/'ordered.bin', 'ordered', 2) == 1
        finally:
            os.fsync = native_sync
        class UnreadSource:
            reads = 0
            def read(self, length):
                self.reads += 1
                raise AssertionError('read before durable directory entry')
        unread = UnreadSource()
        def denied_directory_sync(fd):
            if stat.S_ISDIR(os.fstat(fd).st_mode):
                raise OSError('owned injected directory sync failure')
            native_sync(fd)
        try:
            os.fsync = denied_directory_sync
            try:
                record(unread, Path(directory)/'unsynced.bin', 'unsynced', 2)
            except OSError:
                pass
            else:
                raise AssertionError('directory sync failure ignored')
            assert unread.reads == 0
        finally:
            os.fsync = native_sync
        with (Path(directory)/'unsynced.bin').open('rb') as failed:
            try:
                inspect(failed)
            except ValueError:
                pass
            else:
                raise AssertionError('failed directory barrier became complete evidence')
        path = Path(directory) / 'stream.bin'
        payload = b'EOF\0LIMIT\xff' * 20000
        assert record(io.BytesIO(payload), path, 'owned-test', len(payload)+1) == len(payload)
        saved = path.read_bytes()
        result = inspect(io.BytesIO(saved))
        assert result['transport_complete'] and result['bytes'] == len(payload)
        assert result['payload_sha256'] == hashlib.sha256(payload).hexdigest()
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
    capture.add_argument('--durable-live', action='store_true',
                         help='sync every received frame before mirror; does not certify upstream completeness')
    check = sub.add_parser('inspect')
    check.add_argument('archive')
    check.add_argument('--extract', help='exclusive output; retained prefix is incomplete on validation error')
    seal = sub.add_parser('seal-transport', help='private archive integrity receipt, NOT capture/GPU ACK')
    seal.add_argument('archive')
    seal.add_argument('--receipt', required=True)
    seal.add_argument('--channel', required=True)
    sub.add_parser('self-test')
    args = parser.parse_args()
    if args.command == 'record':
        record(sys.stdin.buffer.raw, args.output, args.channel, args.max_bytes, args.mirror, args.durable_live)
    elif args.command == 'seal-transport':
        print(json.dumps(seal_transport(args.archive, args.receipt, args.channel), separators=(',', ':')))
    elif args.command == 'inspect':
        fd = -1
        try:
            if args.extract:
                fd = os.open(args.extract, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with open(args.archive, 'rb') as source:
                result = inspect(source, (lambda data: write_all(fd, data)) if fd >= 0 else None)
            if fd >= 0:
                os.fsync(fd)
                result['extracted_prefix_only'] = not result['transport_complete']
            print(json.dumps(result, separators=(',', ':')))
        finally:
            if fd >= 0:
                os.close(fd)
        raise SystemExit(0 if result['transport_complete'] else 1)
    else:
        self_test()
