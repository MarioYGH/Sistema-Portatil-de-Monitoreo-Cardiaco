from rest_framework import serializers

class IngestPayload(serializers.Serializer):
    id  = serializers.CharField(max_length=32)
    ts  = serializers.DateTimeField()
    b   = serializers.IntegerField(min_value=20, max_value=220)     # bpm
    fs  = serializers.IntegerField(min_value=100, max_value=2000)   # Hz
    e   = serializers.ListField(
            child=serializers.IntegerField(), min_length=20, max_length=2048
         )  # muestras int16
