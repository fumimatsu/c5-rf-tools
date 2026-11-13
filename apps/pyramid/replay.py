"""Read-only scanner rendering in one bounded CPU worker, outside the USB GIL."""
from concurrent.futures import ProcessPoolExecutor
from contextlib import closing
import json
import multiprocessing
import os
from pathlib import Path
import sqlite3
import threading
from scanner_recording import chart_window


def render_window(path, metadata, end, seconds):
    with closing(sqlite3.connect(Path(path).as_uri()+'?mode=ro',uri=True,timeout=2)) as db:
        result=chart_window(db,metadata,end,seconds)
    return json.dumps(result,ensure_ascii=False,allow_nan=False).encode('utf-8')


class ScannerReplay:
    def __init__(self):
        self.slots=threading.BoundedSemaphore(2)  # one running and at most one waiting
        self.executor=ProcessPoolExecutor(max_workers=1,mp_context=multiprocessing.get_context('spawn'))
        self.executor.submit(os.getpid).result(timeout=10)  # warm before starting USB owner

    def window(self,path,metadata,end,seconds):
        if not self.slots.acquire(blocking=False):
            raise ValueError('再生処理中です。少し待って再操作してください')
        try:
            future=self.executor.submit(render_window,str(path),metadata,end,seconds)
        except Exception:
            self.slots.release()
            raise
        future.add_done_callback(lambda _:self.slots.release())
        try:
            return future.result(timeout=15)
        except TimeoutError:
            raise ValueError('再生集計が制限時間を超えました。受信記録は継続しています') from None

    def close(self):
        self.executor.shutdown(wait=True,cancel_futures=True)
