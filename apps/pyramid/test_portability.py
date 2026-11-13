"""Fresh-PC startup and explicit device ownership; never touches a real USB port."""
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from server import re_serial


class PortableTests(unittest.TestCase):
    def test_explicit_serial_required(self):
        env = dict(os.environ)
        env.pop('C5_SERIAL', None)
        result = subprocess.run([sys.executable, 'server.py'], env=env, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b'Specify --serial', result.stderr)
        for serial in ['', 'auto', None, 'COMx', '00:00:00:00:00:ZZ']:
            self.assertFalse(re_serial(serial))
        self.assertTrue(re_serial('00:00:00:00:00:01'))

    def test_fresh_local_start_without_device_and_origin_guards(self):
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        with tempfile.TemporaryDirectory() as root:
            env = dict(os.environ, C5_SERIAL='00:00:00:00:00:01')
            process = subprocess.Popen([sys.executable, '-u', 'server.py', '--data', root, '--port', str(port)],
                                       env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            base = f'http://127.0.0.1:{port}'
            try:
                deadline = time.monotonic()+10
                while True:
                    try:
                        with urllib.request.urlopen(base, timeout=1) as r:
                            html = r.read().decode('utf-8')
                        break
                    except (urllib.error.URLError, ConnectionError):
                        if time.monotonic()>deadline: self.fail('server failed to start')
                        time.sleep(.05)
                token = re.search(r'name="control-token" content="([^"]+)"', html)[1]
                self.assertNotEqual(token, '__CONTROL_TOKEN__')
                for path, expected in [('/ui.js', b'loadRecording'), ('/ui.css', b'.workspace')]:
                    with urllib.request.urlopen(base+path, timeout=2) as r:
                        self.assertIn(expected, r.read())
                with urllib.request.urlopen(base+'/api/state', timeout=2) as r: state=json.load(r)
                self.assertEqual(state['serial'], '00:00:00:00:00:01')
                self.assertFalse(state['fresh'])
                self.assertEqual(state['levels'], [None]*4)
                with urllib.request.urlopen(base+'/api/recordings', timeout=2) as r:self.assertEqual(json.load(r), [])
                request=urllib.request.Request(base+'/api/control', b'{}', headers={'Content-Type':'application/json'})
                with self.assertRaises(urllib.error.HTTPError) as error: urllib.request.urlopen(request, timeout=2)
                self.assertEqual(error.exception.code, 403)
                request=urllib.request.Request(base+'/api/state',headers={'Host':'untrusted.invalid'})
                with self.assertRaises(urllib.error.HTTPError) as error: urllib.request.urlopen(request, timeout=2)
                self.assertEqual(error.exception.code, 403)
                self.assertTrue((Path(root)/'recordings').is_dir())
            finally:
                process.terminate()
                try: process.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    process.kill();process.communicate(timeout=3)


if __name__ == '__main__':
    unittest.main()
