# dashboard/sse.py
import json
import queue
from django.http import StreamingHttpResponse
from ingest.views import RING, SUBSCRIBERS, SUB_LOCK

def _event_stream():
    q = queue.Queue(maxsize=10)
    with SUB_LOCK:
        SUBSCRIBERS.add(q)
    try:
        yield b"retry: 2000\n\n"
        if len(RING) > 0:
            data = json.dumps(RING[-1]).encode("utf-8")
            yield b"event: ecg\n" + b"data: " + data + b"\n\n"
        while True:
            payload = q.get()
            data = json.dumps(payload).encode("utf-8")
            yield b"event: ecg\n" + b"data: " + data + b"\n\n"
    except GeneratorExit:
        pass
    finally:
        with SUB_LOCK:
            SUBSCRIBERS.discard(q)

def sse_view(request):
    resp = StreamingHttpResponse(_event_stream(), content_type="text/event-stream")
    resp["Cache-Control"] = "no-cache"
    resp["X-Accel-Buffering"] = "no"
    return resp
