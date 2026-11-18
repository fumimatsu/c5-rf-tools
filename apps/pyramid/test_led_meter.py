import json
import unittest

from led_meter import COLORS, MAX_WRITES, Meter, parse_bars, target_pixels


BARS = [list(range(start, start + 9)) for start in range(12, 48, 9)]


class FakeEc:
    def __init__(self):
        self.rgb_mode = 1
        self.writes = []
        self.modes = []

    def mode(self):
        return self.rgb_mode

    def set_mode(self, mode):
        self.rgb_mode = mode
        self.modes.append(mode)

    def call(self, _option):
        return 48

    def set_pixel(self, index, color):
        self.writes.append((index, color))


class LedMeterTests(unittest.TestCase):
    def test_mapping_is_ordered_and_must_cover_every_pixel(self):
        ordered = [list(reversed(bar)) for bar in BARS]
        self.assertEqual(parse_bars(json.dumps(ordered)), ordered)
        with self.assertRaises(ValueError):
            parse_bars(json.dumps([BARS[0]] * 4))
        wrong = [bar.copy() for bar in BARS]
        wrong[0][0] = 0
        with self.assertRaises(ValueError):
            parse_bars(json.dumps(wrong))

    def test_length_follows_each_level_on_its_mapped_bar(self):
        ordered = [list(reversed(bar)) for bar in BARS]
        state = dict(mode='monitor', connection='live', fresh=True, levels=[None, -3.5, 9, 40])
        target = target_pixels(state, ordered, -6, 21)
        for i, (bar, expected) in enumerate(zip(ordered, (0, 1, 5, 9))):
            self.assertEqual([target[n] for n in bar], [COLORS[i]] * expected + [0] * (9-expected))
        top = [0x302000] * 11
        with_top = target_pixels(state, ordered, -6, 21, top)
        self.assertEqual(with_top[:12], top + [0])
        self.assertEqual(with_top[12:], target[12:])
        capture_only = target_pixels(dict(state, mode='background'), ordered, -6, 21, top)
        self.assertEqual(capture_only, top + [0] * 37)
        for mode in ('spectrum', 'timing', 'background'):
            self.assertIsNone(target_pixels(dict(state, mode=mode), BARS, -6, 24))
        self.assertIsNone(target_pixels(dict(state, fresh=False), BARS, -6, 24))

    def test_updates_are_bounded_and_restore_previous_mode(self):
        ec = FakeEc()
        meter = Meter(ec, BARS)
        target = [0] * 48
        for i in range(4):
            meter.update(target)
            self.assertEqual(len(ec.writes), (i + 1) * MAX_WRITES)
        self.assertEqual({index for index, _ in ec.writes}, set(range(48)))
        target[30] = COLORS[2]
        meter.update(target)
        meter.update(target)
        self.assertEqual(ec.writes[48:], [(30, COLORS[2])])
        meter.update(None)
        self.assertEqual(ec.modes, [0, 1])

    def test_another_owner_mode_is_preserved(self):
        ec = FakeEc()
        meter = Meter(ec, BARS)
        target = [0] * 48
        meter.update(target)
        ec.rgb_mode = 3
        meter.update(target)
        meter.update(target)
        meter.release()
        self.assertEqual(ec.mode(), 3)
        self.assertEqual(ec.modes, [0])


if __name__ == '__main__':
    unittest.main()
