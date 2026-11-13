"""Streaming, read-only training-source export. Does not train or infer class labels."""
import argparse
from contextlib import closing
import json
from pathlib import Path
from protocol import reading
from recording import Recordings
from scanner_recording import SCHEMA
from labels import Labels


def export_dataset(recordings, labels, recording_id):
    meta = recordings.metadata(recording_id)
    if meta['status']=='recording' or (recordings.status(recording_id) or {}).get('closing'):
        raise ValueError('学習用エクスポートは記録停止・保存完了後に行ってください')
    ledger = labels.read(recording_id)
    quality = 'software_test' if meta['context'].get('purpose')=='software_test' else 'unvalidated_observation'
    yield dict(kind='manifest',schema='c5-training-source/1',recording=meta,
               annotationRevision=ledger['revision'],splitGroup=recording_id,quality=quality,
               evaluationStatus='not validated; no automatic train/evaluation split',
               labelPolicy='Human annotations are separate; point/interval uncertainty is retained. No frame class is generated from RF or event rows.',
               radioPolicy='scanner dB and native RH units require separate models/axes; no conversion implied')
    active_ids = {r['labelId'] for r in ledger['active']}
    for label in ledger['rows']:
        active = label['labelId'] in active_ids
        usable = active and label['label']!='unsure' and not label['viewedRfWhileLabeling'] and label['method'] in ['human_live','human_video_review'] and quality!='software_test'
        yield dict(kind='human_label',data=label,active=active,
                   independentlyReported=usable,evaluationGroundTruthVerified=False)
    with closing(recordings.connect(recording_id)) as db:
        if meta['schema']==SCHEMA:
            for seq,t,unix,kind,payload in db.execute('SELECT record_seq,t,receivedUnix,kind,payload FROM frames ORDER BY record_seq'):
                if kind!='R':
                    continue
                try:
                    frame=reading(payload)
                except ValueError:
                    yield dict(kind='malformed_frame',sequence=seq,t=t)
                    continue
                samples=[]
                for slot,p in enumerate(frame['pilots']):
                    reasons=[]
                    if p['flags']:reasons.append('flags')
                    if p['clips']:reasons.append('clips')
                    if p['gain']!=30:reasons.append('gain')
                    if p['raw']<=-327.68:reasons.append('missing_sentinel')
                    if slot>=len(meta['context']['frequencies']) or p['mhz']!=meta['context']['frequencies'][slot]:reasons.append('frequency')
                    samples.append(dict(p,slot=slot,validForPowerComparison=not reasons,invalidReasons=reasons))
                yield dict(kind='scanner_frame',sequence=seq,t=t,receivedUnix=unix,
                           wireSeq=frame['seq'],frameFlags=frame['flags'],c5Drops=frame['drops'],
                           cycleUs=frame['cycle'],samples=samples)
        else:
            for (data,) in db.execute('SELECT data FROM rows ORDER BY seq'):
                yield dict(kind='native_record',data=json.loads(data))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data',required=True)
    parser.add_argument('--recording',required=True)
    parser.add_argument('--out',required=True)
    args=parser.parse_args()
    from server import PRESETS
    archive=Recordings(Path(args.data)/'recordings')
    annotations=Labels(Path(args.data)/'labels',archive,PRESETS)
    with Path(args.out).open('x',encoding='utf-8') as output:
        for row in export_dataset(archive,annotations,args.recording):
            output.write(json.dumps(row,ensure_ascii=False,allow_nan=False)+'\n')
