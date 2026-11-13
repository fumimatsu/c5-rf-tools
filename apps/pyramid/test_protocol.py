import json
import struct
import tempfile
import unittest
from protocol import Frames, PassTracker, crc16, reading
from server import Monitor


class ProtocolTests(unittest.TestCase):
    def test_crc_and_fragmented_resync(self):
        self.assertEqual(crc16(b'123456789'), 0x906e)
        payload = struct.pack('<HBBII', 65535, 1, 0, 2, 900) + struct.pack('<IhhHBBBB', 123, -300, -310, 5917, 30, 0, 0, 0)
        body = b'R' + struct.pack('<H', len(payload)) + payload
        frame = b'\xa5\x5a' + body + struct.pack('<H', crc16(body))
        damaged = bytearray(frame)
        damaged[-1] ^= 1
        parser = Frames()
        result = []
        data = b'junk' + damaged + frame
        for byte in data:
            result.extend(parser.feed(bytes([byte])))
        self.assertEqual(parser.errors, 1)
        self.assertEqual(len(result), 1)
        self.assertEqual(reading(result[0][1])['pilots'][0]['db'], -3.1)
        with self.assertRaises(ValueError):
            reading(payload[:-1])

    def test_pass_wrap_baseline_gap_and_minimum(self):
        t = PassTracker()
        def p(counter, now, age=300):
            return dict(counter=counter, hostMid=now, ageMs=age, rtt=.002, peak=100)
        self.assertIsNone(t.update(0, p(255, 10)))
        self.assertTrue(t.update(0, p(0, 10.1))['accepted'])
        self.assertIsNone(t.update(0, p(0, 10.2)))
        self.assertEqual(t.update(0, p(2, 10.3))['reason'], 'counter_gap_or_late_poll')
        self.assertIsNone(t.last.get(0))
        self.assertTrue(t.update(0, p(3, 10.4))['accepted'])
        self.assertFalse(t.update(0, p(4, 10.8))['accepted'])
        self.assertFalse(t.update(0, p(5, 15))['accepted'])
        self.assertIsNone(t.last.get(0))

    def test_config_and_disconnected_controls(self):
        with tempfile.TemporaryDirectory() as root:
            m = Monitor(root, expected_serial='00:00:00:00:00:01')
            self.assertFalse(m.snapshot()['fresh'])
            with self.assertRaises(ValueError):
                m.request([])
            with self.assertRaises(ValueError):
                m.request({'action': 'arm'})
            bad = dict(m.config, channels=['R7', 'F8', 'R2', 'R3'])
            with self.assertRaises(ValueError):
                m.validate(bad)
            with self.assertRaises(ValueError):
                m.validate(dict(m.config, enter=[1, 1, 1, 1]))
            m.last_sample = 1
            m.levels = [12] * 4
            self.assertEqual(m.snapshot()['levels'], [None] * 4)

    def test_old_background_not_automatically_adopted(self):
        from pathlib import Path
        with tempfile.TemporaryDirectory() as root:
            bg = dict(serial='00:00:00:00:00:01', firmware='1.2.5+xiaoc5', settings='g30/o10/bw1/n256', values=[-3] * 137)
            (Path(root) / 'background.json').write_text(json.dumps(bg))
            m = Monitor(root, expected_serial='00:00:00:00:00:01')
            self.assertIsNotNone(m.background)
            self.assertIsNone(m.bg_values([5780]))
            m.background_active = True
            self.assertEqual(m.bg_values([5780]), [-3])


if __name__ == '__main__':
    unittest.main()
