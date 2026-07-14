from django.contrib import admin
from .models import EcgBuffer, InferenceResult

@admin.register(EcgBuffer)
class EcgBufferAdmin(admin.ModelAdmin):
    list_display = ("device_id", "ts", "fs_hz", "bpm", "n_samples", "compressed", "created_at")
    list_filter = ("device_id", "fs_hz", "compressed", "created_at")
    search_fields = ("device_id",)

@admin.register(InferenceResult)
class InferenceResultAdmin(admin.ModelAdmin):
    list_display = ("buffer", "label", "score", "latency_ms", "created_at")
    list_filter = ("label", "created_at")
    search_fields = ("buffer__device_id", "label")
