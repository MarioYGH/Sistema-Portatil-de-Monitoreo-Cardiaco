from pathlib import Path
import os

# === Rutas base ===
BASE_DIR = Path(__file__).resolve().parent.parent

# === Seguridad / Debug (ajústalo después) ===
SECRET_KEY = os.environ.get("DJANGO_SECRET_KEY", "dev-only-change-me")
DEBUG = False  # pon False cuando pases a producción

# IP/hosts permitidos (incluye tu Raspberry)
ALLOWED_HOSTS = [
    "localhost",
    "127.0.0.1",
    "raspberrypi",
    "raspberrypi.local",
    ".trycloudflare.com",
    "10.132.108.208",  # LAN de tu Pi
    # agrega dominio cuando tengas (p.ej. "midominio.com")
]

# (Si usas HTTPS público con dominio, agrega)
CSRF_TRUSTED_ORIGINS = ["https://*.trycloudflare.com"]

SECURE_PROXY_SSL_HEADER = ("HTTP_X_FORWARDED_PROTO","https")

# === Apps ===
INSTALLED_APPS = [
    "django.contrib.admin",
    "django.contrib.auth",
    "django.contrib.contenttypes",
    "django.contrib.sessions",
    "django.contrib.messages",
    "django.contrib.staticfiles",
    "rest_framework",
    "ingest",
    "dashboard",
]

# === Middleware ===
MIDDLEWARE = [
    "django.middleware.security.SecurityMiddleware",
    "whitenoise.middleware.WhiteNoiseMiddleware",     # sirve estáticos sin nginx (dev)
    "django.contrib.sessions.middleware.SessionMiddleware",
    "django.middleware.common.CommonMiddleware",
    "django.middleware.csrf.CsrfViewMiddleware",
    "django.contrib.auth.middleware.AuthenticationMiddleware",
    "django.contrib.messages.middleware.MessageMiddleware",
    "django.middleware.clickjacking.XFrameOptionsMiddleware",
]

# === Enrutado principal (¡esto es lo que faltaba!) ===
ROOT_URLCONF = "ecg_server.urls"
WSGI_APPLICATION = "ecg_server.wsgi.application"  # para runserver/gunicorn WSGI
# (Opcional) si alguna vez migras a ASGI:
ASGI_APPLICATION = "ecg_server.asgi.application"

# === Templates ===
TEMPLATES = [{
    "BACKEND": "django.template.backends.django.DjangoTemplates",
    "DIRS": [BASE_DIR / "templates"],
    "APP_DIRS": True,
    "OPTIONS": {"context_processors": [
        "django.template.context_processors.debug",
        "django.template.context_processors.request",
        "django.contrib.auth.context_processors.auth",
        "django.contrib.messages.context_processors.messages",
    ]},
}]

# === Base de datos (SQLite por compatibilidad; no la usamos aún) ===
DATABASES = {
    "default": {
        "ENGINE": "django.db.backends.sqlite3",
        "NAME": BASE_DIR / "db.sqlite3",
    }
}

# === Zona horaria / i18n ===
LANGUAGE_CODE = "es-mx"
TIME_ZONE = "America/Mexico_City"
USE_I18N = True
USE_TZ = True

# === Archivos estáticos ===
STATIC_URL = "/static/"
STATIC_ROOT = BASE_DIR / "staticfiles"
STATICFILES_DIRS = [BASE_DIR / "static"]
STORAGES = {
    "default": {"BACKEND": "django.core.files.storage.FileSystemStorage"},
    "staticfiles": {"BACKEND": "django.contrib.staticfiles.storage.StaticFilesStorage"},
}

# === Límites de subida (buffers ECG compactos) ===
DATA_UPLOAD_MAX_MEMORY_SIZE = 64 * 1024  # 64 KB aprox

# === REST Framework (mínimo) ===
REST_FRAMEWORK = {}
