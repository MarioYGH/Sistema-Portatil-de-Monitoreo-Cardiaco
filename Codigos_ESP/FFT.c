//
// ============================================================================
//  Adquisición continua de ECG en ESP32-S3 + filtrado en frecuencia (0.5–40 Hz)
//  y salida SOLO de la señal filtrada por stdout (texto o binario).
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_system.h"

#include "esp_adc/adc_continuous.h"
#include "esp_dsp.h"
#include "esp_heap_caps.h"

// === Calibración ADC (ESP-IDF v5.x) ===
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

// ------------------------ Parámetros DSP/ventana -----------------------------
#define FS            1000.0f
#define NUM_MUESTRAS  1024
#define FFT_SIZE      2048
#define TAPAS         513
#define GANANCIA_VISUAL 1.0f

// --- Salida: texto vs binario ---
#define OUTPUT_BINARY   0
#define PRINT_DECIMALS  4
#define PRINT_EVERY     4

// ------------------------ Config ADC continuo (DMA) --------------------------
#define ADC_UNIT        ADC_UNIT_1
#define ADC_CHANNEL     ADC_CHANNEL_8
#define ADC_ATTEN       ADC_ATTEN_DB_12
#define ADC_BIT_WIDTH   ADC_BITWIDTH_12
#define SAMPLE_FREQ_HZ  ((uint32_t)FS)
#define FRAME_SIZE      256
#define STORE_SIZE      1024

// ------------------------ Multisampling (promedio móvil) ---------------------
#define USE_MOVING_AVG     1      // 1=activar, 0=desactivar
#define MULTISAMPLE_WIN    8      // ventana del promedio móvil (p.ej., 4–16)

// ------------------------ Buffers y sincronización ---------------------------
static float buffer[NUM_MUESTRAS];
static int write_index = 0;

static adc_continuous_handle_t adc_handle = NULL;
static SemaphoreHandle_t semaforo_fft;
static float *H_bp = NULL;

// Calibración
static adc_cali_handle_t adc_cali_handle = NULL;
static bool adc_cali_enabled = false;

// ------------------------ Salida unificada -----------------------------------
static inline void salida_filtrada(float v) {
#if OUTPUT_BINARY
    fwrite(&v, sizeof(float), 1, stdout);
#else
    printf("%.*f\r\n", PRINT_DECIMALS, v);
#endif
}

// ------------------------ Calibración ADC ------------------------------------
static bool init_adc_calibration(adc_unit_t unit, adc_channel_t chan,
                                 adc_atten_t atten, adc_bitwidth_t bitwidth)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal_cfg = {
        .unit_id  = unit,
        .chan     = chan,
        .atten    = atten,
        .bitwidth = bitwidth,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal_cfg, &adc_cali_handle) == ESP_OK) {
        return true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cal_cfg2 = {
        .unit_id  = unit,
        .atten    = atten,
        .bitwidth = bitwidth,
    };
    if (adc_cali_create_scheme_line_fitting(&cal_cfg2, &adc_cali_handle) == ESP_OK) {
        return true;
    }
#endif
    return false;
}

static inline float raw_to_volts(uint16_t raw)
{
    if (adc_cali_enabled && adc_cali_handle) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(adc_cali_handle, (int)raw, &mv) == ESP_OK) {
            return (float)mv / 1000.0f;
        }
    }
    return (raw * 3.3f) / 4095.0f; // fallback aproximado
}

// ------------------------ Config ADC continuo (DMA) --------------------------
static void config_ADC_continuous(void) {
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = STORE_SIZE,
        .conv_frame_size    = FRAME_SIZE,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &adc_handle));

    adc_digi_pattern_config_t pattern = {
        .atten     = ADC_ATTEN,
        .channel   = ADC_CHANNEL,
        .unit      = 0, // ADC1
        .bit_width = ADC_BIT_WIDTH,
    };

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = SAMPLE_FREQ_HZ,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
        .pattern_num    = 1,
        .adc_pattern    = &pattern,
    };

    ESP_ERROR_CHECK(adc_continuous_config(adc_handle, &dig_cfg));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle));
}

// ------------------------ Diseño FIR -----------------------------------------
static float* generar_fir(float fc, int N) {
    float *h = (float *)malloc(N * sizeof(float));
    if (!h) return NULL;
    for (int k = 0; k < N; k++) {
        float n = k - (N - 1) / 2.0f;
        float sinc_val = (fabsf(n) < 1e-6f) ? 1.0f : sinf(2 * M_PI * fc * n) / (M_PI * n);
        float hamming  = 0.54f - 0.46f * cosf(2 * M_PI * k / (N - 1));
        h[k] = 2 * fc * sinc_val * hamming;
    }
    return h;
}

static void precomputar_filtro_bp(void) {
    float *lp_lo = generar_fir(0.5f  / (FS / 2), TAPAS);
    float *lp_hi = generar_fir(40.0f / (FS / 2), TAPAS);

    H_bp = heap_caps_calloc(FFT_SIZE * 2, sizeof(float), MALLOC_CAP_8BIT);
    for (int i = 0; i < FFT_SIZE; i++) {
        H_bp[2*i]   = (i < TAPAS) ? (lp_hi[i] - lp_lo[i]) : 0.0f;
        H_bp[2*i+1] = 0.0f;
    }
    free(lp_lo); free(lp_hi);

    dsps_fft2r_fc32(H_bp, FFT_SIZE);
    dsps_bit_rev2r_fc32(H_bp, FFT_SIZE);
}

static void aplicar_filtro_en_frecuencia(float *fft_data, float *h_fir, int N) {
    for (int k = 0; k < N; k++) {
        float xr = fft_data[2 * k],     xi = fft_data[2 * k + 1];
        float hr = h_fir[2 * k],        hi = h_fir[2 * k + 1];
        float re = xr * hr - xi * hi;
        float im = xr * hi + xi * hr;
        fft_data[2 * k]     = re;
        fft_data[2 * k + 1] = im;
    }
}

// ------------------------ Procesamiento por bloques --------------------------
static void procesar_ECG(void) {
    static __attribute__((aligned(16))) float input[FFT_SIZE * 2] = {0};

    // Copiar ventana + zero-padding
    for (int i = 0; i < NUM_MUESTRAS; i++) {
        int idx = (write_index - NUM_MUESTRAS + i + NUM_MUESTRAS) % NUM_MUESTRAS;
        input[2*i]   = buffer[idx];
        input[2*i+1] = 0.0f;
    }
    for (int i = NUM_MUESTRAS; i < FFT_SIZE; i++) {
        input[2*i]   = 0.0f;
        input[2*i+1] = 0.0f;
    }

    // Quitar DC
    float mean = 0;
    for (int i = 0; i < NUM_MUESTRAS; i++) mean += input[2*i];
    mean /= NUM_MUESTRAS;
    for (int i = 0; i < NUM_MUESTRAS; i++) input[2*i] -= mean;

    // FFT -> Filtrado -> IFFT
    dsps_fft2r_fc32(input, FFT_SIZE);
    dsps_bit_rev2r_fc32(input, FFT_SIZE);
    aplicar_filtro_en_frecuencia(input, H_bp, FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; i++) input[2*i+1] *= -1.0f;
    dsps_fft2r_fc32(input, FFT_SIZE);
    dsps_bit_rev2r_fc32(input, FFT_SIZE);

    // Emitir solo filtrada (diezmada para salida)
    for (int i = 0; i < NUM_MUESTRAS; i++) {
        if ((i % PRINT_EVERY) == 0) {
            float filtrada = (input[2*i] / FFT_SIZE) * GANANCIA_VISUAL;
            salida_filtrada(filtrada);
        }
    }
}

// ------------------------ Tarea de lectura ADC (productora) ------------------
// Con promedio móvil deslizante sobre V_cal (no cambia Fs)
static void read_task_continuous(void *pvParameters) {
    uint8_t result[FRAME_SIZE];
    adc_digi_output_data_t *sample;
    static bool pendiente_fft = false;

#if USE_MOVING_AVG
    // Estado del promedio móvil
    static float ma_buf[MULTISAMPLE_WIN] = {0};
    static int   ma_idx = 0;
    static float ma_sum = 0.0f;
    const float invN = 1.0f / (float)MULTISAMPLE_WIN;
#endif

    while (1) {
        uint32_t bytes_read = 0;
        esp_err_t ret = adc_continuous_read(adc_handle, result, FRAME_SIZE, &bytes_read, 1000);
        if (ret == ESP_OK && bytes_read > 0) {
            for (int i = 0; i < bytes_read; i += sizeof(adc_digi_output_data_t)) {
                sample = (adc_digi_output_data_t*)&result[i];

                if (sample->type2.unit == 0 && sample->type2.channel == ADC_CHANNEL) {
                    uint16_t raw = sample->type2.data;

                    // 1) raw -> voltios (calibrado o fallback)
                    float v = raw_to_volts(raw);

#if USE_MOVING_AVG
                    // 2) Promedio móvil deslizante (mantiene Fs)
                    ma_sum += v - ma_buf[ma_idx];
                    ma_buf[ma_idx] = v;
                    ma_idx = (ma_idx + 1) % MULTISAMPLE_WIN;
                    float v_smooth = ma_sum * invN;
#else
                    float v_smooth = v;
#endif

                    // 3) Escribir al buffer circular
                    buffer[write_index] = v_smooth;
                    write_index = (write_index + 1) % NUM_MUESTRAS;

                    // 4) Disparo de procesamiento cada 128 nuevas muestras
                    if (!pendiente_fft && (write_index % 128 == 0)) {
                        xSemaphoreGive(semaforo_fft);
                        pendiente_fft = true;
                    }
                    if (write_index % 128 != 0) pendiente_fft = false;
                }
            }
        }
    }
}

// ------------------------ Tarea de FFT (consumidora) -------------------------
static void fft_task(void *pvParameters) {
    while (1) {
        if (xSemaphoreTake(semaforo_fft, portMAX_DELAY)) {
            procesar_ECG();
        }
    }
}

// ------------------------ app_main -------------------------------------------
void app_main(void) {
    esp_log_level_set("*", ESP_LOG_ERROR);

    // ADC continuo + calibración
    config_ADC_continuous();
    adc_cali_enabled = init_adc_calibration(ADC_UNIT, ADC_CHANNEL, ADC_ATTEN, ADC_BIT_WIDTH);

    // FFT y filtro
    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, FFT_SIZE));
    semaforo_fft = xSemaphoreCreateBinary();
    precomputar_filtro_bp();

    // Tareas
    xTaskCreatePinnedToCore(read_task_continuous, "ReadECG_DMA", 4096, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(fft_task,           "FFTProc",     8192, NULL, 3, NULL, 0);
}
