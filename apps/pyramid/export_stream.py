"""Read-only export worker: never opens the C5 or changes an archive."""
import json
from pathlib import Path
import sys
from recording import Recordings
from dataset import export_dataset
from labels import Labels


def lines(root,recording_id,kind):
    archive=Recordings(Path(root)/'recordings')
    if kind=='raw':
        yield from archive.export(recording_id)
    elif kind=='training':
        from server import PRESETS
        labels=Labels(Path(root)/'labels',archive,PRESETS)
        for row in export_dataset(archive,labels,recording_id):
            yield json.dumps(row,ensure_ascii=False,allow_nan=False)+'\n'
    else:raise ValueError('Unknown export kind')


if __name__=='__main__':
    buffer=bytearray()
    for line in lines(*sys.argv[1:]):
        buffer.extend(line.encode('utf-8'))
        if len(buffer)>=65536:
            sys.stdout.buffer.write(buffer);buffer.clear()
    if buffer:sys.stdout.buffer.write(buffer)
