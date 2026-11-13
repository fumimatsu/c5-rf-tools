"""Bounded asynchronous archive of CRC-verified scanner payloads, not RF I/Q."""
import base64
import copy
import json
from pathlib import Path
import queue
import secrets
import shutil
import sqlite3
import struct
import threading
import time

SCHEMA = 'c5-scanner-recording/1'


def storage_size(path):
    total=0
    for p in [path,Path(str(path)+'-wal')]:
        try:
            total+=p.stat().st_size
        except FileNotFoundError:
            pass  # WAL may disappear when its final connection closes.
    return total


class Counters:
    """Wire counters overlap; never add C5 drops and sequence gaps together."""
    def __init__(self):
        self.previous = None
        self.metrics = dict(rFrames=0, seqGaps=0, duplicates=0, counterResets=0,
                            clockWraps=0, c5DropsStart=None, c5DropsEnd=None,
                            c5DropsIncrease=0, malformed=0, settingsNotices=0)

    def observe(self, kind, payload):
        if kind != 'R':
            return
        m = self.metrics
        m['rFrames'] += 1
        if len(payload) < 12:
            m['malformed'] += 1
            return
        seq, count, flags, drops, _ = struct.unpack_from('<HBBII', payload)
        if not 1 <= count <= 8 or len(payload) != 12 + 14 * count:
            m['malformed'] += 1
            return
        clock = struct.unpack_from('<I', payload, 12)[0]
        m['settingsNotices'] += bool(flags & 1)
        if m['c5DropsStart'] is None:
            m['c5DropsStart'] = drops
        m['c5DropsEnd'] = drops
        if self.previous:
            old_seq, old_drops, old_clock = self.previous
            reset = ((drops-old_drops) & 0xffffffff) > 0x7fffffff
            if clock < old_clock:
                if old_clock-clock > 0x80000000:
                    m['clockWraps'] += 1
                else:
                    reset = True
            delta = (seq-old_seq) & 65535
            if delta > 32768:
                reset = True
            if reset:
                m['counterResets'] += 1
            else:
                m['c5DropsIncrease'] += (drops-old_drops) & 0xffffffff
                m['duplicates'] += delta == 0
                if delta:
                    m['seqGaps'] += delta-1
        self.previous = seq, drops, clock


class ScannerWriter:
    def __init__(self, root, context, capacity=16384, disk_min_bytes=256*1024**2,
                 disk_free=None):
        self.id = secrets.token_hex(8)
        self.path = Path(root)/(self.id+'.sqlite3')
        self.started, self.created = time.monotonic(), time.time()
        self.done = threading.Event()
        self.lock = threading.RLock()
        self.queue = queue.Queue(capacity)
        self.disk_free = disk_free or (lambda: shutil.disk_usage(self.path.parent).free)
        self.disk_min_bytes = disk_min_bytes
        if self.disk_free() < disk_min_bytes:
            raise ValueError('記録用の空き容量が下限を下回っています')
        self.seq = self.written = self.dropped = self.payload_bytes = 0
        self.error = self.reason = None
        self.counters = Counters()
        crc_start = context.get('crcErrorsStart',0)
        self.counters.metrics.update(crcErrorsStart=crc_start,crcErrorsEnd=crc_start,crcErrorsIncrease=0)
        self.meta = dict(schema=SCHEMA, id=self.id, createdUnix=self.created,
                         startMonotonic=self.started, status='recording',
                         context=copy.deepcopy(context), rows=0, duration=0, dropped=0,
                         units='receiver dB (raw/filt; centi-dB in payload)',
                         representation='Every CRC-verified C5MK USB scanner frame received during this session; losses are counted separately',
                         nativeCrossingAvailable=False, rhScale='not applicable',
                         diskMinimumBytes=disk_min_bytes, crcPolicy='CRC-invalid wire data is counted, not archived',
                         timestampBasis='host parser receipt; per-pilot C5 microsecond clocks remain in payload')
        self.ready = threading.Event()
        self.thread = threading.Thread(target=self.run, name='scanner-writer', daemon=True)
        self.thread.start()

    def append_frame(self, kind, payload, at=None, received_unix=None):
        with self.lock:
            if self.done.is_set():
                return False
            now = time.monotonic() if at is None else at
            self.seq += 1  # Host order, NOT the wrapping C5 uint16 sequence.
            self.counters.observe(kind, payload)
            row = (self.seq, now-self.started,
                   time.time() if received_unix is None else received_unix, kind, bytes(payload))
            try:
                self.queue.put_nowait(row)
                return True
            except queue.Full:
                self.dropped += 1
                self.stop('queue_overflow')
                return False

    def stop(self, reason):
        with self.lock:
            if not self.done.is_set():
                self.reason = reason
                self.ended = time.monotonic()
                self.done.set()

    def observe_crc(self, count):
        with self.lock:
            m=self.counters.metrics
            m['crcErrorsEnd']=count
            m['crcErrorsIncrease']=max(0,count-m['crcErrorsStart'])

    def status(self):
        with self.lock:
            return dict(id=self.id, schema=SCHEMA, recording=not self.done.is_set(),
                        duration=round(getattr(self,'ended',time.monotonic())-self.started,3),
                        rows=self.written, queued=self.queue.qsize(), dropped=self.dropped,
                        storageBytes=storage_size(self.path),
                        payloadBytes=self.payload_bytes, purpose=self.meta['context'].get('purpose','observation'), startMonotonic=self.started,
                        createdUnix=self.created, error=self.error, reason=self.reason,
                        closing=self.done.is_set() and self.thread.is_alive(), **self.counters.metrics)

    def initialize_db(self, db):
        db.executescript('PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;'
            'CREATE TABLE metadata(id INTEGER PRIMARY KEY,data TEXT NOT NULL);'
            'CREATE TABLE frames(record_seq INTEGER PRIMARY KEY,t REAL NOT NULL,'
            'receivedUnix REAL NOT NULL,kind TEXT NOT NULL,payload BLOB NOT NULL);'
            'CREATE INDEX frames_time ON frames(t,record_seq);'
            'CREATE TABLE events(seq INTEGER PRIMARY KEY,t REAL NOT NULL,data TEXT NOT NULL);')
        db.execute('INSERT INTO metadata VALUES(1,?)',(json.dumps(self.meta),))
        db.commit()
        self.ready.set()

    def run(self):
        db = None
        try:
            db = sqlite3.connect(self.path)
            self.initialize_db(db)
            event_seq = 1
            db.execute('INSERT INTO events VALUES(?,?,?)',
                       (event_seq,0,json.dumps(dict(kind='start',context=self.meta['context']))))
            last_check = 0
            while True:
                batch = []
                until = time.monotonic()+.1
                while len(batch) < 1000:
                    if self.done.is_set() and self.queue.empty():
                        break
                    remaining = until-time.monotonic()
                    if remaining <= 0:
                        break
                    try:
                        batch.append(self.queue.get(timeout=min(.02,remaining)))
                    except queue.Empty:
                        pass
                if batch:
                    db.executemany('INSERT INTO frames VALUES(?,?,?,?,?)',batch)
                    self.written += len(batch)
                    self.payload_bytes += sum(len(r[4]) for r in batch)
                if time.monotonic()-last_check >= .5:
                    if self.disk_free() < self.disk_min_bytes:
                        self.stop('disk_low')
                    last_check = time.monotonic()
                s = self.status()
                self.meta.update(rows=self.written,duration=s['duration'],dropped=self.dropped,
                                 counters=self.counters.metrics.copy(),payloadBytes=self.payload_bytes)
                db.execute('UPDATE metadata SET data=? WHERE id=1',(json.dumps(self.meta),))
                db.commit()
                if self.done.is_set() and self.queue.empty():
                    break
            incomplete = self.dropped or any(self.counters.metrics[k] for k in ['seqGaps','c5DropsIncrease','counterResets','malformed','crcErrorsIncrease'])
            self.meta.update(status='incomplete' if incomplete else 'complete',reason=self.reason,
                             endedUnix=time.time())
            event_seq += 1
            db.execute('INSERT INTO events VALUES(?,?,?)',
                       (event_seq,self.meta['duration'],json.dumps(dict(kind='stop',reason=self.reason,
                              counters=self.counters.metrics,dropped=self.dropped))))
            db.execute('UPDATE metadata SET data=? WHERE id=1',(json.dumps(self.meta),))
            db.commit()
            db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
        except Exception as e:
            self.error = str(e)
            self.stop('storage_error')
        finally:
            self.ready.set()
            if db:
                db.close()


def encoded_frame(row):
    seq,t,received,kind,payload = row
    return dict(kind='frame',sequence=seq,t=t,receivedUnix=received,frameKind=kind,
                payloadBase64=base64.b64encode(payload).decode('ascii'))


def chart_window(db, meta, end, seconds=30, bins=500):
    if not 1 <= seconds <= 30 or not 0 <= end <= 1e8:
        raise ValueError('scanner再生範囲は最大30秒です')
    end=min(end,meta['duration']);start=max(0,end-seconds)
    span=max(.001,end-start)
    # Fixed-size bucket arrays avoid allocating dictionaries for every RF sample.
    points=[[None]*bins for _ in range(4)]
    frequencies=meta['context']['frequencies']
    frames=malformed=0;previous=None;truncated=False
    for seq,t,kind,payload in db.execute(
            'SELECT record_seq,t,kind,payload FROM frames WHERE t>=? AND t<=? ORDER BY t,record_seq LIMIT 100001',
            (start,end)):
        if frames==100000:
            truncated=True;break
        frames+=1
        if kind!='R':continue
        if len(payload)<12 or not 1<=payload[2]<=8 or len(payload)!=12+14*payload[2]:
            malformed+=1;continue
        wire_seq=struct.unpack_from('<H',payload,0)[0]
        gap=previous is not None and (t-previous[1]>.1 or ((wire_seq-previous[0])&65535)!=1)
        previous=wire_seq,t
        index=min(bins-1,int((t-start)/span*bins))
        for slot in range(min(4,payload[2])):
            p=struct.unpack_from('<IhhHBBBB',payload,12+14*slot)
            raw,filt=p[1],p[2]
            bucket=points[slot][index]
            if bucket is None:
                bucket=[start+(index+.5)/bins*span,0,0,False,None,None,None,None,None,0,0,0]
                points[slot][index]=bucket
            bucket[1]+=1
            bucket[2]+=bool(p[6] or p[5] or p[4]!=30 or raw==-32768 or slot>=len(frequencies) or p[3]!=frequencies[slot])
            bucket[3]|=gap
            if raw!=-32768:
                if bucket[4] is None or raw<bucket[4]:bucket[4]=raw
                if bucket[5] is None or raw>bucket[5]:bucket[5]=raw
            if filt!=-32768:
                if bucket[6] is None or filt<bucket[6]:bucket[6]=filt
                if bucket[7] is None or filt>bucket[7]:bucket[7]=filt
            bucket[8]=p;bucket[9]=t;bucket[10]=wire_seq;bucket[11]=seq
    series=[]
    def db_value(v):return v/100 if v is not None else None
    for channel in points:
        rows=[]
        for b in channel:
            if b is None:continue
            p=b[8]
            rows.append(dict(t=b[0],count=b[1],invalid=b[2],gap=b[3],
                rawMin=db_value(b[4]),rawMax=db_value(b[5]),filtMin=db_value(b[6]),filtMax=db_value(b[7]),
                last=dict(t=p[0],raw=p[1]/100,db=p[2]/100,mhz=p[3],gain=p[4],clips=p[5],flags=p[6],
                          hostT=b[9],wireSeq=b[10],recordSequence=b[11])))
        series.append(rows)
    return dict(metadata=meta,start=start,end=end,series=series,records=[],frames=frames,
                malformed=malformed,truncated=truncated,
                representation='Time-bin min/max envelopes of archived frames; marked samples retained')
