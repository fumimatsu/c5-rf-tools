"""Pyramid Pro top-button confirmation for the existing background API."""
import json
import logging
import queue
import re
import secrets
import subprocess
import threading
import time
import urllib.request


LOG = logging.getLogger('c5-led')
ORIGIN = 'http://127.0.0.1:8765'
HOLD_MIN = .7
HOLD_MAX = 2.5
CONFIRM_SECONDS = 12
CAPTURE_SECONDS = 20
RESULT_SECONDS = 3

TOP_INDICES = range(11)
TOP_COLORS = {
    'ready': 0x000c00,
    'confirm': 0x302000,
    'running': 0x000030,
    'saved': 0x003000,
    'failed': 0x300000,
}


def can_capture(state):
    return (state.get('mode') == 'monitor' and state.get('connection') == 'live'
            and state.get('fresh') and type(state.get('epoch')) is int
            and not state.get('recordingActive') and not state.get('timingArmed')
            and not (state.get('recording') or {}).get('closing'))


def button_edge(line):
    try:
        event = json.loads(line)
    except (UnicodeError, ValueError):
        return None
    if not isinstance(event, dict) or event.get('work_id') != 'buttons_thread':
        return None
    data = event.get('data')
    if not isinstance(data, dict) or data.get('value', data.get('vale')) != 204:
        return None
    code = data.get('code')
    # The Pyramid Pro top button emits 0 on press and 1 on release.
    return code == 0 if type(code) is int and code in (0, 1) else None


class ButtonEvents:
    def __init__(self):
        self.events = queue.SimpleQueue()
        self.process = subprocess.Popen(['/usr/bin/ec_cli', 'echo', '--button'],
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            edge = button_edge(line)
            if edge is not None:
                self.events.put((edge, time.monotonic()))

    def drain(self):
        while True:
            try:
                yield self.events.get_nowait()
            except queue.Empty:
                return

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=1)
        self.reader.join(timeout=1)
        self.process.stdout.close()


class ButtonCapture:
    def __init__(self):
        self.down_at = None
        self.armed = None
        self.pending = None
        self.result = None

    def cancel_confirmation(self):
        self.down_at = None
        self.armed = None

    def update(self, state, now):
        if not can_capture(state):
            self.cancel_confirmation()
        if self.armed and (now > self.armed[1] or self.armed[0] != state.get('epoch')):
            self.cancel_confirmation()
        if self.result and now >= self.result[1]:
            self.result = None
        if self.pending:
            capture = state.get('backgroundCapture') or {}
            if capture.get('id') == self.pending[0] and capture.get('status') in ('saved', 'failed'):
                self.finish(capture['status'], now)
            elif now >= self.pending[1] or state.get('connection') in ('error', 'disconnected'):
                self.finish('failed', now)

    def finish(self, result, now):
        self.pending = None
        self.result = (result, now + RESULT_SECONDS)
        LOG.info('Top button background capture: %s', result)

    def edge(self, pressed, at, state):
        if (not can_capture(state) or self.pending or self.result):
            self.cancel_confirmation()
            return None
        if pressed:
            if self.down_at is None:
                self.down_at = at
            return None
        if self.down_at is None:
            return None
        held = at - self.down_at
        self.down_at = None
        if not HOLD_MIN <= held <= HOLD_MAX:
            return None
        epoch = state['epoch']
        if self.armed and self.armed[0] == epoch and at <= self.armed[1]:
            self.armed = None
            ticket = dict(id=secrets.token_hex(8), epoch=epoch)
            self.pending = (ticket['id'], at + CAPTURE_SECONDS)
            return ticket
        self.armed = (epoch, at + CONFIRM_SECONDS)
        LOG.info('Top button waiting for all-VTX-off confirmation')
        return None

    def pixels(self, state, button_available=True):
        if self.result:
            phase, count = self.result[0], len(TOP_INDICES)
        elif state.get('mode') == 'background' or self.pending:
            progress = state.get('backgroundProgress') or 0
            phase, count = 'running', max(1, min(len(TOP_INDICES), round(progress * len(TOP_INDICES) / 100)))
        elif self.armed:
            phase, count = 'confirm', len(TOP_INDICES)
        elif button_available and can_capture(state):
            phase, count = 'ready', 1
        else:
            return []
        return [TOP_COLORS[phase]] * count


def request_capture(opener, ticket):
    with opener.open(ORIGIN + '/', timeout=.7) as response:
        html = response.read().decode('utf-8')
    token = re.search(r'name="control-token" content="([A-Za-z0-9_-]+)"', html)
    if not token:
        raise ValueError('Background API control token is missing')
    request = urllib.request.Request(
        ORIGIN + '/api/control', json.dumps(dict(action='background', allOffConfirmed=True, button=ticket)).encode(),
        headers={'Content-Type': 'application/json', 'Origin': ORIGIN, 'X-Control-Token': token[1]})
    with opener.open(request, timeout=.7) as response:
        if response.status != 202:
            raise OSError('Background API did not queue capture')
