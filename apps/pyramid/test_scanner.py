import base64
from contextlib import closing
import hashlib
import json
from pathlib import Path
import secrets
import sqlite3
import struct
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from protocol import Frames,crc16
from scanner_recording import Counters,ScannerWriter,SCHEMA
from recording import Recordings
from labels import Labels
from dataset import export_dataset
from server import Monitor,PRESETS


def payload(seq=0,drops=0,clock=1000,flags=0,gain=30,clips=0,raw=-300):
    return struct.pack('<HBBII',seq,4,0,drops,1000)+b''.join(
        struct.pack('<IhhHBBBB',clock,raw,-310,f,gain,clips,flags,0)
        for f in [5685,5705,5780,5820])


def context():
    return dict(config=dict(channels=['E2','E1','F3','F5']),frequencies=[5685,5705,5780,5820],
                firmware='test',purpose='software_test')


def label(source='replay_marker',**overrides):
    c=dict(labelId=secrets.token_hex(16),operation='add',slot=0,label='pass',tStart=0,tEnd=0,
           timeSource=source,method={'replay_marker':'human_replay','live_button':'human_live','video_review':'human_video_review'}[source],
           viewedRfWhileLabeling=True,uncertaintySec=1,operator='test',note='synthetic test')
    c.update(overrides)
    return c


class ScannerTests(unittest.TestCase):
    def test_database_initialization_does_not_block_receiver(self):
        with tempfile.TemporaryDirectory() as root:
            gate=threading.Event();original=ScannerWriter.initialize_db
            def delayed(w,db):gate.wait(2);original(w,db)
            with patch.object(ScannerWriter,'initialize_db',delayed):
                started=time.monotonic()
                w=ScannerWriter(root,context(),disk_min_bytes=0)
                self.assertFalse(w.ready.is_set())
                self.assertTrue(w.append_frame('R',payload()))
                w.stop('test');gate.set();w.thread.join(3)
            self.assertEqual(Recordings(root).metadata(w.id)['rows'],1)

    def test_blob_roundtrip_all_kinds_and_flagged_samples(self):
        with tempfile.TemporaryDirectory() as root:
            a=Recordings(root);rid=a.start(context(),'scanner')
            packets=[('R',payload(65535,clock=1)),('R',payload(0,clock=2,flags=3,gain=29,clips=4)),('T',b'\0status\xff'),('X',b'\x01\xff')]
            for kind,p in packets:a.append_frame(kind,p)
            a.shutdown()
            with closing(a.connect(rid)) as db:
                rows=db.execute('SELECT kind,payload FROM frames ORDER BY record_seq').fetchall()
            self.assertEqual(rows,packets)
            self.assertEqual(a.metadata(rid)['counters']['seqGaps'],0)
            exported=[json.loads(s) for s in a.export(rid)]
            frames=[r for r in exported if r['kind']=='frame']
            self.assertEqual([base64.b64decode(r['payloadBase64']) for r in frames],[p for k,p in packets])
            chart=a.window(rid,10,30)
            self.assertEqual(sum(b['count'] for b in chart['series'][0]),2)
            self.assertEqual(sum(b['invalid'] for b in chart['series'][0]),1)
            samples=[r for r in export_dataset(a,Labels(Path(root)/'labels',a,PRESETS),rid) if r['kind']=='scanner_frame']
            self.assertFalse(samples[1]['samples'][0]['validForPowerComparison'])
            self.assertEqual(samples[1]['samples'][0]['gain'],29)

    def test_counter_wrap_gap_duplicate_and_reset_separate(self):
        c=Counters()
        for seq,drops,clock in [(65535,0xfffffffe,0xfffffffe),(0,1,2),(3,3,3),(3,3,4),(0,0,1)]:
            c.observe('R',payload(seq,drops,clock))
        m=c.metrics
        self.assertEqual(m['seqGaps'],2)
        self.assertEqual(m['c5DropsIncrease'],5)
        self.assertEqual(m['clockWraps'],1)
        self.assertEqual(m['duplicates'],1)
        self.assertEqual(m['counterResets'],1)

    def test_overflow_stops_recording_without_blocking_producer(self):
        with tempfile.TemporaryDirectory() as root:
            gate=threading.Event();original=ScannerWriter.run
            def delayed(w):gate.wait(2);original(w)
            with patch.object(ScannerWriter,'run',delayed):
                w=ScannerWriter(root,context(),capacity=1,disk_min_bytes=0)
                self.assertTrue(w.append_frame('R',payload()))
                self.assertFalse(w.append_frame('R',payload(1)))
                self.assertEqual(w.status()['reason'],'queue_overflow')
                gate.set();w.thread.join(3)
            a=Recordings(root)
            self.assertEqual(a.metadata(w.id)['status'],'incomplete')
            self.assertEqual(a.metadata(w.id)['dropped'],1)

    def test_disk_low_flushes_and_records_stop_reason(self):
        with tempfile.TemporaryDirectory() as root:
            free=[1000]
            w=ScannerWriter(root,context(),disk_min_bytes=100,disk_free=lambda:free[0])
            w.append_frame('R',payload())
            free[0]=0
            w.thread.join(3)
            self.assertFalse(w.thread.is_alive())
            self.assertEqual(Recordings(root).metadata(w.id)['reason'],'disk_low')
            with self.assertRaises(ValueError):ScannerWriter(root,context(),disk_min_bytes=100,disk_free=lambda:0)

    def test_monitor_hook_precedes_display_filter_and_preserves_unknown_kind(self):
        def wire(kind,p):
            body=kind.encode()+struct.pack('<H',len(p))+p
            return b'\xa5\x5a'+body+struct.pack('<H',crc16(body))
        class Port:
            def __init__(self,data):self.data=data
            @property
            def in_waiting(self):return len(self.data)
            def read(self,n):r=self.data[:n];self.data=self.data[n:];return r
        with tempfile.TemporaryDirectory() as root:
            m=Monitor(root,expected_serial='00:00:00:00:00:01')
            m.mode='monitor';m.state['connection']='live'
            m.apply(None,None,dict(action='arm',purpose='software_test'))
            rid=m.state['session']
            p=payload(flags=1,gain=29,clips=4)
            bad=bytearray(wire('R',p));bad[-1]^=1
            m.pump(Port(bytes(bad)+wire('R',p)+wire('R',b'malformed')+wire('X',b'keep')),Frames(),.01,lambda r:None)
            with self.assertRaises(ValueError):m.request(dict(action='mode',mode='timing'))
            with self.assertRaises(ValueError):m.apply(None,None,dict(action='configure',config=m.config))
            m.close_recording('test');m.recordings.shutdown()
            with closing(m.recordings.connect(rid)) as db:
                self.assertEqual(db.execute('SELECT payload FROM frames ORDER BY record_seq').fetchall(),[(p,),(b'malformed',),(b'keep',)])
            self.assertEqual(m.recordings.metadata(rid)['counters']['crcErrorsIncrease'],1)
            self.assertEqual(m.recordings.metadata(rid)['counters']['malformed'],1)
            self.assertEqual(m.state['malformedFrames'],1)


class LabelTests(unittest.TestCase):
    def fixture(self,root,kind='scanner'):
        a=Recordings(Path(root)/'recordings');rid=a.start(context(),kind)
        if kind=='scanner':a.append_frame('R',payload())
        else:a.append('pass',dict(accepted=True))
        a.shutdown()
        return a,rid,Labels(Path(root)/'labels',a,PRESETS)

    def test_append_replace_retract_idempotency_and_source_unchanged(self):
        for kind in ['scanner','native']:
            with self.subTest(kind=kind),tempfile.TemporaryDirectory() as root:
                a,rid,labels=self.fixture(root,kind)
                p=a.root/(rid+'.sqlite3');before=hashlib.sha256(p.read_bytes()).hexdigest()
                self.assertEqual(labels.read(rid)['active'],[])
                c=label();first=labels.append(rid,c)
                self.assertEqual(labels.append(rid,c)['labelId'],first['labelId'])
                correction=labels.append(rid,label(operation='replace',supersedes=first['labelId'],label='no_pass'))
                labels.append(rid,label(operation='retract',supersedes=correction['labelId']))
                ledger=labels.read(rid)
                self.assertEqual(len(ledger['rows']),3);self.assertEqual(ledger['active'],[])
                self.assertEqual(hashlib.sha256(p.read_bytes()).hexdigest(),before)
                with closing(sqlite3.connect(labels.path(rid))) as db:
                    with self.assertRaises(sqlite3.IntegrityError):db.execute('DELETE FROM labels')
                with self.assertRaises(ValueError):labels.append(rid,label(operation='replace',supersedes=first['labelId']))

    def test_live_uses_server_clock_and_training_export_keeps_uncertainty(self):
        with tempfile.TemporaryDirectory() as root:
            a=Recordings(Path(root)/'recordings');ctx=context();ctx['purpose']='observation'
            rid=a.start(ctx,'scanner');origin=a.status()['startMonotonic']
            labels=Labels(Path(root)/'labels',a,PRESETS)
            row=labels.append(rid,label('live_button',viewedRfWhileLabeling=False),origin+.1)
            self.assertAlmostEqual(row['t'],.1)
            corrected=labels.append(rid,label('live_button',operation='replace',supersedes=row['labelId'],viewedRfWhileLabeling=False),origin+1)
            self.assertAlmostEqual(corrected['t'],.1)
            a.append_frame('R',payload(),at=origin+.2);a.shutdown()
            exported=list(export_dataset(a,labels,rid))
            historical=next(r for r in exported if r['kind']=='human_label' and r['data']['labelId']==row['labelId'])
            self.assertFalse(historical['independentlyReported'])
            human=next(r for r in exported if r['kind']=='human_label' and r['data']['labelId']==corrected['labelId'])
            self.assertTrue(human['independentlyReported'])
            self.assertFalse(human['evaluationGroundTruthVerified'])
            self.assertEqual(human['data']['uncertaintySec'],1)
            with self.assertRaises(ValueError):labels.append(rid,label('live_button'))
            with self.assertRaises(ValueError):labels.append(rid,label(tStart=-1))
            with self.assertRaises(ValueError):labels.append(rid,label(viewedRfWhileLabeling=None))
            with self.assertRaises(ValueError):labels.append('../bad',label())


if __name__=='__main__':unittest.main()
