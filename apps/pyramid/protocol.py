"""C5MK 1.2.5 scanner / native RotorHazard protocol client.

Wire formats derived from Louis Hitchcock's FPVGateC5MK, CC BY-NC-SA 4.0.
No RF algorithm is changed. See NOTICE.txt.
"""
import binascii
import struct
import time

REVERSE = bytes(int(f'{i:08b}'[::-1], 2) for i in range(256))


def crc16(data):
    c = binascii.crc_hqx(bytes(data).translate(REVERSE), 0xffff)
    return ((REVERSE[c & 255] << 8) | REVERSE[c >> 8]) ^ 0xffff


class Frames:
    def __init__(self):
        self.buf = bytearray()
        self.errors = 0

    def feed(self, data):
        self.buf.extend(data)
        while True:
            pos = self.buf.find(b'\xa5\x5a')
            if pos < 0:
                self.buf[:] = self.buf[-1:] if self.buf.endswith(b'\xa5') else b''
                return
            del self.buf[:pos]
            if len(self.buf) < 5:
                return
            size = int.from_bytes(self.buf[3:5], 'little')
            if size > 4096:
                del self.buf[:2]
                continue
            if len(self.buf) < size + 7:
                return
            body = bytes(self.buf[2:size + 5])
            check = int.from_bytes(self.buf[size + 5:size + 7], 'little')
            if crc16(body) != check:
                self.errors += 1
                del self.buf[:2]
                continue
            del self.buf[:size + 7]
            yield chr(body[0]), body[3:]


def reading(payload):
    if len(payload) < 12:
        raise ValueError('short scanner frame')
    seq, count, flags, drops, cycle = struct.unpack_from('<HBBII', payload)
    if not 1 <= count <= 8 or len(payload) != 12 + 14 * count:
        raise ValueError('invalid scanner frame length')
    pilots = []
    for i in range(count):
        t, raw, filt, mhz, gain, clips, pf, _ = struct.unpack_from('<IhhHBBBB', payload, 12 + 14 * i)
        pilots.append(dict(t=t, raw=raw / 100, db=filt / 100, mhz=mhz,
                           gain=gain, clips=clips, flags=pf))
    return dict(seq=seq, flags=flags, drops=drops, cycle=cycle, pilots=pilots)


class RH:
    """One USB owner. Replies are checksummed; select/setting readback is required."""
    def __init__(self, port):
        self.port = port

    def read(self, cmd, size):
        self.port.reset_input_buffer()
        before = time.monotonic()
        self.port.write(bytes([cmd]))
        data = self.port.read(size + 1)
        after = time.monotonic()
        if len(data) != size + 1 or sum(data[:-1]) & 255 != data[-1]:
            raise IOError(f'RH reply/checksum error command={cmd:02x}')
        return data[:-1], (before + after) / 2, after - before

    def write(self, cmd, data):
        self.port.write(bytes([cmd]) + data + bytes([sum(data) & 255]))

    def select(self, slot):
        self.write(0x7a, bytes([slot]))
        if self.read(0x3a, 1)[0][0] != slot:
            raise IOError('RH seat selection mismatch')

    def configure(self, frequencies, enter, exit):
        identity = self.read(0x22, 2)[0]
        if identity != bytes([0x25, 36]):
            raise IOError(f'unsupported RH node identity {identity.hex()}')
        version = self.read(0x3d, 16)[0].split(b'\0')[0].decode()
        board = self.read(0x40, 16)[0].split(b'\0')[0].decode()
        if version != '1.2.5' or board != 'xiaoc5':
            raise IOError(f'unexpected timing firmware {version}+{board}')
        if self.read(0x39, 1)[0][0] != 8:
            raise IOError('unexpected RH seat count')
        for i in range(8):
            self.select(i)
            mhz = frequencies[i] if i < 4 else 0
            self.write(0x51, struct.pack('>H', mhz))
            actual = int.from_bytes(self.read(0x03, 2)[0], 'big')
            if actual != mhz:
                raise IOError('RH frequency readback mismatch')
            if i < 4:
                for command, read_command, value in [(0x71, 0x31, enter[i]), (0x72, 0x32, exit[i])]:
                    self.write(command, bytes([value]))
                    if self.read(read_command, 1)[0][0] != value:
                        raise IOError('RH threshold readback mismatch')
            self.write(0x78, b'\0')
        return version + '+' + board

    def poll(self, slot):
        self.select(slot)
        clock0 = int.from_bytes(self.read(0x33, 4)[0], 'big')
        b, host_mid, rtt = self.read(0x05, 16)
        clock1 = int.from_bytes(self.read(0x33, 4)[0], 'big')
        return decode_rh_stats(b, host_mid, rtt, clock0, clock1)


def decode_rh_stats(b, host_mid, rtt, clock0, clock1):
    if len(b) != 16:
        raise ValueError('short RH statistics')
    span = (clock1 - clock0) & 0xffffffff
    age = int.from_bytes(b[12:14], 'big')
    hold = int.from_bytes(b[14:16], 'big')
    return dict(counter=b[0], ageMs=int.from_bytes(b[1:3], 'big'), rssi=b[3],
                nodePeak=b[4], peak=b[5], loopUs=int.from_bytes(b[6:8], 'big'),
                crossing=bool(b[8] & 1), passNadir=b[9], nodeNadir=b[10],
                extremum=b[11], extremumIsPeak=bool(b[8] & 2),
                extremumAgeMs=age, extremumHoldMs=hold, rawFlags=b[8],
                clockBeforeMs=clock0, clockAfterMs=clock1, clockWindowMs=span,
                clockBracketValid=span <= 100,
                extremumValid=span <= 100 and age < 65535 and hold <= age,
                # Host-time estimates, not an exact synchronized C5 clock.
                extremumFirstHost=host_mid - age / 1000,
                extremumLastHost=host_mid - age / 1000 + hold / 1000,
                rawHex=b.hex(), hostMid=host_mid, rtt=rtt)


class PassTracker:
    """Counter wrap, stale/late polls, first pass, minimum interval, no replay."""
    def __init__(self):
        self.previous = {}
        self.last = {}

    def reset(self):
        self.previous.clear()
        self.last.clear()

    def update(self, slot, poll, minimum_ms=2000):
        now = poll['hostMid']
        previous = self.previous.get(slot)
        self.previous[slot] = (poll['counter'], now)
        if previous is None or previous[0] == poll['counter']:
            return None
        count = (poll['counter'] - previous[0]) & 255
        if (count != 1 or now - previous[1] > 1.0 or poll['ageMs'] >= 65535
                or poll['rtt'] > .1 or not poll.get('clockBracketValid', True)):
            self.last.pop(slot, None)
            return dict(accepted=False, reason='counter_gap_or_late_poll', slot=slot)
        peak_time = now - poll['ageMs'] / 1000
        if peak_time <= previous[1] - 1.0 or peak_time > now:
            self.last.pop(slot, None)
            return dict(accepted=False, reason='invalid_peak_age', slot=slot)
        last = self.last.get(slot)
        interval = round((peak_time - last) * 1000) if last is not None else None
        if interval is not None and interval < minimum_ms:
            return dict(accepted=False, reason='minimum_interval', slot=slot)
        self.last[slot] = peak_time
        return dict(accepted=True, slot=slot, peakMonotonic=peak_time, intervalMs=interval,
                    peakRssi=poll['peak'], counter=poll['counter'],
                    rttMs=round(poll['rtt'] * 1000, 3), reason='first_pass' if interval is None else 'pass')
