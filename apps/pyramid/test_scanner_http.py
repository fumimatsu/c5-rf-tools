import base64
import hashlib
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
from recording import Recordings
from test_scanner import context,payload,label
from test_portability import stop_server


class HttpTests(unittest.TestCase):
    def test_both_schemas_label_api_guards_and_training_export(self):
        with tempfile.TemporaryDirectory() as root:
            data=Path(root)
            a=Recordings(data/'recordings')
            native=a.start(context());a.append('pass',dict(accepted=True));a.stop('test')
            scanner=a.start(context(),'scanner');a.append_frame('R',payload(flags=3));a.shutdown()
            hashes={rid:hashlib.sha256((a.root/(rid+'.sqlite3')).read_bytes()).hexdigest() for rid in [native,scanner]}
            with socket.socket() as sock:sock.bind(('127.0.0.1',0));port=sock.getsockname()[1]
            proc=subprocess.Popen([sys.executable,'server.py','--data',root,'--port',str(port)],
                    env=dict(os.environ,C5_SERIAL='00:00:00:00:00:01'),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            url=f'http://127.0.0.1:{port}'
            def get(path):
                with urllib.request.urlopen(url+path,timeout=4) as r:return r.read()
            try:
                end=time.monotonic()+10
                while True:
                    try:html=get('/').decode();break
                    except (ConnectionError,urllib.error.URLError):
                        if time.monotonic()>end:self.fail('HTTP fixture server did not start')
                        time.sleep(.05)
                token=re.search('name="control-token" content="([^"]+)"',html)[1]
                for rid in [native,scanner]:
                    endpoint=url+'/api/recordings/'+rid+'/labels'
                    request=urllib.request.Request(endpoint,json.dumps(label()).encode(),headers={'Content-Type':'application/json'})
                    with self.assertRaises(urllib.error.HTTPError) as error:urllib.request.urlopen(request,timeout=3)
                    self.assertEqual(error.exception.code,403)
                    headers={'Content-Type':'application/json','Origin':url,'X-Control-Token':token}
                    command=label()
                    with urllib.request.urlopen(urllib.request.Request(endpoint,json.dumps(command).encode(),headers=headers),timeout=3) as r:
                        self.assertEqual(r.status,201)
                    ledger=json.loads(get('/api/recordings/'+rid+'/labels'))
                    self.assertEqual(len(ledger['active']),1)
                    training=[json.loads(s) for s in get('/api/recordings/'+rid+'/training').splitlines()]
                    self.assertEqual(training[0]['schema'],'c5-training-source/1')
                    self.assertEqual(training[0]['quality'],'software_test')
                    self.assertFalse(next(r for r in training if r['kind']=='human_label')['independentlyReported'])
                    self.assertEqual(hashlib.sha256((a.root/(rid+'.sqlite3')).read_bytes()).hexdigest(),hashes[rid])
                exported=[json.loads(s) for s in get('/api/recordings/'+scanner+'/export').splitlines()]
                self.assertEqual(base64.b64decode(next(r for r in exported if r['kind']=='frame')['payloadBase64']),payload(flags=3))
                self.assertEqual(json.loads(get('/api/recordings/'+scanner+'/window?end=10'))['metadata']['recordingKind'],'scanner')
                self.assertEqual(hashlib.sha256((a.root/(scanner+'.sqlite3')).read_bytes()).hexdigest(),hashes[scanner])
            finally:
                stop_server(proc)


if __name__=='__main__':unittest.main()
