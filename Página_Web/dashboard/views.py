from django.shortcuts import render
from django.urls import path
from .sse import sse_view

def index(request):
    return render(request, "index.html")

# Rutas extra (SSE)
urlpatterns_extra = [
    path("sse", sse_view, name="sse"),
]
