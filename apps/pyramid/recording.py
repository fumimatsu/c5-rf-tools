"""Versioned native RH snapshot archive. No raw-1kHz/recalculation claims."""
import copy
from contextlib import closing
import json
from pathlib import Path
import queue
import re
import secrets
import sqlite3
import threading
import time
from scanner_recording import ScannerWriter, SCHEMA as SCANNER_SCHEMA, encoded_frame, chart_window


class Writer:
    def __init__(self, root, context, capacity=2048):
        self.id = secrets.token_hex(8)
        self.path = Path(root) / (self.id + '.sqlite3')
        self.queue = queue.Queue(maxsize=capacity)
        self.done = threading.Event()
        self.lock = threading.Lock()
        self.seq = 0
        self.dropped = 0
        self.written = 0
        self.error = None
        self.reason = None
        self.started = time.monotonic()
        self.created = time.time()
        self.meta = dict(schema='c5-native-recording/1', id=self.id, createdUnix=self.created,
                         startMonotonic=self.started, status='recording', context=copy.deepcopy(context),
                         representation='RH36 snapshots and timed extrema; not full 1kHz samples',
                         timeBasis='host monotonic; C5 clock brackets/ages retained as reported',
                         units='RH 1..254', dropped=0, rows=0, duration=0)
        db = sqlite3.connect(self.path)
        db.executescript('PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;'
                         'CREATE TABLE metadata (id INTEGER PRIMARY KEY, data TEXT NOT NULL);'
                         'CREATE TABLE rows (seq INTEGER PRIMARY KEY, kind TEXT NOT NULL, '
                         'slot INTEGER, t REAL NOT NULL, data TEXT NOT NULL);'
                         'CREATE INDEX rows_time ON rows(t);')
        db.execute('INSERT INTO metadata VALUES(1, ?)', (json.dumps(self.meta),))
        db.commit()
        db.close()
        self.thread = threading.Thread(target=self.run, name='recording-writer', daemon=True)
        self.thread.start()

    def append(self, kind, data, slot=None, at=None):
        with self.lock:
            if self.done.is_set() or self.error:
                return False
            self.seq += 1
            record = dict(sequence=self.seq, kind=kind, slot=slot,
                          t=round((at if at is not None else time.monotonic()) - self.started, 6),
                          receivedUnix=time.time(), data=copy.deepcopy(data))
            try:
                self.queue.put_nowait(record)
                return True
            except queue.Full:
                self.dropped += 1
                return False

    def stop(self, reason):
        with self.lock:
            if not self.done.is_set():
                self.reason = reason
                self.ended = time.monotonic()
                self.done.set()

    def status(self):
        with self.lock:
            return dict(id=self.id, schema='c5-native-recording/1', recording=not self.done.is_set() and not self.error,
                        duration=round(getattr(self, 'ended', time.monotonic()) - self.started, 2),
                        rows=self.written, queued=self.queue.qsize(), dropped=self.dropped,
                        startMonotonic=self.started, createdUnix=self.created,
                        error=self.error, reason=self.reason, closing=self.done.is_set() and self.thread.is_alive())

    def run(self):
        db = None
        try:
            db = sqlite3.connect(self.path)
            db.execute('PRAGMA synchronous=FULL')
            last_flush = time.monotonic()
            pending = 0
            while not self.done.is_set() or not self.queue.empty():
                try:
                    row = self.queue.get(timeout=.1)
                except queue.Empty:
                    row = None
                if row:
                    db.execute('INSERT INTO rows VALUES(?,?,?,?,?)',
                               (row['sequence'], row['kind'], row['slot'], row['t'],
                                json.dumps(row, ensure_ascii=False, allow_nan=False)))
                    self.written += 1
                    pending += 1
                if pending and time.monotonic() - last_flush >= .5:
                    self.meta.update(rows=self.written, dropped=self.dropped,
                                     duration=round(time.monotonic() - self.started, 6))
                    db.execute('UPDATE metadata SET data=? WHERE id=1', (json.dumps(self.meta),))
                    db.commit()
                    pending = 0
                    last_flush = time.monotonic()
            self.meta.update(status='complete' if self.dropped == 0 else 'incomplete',
                             reason=self.reason, rows=self.written, dropped=self.dropped,
                             duration=round(self.ended - self.started, 6), endedUnix=time.time())
            db.execute('UPDATE metadata SET data=? WHERE id=1', (json.dumps(self.meta),))
            db.commit()
            db.execute('PRAGMA wal_checkpoint(TRUNCATE)')
        except Exception as error:
            self.error = str(error)
            self.done.set()
            self.ended = time.monotonic()
        finally:
            if db:
                db.close()


class Recordings:
    def __init__(self, root, disk_min_bytes=256*1024**2):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.current = None
        self.last = None
        self.writers = {}
        self.disk_min_bytes = disk_min_bytes

    def start(self, context, kind='native'):
        with self.lock:
            if self.current:
                raise ValueError('既に記録中です')
            self.current = (ScannerWriter(self.root, context, disk_min_bytes=self.disk_min_bytes)
                            if kind == 'scanner' else Writer(self.root, context))
            self.last = self.current
            self.writers = {i: w for i, w in self.writers.items() if w.thread.is_alive() or w.error}
            self.writers[self.current.id] = self.current
            return self.current.id

    def append(self, kind, data, slot=None, at=None):
        with self.lock:
            return self.current.append(kind, data, slot, at) if self.current else False

    def append_frame(self, kind, payload, at=None):
        with self.lock:
            return self.current.append_frame(kind,payload,at) if isinstance(self.current,ScannerWriter) else False

    def observe_crc(self,count):
        with self.lock:
            if isinstance(self.current,ScannerWriter):
                self.current.observe_crc(count)

    def stop(self, reason):
        with self.lock:
            writer = self.current
            self.current = None
            if writer:
                writer.stop(reason)

    def shutdown(self):
        self.stop('service_stopped')
        for writer in list(self.writers.values()):
            writer.thread.join(timeout=5)

    def status(self, recording_id=None):
        with self.lock:
            writer = self.writers.get(recording_id) if recording_id else self.last
            return writer.status() if writer else None

    def connect(self, recording_id):
        if not isinstance(recording_id, str) or not re.fullmatch('[a-f0-9]{16}', recording_id):
            raise ValueError('記録IDが不正です')
        p = self.root / (recording_id + '.sqlite3')
        if not p.is_file():
            raise FileNotFoundError('記録が見つかりません')
        return sqlite3.connect(p.as_uri() + '?mode=ro', uri=True, timeout=2)

    def metadata(self, recording_id):
        writer=self.writers.get(recording_id)
        if isinstance(writer,ScannerWriter):
            if not writer.ready.wait(2):
                raise ValueError('記録ファイルを準備しています')
            if writer.error:
                meta=copy.deepcopy(writer.meta)
                meta.update(status='error',error=writer.error,rows=writer.written,recordingKind='scanner')
                return meta
        with closing(self.connect(recording_id)) as db:
            meta = json.loads(db.execute('SELECT data FROM metadata WHERE id=1').fetchone()[0])
            if meta.get('schema') not in [SCANNER_SCHEMA,'c5-native-recording/1']:
                raise ValueError('未対応の記録形式です')
            if meta.get('schema') == SCANNER_SCHEMA:
                duration = db.execute('SELECT max(t) FROM frames').fetchone()[0]
                count = meta.get('rows',0)  # Committed with the frame batch, no full-table count per refresh.
            else:
                count, duration = db.execute('SELECT count(*),max(t) FROM rows').fetchone()
        live = self.status(recording_id)
        meta['rows'] = count
        meta['duration'] = max(meta.get('duration', 0), duration or 0)
        if meta['status'] == 'recording' and not (live and live['id'] == recording_id and
                                                 (live['recording'] or live['closing'])):
            meta['status'] = 'interrupted'
        if live and live['id'] == recording_id:
            meta.update(dropped=live['dropped'], error=live['error'])
            if live['error']:
                meta['status'] = 'error'
        meta['recordingKind'] = 'scanner' if meta.get('schema') == SCANNER_SCHEMA else 'native'
        return meta

    def catalog(self):
        entries = []
        for p in sorted(self.root.glob('*.sqlite3'), key=lambda x: x.stat().st_mtime, reverse=True)[:40]:
            try:
                entries.append(self.metadata(p.stem))
            except (sqlite3.Error, ValueError, FileNotFoundError):
                entries.append(dict(id=p.stem, status='unreadable', rows=0, duration=0))
        return entries

    def window(self, recording_id, end, seconds=30):
        if not 1 <= seconds <= 120 or not 0 <= end <= 1e8:
            raise ValueError('再生範囲が不正です')
        meta = self.metadata(recording_id)
        if meta.get('schema') == SCANNER_SCHEMA:
            with closing(self.connect(recording_id)) as db:
                return chart_window(db,meta,end,seconds)
        end = min(end, meta['duration'])
        start = max(0, end - seconds)
        with closing(self.connect(recording_id)) as db:
            records = [json.loads(row[0]) for row in db.execute(
                'SELECT data FROM rows WHERE t>=? AND t<=? ORDER BY seq LIMIT 12001', (start, end))]
        return dict(metadata=meta, start=start, end=end, records=records[:12000],
                    truncated=len(records) > 12000)

    def export(self, recording_id):
        meta = self.metadata(recording_id)
        yield json.dumps(dict(kind='metadata', data=meta), ensure_ascii=False) + '\n'
        with closing(self.connect(recording_id)) as db:
            if meta.get('schema') == SCANNER_SCHEMA:
                cutoff = db.execute('SELECT max(record_seq) FROM frames').fetchone()[0] or 0
                for row in db.execute('SELECT record_seq,t,receivedUnix,kind,payload FROM frames WHERE record_seq<=? ORDER BY record_seq',(cutoff,)):
                    yield json.dumps(encoded_frame(row),ensure_ascii=False)+'\n'
                for row in db.execute('SELECT data FROM events ORDER BY seq'):
                    yield json.dumps(dict(kind='archive_event',data=json.loads(row[0])))+'\n'
            else:
                for row in db.execute('SELECT data FROM rows ORDER BY seq'):
                    yield row[0] + '\n'
