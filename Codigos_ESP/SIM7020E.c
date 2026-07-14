#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "SIM7020_TEST";

/* Pines (ajusta a tu wiring real) */
#define MODEM_UART_NUM UART_NUM_1
#define MODEM_TX_GPIO GPIO_NUM_10
#define MODEM_RX_GPIO GPIO_NUM_11
#define MODEM_PWRKEY_GPIO GPIO_NUM_4

#define MODEM_UART_BAUDRATE 115200
#define RX_BUF_SIZE 1024

// =======================================================
// CONFIGURACIÓN MQTT (Mosquitto expuesto por ngrok TCP)
// =======================================================
#define MQTT_BROKER_HOST "8.tcp.ngrok.io"
#define MQTT_BROKER_PORT_STR "14787"
#define MQTT_CLIENT_ID "ecg_d1"
#define MQTT_TOPIC "test"

// Si tu Mosquitto pide auth, usa strings (NO NULL). Si no, deja vacío.
#define MQTT_USERNAME ""
#define MQTT_PASSWORD ""

// =======================================================
// CONFIGURACIÓN DE BANDA NB-IoT
// Para Telcel México: banda 28 (700 MHz)
// Si usas AT&T México: cambia a "AT+CBAND=5" (banda 5, 850 MHz)
// =======================================================
#define NB_IOT_BAND_CMD "AT+CBAND=5"

static int g_mqtt_id = -1;

// Buffers globales
static char g_resp_big[2048];
static char g_resp[1024];
static char g_cmd[512];

// -------------------------------------------------------
// Encendido del módulo
// -------------------------------------------------------
static void sim7020_power_on(void) {
  ESP_LOGI(TAG, "Pulsando PWRKEY para encender el modulo");

  gpio_config_t io_conf = {.pin_bit_mask = 1ULL << MODEM_PWRKEY_GPIO,
                           .mode = GPIO_MODE_OUTPUT,
                           .pull_up_en = GPIO_PULLUP_DISABLE,
                           .pull_down_en = GPIO_PULLDOWN_DISABLE,
                           .intr_type = GPIO_INTR_DISABLE};
  gpio_config(&io_conf);

  // PWRKEY activo bajo
  gpio_set_level(MODEM_PWRKEY_GPIO, 1);
  vTaskDelay(pdMS_TO_TICKS(100));

  gpio_set_level(MODEM_PWRKEY_GPIO, 0);
  vTaskDelay(pdMS_TO_TICKS(1200));
  gpio_set_level(MODEM_PWRKEY_GPIO, 1);

  vTaskDelay(pdMS_TO_TICKS(5000));
}

// -------------------------------------------------------
// Envía comando AT y acumula respuesta en `out`.
// Devuelve true si vio "OK" y false si vio "ERROR".
// Busca OK/ERROR en el buffer ACUMULADO (no en cada fragmento)
// para evitar perder respuestas que llegan partidas.
// -------------------------------------------------------
static bool sim7020_send_cmd_collect(const char *cmd, int timeout_ms, char *out,
                                     size_t out_sz) {
  uint8_t rx_data[RX_BUF_SIZE];
  int len;

  if (out && out_sz)
    out[0] = '\0';

  ESP_LOGI(TAG, ">> %s", cmd);

  // Flush RX — vaciar cualquier dato residual en el buffer UART
  while ((len = uart_read_bytes(MODEM_UART_NUM, rx_data, sizeof(rx_data),
                                pdMS_TO_TICKS(20))) > 0) {
  }

  // Enviar comando + CRLF
  uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
  uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);

  int64_t start = esp_timer_get_time() / 1000;
  bool saw_ok = false;
  bool saw_err = false;

  while ((esp_timer_get_time() / 1000 - start) < timeout_ms) {
    len = uart_read_bytes(MODEM_UART_NUM, rx_data, sizeof(rx_data) - 1,
                          pdMS_TO_TICKS(100));
    if (len > 0) {
      rx_data[len] = 0;
      ESP_LOGI(TAG, "<< %s", (char *)rx_data);

      // Acumular en el buffer de salida
      if (out && out_sz) {
        size_t cur = strlen(out);
        size_t add = (size_t)len;
        if (cur + add + 1 < out_sz) {
          memcpy(out + cur, rx_data, add);
          out[cur + add] = '\0';
        }
      }

      // Buscar OK/ERROR en el buffer ACUMULADO, no en el fragmento actual.
      // Esto evita perder un "OK" que llega partido entre dos lecturas.
      if (out) {
        if (strstr(out, "ERROR"))
          saw_err = true;
        if (strstr(out, "\r\nOK\r\n") || strstr(out, "\nOK\r\n") ||
            strstr(out, "\nOK\n")) {
          saw_ok = true;
        }
        // Fallback: si el buffer termina con "OK\r\n" o solo "OK"
        size_t out_len = strlen(out);
        if (out_len >= 2) {
          // Buscar "OK" al final del buffer (con posible \r\n trailing)
          const char *tail = out + out_len;
          // Retroceder saltos de línea
          while (tail > out && (*(tail - 1) == '\r' || *(tail - 1) == '\n'))
            tail--;
          if (tail - out >= 2 && tail[-2] == 'O' && tail[-1] == 'K') {
            // Verificar que antes de "OK" hay un \n o es inicio de buffer
            if (tail - out == 2 || tail[-3] == '\n') {
              saw_ok = true;
            }
          }
        }
      }

      if (saw_ok || saw_err)
        break;
    }
  }

  if (!saw_ok && !saw_err) {
    ESP_LOGW(TAG, "Timeout esperando respuesta a: %s", cmd);
  }

  return (saw_ok && !saw_err);
}

// -------------------------------------------------------
// Parse helper: extrae entero después de "+CMQNEW:"
// -------------------------------------------------------
static bool parse_cmqnew_id(const char *resp, int *out_id) {
  const char *p = strstr(resp, "+CMQNEW:");
  if (!p)
    return false;

  p += strlen("+CMQNEW:");
  while (*p == ' ' || *p == '\t')
    p++;

  int id = atoi(p);
  if (id < 0 || id > 4)
    return false;

  *out_id = id;
  return true;
}

// -------------------------------------------------------
// Parse helper: +CEREG: <n>,<stat>
// stat 1=home, 5=roaming
// -------------------------------------------------------
static bool parse_cereg_stat(const char *resp, int *out_stat) {
  const char *p = strstr(resp, "+CEREG:");
  if (!p)
    return false;

  int n = -1, stat = -1;
  if (sscanf(p, "+CEREG: %d,%d", &n, &stat) == 2) {
    *out_stat = stat;
    return true;
  }
  return false;
}

// -------------------------------------------------------
// Espera a que el módulo responda "AT" -> "OK"
// Útil después de un reset para confirmar que ya arrancó.
// -------------------------------------------------------
static bool wait_for_module_ready(int max_attempts) {
  for (int i = 0; i < max_attempts; i++) {
    ESP_LOGI(TAG, "Esperando modulo listo... intento %d/%d", i + 1,
             max_attempts);
    if (sim7020_send_cmd_collect("AT", 2000, g_resp, sizeof(g_resp))) {
      ESP_LOGI(TAG, "Modulo respondió OK");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
  ESP_LOGE(TAG, "Modulo no respondió después de %d intentos", max_attempts);
  return false;
}

static bool wait_for_network_registration(int total_wait_ms) {
  int waited = 0;
  while (waited < total_wait_ms) {
    sim7020_send_cmd_collect("AT+CEREG?", 3000, g_resp, sizeof(g_resp));

    int stat = -1;
    if (parse_cereg_stat(g_resp, &stat)) {
      ESP_LOGI(TAG, "CEREG stat=%d", stat);
      if (stat == 1 || stat == 5)
        return true; // registrado
    }

    vTaskDelay(pdMS_TO_TICKS(3000));
    waited += 3000;
  }
  return false;
}

// -------------------------------------------------------
// Secuencia inicial para SIM7020E (basada en WizzDev)
// -------------------------------------------------------
static bool sim7020_initial_configuration(void) {
  // Verificar comunicación básica con el módulo
  if (!sim7020_send_cmd_collect("AT", 2000, g_resp, sizeof(g_resp)))
    return false;

  // Apagar echo para simplificar parseo de respuestas
  sim7020_send_cmd_collect("ATE0", 2000, g_resp, sizeof(g_resp));

  // Habilitar mensajes de error detallados
  sim7020_send_cmd_collect("AT+CMEE=2", 2000, g_resp, sizeof(g_resp));

  // Fijar baudrate
  sim7020_send_cmd_collect("AT+IPR=115200", 3000, g_resp, sizeof(g_resp));

  // Deshabilitar sleep
  sim7020_send_cmd_collect("AT+CSCLK=0", 3000, g_resp, sizeof(g_resp));

  // Selección de banda NB-IoT
  // Telcel México: banda 28 (700 MHz)
  // Si usas AT&T México: cambia NB_IOT_BAND_CMD a "AT+CBAND=5"
  sim7020_send_cmd_collect(NB_IOT_BAND_CMD, 3000, g_resp, sizeof(g_resp));

  // Apagar RF para poder configurar APN
  sim7020_send_cmd_collect("AT+CFUN=0", 5000, g_resp, sizeof(g_resp));

  // Configurar APN
  sim7020_send_cmd_collect("AT*MCGDEFCONT=\"IP\",\"terminal.apn\"", 5000,
                           g_resp, sizeof(g_resp));

  // Reset del módulo para aplicar la configuración
  sim7020_send_cmd_collect("AT+CRESET", 5000, g_resp, sizeof(g_resp));

  // Esperar >=30s para que el módulo reinicie completamente
  ESP_LOGI(TAG, "Esperando 30s para que el modulo reinicie...");
  vTaskDelay(pdMS_TO_TICKS(30000));

  // Verificar que el módulo ya arrancó enviando AT hasta que responda OK
  if (!wait_for_module_ready(10)) {
    ESP_LOGE(TAG, "Modulo no arrancó después del reset.");
    return false;
  }

  // Re-habilitar radio explícitamente después del reset
  sim7020_send_cmd_collect("AT+CFUN=1", 5000, g_resp, sizeof(g_resp));

  // Apagar echo de nuevo (el reset lo pudo haber reactivado)
  sim7020_send_cmd_collect("ATE0", 2000, g_resp, sizeof(g_resp));

  // Re-habilitar errores detallados (se pierden con el reset)
  sim7020_send_cmd_collect("AT+CMEE=2", 2000, g_resp, sizeof(g_resp));

  // Verificar SIM lista
  sim7020_send_cmd_collect("AT+CPIN?", 5000, g_resp, sizeof(g_resp));
  if (!strstr(g_resp, "READY")) {
    ESP_LOGW(TAG, "SIM no reporta READY. Resp: %s", g_resp);
    // No retornar false — puede que aún funcione, solo advertir
  }

  // Verificar señal
  sim7020_send_cmd_collect("AT+CSQ", 3000, g_resp, sizeof(g_resp));

  // Espera registro en la red (hasta 90s)
  if (!wait_for_network_registration(90000)) {
    ESP_LOGE(TAG, "No se registró a la red (CEREG != 1/5).");
    return false;
  }

  // Attach explícito a la red de datos
  sim7020_send_cmd_collect("AT+CGATT=1", 30000, g_resp, sizeof(g_resp));

  return true;
}

// -------------------------------------------------------
// Abre y conecta MQTT (CREVHEX, CMQNEW, CMQCON, CMQSUB)
// -------------------------------------------------------
static bool sim7020_mqtt_open_and_connect(void) {
  // Formato raw para recibir datos legibles (importante en SIM7020E)
  sim7020_send_cmd_collect("AT+CREVHEX=0", 5000, g_resp, sizeof(g_resp));

  // CMQNEW: crear conexión TCP al broker MQTT
  // host y puerto van como strings entre comillas, buffer=1024
  snprintf(g_cmd, sizeof(g_cmd), "AT+CMQNEW=\"%s\",\"%s\",12000,1024",
           MQTT_BROKER_HOST, MQTT_BROKER_PORT_STR);

  if (!sim7020_send_cmd_collect(g_cmd, 60000, g_resp_big, sizeof(g_resp_big))) {
    ESP_LOGE(TAG, "CMQNEW falló. Resp: %s", g_resp_big);
    return false;
  }

  int id = -1;
  if (!parse_cmqnew_id(g_resp_big, &id)) {
    ESP_LOGE(TAG, "No pude parsear +CMQNEW:<id>. Resp:\n%s", g_resp_big);
    return false;
  }
  g_mqtt_id = id;
  ESP_LOGI(TAG, "mqtt_id asignado = %d", g_mqtt_id);

  // CMQCON: conectar al broker MQTT
  // version=4 (MQTT 3.1.1), keepalive=600s, cleansession=1
  if (MQTT_USERNAME[0] != '\0' && MQTT_PASSWORD[0] != '\0') {
    snprintf(g_cmd, sizeof(g_cmd),
             "AT+CMQCON=%d,4,\"%s\",600,1,0,\"%s\",\"%s\"", g_mqtt_id,
             MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
  } else {
    snprintf(g_cmd, sizeof(g_cmd), "AT+CMQCON=%d,4,\"%s\",600,1,0", g_mqtt_id,
             MQTT_CLIENT_ID);
  }

  if (!sim7020_send_cmd_collect(g_cmd, 60000, g_resp_big, sizeof(g_resp_big))) {
    ESP_LOGE(TAG, "CMQCON falló. Resp: %s", g_resp_big);
    return false;
  }

  // Dar tiempo para que la conexión MQTT se estabilice
  vTaskDelay(pdMS_TO_TICKS(2000));

  return true;
}

// -------------------------------------------------------
// Publica MQTT con payload en texto plano.
// Con AT+CREVHEX=0 el SIM7020 acepta ASCII directo.
//
// Formato: AT+CMQPUB=<id>,"<topic>",<qos>,<retained>,<dup>,
//          <message_length>,"<message>"
// -------------------------------------------------------
static bool sim7020_mqtt_publish(const char *topic, const char *payload) {
  if (g_mqtt_id < 0)
    return false;

  int payload_len = (int)strlen(payload);

  // QoS=0, retained=0, dup=0 (coincide con mosquitto_pub -q 0)
  snprintf(g_cmd, sizeof(g_cmd), "AT+CMQPUB=%d,\"%s\",0,0,0,%d,\"%s\"",
           g_mqtt_id, topic, payload_len, payload);

  ESP_LOGI(TAG, "Publicando MQTT: topic=%s, payload=\"%s\" (len=%d)", topic,
           payload, payload_len);
  return sim7020_send_cmd_collect(g_cmd, 30000, g_resp_big, sizeof(g_resp_big));
}

// -------------------------------------------------------
static void sim7020_mqtt_disconnect(void) {
  if (g_mqtt_id < 0)
    return;
  snprintf(g_cmd, sizeof(g_cmd), "AT+CMQDISCON=%d", g_mqtt_id);
  sim7020_send_cmd_collect(g_cmd, 20000, g_resp, sizeof(g_resp));
  g_mqtt_id = -1;
}

// -------------------------------------------------------
void app_main(void) {
  ESP_LOGI(TAG, "Inicializando UART para SIM7020");

  const uart_config_t uart_config = {
      .baud_rate = MODEM_UART_BAUDRATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };

  ESP_ERROR_CHECK(
      uart_driver_install(MODEM_UART_NUM, RX_BUF_SIZE * 2, 0, 0, NULL, 0));
  ESP_ERROR_CHECK(uart_param_config(MODEM_UART_NUM, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(MODEM_UART_NUM, MODEM_TX_GPIO, MODEM_RX_GPIO,
                               UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

  sim7020_power_on();

  ESP_LOGI(TAG, "=== SECUENCIA INICIAL SIM7020E ===");
  if (!sim7020_initial_configuration()) {
    ESP_LOGE(TAG, "Fallo en configuracion inicial / registro de red.");
    while (1)
      vTaskDelay(pdMS_TO_TICKS(1000));
  }

  ESP_LOGI(TAG, "=== MQTT: CMQNEW/CMQCON/CMQSUB/CMQPUB ===");
  if (sim7020_mqtt_open_and_connect()) {
    // Publicar mensaje de prueba
    if (!sim7020_mqtt_publish(MQTT_TOPIC, "helloWorldFromSIM7020")) {
      ESP_LOGE(TAG, "Publish falló.");
    } else {
      ESP_LOGI(TAG, "Publish exitoso!");
    }
  } else {
    ESP_LOGE(TAG, "No se pudo abrir/conectar MQTT. Revisa "
                  "broker/puerto/ngrok/mosquitto y logs del broker.");
  }

  sim7020_mqtt_disconnect();

  ESP_LOGI(TAG, "Terminado. Revisa mosquitto_sub del lado Raspberry.");
  while (1)
    vTaskDelay(pdMS_TO_TICKS(1000));
}
