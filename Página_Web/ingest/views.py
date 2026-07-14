# ingest/views.py
import time
import queue
import threading
import array
import zlib
from collections import deque
from typing import Deque, Dict, Any, List, Set

from rest_framework.views import APIView
from rest_framework.response import Response
from rest_framework import status

from django.utils.decorators import method_decorator
from django.views.decorators.csrf import csrf_exempt

from .serializers import IngestPayload
from .models import EcgBuffer, InferenceResult

# ====== Ring buffer para el dashboard (historia corta en RAM) ======
RING_CAPACITY = 300
RING: Deque[Dict[str, Any]] = deque(maxlen=RING_CAPACITY)

# ====== Suscriptores SSE (modo hilo) ======
SUBSCRIBERS: Set[queue.Queue] = set()
SUB_LOCK = threading.Lock()

def _broadcast(payload: Dict[str, Any]) -> None:
    with SUB_LOCK:
        muertos = []
        for q in list(SUBSCRIBERS):
            try:
                q.put_nowait(payload)
            except Exception:
                muertos.append(q)
        for q in muertos:
            SUBSCRIBERS.discard(q)

# ====== Autenticación mínima ======
DEVICE_TOKENS = {"d1": "p5wqg8k3Yw2Xg1QeWJ9v3m8mH1WbJrL3Yb2Q"}

# ====== Clasificador placeholder ======
def classify_ecg(fs: int, ecg: List[int]) -> Dict[str, Any]:
    # Sustituir por tu modelo real
    return {"label": "NSR", "score": 0.95}

def pack_int16_compressed(int_list: List[int]) -> bytes:
    """
    Empaqueta una lista de enteros como int16 little-endian y la comprime con zlib.
    """
    arr = array.array("h", int_list)  # int16
    raw = arr.tobytes()
    return zlib.compress(raw, level=6)

def downsample_for_plot(vec: List[int], max_points: int = 250) -> List[int]:
    if len(vec) <= max_points:
        return vec
    step = max(1, len(vec) // max_points)
    return vec[::step]

@method_decorator(csrf_exempt, name="dispatch")
class IngestView(APIView):
    authentication_classes = []
    permission_classes = []

    def post(self, request):
        # Auth
        dev_token = request.headers.get("X-Device-Token")
        raw = request.data if isinstance(request.data, dict) else {}
        dev_id = raw.get("id")
        if not dev_id or DEVICE_TOKENS.get(dev_id) != dev_token:
            return Response({"detail": "unauthorized"}, status=401)

        # Validación
        ser = IngestPayload(data=raw)
        if not ser.is_valid():
            return Response(ser.errors, status=400)
        v = ser.validated_data

        # Inferencia
        t0 = time.perf_counter()
        inf = classify_ecg(v["fs"], v["e"])
        latency_ms = int((time.perf_counter() - t0) * 1000)

        # Persistencia en DB
        blob = pack_int16_compressed(v["e"])
        buf = EcgBuffer.objects.create(
            device_id=v["id"],
            ts=v["ts"],
            fs_hz=v["fs"],
            bpm=v["b"],
            n_samples=len(v["e"]),
            ecg_blob=blob,
            compressed=True,
        )
        InferenceResult.objects.create(
            buffer=buf,
            label=inf["label"],
            score=inf["score"],
            latency_ms=latency_ms,
        )

        # Para el dashboard (RAM) + SSE
        ecg_plot = downsample_for_plot(v["e"], 250)
        payload = {
            "id": v["id"], "ts": v["ts"].isoformat(),
            "bpm": v["b"], "fs": v["fs"],
            "e": ecg_plot,
            "label": inf["label"], "score": inf["score"],
            "lat_ms": latency_ms,
        }
        RING.append(payload)
        _broadcast(payload)

        return Response({"ok": True, **inf, "lat_ms": latency_ms}, status=201)

class LatestView(APIView):
    authentication_classes = []
    permission_classes = []

    def get(self, request):
        k = int(request.GET.get("k", 30))
        data = list(RING)[-k:]
        return Response({"items": data}, status=200)
