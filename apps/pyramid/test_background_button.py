import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from background_button import ButtonCapture, TOP_COLORS, button_edge
from server import Monitor


LIVE = dict(mode='monitor', connection='live', fresh=True, epoch=3, recordingActive=False)


class GestureTests(unittest.TestCase):
    def setUp(self):
        self.capture = ButtonCapture()

    def hold(self, at, duration=1, state=LIVE):
        self.capture.update(state, at)
        self.capture.edge(True, at, state)
        return self.capture.edge(False, at + duration, state)

    def test_filters_vendor_events_and_duplicate_edges(self):
        for key in ('value', 'vale'):
            for code in (0, 1):
                edge = button_edge(json.dumps(dict(work_id='buttons_thread', data={key: 204, 'code': code})))
                self.assertIs(edge, code == 0)
        for message in ('not json', '[]', '{"data":{"code":1,"value":204}}',
                        '{"work_id":"buttons_thread","data":{"code":2,"value":204}}',
                        '{"work_id":"buttons_thread","data":{"code":1,"value":1}}'):
            self.assertIsNone(button_edge(message))
        self.assertIsNone(self.capture.edge(False, 0, LIVE))
        self.capture.edge(True, 1, LIVE)
        self.capture.edge(True, 1.7, LIVE)
        self.assertIsNone(self.capture.edge(False, 2, LIVE))
        self.assertIsNone(self.capture.edge(False, 2.1, LIVE))
        self.assertEqual(self.capture.pixels(LIVE), [TOP_COLORS['confirm']] * 11)

    def test_pyramid_pro_trace_ignores_short_press_and_arms_after_hold_release(self):
        # Two KEY204 pairs measured on the Pyramid Pro: 0.32 s and 2.23 s.
        for at, code in ((0, 0), (.32, 1), (3.11, 0), (5.34, 1)):
            event = json.dumps(dict(work_id='buttons_thread', data={'vale': 204, 'code': code}))
            self.assertIsNone(self.capture.edge(button_edge(event), at, LIVE))
            if at < 5.34:
                self.assertIsNone(self.capture.armed)
        self.assertEqual(self.capture.pixels(LIVE), [TOP_COLORS['confirm']] * 11)

    def test_confirmation_only_allows_two_deliberate_holds_in_same_session(self):
        self.assertIsNone(self.hold(1, .2))
        self.assertIsNone(self.hold(2, 3))
        self.assertEqual(self.capture.pixels(LIVE), [TOP_COLORS['ready']])
        self.assertIsNone(self.hold(6))
        self.assertIsNone(self.hold(20))
        moved = dict(LIVE, epoch=4)
        self.assertIsNone(self.hold(24, state=moved))
        for state in (dict(moved, recordingActive=True), dict(moved, recording={'closing': True}),
                      dict(moved, fresh=False), dict(moved, mode='timing')):
            self.assertIsNone(self.hold(26, state=state))
            self.assertIsNone(self.capture.armed)
        self.assertIsNone(self.hold(28, state=moved))
        self.assertEqual(self.hold(30, state=moved)['epoch'], 4)
        self.assertIsNone(self.hold(32, state=moved))

    def test_progress_and_result_correlate_with_the_requested_capture(self):
        self.hold(1)
        ticket = self.hold(4)
        state = dict(LIVE, mode='background', backgroundProgress=60)
        self.capture.update(state, 6)
        self.assertEqual(self.capture.pixels(state), [TOP_COLORS['running']] * 7)
        self.capture.update(dict(LIVE, backgroundCapture={'id': 'other', 'status': 'saved'}), 7)
        self.assertIsNotNone(self.capture.pending)
        self.capture.update(dict(LIVE, backgroundCapture={'id': ticket['id'], 'status': 'saved'}), 8)
        self.assertEqual(self.capture.pixels(LIVE), [TOP_COLORS['saved']] * 11)
        self.capture.update(LIVE, 11)
        self.assertEqual(self.capture.pixels(LIVE), [TOP_COLORS['ready']])
        self.hold(12)
        ticket = self.hold(14)
        self.capture.update(dict(LIVE, backgroundCapture={'id': ticket['id'], 'status': 'failed'}), 16)
        self.assertEqual(self.capture.pixels(LIVE), [TOP_COLORS['failed']] * 11)


class BackgroundCommandTests(unittest.TestCase):
    def setUp(self):
        root = tempfile.TemporaryDirectory()
        self.addCleanup(root.cleanup)
        self.monitor = Monitor(Path(root.name), '00:00:00:00:00:01')
        self.monitor.state['connection'] = 'live'
        self.monitor.last_sample = time.monotonic()
        self.ticket = dict(id='1234567890abcdef', epoch=self.monitor.stream_epoch)
        self.command = dict(action='background', allOffConfirmed=True, button=self.ticket)

    def test_request_and_worker_check_the_recording_state_and_epoch(self):
        monitor = self.monitor
        self.assertRaises(ValueError, monitor.request, dict(self.command, allOffConfirmed=False))
        for mode in ('timing', 'spectrum'):
            monitor.mode = mode
            self.assertRaises(ValueError, monitor.request, self.command)
        monitor.mode = 'monitor'
        monitor.state['timingArmed'] = True
        self.assertRaises(ValueError, monitor.request, self.command)
        monitor.state['timingArmed'] = False
        with patch.object(monitor.recordings, 'status', return_value={'closing': True}):
            self.assertRaises(ValueError, monitor.request, self.command)
        monitor.request(self.command)
        monitor.stream_epoch += 1
        self.assertTrue(monitor.apply_safe(None, None, monitor.next_command()))
        self.assertEqual(monitor.mode, 'monitor')
        self.assertEqual(monitor.state['backgroundCapture'], dict(id=self.ticket['id'], status='failed'))

    def test_existing_background_survives_missing_samples(self):
        monitor = self.monitor
        monitor.request(self.command)
        with patch.object(monitor, 'command'), patch.object(monitor, 'pump'):
            self.assertFalse(monitor.apply_safe(None, None, monitor.next_command()))
        path = monitor.root / 'background.json'
        path.write_bytes(b'prior background')
        with patch.object(monitor, 'spectrum', return_value={'values': [None]}):
            self.assertRaises(OSError, monitor.background_loop, None, None)
        self.assertEqual(path.read_bytes(), b'prior background')
        self.assertEqual(monitor.mode, 'monitor')
        self.assertEqual(monitor.state['backgroundCapture'], dict(id=self.ticket['id'], status='failed'))

    def test_success_uses_existing_background_capture_and_records_result(self):
        monitor = self.monitor
        monitor.request(self.command)
        with patch.object(monitor, 'command'), patch.object(monitor, 'pump'):
            self.assertFalse(monitor.apply_safe(None, None, monitor.next_command()))
        with patch.object(monitor, 'spectrum', return_value={'values': [-4.5] * 137}) as sweep:
            monitor.background_loop(None, None)
        self.assertEqual(sweep.call_count, 5)
        saved = json.loads((monitor.root / 'background.json').read_text())
        self.assertEqual(saved['values'], [-4.5] * 137)
        self.assertTrue(monitor.background_active)
        self.assertEqual(monitor.state['backgroundCapture'], dict(id=self.ticket['id'], status='saved'))
        self.assertEqual(monitor.mode, 'monitor')


if __name__ == '__main__':
    unittest.main()
