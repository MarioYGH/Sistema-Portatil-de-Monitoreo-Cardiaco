from django.urls import path
from .views import IngestView, LatestView

urlpatterns = [
    path("ingest", IngestView.as_view(), name="ingest"),
    path("latest", LatestView.as_view(), name="latest"),
]
