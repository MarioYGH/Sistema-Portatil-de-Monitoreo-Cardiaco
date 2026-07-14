# Sistema Portátil de Monitoreo Cardiaco con ECG y Comunicación NB-IoT

<p align="center">
  <img src="https://img.shields.io/badge/ESP32--S3-Microcontroller-blue" />
  <img src="https://img.shields.io/badge/ECG-AD8232-red" />
  <img src="https://img.shields.io/badge/NB--IoT-SIM7020E-green" />
  <img src="https://img.shields.io/badge/MQTT-IoT-orange" />
  <img src="https://img.shields.io/badge/Django-Web%20Platform-purple" />
</p>

## Descripción general

Este proyecto consiste en el diseño e implementación de un **sistema portátil de adquisición, procesamiento y monitoreo remoto de señales electrocardiográficas (ECG)** utilizando un microcontrolador ESP32-S3, comunicación inalámbrica mediante NB-IoT y una plataforma web para visualización y almacenamiento de datos.

El sistema permite adquirir señales ECG provenientes de un sensor analógico AD8232, realizar procesamiento digital en tiempo real directamente sobre el dispositivo embebido y transmitir fragmentos de la señal hacia un servidor remoto mediante el protocolo MQTT.

La arquitectura propuesta busca integrar conceptos de:

* Sistemas embebidos.
* Procesamiento digital de señales.
* Internet de las cosas (IoT).
* Comunicación inalámbrica celular.
* Desarrollo de plataformas web para monitoreo remoto.

---

# Arquitectura del sistema

El sistema completo está dividido en tres capas principales:

```
                 SEÑAL ECG
                           │
                          ▼
        ┌─────────────┐
        │    Sensor AD8232     │
        │    Front-End ECG     │
        └─────────────┘
                          │
                         ▼
        ┌─────────────  ┐
        │ ESP32-S3                  │
        │ - Adquisición ADC     │
        │ - Filtrado FIR             │
        │ - FFT/IFFT                 │
        │ - Buffer circular          │
        │ - Empaquetamiento   │
        └────────────    ┘
                         │ UART
                        ▼
        ┌─────────────┐
        │         SIM7020E        │
        │           NB-IoT           │
        └─────────────┘
                        │ MQTT
                       ▼
        ┌────────────┐
        │     Broker MQTT    │
        │      Raspberry Pi    │
        └────────────┘
                       │
                      ▼
        ┌────────────┐
        │ Servidor Django     │
        │ Base de datos       │
        │ Visualización ECG │
        └────────────┘
```

---

# Características principales

## Adquisición de señal ECG

* Sensor utilizado: **AD8232 ECG Analog Front End**.
* Microcontrolador principal: **ESP32-S3**.
* Conversión mediante ADC interno.
* Frecuencia de muestreo:

```
Fs = 1000 Hz
```

* Resolución ADC:

```
12 bits
```

---

## Procesamiento digital de señales

El procesamiento se realiza directamente en el ESP32-S3 para reducir la cantidad de información transmitida.

Implementaciones principales:

### Eliminación de componente DC

Permite centrar la señal alrededor de cero para mejorar el procesamiento posterior.

### Suavizado mediante filtro móvil

Se utiliza un promedio móvil para reducir ruido de alta frecuencia.

### Filtrado FIR pasa banda

Filtro diseñado para conservar componentes fisiológicas del ECG:

```
0.5 Hz - 40 Hz
```

Características:

* Ventana Hamming.
* Diseño mediante combinación de filtros pasa bajas.
* Orden:

```
N = 513 coeficientes
```

### Procesamiento espectral

Se implementa procesamiento basado en FFT:

* Transformada rápida de Fourier.
* Filtrado en dominio frecuencial.
* Transformada inversa para reconstrucción temporal.

---

# Arquitectura de software embebido

El firmware fue desarrollado utilizando:

* ESP-IDF v5.x.
* FreeRTOS.

Se implementó una arquitectura productor-consumidor:

## Tarea Productora

Responsabilidades:

* Lectura ADC.
* Adquisición continua de muestras.
* Aplicación de preprocesamiento.
* Escritura en buffer circular.

## Tarea Consumidora

Responsabilidades:

* Lectura de bloques procesados.
* Ejecución del procesamiento DSP.
* Preparación de datos para transmisión.

Esta arquitectura permite mantener la adquisición continua evitando pérdida de muestras durante operaciones computacionalmente intensivas.

---

# Comunicación inalámbrica

## Módulo NB-IoT

Módulo utilizado:

```
SIM7020E
```

Comunicación:

```
ESP32-S3 ⇄ UART ⇄ SIM7020E
```

Configuración:

* Baudrate:

```
115200 bps
```

* Comunicación mediante comandos AT.
* Conexión celular NB-IoT.
* Publicación mediante MQTT.

---

# Protocolo MQTT

Los datos ECG son enviados utilizando MQTT debido a sus características:

* Bajo consumo de ancho de banda.
* Comunicación ligera.
* Modelo publicación/suscripción.
* Adecuado para dispositivos IoT.

Flujo:

```
ESP32-S3
     │
     ▼
 MQTT Publish
     │
     ▼
Broker MQTT
     │
     ▼
Servidor
```

---

# Plataforma Web

El backend fue desarrollado utilizando:

* Python.
* Django.
* Base de datos para almacenamiento de señales.
* Interfaz web para visualización remota.

Funciones principales:

* Recepción de datos ECG.
* Almacenamiento.
* Visualización de señales.
* Monitoreo remoto.

---

# Hardware utilizado

| Componente           | Modelo                            |
| -------------------- | --------------------------------- |
| Microcontrolador     | ESP32-S3                          |
| Sensor ECG           | AD8232                            |
| Comunicación celular | SIM7020E NB-IoT                   |
| RTC                  | DS1307                            |
| Alimentación         | Batería Li-Po                     |
| PCB                  | Diseño personalizado              |
| Carcasa              | Fabricación mediante impresión 3D |

---

# Organización del repositorio

```
Sistema-Portatil-de-Monitoreo-Cardiaco/

│
├── firmware/
│   ├── ESP32-S3/
│   └── ESP-IDF project
│
├── backend/
│   ├── Django server
│   └── API ingestion
│
├── hardware/
│   ├── PCB
│   └── Diagramas electrónicos
│
├── docs/
│   ├── Diagramas
│   └── Documentación técnica
│
└── README.md
```

---

# Requisitos

## Hardware

* ESP32-S3.
* AD8232.
* SIM7020E.
* Antena NB-IoT.
* Fuente de alimentación estable.

## Software

* ESP-IDF 5.x.
* Python 3.x.
* Django.
* Broker MQTT.

---

# Instalación

## Firmware ESP32

Clonar el repositorio:

```bash
git clone https://github.com/MarioYGH/Sistema-Portatil-de-Monitoreo-Cardiaco.git
```

Ingresar al proyecto ESP-IDF:

```bash
cd firmware
```

Compilar:

```bash
idf.py build
```

Cargar al ESP32:

```bash
idf.py flash monitor
```

---

# Resultados obtenidos

El prototipo desarrollado permite:

✅ Adquisición continua de señales ECG.
✅ Procesamiento digital en tiempo real.
✅ Comunicación remota mediante red celular NB-IoT.
✅ Transmisión mediante MQTT.
✅ Visualización remota mediante plataforma web.
✅ Implementación sobre hardware portátil.

---

# Trabajo futuro

Como líneas futuras de desarrollo se consideran:

* Implementación de detección automática de arritmias.
* Optimización del consumo energético.
* Validación clínica con bases de datos ECG.
* Integración de modelos de aprendizaje automático.
* Miniaturización adicional del hardware.

---

# Autor

**Mario Yahir García Hernández**

Proyecto desarrollado como parte del trabajo de investigación:

**"Adquisición, procesamiento y monitoreo remoto de señales ECG usando ESP32 con conexión NB-IoT"**

---

# Licencia

Este proyecto se encuentra disponible con fines educativos y de investigación.
