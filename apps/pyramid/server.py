#!/usr/bin/env python3
"""Single-owner C5 receiver, LAN HTML display and native timing shadow log."""
import argparse
import copy
import json
import math
import os
from pathlib import Path
import queue
import secrets
import signal
import shutil
import socket
import sqlite3
import statistics
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, parse_qs

import serial
from serial.tools import list_ports
from protocol import Frames, RH, PassTracker, reading
from recording import Recordings
from labels import Labels

VERSION = '0.2.2'
SERIAL = os.environ.get('C5_SERIAL', '')
BANDS = {
    'R': [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917],
    'A': [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725],
    'B': [5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866],
    'E': [5705, 5685, 5665, 5645, 5885, 5905],
    'F': [5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880],
}
PRESETS = {f'{band}{i+1}': mhz for band, fs in BANDS.items() for i, mhz in enumerate(fs)}


def re_serial(value):
    import re
    return bool(isinstance(value, str) and re.fullmatch(r'(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}', value))


def power_mean(values):
    return 10 * math.log10(sum(10 ** (x / 10) for x in values) / len(values))


def atomic_json(path, data):
    tmp = path.with_suffix('.tmp')
    tmp.write_text(json.dumps(data, ensure_ascii=False, allow_nan=False, indent=2), encoding='utf-8')
    os.replace(tmp, path)


class Monitor:
    def __init__(self, data_dir, expected_serial=SERIAL, disk_min_bytes=256*1024**2):
        self.root = Path(data_dir)
        self.root.mkdir(parents=True, exist_ok=True)
        self.serial = expected_serial
        self.lock = threading.RLock()
        self.stop = threading.Event()
        self.commands = queue.Queue(maxsize=12)
        self.recordings = Recordings(self.root / 'recordings',disk_min_bytes)
        self.labels = Labels(self.root/'labels',self.recordings,PRESETS)
        self.config = {'channels': ['E2', 'E1', 'F3', 'F5'], 'step': 4,
                       'enter': [90] * 4, 'exit': [80] * 4, 'minimumMs': 2000}
        if (self.root / 'config.json').exists():
            self.config = self.validate(json.loads((self.root / 'config.json').read_text()))
        self.background = None
        self.background_active = False
        if (self.root / 'background.json').exists():
            bg = json.loads((self.root / 'background.json').read_text())
            if (bg.get('serial') == self.serial and bg.get('firmware') == '1.2.5+xiaoc5'
                    and bg.get('settings') == 'g30/o10/bw1/n256' and len(bg.get('values', [])) == 137
                    and all(isinstance(x, (int, float)) and math.isfinite(x) for x in bg['values'])):
                self.background = bg
        self.mode = 'monitor'
        self.state = dict(version=VERSION, connection='disconnected', message='C5の接続を待っています',
                          serial=self.serial, device=None, firmware=None, records=0, crcErrors=0,
                          seqGaps=0, chipDrops=0, rfErrors=0, malformedFrames=0, reconnects=0, sweep=None,
                          sweepProgress=0, backgroundProgress=None, timingArmed=False,
                          session=None, events=[], mqtt='未接続：端末内の試験記録のみ',
                          timingPolls=0, timingErrors=0, timingLate=0, timingGaps=0)
        self.levels = [None] * 4
        self.history = [deque(maxlen=300) for _ in range(4)]
        self.samples = [[] for _ in range(4)]
        self.native_stats = [None] * 4
        self.timing_history = [deque(maxlen=600) for _ in range(4)]
        self.timing_previous = {}
        self.sweeps = deque(maxlen=120)
        self.last_sample = 0
        self.last_emit = 0
        self.sweep_id = 0
        self.tracker = PassTracker()
        self.previous_seq = None
        self.chip_drop_base = None
        self.timing_seen = False
        self.stream_epoch = 0
        self.thread = threading.Thread(target=self.worker, daemon=True)

    @staticmethod
    def validate(c):
        if not isinstance(c, dict) or len(c.get('channels', [])) != 4:
            raise ValueError('4枠のチャンネルを選んでください')
        if any(x not in PRESETS for x in c['channels']):
            raise ValueError('未対応チャンネルです')
        if len({PRESETS[x] for x in c['channels']}) != 4:
            raise ValueError('同じ周波数を複数枠には指定できません')
        if c.get('step') not in [2, 4]:
            raise ValueError('周波数刻みは2または4MHzです')
        for key in ['enter', 'exit']:
            if len(c.get(key, [])) != 4 or any(type(x) is not int or not 1 <= x <= 254 for x in c[key]):
                raise ValueError('計測しきい値は1〜254で指定してください')
        if any(a <= b for a, b in zip(c['enter'], c['exit'])):
            raise ValueError('進入しきい値は退出より大きくしてください')
        if type(c.get('minimumMs')) is not int or not 500 <= c['minimumMs'] <= 600000:
            raise ValueError('最小通過間隔は500〜600000msです')
        return copy.deepcopy(c)

    def snapshot(self):
        with self.lock:
            d = copy.deepcopy(self.state)
            age = time.monotonic() - self.last_sample if self.last_sample else None
            now = time.monotonic()
            d.update(mode=self.mode, config=copy.deepcopy(self.config), presets=PRESETS,
                     levels=copy.deepcopy(self.levels), history=[list(x) for x in self.history],
                     nativeStats=copy.deepcopy(self.native_stats), nowMonotonic=now,
                     timingHistory=[[p for p in h if p[0] >= now - 30] for h in self.timing_history],
                     recording=self.recordings.status(), recordingActive=self.state['timingArmed'],
                     diskFreeBytes=shutil.disk_usage(self.root).free,
                     diskMinimumBytes=self.recordings.disk_min_bytes,
                     ageSeconds=round(age, 2) if age is not None else None,
                     fresh=bool(age is not None and age < 1 and self.state['connection'] == 'live'),
                     background={'saved': self.background is not None, 'active': self.background_active,
                                 'recordedAt': self.background.get('recordedAt') if self.background else None},
                     epoch=self.stream_epoch)
            if not d['fresh']:
                d['levels'] = [None] * 4
            return d

    def request(self, command):
        if not isinstance(command, dict):
            raise ValueError('操作はJSONオブジェクトで指定してください')
        action = command.get('action')
        if action == 'configure':
            command = dict(action=action, config=self.validate(command.get('config')))
        elif action == 'mode' and command.get('mode') in ['monitor', 'spectrum', 'timing']:
            pass
        elif action == 'background' and command.get('allOffConfirmed') is True:
            pass
        elif action in ['adopt_background', 'arm', 'disarm']:
            pass
        else:
            raise ValueError('操作内容を確認してください')
        with self.lock:
            if action=='arm' and (self.mode not in ['monitor','timing'] or shutil.disk_usage(self.root).free < self.recordings.disk_min_bytes):
                raise ValueError('記録モードと空き容量を確認してください')
            if self.state['connection'] != 'live':
                raise ValueError('C5がLIVEになってから操作してください')
            if self.state['timingArmed'] and action not in ['disarm']:
                raise ValueError('先に計測記録を停止してください')
            if self.mode == 'background':
                raise ValueError('背景記録の完了を待ってください')
        self.commands.put_nowait(command)

    def command(self, port, text):
        port.write((text + '\n').encode('ascii'))

    def pump(self, port, frames, seconds, callback):
        end = time.monotonic() + seconds
        while time.monotonic() < end and not self.stop.is_set():
            data = port.read(min(port.in_waiting or 1, 16384))
            for kind, payload in frames.feed(data):
                # Preserve every CRC-valid payload before semantic decoding or display filtering.
                self.recordings.observe_crc(frames.errors)
                self.recordings.append_frame(kind,payload,at=time.monotonic())
                if kind == 'R':
                    try:
                        r = reading(payload)
                    except ValueError:
                        # A CRC-valid malformed payload is evidence, not a reason to discard
                        # the remaining already-received frames in this parser batch.
                        self.state['malformedFrames'] += 1
                        continue
                    if self.previous_seq is not None:
                        gap = ((r['seq'] - self.previous_seq) & 65535) - 1
                        if gap >= 0:
                            self.state['seqGaps'] += gap
                    self.previous_seq = r['seq']
                    if self.chip_drop_base is None:
                        self.chip_drop_base = r['drops']
                    self.state['chipDrops'] = (r['drops'] - self.chip_drop_base) & 0xffffffff
                    self.state['records'] += 1
                    self.state['rfErrors'] += sum(bool(p['flags']) for p in r['pilots'])
                    callback(r)
                elif kind == 'T':
                    text = payload.decode(errors='replace')
                    self.status_text += text
                    self.status_text = self.status_text[-16000:]
            self.state['crcErrors'] = frames.errors
            self.recordings.observe_crc(frames.errors)
            self.check_recording()

    def initialize(self, port):
        self.close_recording('receiver_reinitialize')
        self.state.update(connection='initializing', message='ファームと受信状態を確認中', timingArmed=False)
        self.tracker.reset()
        self.background_active = False
        self.status_text = ''
        time.sleep(1.8)  # Linux CDC open may reset the C5.
        port.reset_input_buffer()
        f = Frames()
        self.command(port, 'status')
        self.pump(port, f, 1.2, lambda _: None)
        if 'firmware 1.2.5+xiaoc5' not in self.status_text or 'wifi=1' not in self.status_text:
            raise IOError('C5MK 1.2.5+xiaoc5 / wifi=1を確認できません')
        self.state['firmware'] = '1.2.5+xiaoc5'
        self.command(port, 'scan off\nscanmode fast\ngain 30\noffset 10\nbw 1\nnoden 256\nalpha 0.3\nscan on')
        self.previous_seq = None
        self.chip_drop_base = None
        self.clear_view()
        self.state.update(connection='live', message='USB接続済み')
        return f

    def clear_view(self):
        self.levels = [None] * 4
        self.samples = [[] for _ in range(4)]
        self.native_stats = [None] * 4
        self.timing_previous.clear()
        for h in self.timing_history:
            h.clear()
        for h in self.history:
            h.clear()
        self.state['sweep'] = None
        self.sweeps.clear()
        self.last_sample = 0
        self.stream_epoch += 1

    def apply(self, port, f, cmd):
        action = cmd['action']
        if self.state['timingArmed'] and action!='disarm':
            raise ValueError('先に記録を停止してください')
        if action == 'disarm':
            self.close_recording('operator_stopped')
            self.tracker.reset()
            return True
        if action == 'arm':
            if self.mode not in ['timing','monitor']:
                raise ValueError('4ch監視または計測タブで記録してください')
            if self.state['timingArmed']:
                return True
            self.tracker.reset()
            purpose = cmd.get('purpose','observation')
            if purpose not in ['observation','software_test']:
                raise ValueError('記録目的が不正です')
            self.state['session'] = self.recordings.start(dict(
                appVersion=VERSION, purpose=purpose, serial=self.serial, firmware=self.state['firmware'],
                config=copy.deepcopy(self.config), nativeApi=36 if self.mode=='timing' else None,
                hostRequestedSettings='gain30/offset10/bw1/noden256',
                  rfErrorFlagsAvailable=self.mode=='monitor', independentPassLabelsAvailable=True,
                frequencies=[PRESETS[x] for x in self.config['channels']],
                scannerCommands='scanmode fast; gain 30; offset 10; bw 1; noden 256; alpha 0.3',
                profileCommand='pilots '+','.join(str(PRESETS[x]) for x in self.config['channels']),
                background=dict(saved=self.background is not None,active=self.background_active,
                                recordedAt=self.background.get('recordedAt') if self.background else None),
                crcErrorsStart=f.errors if f else 0),kind='scanner' if self.mode=='monitor' else 'native')
            self.state['timingArmed'] = True
            return True
        if action == 'configure':
            self.config = cmd['config']
            atomic_json(self.root / 'config.json', self.config)
        elif action == 'mode':
            self.mode = cmd['mode']
        elif action == 'background':
            self.return_mode = self.mode
            self.mode = 'background'
            self.state['backgroundProgress'] = 0
        elif action == 'adopt_background':
            if not self.background:
                raise ValueError('保存された背景がありません')
            self.background_active = True
        self.state['timingArmed'] = False
        self.tracker.reset()
        self.clear_view()
        self.state['message'] = '4ch監視 / USB接続済み' if self.mode == 'monitor' else '帯域走査 / USB接続済み'
        self.command(port, 'scan on')  # Returns the USB from RH to scanner mode.
        self.pump(port, f, .12, lambda _: None)
        return False

    def close_recording(self, reason):
        self.recordings.stop(reason)
        self.state['timingArmed'] = False
        self.state['session'] = None

    def apply_safe(self,port,f,cmd):
        try:
            return self.apply(port,f,cmd)
        except ValueError as error:
            self.state['message']='操作拒否: '+str(error)
            return True

    def check_recording(self):
        archive=self.recordings.status()
        if self.state['timingArmed'] and archive and (not archive['recording'] or archive['error']):
            reason=archive.get('reason') or 'storage_error'
            self.close_recording(reason)
            self.state['message']='記録停止: '+reason+((' / '+archive['error']) if archive['error'] else '')

    def next_command(self):
        try:
            return self.commands.get_nowait()
        except queue.Empty:
            return None

    def set_profile(self, port, f, frequencies):
        self.command(port, 'pilots ' + ','.join(map(str, frequencies)))
        # Drain settings changes and settling data, checking actual per-frame frequencies below.
        self.pump(port, f, .025, lambda _: None)

    def values(self, record, frequencies):
        ps = record['pilots']
        if [p['mhz'] for p in ps] != frequencies:
            return None
        return [p['db'] if p['flags'] == 0 and p['gain'] == 30 and not p['clips'] else None for p in ps]

    def monitor_loop(self, port, f):
        fs = [PRESETS[x] for x in self.config['channels']]
        self.set_profile(port, f, fs)
        started = time.monotonic()
        def capture(r):
            vs = self.values(r, fs)
            if vs is None:
                return
            self.last_sample = time.monotonic()
            for i, v in enumerate(vs):
                if v is not None:
                    self.samples[i].append(v)
            now = time.monotonic()
            if now - self.last_emit >= .1:
                with self.lock:
                    for i in range(4):
                        self.levels[i] = round(power_mean(self.samples[i]), 2) if self.samples[i] else None
                        self.history[i].append(self.levels[i])
                        self.samples[i].clear()
                    self.last_emit = now
        while not self.stop.is_set():
            cmd = self.next_command()
            if cmd:
                if not self.apply_safe(port, f, cmd):
                    return
            self.pump(port, f, .03, capture)
            if time.monotonic() - (self.last_sample or started) > 2:
                raise IOError('C5受信が2秒以上途切れました')

    def spectrum(self, port, f, step):
        fs = list(range(5645, 5918, step))
        values = []
        started = time.monotonic()
        for offset in range(0, len(fs), 8):
            group = fs[offset:offset + 8]
            self.set_profile(port, f, group)
            bins = [[] for _ in group]
            def capture(r):
                vs = self.values(r, group)
                if vs is not None:
                    for i, v in enumerate(vs):
                        if v is not None:
                            bins[i].append(v)
            self.pump(port, f, .025, capture)
            values.extend(round(power_mean(b), 2) if len(b) >= 3 else None for b in bins)
            self.state['sweepProgress'] = round(min(offset + 8, len(fs)) / len(fs) * 100)
            if self.stop.is_set():
                return None
        self.sweep_id += 1
        if not any(v is not None for v in values):
            raise IOError('全走査点が欠測です。C5の応答を確認してください')
        sweep = dict(id=self.sweep_id, frequencies=fs, values=values,
                     duration=round(time.monotonic() - started, 3), time=time.time(), step=step)
        self.last_sample = time.monotonic()
        return sweep

    def spectrum_loop(self, port, f):
        while not self.stop.is_set():
            cmd = self.next_command()
            if cmd and not self.apply(port, f, cmd):
                return
            sweep = self.spectrum(port, f, self.config['step'])
            if sweep:
                with self.lock:
                    sweep['background'] = self.bg_values(sweep['frequencies'])
                    self.state['sweep'] = sweep
                    self.sweeps.append(sweep)

    def bg_values(self, frequencies):
        if not self.background_active or not self.background:
            return None
        result = []
        for mhz in frequencies:
            x = (mhz - 5645) / 2
            i = int(x)
            a = self.background['values'][i]
            b = self.background['values'][min(i + 1, 136)]
            v = 10 * math.log10((1 - (x - i)) * 10 ** (a / 10) + (x - i) * 10 ** (b / 10))
            result.append(round(v, 2))
        return result

    def background_loop(self, port, f):
        sweeps = []
        try:
            for i in range(5):
                sw = self.spectrum(port, f, 2)
                if not sw or any(v is None for v in sw['values']):
                    raise IOError('背景記録に欠測があり保存しませんでした')
                sweeps.append(sw['values'])
                self.state['backgroundProgress'] = (i + 1) * 20
            bg = dict(serial=self.serial, firmware='1.2.5+xiaoc5', settings='g30/o10/bw1/n256',
                      recordedAt=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                      values=[round(statistics.median(vs), 2) for vs in zip(*sweeps)])
            atomic_json(self.root / 'background.json', bg)
            self.background = bg
            self.background_active = True
            self.state['message'] = 'BG SAVED：全機OFF時の背景を保存しました'
        finally:
            self.state['backgroundProgress'] = None
            self.mode = self.return_mode
            self.clear_view()

    def timing_loop(self, port, f):
        self.command(port, 'scan off')
        self.pump(port, f, .12, lambda _: None)
        port.reset_input_buffer()
        old_timeout = port.timeout
        port.timeout = .25
        rh = RH(port)
        self.state['firmware'] = rh.configure([PRESETS[x] for x in self.config['channels']],
                                             self.config['enter'], self.config['exit'])
        self.state['message'] = 'C5ネイティブ通過検出／試験記録のみ・MADSYSTEM未送信'
        self.tracker.reset()
        started = time.monotonic()
        try:
            while not self.stop.is_set():
                cmd = self.next_command()
                if cmd:
                    port.timeout = old_timeout
                    if not self.apply_safe(port, f, cmd):
                        return
                    port.timeout = .25
                for slot in range(4):
                    poll = rh.poll(slot)
                    now = time.monotonic()
                    with self.lock:
                        self.last_sample = now
                        self.state['timingPolls'] += 1
                        late = poll['rtt'] > .1 or not poll['clockBracketValid']
                        self.state['timingLate'] += int(late)
                        previous = self.timing_previous.get(slot)
                        gap = previous is not None and now - previous > .5
                        self.state['timingGaps'] += int(gap)
                        self.timing_previous[slot] = now
                        self.native_stats[slot] = poll
                        self.timing_history[slot].append([
                            poll['hostMid'], poll['rssi'], poll['extremum'],
                            poll['extremumFirstHost'], poll['extremumLastHost'],
                            poll['extremumIsPeak'], poll['crossing'],
                            poll['extremumValid'] and not late and not gap,
                            round(poll['rtt'] * 1000, 3)])
                        if self.state['timingArmed']:
                            self.recordings.append('sample', dict(poll, gapBefore=gap, late=late), slot,
                                                   at=poll['hostMid'])
                            archive = self.recordings.status()
                            if archive and archive['error']:
                                self.close_recording('storage_error')
                                self.state['message'] = '記録エラー：' + archive['error']
                        # RH 1..254 is a distinct scale; never mix with receiver dB history.
                        self.levels[slot] = poll['rssi']
                        self.history[slot].append(poll['rssi'])
                        event = self.tracker.update(slot, poll, self.config['minimumMs'])
                        if event and self.state['timingArmed'] and now - started > 1:
                            event.update(id=secrets.token_hex(8), channel=self.config['channels'][slot],
                                         mhz=PRESETS[self.config['channels'][slot]], session=self.state['session'],
                                         receivedUtc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                                         source='C5-native-RH36', sentToMadsystem=False)
                            if 'peakMonotonic' in event:
                                event['peakSessionSeconds'] = event['peakMonotonic'] - self.recordings.status()['startMonotonic']
                            self.state['events'].insert(0, event)
                            self.state['events'] = self.state['events'][:50]
                            self.recordings.append('pass', event, slot)
                            with (self.root / 'passes.jsonl').open('a', encoding='utf-8') as output:
                                output.write(json.dumps(event, ensure_ascii=False) + '\n')
                self.stop.wait(.08)
        finally:
            port.timeout = old_timeout

    def worker(self):
        while not self.stop.is_set():
            port = None
            try:
                candidates = [p for p in list_ports.comports() if p.vid == 0x303a and p.pid == 0x1001
                              and (p.serial_number or '').upper() == self.serial]
                if len(candidates) != 1:
                    self.state.update(connection='disconnected', message='対象C5のUSB接続を待っています',
                                      timingArmed=False, device=None)
                    self.last_sample = 0
                    self.stop.wait(1)
                    continue
                p = candidates[0]
                port = serial.Serial()
                port.port, port.baudrate, port.timeout = p.device, 115200, .01
                port.dtr = port.rts = False
                port.exclusive = True
                port.open()
                self.state['device'] = p.device
                self.mode = 'monitor'  # Never automatically resume an armed timing session.
                f = self.initialize(port)
                while not self.stop.is_set():
                    if self.mode == 'monitor':
                        self.monitor_loop(port, f)
                    elif self.mode == 'spectrum':
                        self.spectrum_loop(port, f)
                    elif self.mode == 'background':
                        self.background_loop(port, f)
                    elif self.mode == 'timing':
                        self.timing_loop(port, f)
            except Exception as error:
                if self.mode == 'timing':
                    self.state['timingErrors'] += 1
                self.close_recording('receiver_error: ' + str(error))
                self.state.update(connection='error', message=str(error), timingArmed=False, session=None)
                self.last_sample = 0
                self.background_active = False
                self.state['reconnects'] += 1
                self.tracker.reset()
                while self.next_command() is not None:
                    pass
                self.stop.wait(2)
            finally:
                if port:
                    port.close()
        self.close_recording('service_stopped')


def serve(monitor, bind, port):
    from replay import ScannerReplay
    scanner_replay=ScannerReplay()
    export_slot=threading.BoundedSemaphore(1)
    token = secrets.token_urlsafe(24)
    html = (Path(__file__).parent / 'index.html').read_text(encoding='utf-8').replace('__CONTROL_TOKEN__', token)
    allowed_hosts = {'localhost', '127.0.0.1', socket.gethostname()}
    allowed_hosts.update(socket.gethostbyname_ex(socket.gethostname())[2])
    # Addresses used by local LAN interfaces; no DNS query from HTTP Host.
    import subprocess
    if os.name != 'nt':
        for address in subprocess.check_output(['hostname', '-I'], text=True).split():
            allowed_hosts.add(address)

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def log_message(self, *args):
            return

        def reply(self, status, body, kind='application/json; charset=utf-8'):
            data = body if isinstance(body, bytes) else body.encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('X-Frame-Options', 'DENY')
            self.end_headers()
            self.wfile.write(data)

        def host_ok(self):
            return self.headers.get('Host', '').split(':')[0] in allowed_hosts

        def stream_lines(self,lines):
            buffer=bytearray()
            for line in lines:
                buffer.extend(line.encode('utf-8'))
                if len(buffer)>=65536:
                    self.wfile.write(buffer);buffer.clear()
            if buffer:self.wfile.write(buffer)

        def export_worker(self,recording_id,kind):
            if not export_slot.acquire(blocking=False):
                raise ValueError('別のエクスポートが進行中です')
            child=None
            try:
                child=subprocess.Popen([sys.executable,'-u',str(Path(__file__).parent/'export_stream.py'),
                    str(monitor.root.resolve()),recording_id,kind],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                first=child.stdout.readline()  # errors before the first row remain HTTP errors
                if not first:
                    raise ValueError('エクスポートを開始できません: '+child.communicate(timeout=3)[1].decode(errors='replace')[-1000:])
                self.send_response(200)
                self.send_header('Content-Type','application/x-ndjson; charset=utf-8')
                self.send_header('Content-Disposition',f'attachment; filename="{kind}-{recording_id}.jsonl"')
                self.send_header('Cache-Control','no-store');self.send_header('Connection','close')
                self.end_headers();self.close_connection=True;self.wfile.write(first)
                while True:
                    block=child.stdout.read(65536)
                    if not block:break
                    self.wfile.write(block)
                child.communicate(timeout=3)
                if child.returncode:raise ConnectionError('エクスポート処理が中断しました')
            finally:
                if child:
                    if child.poll() is None:child.terminate()
                    try:child.communicate(timeout=3)
                    except subprocess.TimeoutExpired:child.kill();child.communicate()
                export_slot.release()

        def do_GET(self):
            if not self.host_ok():
                return self.reply(403, '{}')
            path = urlsplit(self.path).path
            if path.startswith('/api/recordings'):
                try:
                    parts=path.strip('/').split('/')
                    if len(parts)==4 and parts[3]=='labels':
                        return self.reply(200,json.dumps(monitor.labels.read(parts[2]),ensure_ascii=False))
                    if len(parts)==4 and parts[3]=='training':
                        meta=monitor.recordings.metadata(parts[2])
                        if meta['status']=='recording' or (monitor.recordings.status(parts[2]) or {}).get('closing'):
                            raise ValueError('学習用エクスポートは記録停止・保存完了後に行ってください')
                        return self.export_worker(parts[2],'training')
                    parts = path.strip('/').split('/')
                    if path == '/api/recordings':
                        return self.reply(200, json.dumps(monitor.recordings.catalog(), ensure_ascii=False))
                    if len(parts) not in [3, 4] or parts[:2] != ['api', 'recordings']:
                        return self.reply(404, '{}')
                    recording_id = parts[2]
                    if len(parts) == 3:
                        return self.reply(200, json.dumps(monitor.recordings.metadata(recording_id)))
                    if parts[3] == 'window':
                        query = parse_qs(urlsplit(self.path).query)
                        meta=monitor.recordings.metadata(recording_id)
                        if meta.get('recordingKind')=='scanner':
                            body=scanner_replay.window(monitor.recordings.root/(recording_id+'.sqlite3'),meta,
                                float(query.get('end',['0'])[0]),float(query.get('seconds',['30'])[0]))
                            return self.reply(200,body)
                        window = monitor.recordings.window(recording_id,
                            float(query.get('end', ['0'])[0]), float(query.get('seconds', ['30'])[0]))
                        return self.reply(200, json.dumps(window, ensure_ascii=False, allow_nan=False))
                    if parts[3] == 'export':
                        # Check ID/existence before writing HTTP headers.
                        export_meta=monitor.recordings.metadata(recording_id)
                        if export_meta.get('recordingKind')=='scanner' and export_meta['status']=='recording':
                            raise ValueError('scanner原本は記録停止・保存完了後に出力してください')
                        if export_meta.get('recordingKind')=='scanner':
                            return self.export_worker(recording_id,'raw')
                        self.send_response(200)
                        self.send_header('Content-Type', 'application/x-ndjson; charset=utf-8')
                        self.send_header('Content-Disposition', f'attachment; filename="c5-{recording_id}.jsonl"')
                        self.send_header('Cache-Control', 'no-store')
                        self.send_header('Connection', 'close')
                        self.end_headers()
                        self.close_connection = True
                        self.stream_lines(monitor.recordings.export(recording_id))
                        return
                    return self.reply(404, '{}')
                except (ValueError, FileNotFoundError, sqlite3.Error) as error:
                    return self.reply(400, json.dumps({'error': str(error)}, ensure_ascii=False))
            if path == '/':
                self.reply(200, html, 'text/html; charset=utf-8')
            elif path == '/api/state':
                self.reply(200, json.dumps(monitor.snapshot(), ensure_ascii=False, allow_nan=False))
            elif path in ['/ui.js', '/ui.css','/labels-ui.js']:
                kind = 'text/javascript; charset=utf-8' if path.endswith('.js') else 'text/css; charset=utf-8'
                self.reply(200, (Path(__file__).parent / path[1:]).read_bytes(), kind)
            elif path == '/api/waterfall':
                with monitor.lock:
                    body = json.dumps(list(monitor.sweeps), allow_nan=False)
                self.reply(200, body)
            elif path == '/api/passes':
                p = monitor.root / 'passes.jsonl'
                self.reply(200, p.read_bytes() if p.exists() else b'', 'application/x-ndjson')
            elif path == '/notice':
                self.reply(200, (Path(__file__).parent / 'NOTICE.txt').read_bytes(), 'text/plain; charset=utf-8')
            else:
                self.reply(404, '{}')

        def do_POST(self):
            if (not self.host_ok() or self.headers.get('X-Control-Token') != token
                    or self.headers.get('Origin') != 'http://' + self.headers.get('Host', '')
                    or self.headers.get('Content-Type', '').split(';')[0] != 'application/json'):
                return self.reply(403, '{}')
            path=urlsplit(self.path).path
            parts=path.strip('/').split('/')
            is_label=len(parts)==4 and parts[:2]==['api','recordings'] and parts[3]=='labels'
            if self.path != '/api/control' and not is_label:
                return self.reply(404, '{}')
            try:
                size = int(self.headers.get('Content-Length', '0'))
                if not 0 < size <= 8192:
                    raise ValueError('invalid body size')
                received=time.monotonic()
                body=json.loads(self.rfile.read(size))
                if is_label:
                    saved=monitor.labels.append(parts[2],body,received)
                    return self.reply(201,json.dumps(saved,ensure_ascii=False))
                monitor.request(body)
                self.reply(202, '{"queued":true}')
            except (ValueError, TypeError, queue.Full, OSError, sqlite3.Error) as error:
                self.reply(400, json.dumps({'error': str(error)}, ensure_ascii=False))

    server = ThreadingHTTPServer((bind, port), Handler)
    server.daemon_threads = True
    monitor.thread.start()
    print(f'C5 monitor {VERSION} HTTP {bind}:{port}', flush=True)
    def terminate(_signum, _frame):
        monitor.stop.set()
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGTERM, terminate)
    try:
        server.serve_forever()
    finally:
        monitor.stop.set()
        monitor.thread.join(timeout=3)
        monitor.recordings.shutdown()
        scanner_replay.close()
        server.server_close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bind', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=8765)
    parser.add_argument('--data', default='.local/data')
    parser.add_argument('--disk-min-mib',type=int,default=256)
    parser.add_argument('--serial', default=SERIAL, help='Owned C5 USB serial (or C5_SERIAL environment variable)')
    args = parser.parse_args()
    if not re_serial(args.serial):
        parser.error('Specify --serial or C5_SERIAL as six colon-separated hexadecimal bytes')
    if args.disk_min_mib < 1:
        parser.error('--disk-min-mib must be positive')
    serve(Monitor(args.data, args.serial.upper(),args.disk_min_mib*1024**2), args.bind, args.port)
