import json
from contextlib import closing
import sqlite3
import tempfile
import threading
import unittest
from unittest.mock import patch
from recording import Recordings, Writer
from protocol import decode_rh_stats
from server import Monitor


class RecordingTests(unittest.TestCase):
    def test_archive_context_flush_window_export_and_restart(self):
        with tempfile.TemporaryDirectory() as root:
            archive = Recordings(root)
            context = dict(config=dict(channels=['E2', 'E1', 'F3', 'R8']), firmware='test')
            rid = archive.start(context)
            origin = archive.status()['startMonotonic']
            context['config']['channels'][0] = 'R1'
            for slot in range(4):
                archive.append('sample', dict(rawHex='000102', clockBeforeMs=100), slot, origin + slot + 1)
            with self.assertRaises(ValueError):
                archive.start({})
            archive.stop('operator_stopped')
            archive.shutdown()
            restored = Recordings(root)
            meta = restored.metadata(rid)
            self.assertEqual(meta['status'], 'complete')
            self.assertEqual(meta['rows'], 4)
            self.assertEqual(meta['context']['config']['channels'][0], 'E2')
            window = restored.window(rid, 4, 2)
            self.assertEqual([r['slot'] for r in window['records']], [1, 2, 3])
            exported = [json.loads(s) for s in restored.export(rid)]
            self.assertEqual(exported[1]['data']['rawHex'], '000102')
            self.assertEqual(len(exported), 5)
            for bad in ['../data', 'A'*16, '', None]:
                with self.assertRaises(ValueError):
                    restored.metadata(bad)
            for end in [float('nan'), -1, float('inf')]:
                with self.assertRaises(ValueError):
                    restored.window(rid, end)

    def test_queue_overflow_is_explicit_and_not_complete(self):
        with tempfile.TemporaryDirectory() as root:
            gate = threading.Event()
            original = Writer.run
            def delayed(writer):
                gate.wait(2)
                original(writer)
            with patch.object(Writer, 'run', delayed):
                writer = Writer(root, {}, capacity=1)
                self.assertTrue(writer.append('sample', {}, 0))
                self.assertFalse(writer.append('sample', {}, 0))
                writer.stop('test')
                gate.set()
                writer.thread.join(3)
            meta = Recordings(root).metadata(writer.id)
            self.assertEqual(meta['status'], 'incomplete')
            self.assertEqual(meta['dropped'], 1)
            self.assertEqual(meta['rows'], 1)

    def test_unclosed_archive_reported_interrupted_and_readonly(self):
        with tempfile.TemporaryDirectory() as root:
            a = Recordings(root)
            rid = a.start({})
            a.shutdown()
            path = a.root / (rid + '.sqlite3')
            with closing(sqlite3.connect(path)) as db:
                row = json.loads(db.execute('SELECT data FROM metadata').fetchone()[0])
                row['status'] = 'recording'
                db.execute('UPDATE metadata SET data=?', (json.dumps(row),))
                db.commit()
            self.assertEqual(Recordings(root).metadata(rid)['status'], 'interrupted')
            db = a.connect(rid)
            try:
                with self.assertRaises(sqlite3.OperationalError):
                    db.execute('DELETE FROM rows')
            finally:
                db.close()

    def test_monitor_arm_stop_retains_settings(self):
        with tempfile.TemporaryDirectory() as root:
            m = Monitor(root, expected_serial='00:00:00:00:00:01')
            m.mode = 'timing'
            m.apply(None, None, dict(action='arm'))
            self.assertTrue(m.state['timingArmed'])
            rid = m.state['session']
            m.apply(None, None, dict(action='disarm'))
            m.recordings.shutdown()
            self.assertFalse(m.state['timingArmed'])
            self.assertEqual(m.recordings.metadata(rid)['context']['config'], m.config)

    def test_native_byte_order_clock_wrap_and_invalid_extrema(self):
        b = bytes([255, 1, 2, 70, 100, 99, 3, 4, 3, 20, 10, 80, 1, 44, 0, 100])
        p = decode_rh_stats(b, 10, .002, 0xfffffffe, 2)
        self.assertEqual(p['ageMs'], 258)
        self.assertEqual(p['loopUs'], 772)
        self.assertEqual(p['clockWindowMs'], 4)
        self.assertAlmostEqual(p['extremumFirstHost'], 9.7)
        self.assertAlmostEqual(p['extremumLastHost'], 9.8)
        self.assertTrue(p['extremumValid'])
        for raw in [b[:12]+b'\xff\xff'+b[14:], b[:14]+b'\x02\x00']:
            self.assertFalse(decode_rh_stats(raw, 10, .002, 1, 2)['extremumValid'])
        self.assertFalse(decode_rh_stats(b, 10, .002, 1, 102)['clockBracketValid'])
        with self.assertRaises(ValueError):
            decode_rh_stats(b[:-1], 10, .002, 1, 2)


if __name__ == '__main__':
    unittest.main()
