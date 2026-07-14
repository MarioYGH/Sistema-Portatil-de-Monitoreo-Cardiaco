from django.db import models

class EcgBuffer(models.Model):
    """
    Un paquete de datos ECG recibido del dispositivo.
    - Guardamos la señal como binario (int16 empaquetado y, opcionalmente, comprimido).
    """
    device_id   = models.CharField(max_length=64, db_index=True)
    ts          = models.DateTimeField(db_index=True)  # timestamp que manda el dispositivo
    fs_hz       = models.PositiveIntegerField()        # frecuencia de muestreo
    bpm         = models.PositiveIntegerField()        # bpm reportado
    n_samples   = models.PositiveIntegerField()        # número de muestras crudas
    ecg_blob    = models.BinaryField()                 # array int16 empaquetado (posible compresión)
    compressed  = models.BooleanField(default=True)    # marcamos si se comprimió o no

    created_at  = models.DateTimeField(auto_now_add=True, db_index=True)

    class Meta:
        indexes = [
            models.Index(fields=["device_id", "ts"]),
        ]
        ordering = ["-ts"]

    def __str__(self):
        return f"{self.device_id} @ {self.ts.isoformat()} ({self.n_samples} smp)"


class InferenceResult(models.Model):
    """
    Resultado de clasificación ligado 1:1 a un buffer.
    """
    buffer      = models.OneToOneField(EcgBuffer, on_delete=models.CASCADE, related_name="inference")
    label       = models.CharField(max_length=64)
    score       = models.FloatField()
    latency_ms  = models.PositiveIntegerField()
    created_at  = models.DateTimeField(auto_now_add=True, db_index=True)

    def __str__(self):
        return f"{self.label} ({self.score:.3f})"
