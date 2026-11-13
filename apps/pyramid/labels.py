"""Human annotation ledger. Never alters source archives or derives labels from RF events."""
from contextlib import closing
import hashlib
import json
import math
from pathlib import Path
import re
import sqlite3
import threading
import time

METHODS = {'live_button':'human_live','replay_marker':'human_replay','video_review':'human_video_review'}


class Labels:
    def __init__(self, root, recordings, presets):
        self.root = Path(root)
        self.recordings = recordings
        self.presets = presets
        self.lock = threading.RLock()

    def path(self, recording_id):
        if not isinstance(recording_id,str) or not re.fullmatch('[a-f0-9]{16}',recording_id):
            raise ValueError('記録IDが不正です')
        return self.root/(recording_id+'.labels.sqlite3')

    def read(self, recording_id):
        self.recordings.metadata(recording_id)
        p = self.path(recording_id)
        if not p.exists():
            return dict(revision=0,rows=[],active=[])
        with closing(sqlite3.connect(p.resolve().as_uri()+'?mode=ro',uri=True)) as db:
            rows = [json.loads(r[0]) for r in db.execute('SELECT data FROM labels ORDER BY seq')]
        superseded = {r['supersedes'] for r in rows if r.get('supersedes')}
        active = [r for r in rows if r['labelId'] not in superseded and r['operation']!='retract']
        return dict(revision=len(rows),rows=rows,active=active)

    def append(self, recording_id, command, received_monotonic=None):
        if not isinstance(command,dict):
            raise ValueError('ラベルはJSONオブジェクトで指定してください')
        meta = self.recordings.metadata(recording_id)
        identity = command.get('labelId')
        if not isinstance(identity,str) or not re.fullmatch('[a-f0-9]{32}',identity):
            raise ValueError('ラベルIDは32桁の小文字hexです')
        operation = command.get('operation','add')
        if operation not in ['add','replace','retract']:
            raise ValueError('ラベル操作が不正です')
        if command.get('label') not in ['pass','no_pass','unsure'] and operation!='retract':
            raise ValueError('ラベルは通過/非通過/不明から選んでください')
        source = command.get('timeSource')
        if source not in METHODS or command.get('method') != METHODS[source]:
            raise ValueError('確認方法と時刻の出典が一致しません')
        if not isinstance(command.get('viewedRfWhileLabeling'),bool):
            raise ValueError('RF波形を見て教示したか指定してください')
        uncertainty = command.get('uncertaintySec')
        if isinstance(uncertainty,bool) or not isinstance(uncertainty,(int,float)) or not math.isfinite(uncertainty) or not 0 <= uncertainty <= 3600:
            raise ValueError('時刻の不確かさを0〜3600秒で指定してください')
        slot = command.get('slot')
        channels = meta['context']['config']['channels']
        if type(slot) is not int or not 0 <= slot < len(channels):
            raise ValueError('対象枠が不正です')
        supersedes = command.get('supersedes')
        if (operation=='add' and supersedes) or (operation!='add' and not supersedes):
            raise ValueError('訂正/取消は対象のラベルIDが必要です')
        text = {}
        for name,limit in [('operator',100),('note',2000)]:
            value = command.get(name,'')
            if not isinstance(value,str) or len(value)>limit:
                raise ValueError('記録者/メモが長すぎます')
            text[name] = value
        with self.lock:
            existing = self.read(recording_id)
            for previous in existing['rows']:
                if previous['labelId']==identity:
                    if previous.get('requestHash')==self.request_hash(command):
                        return previous  # Same request retry is idempotent.
                    raise ValueError('同じラベルIDの内容が異なります')
            old = next((r for r in existing['active'] if r['labelId']==supersedes),None)
            if operation!='add' and (not old or old['slot']!=slot):
                raise ValueError('対象ラベルは取消/訂正済み、または枠が一致しません')
            if source=='live_button':
                live = self.recordings.status(recording_id)
                if not live or not live['recording'] or self.recordings.current is not self.recordings.writers.get(recording_id):
                    raise ValueError('この記録は記録中ではありません。再生画面から指定してください')
                if operation in ['replace','retract']:
                    start,end=old['tStart'],old['tEnd']
                else:
                    start = (received_monotonic if received_monotonic is not None else time.monotonic())-meta['startMonotonic']
                    end = start
                if start < 0:
                    raise ValueError('記録開始前のラベルです')
            elif operation=='retract':
                start,end=old['tStart'],old['tEnd']
            else:
                start = command.get('tStart',command.get('t'))
                end = command.get('tEnd',start)
                if any(isinstance(t,bool) or not isinstance(t,(int,float)) or not math.isfinite(t) for t in [start,end]):
                    raise ValueError('記録内の時刻を指定してください')
                if not 0 <= start <= end <= meta['duration']:
                    raise ValueError('記録の範囲外です')
            if operation=='retract':
                start,end = old['tStart'],old['tEnd']
            label = dict(labelId=identity,recordingId=recording_id,recordingSchema=meta['schema'],
                         operation=operation,slot=slot,channel=channels[slot],mhz=self.presets[channels[slot]],
                         label=old['label'] if operation=='retract' else command['label'],
                         t=start,tStart=start,tEnd=end,timeSource=source,method=METHODS[source],
                         uncertaintySec=uncertainty,viewedRfWhileLabeling=command['viewedRfWhileLabeling'],
                         supersedes=supersedes,createdUnix=time.time(),requestHash=self.request_hash(command),
                         timeBasis='host monotonic session-relative; live time is server receipt, not browser click',
                         contextSha256=hashlib.sha256(json.dumps(meta['context'],sort_keys=True).encode()).hexdigest(),**text)
            self.root.mkdir(parents=True,exist_ok=True)
            with closing(sqlite3.connect(self.path(recording_id),timeout=3)) as db:
                db.executescript('PRAGMA synchronous=FULL;'
                    'CREATE TABLE IF NOT EXISTS labels(seq INTEGER PRIMARY KEY AUTOINCREMENT,label_id TEXT UNIQUE,data TEXT NOT NULL);'
                    "CREATE TRIGGER IF NOT EXISTS no_label_update BEFORE UPDATE ON labels BEGIN SELECT RAISE(ABORT,'append only'); END;"
                    "CREATE TRIGGER IF NOT EXISTS no_label_delete BEFORE DELETE ON labels BEGIN SELECT RAISE(ABORT,'append only'); END;")
                db.execute('INSERT INTO labels(label_id,data) VALUES(?,?)',(identity,json.dumps(label,ensure_ascii=False,allow_nan=False)))
                db.commit()
            return label

    @staticmethod
    def request_hash(command):
        return hashlib.sha256(json.dumps(command,sort_keys=True,allow_nan=False).encode()).hexdigest()
