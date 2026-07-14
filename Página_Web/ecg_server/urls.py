from django.contrib import admin
from django.urls import path, include
from dashboard.views import index
from dashboard import views as dash_views

urlpatterns = [
    path("admin/", admin.site.urls),
    path("", index, name="index"),
    path("api/", include("ingest.urls")),
    path("stream/", include((dash_views.urlpatterns_extra, "dashboard"))),
]
