#!/usr/bin/env python3
"""Optional Pyramid Pro LED meter. The C5 service owns the receiver."""
import argparse
import json
import logging
import math
import os
import signal
import subprocess
import threading
import time
import urllib.error
import urllib.request

from background_button import ButtonCapture, ButtonEvents, ORIGIN, TOP_INDICES, request_capture

STATE_URL = ORIGIN + '/api/state'
PIXELS = 48
# EC firmware uses indices 0–11 for the top and four 9-LED strips below.
STRIP_INDICES = range(12, PIXELS)
COLORS = (0x300000, 0x003000, 0x000030, 0x303000)  # Red, green, blue, yellow in slot order.
UPDATE_SECONDS = .25
MAX_WRITES = 12
LOG = logging.getLogger('c5-led')


def parse_bars(value):
    bars = json.loads(value)
    if not isinstance(bars, list) or len(bars) != 4 or any(
            not isinstance(bar, list) or len(bar) != 9 for bar in bars):
        raise ValueError('Specify four ordered arrays of 9 LED indices')
    indices = [index for bar in bars for index in bar]
    if any(type(index) is not int for index in indices) or set(indices) != set(STRIP_INDICES):
        raise ValueError('Each strip LED index from 12 through 47 must occur exactly once')
    physical_strips = {frozenset(range(start, start + 9)) for start in range(12, PIXELS, 9)}
    if {frozenset(bar) for bar in bars} != physical_strips:
        raise ValueError('Each bar must contain exactly one physical strip')
    return bars


def target_pixels(state, bars, low, high, top=()):
    live = state.get('mode') == 'monitor' and state.get('connection') == 'live' and state.get('fresh')
    levels = state.get('levels') if live else None
    if not isinstance(levels, list) or len(levels) != 4:
        if not top:
            return None
        levels = ()
    target = [0] * PIXELS
    for index, color in zip(TOP_INDICES, top):
        target[index] = color
    for level, bar, color in zip(levels, bars, COLORS):
        if type(level) not in (float, int) or not math.isfinite(level):
            continue
        fraction = max(0, min(1, (level - low) / (high - low)))
        count = int(fraction * len(bar) + .5)
        for index in bar[:count]:
            target[index] = color
    return target


class EcLeds:
    def call(self, option, data=None):
        args = ['/usr/bin/ec_cli', 'device', option]
        if data is not None:
            args.extend(('-d', json.dumps(data, separators=(',', ':'))))
        result = subprocess.run(args, check=True, capture_output=True, text=True, timeout=.5)
        return json.loads(result.stdout)['data']

    def mode(self):
        mode = self.call('--rgb')
        if type(mode) is not int or not 0 <= mode <= 5:
            raise ValueError('Unexpected EC RGB mode')
        return mode

    def set_mode(self, mode):
        if self.call('--rgb', mode) != 'ok' or self.mode() != mode:
            raise OSError('EC did not apply RGB mode')

    def set_pixel(self, index, color):
        if self.call('--rgb_set_color', {'rgb_index': index, 'rgb_color': color}) != 'ok':
            raise OSError(f'EC rejected LED {index}')


class Meter:
    def __init__(self, ec, bars):
        self.ec = ec
        self.indices = [*TOP_INDICES, len(TOP_INDICES)] + [index for bar in bars for index in bar]
        self.previous_mode = None
        self.yielded = False
        self.current = [None] * PIXELS

    def release(self):
        if self.previous_mode is not None:
            if self.ec.mode() == 0:
                self.ec.set_mode(self.previous_mode)
                LOG.info('Restored EC RGB mode %d', self.previous_mode)
            self.previous_mode = None

    def update(self, target):
        if target is None:
            self.release()
            self.yielded = False
            return
        if self.yielded:
            return
        mode = self.ec.mode()
        if self.previous_mode is None:
            if mode == 0:
                self.yielded = True
                LOG.warning('EC is already in manual RGB mode. Meter waits for a new monitor session')
                return
            if self.ec.call('--rgb_size') != PIXELS:
                raise ValueError('EC LED count does not match Pyramid Pro')
            self.previous_mode = mode
            self.ec.set_mode(0)
            self.current = [None] * PIXELS
            LOG.info('EC RGB meter active, prior mode %d', mode)
        elif mode != 0:
            self.previous_mode = None
            self.yielded = True
            LOG.warning('Another application changed the EC RGB mode. Yielding until the next monitor session')
            return
        writes = 0
        for index in self.indices:
            color = target[index]
            if self.current[index] != color:
                self.ec.set_pixel(index, color)
                self.current[index] = color
                writes += 1
                if writes >= MAX_WRITES:
                    break


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bars', default=os.environ.get('C5_LED_BARS', ''))
    parser.add_argument('--low', type=float, default=os.environ.get('C5_LED_LOW', '-6'))
    parser.add_argument('--high', type=float, default=os.environ.get('C5_LED_HIGH', '21'))
    args = parser.parse_args()
    try:
        bars = parse_bars(args.bars)
    except ValueError as error:
        parser.error(str(error))
    if not all(map(math.isfinite, (args.low, args.high))) or args.low >= args.high:
        parser.error('LED scale must use finite receiver dB limits, low < high')
    logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')
    stop = threading.Event()
    for sig in (signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: stop.set())
    meter = Meter(EcLeds(), bars)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    failures = (OSError, ValueError, KeyError, subprocess.SubprocessError)
    capture = ButtonCapture()
    buttons = None
    retry_at = 0
    try:
        while not stop.is_set():
            now = time.monotonic()
            if buttons and buttons.process.poll() is not None:
                buttons.close()
                buttons = None
                capture.cancel_confirmation()
                retry_at = now + 2
                LOG.warning('EC button event stream exited')
            if buttons is None and now >= retry_at:
                try:
                    buttons = ButtonEvents()
                    LOG.info('Listening for EC top button')
                except OSError as error:
                    retry_at = now + 2
                    LOG.warning('Could not subscribe to EC button events: %s', error)
            try:
                with opener.open(STATE_URL, timeout=.5) as response:
                    state = json.load(response)
            except (OSError, ValueError):
                state = {}
            now = time.monotonic()
            capture.update(state, now)
            if buttons:
                for pressed, at in buttons.drain():
                    if now - at > 2 or meter.previous_mode is None:
                        capture.cancel_confirmation()
                        continue
                    ticket = capture.edge(pressed, at, state)
                    if ticket:
                        try:
                            request_capture(opener, ticket)
                            LOG.info('Top button background capture queued')
                        except (urllib.error.HTTPError, ValueError) as error:
                            LOG.warning('Top button background capture rejected: %s', error)
                            capture.finish('failed', time.monotonic())
                        except OSError as error:
                            LOG.warning('Top button background response unavailable: %s', error)
            top = capture.pixels(state, button_available=buttons is not None)
            try:
                meter.update(target_pixels(state, bars, args.low, args.high, top))
                stop.wait(UPDATE_SECONDS)
            except failures as error:
                capture.cancel_confirmation()
                LOG.warning('EC meter update failed: %s', error)
                stop.wait(2)
    finally:
        try:
            if buttons:
                buttons.close()
        finally:
            try:
                meter.release()
            except failures as error:
                LOG.error('Could not restore previous EC RGB mode: %s', error)


if __name__ == '__main__':
    main()
