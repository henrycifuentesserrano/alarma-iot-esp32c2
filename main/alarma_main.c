#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"

#define AWS_ENDPOINT      "ar3tq3jydqahr-ats.iot.us-east-1.amazonaws.com"
#define AWS_THING_NAME    "esp32-rele-finca"
#define TOPIC_CONTROL     "finca/rele/control"
#define TOPIC_STATUS      "finca/rele/status"

#define PIN_RELE          GPIO_NUM_5
#define LED_RELE          GPIO_NUM_10
#define LED_WIFI          GPIO_NUM_18
#define BTN_RESET         GPIO_NUM_9

#define NVS_NAMESPACE     "wifi_config"
#define NVS_KEY_SSID      "ssid"
#define NVS_KEY_PASS      "password"
#define NVS_KEY_LAT       "latitude"
#define NVS_KEY_LON       "longitude"

static const char *TAG = "aws-rele";

extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[]   asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[]     asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[]       asm("_binary_private_pem_key_end");
extern const uint8_t root_ca_pem_start[]         asm("_binary_root_ca_pem_start");
extern const uint8_t root_ca_pem_end[]           asm("_binary_root_ca_pem_end");

typedef enum {
    ESTADO_INICIANDO,
    ESTADO_CONECTANDO_WIFI,
    ESTADO_CONECTANDO_AWS,
    ESTADO_OPERANDO,
    ESTADO_RECONECTANDO,
    ESTADO_PROVISIONING
} estado_t;

static estado_t estado_actual = ESTADO_INICIANDO;
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static esp_mqtt_client_handle_t mqtt_client = NULL;
static httpd_handle_t server = NULL;
static char wifi_ssid[64] = {0};
static char wifi_pass[64] = {0};
static char device_lat[20] = {0};
static char device_lon[20] = {0};

// Tarea LED
static void tarea_led(void *pvParameters)
{
    while (1) {
        switch (estado_actual) {
            case ESTADO_INICIANDO:
            case ESTADO_CONECTANDO_WIFI:
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(500));
                gpio_set_level(LED_WIFI, 0);
                vTaskDelay(pdMS_TO_TICKS(500));
                break;
            case ESTADO_CONECTANDO_AWS:
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(250));
                gpio_set_level(LED_WIFI, 0);
                vTaskDelay(pdMS_TO_TICKS(250));
                break;
            case ESTADO_OPERANDO:
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                break;
            case ESTADO_RECONECTANDO:
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                gpio_set_level(LED_WIFI, 0);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case ESTADO_PROVISIONING:
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_WIFI, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(LED_WIFI, 0);
                vTaskDelay(pdMS_TO_TICKS(700));
                break;
        }
    }
}

// NVS
static bool nvs_load_wifi(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t len = sizeof(wifi_ssid);
    bool ok = (nvs_get_str(nvs, NVS_KEY_SSID, wifi_ssid, &len) == ESP_OK);
    len = sizeof(wifi_pass);
    ok &= (nvs_get_str(nvs, NVS_KEY_PASS, wifi_pass, &len) == ESP_OK);
    len = sizeof(device_lat);
    nvs_get_str(nvs, NVS_KEY_LAT, device_lat, &len);
    len = sizeof(device_lon);
    nvs_get_str(nvs, NVS_KEY_LON, device_lon, &len);
    nvs_close(nvs);
    return ok && strlen(wifi_ssid) > 0;
}

static void nvs_save_wifi(const char *ssid, const char *pass, const char *lat, const char *lon)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    nvs_set_str(nvs, NVS_KEY_PASS, pass);
    nvs_set_str(nvs, NVS_KEY_LAT, lat);
    nvs_set_str(nvs, NVS_KEY_LON, lon);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "WiFi guardado: %s", ssid);
}

static void nvs_clear_wifi(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Credenciales borradas");
}

// Relé
static void set_rele(int state)
{
    gpio_set_level(PIN_RELE, state);
    gpio_set_level(LED_RELE, state);
    ESP_LOGI(TAG, "Rele %s", state ? "ON" : "OFF");
    if (mqtt_client) {
        char payload[32];
	snprintf(payload, sizeof(payload), "{\"rele\":%d}", state);
        esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS, payload, 0, 1, 0);
    }
}

// Botón reset
static void tarea_boton(void *pvParameters)
{
    int contador = 0;
    while (1) {
        if (gpio_get_level(BTN_RESET) == 0) {
            contador++;
            if (contador >= 50) {
                ESP_LOGW(TAG, "Reset provisioning por boton");
                gpio_set_level(LED_WIFI, 1);
                vTaskDelay(pdMS_TO_TICKS(1000));
                nvs_clear_wifi();
                esp_restart();
            }
        } else {
            contador = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// Página web de provisioning

static const char *html_form =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>YUMA Connect - Configuracion</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0;}"
    "body{font-family:sans-serif;min-height:100vh;background:#0a2342;display:flex;align-items:center;justify-content:center;padding:20px;}"
    ".card{background:#1a4a8a;border-radius:16px;padding:30px;width:100%;max-width:380px;}"
    ".logo{text-align:center;margin-bottom:24px;}"
    ".logo h1{color:#2ecc71;font-size:32px;font-weight:bold;letter-spacing:2px;}"
    ".logo .isf{color:#ffffff;font-size:12px;margin-top:4px;}"
    ".logo .subtitle{color:#ffffff;font-size:15px;margin-top:12px;font-weight:bold;}"
    ".divider{border:none;border-top:1px solid #0a2342;margin:12px 0;}"
    "label{display:block;color:#ffffff;font-size:15px;font-weight:bold;margin-bottom:6px;margin-top:14px;}"
    "input{width:100%;padding:12px;border:none;border-radius:8px;background:#0a2342;color:#fff;font-size:15px;}"
    "input::placeholder{color:#7f8c8d;}"
    ".btn{width:100%;padding:14px;margin-top:24px;background:#2ecc71;color:#fff;border:none;border-radius:8px;font-size:16px;font-weight:bold;cursor:pointer;}"
    ".btn:hover{background:#27ae60;}"
    "</style></head><body>"
    "<div class='card'>"
    "<div class='logo'>"
    "<h1>YUMA</h1>"
    "<p class='isf'>Intelligent Systems Flow</p>"
    "<hr class='divider'>"
    "<p class='subtitle'>Configuracion del dispositivo</p>"
    "</div>"
    "<form action='/save' method='POST'>"
    "<label>Red WiFi</label>"
    "<input name='ssid' placeholder='Nombre de la red WiFi' required>"
    "<label>Contrasena WiFi</label>"
    "<input name='pass' type='password' placeholder='Contrasena' required>"
    "<label>Latitud</label>"
    "<input name='lat' placeholder='Ej: 4.1234' required>"
    "<label>Longitud</label>"
    "<input name='lon' placeholder='Ej: -74.1234' required>"
    "<button class='btn' type='submit'>Guardar y Conectar</button>"
    "</form>"
    "</div></body></html>";

static const char *html_ok =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>YUMA Connect</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0;}"
    "body{font-family:sans-serif;min-height:100vh;background:#0a2342;display:flex;align-items:center;justify-content:center;padding:20px;}"
    ".card{background:#1a4a8a;border-radius:16px;padding:40px;width:100%;max-width:380px;text-align:center;}"
    ".logo h1{color:#2ecc71;font-size:32px;font-weight:bold;letter-spacing:2px;}"
    ".logo .isf{color:#ffffff;font-size:12px;margin-top:4px;margin-bottom:24px;}"
    ".divider{border:none;border-top:1px solid #0a2342;margin:12px 0;}"
    ".check{color:#2ecc71;font-size:48px;margin:16px 0;}"
    "h2{color:#2ecc71;font-size:20px;margin-bottom:12px;}"
    "p{color:#ffffff;font-size:15px;line-height:1.6;}"
    "</style></head><body>"
    "<div class='card'>"
    "<div class='logo'>"
    "<h1>YUMA</h1>"
    "<p class='isf'>Intelligent Systems Flow</p>"
    "<hr class='divider'>"
    "</div>"
    "<div class='check'>&#10003;</div>"
    "<h2>Configuracion guardada</h2>"
    "<p>El dispositivo se esta conectando a la red WiFi.</p>"
    "<p>Puedes cerrar esta pagina.</p>"
    "</div></body></html>";

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_form, strlen(html_form));
    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) return ESP_FAIL;
    buf[ret] = 0;

    char ssid[64] = {0}, pass[64] = {0}, lat[20] = {0}, lon[20] = {0};

    httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(buf, "pass", pass, sizeof(pass));
    httpd_query_key_value(buf, "lat", lat, sizeof(lat));
    httpd_query_key_value(buf, "lon", lon, sizeof(lon));

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_ok, strlen(html_ok));

    nvs_save_wifi(ssid, pass, lat, lon);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static void iniciar_servidor_web(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = {.uri = "/", .method = HTTP_GET, .handler = handler_root};
        httpd_uri_t uri_save = {.uri = "/save", .method = HTTP_POST, .handler = handler_save};
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_save);
        ESP_LOGI(TAG, "Servidor web iniciado");
    }
}

// Modo AP provisioning
static void iniciar_modo_ap(void)
{
    estado_actual = ESTADO_PROVISIONING;
    ESP_LOGI(TAG, "Iniciando modo AP: Alarma-Config");
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = "Alarma-Config",
            .password = "alarma123",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();
    iniciar_servidor_web();
}

// WiFi Station
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    static int retry_count = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        retry_count++;
        ESP_LOGW(TAG, "WiFi desconectado, intento %d", retry_count);
        estado_actual = ESTADO_RECONECTANDO;
        if (retry_count >= 3) {
            ESP_LOGE(TAG, "3 intentos fallidos, volviendo a provisioning");
            nvs_clear_wifi();
            esp_restart();
        }
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        retry_count = 0;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        estado_actual = ESTADO_CONECTANDO_AWS;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, wifi_pass, sizeof(wifi_config.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
}

// MQTT AWS
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event_id) {
        case MQTT_EVENT_CONNECTED:
    	    ESP_LOGI(TAG, "Conectado a AWS IoT Core!");
    	    estado_actual = ESTADO_OPERANDO;
    	    esp_mqtt_client_subscribe(mqtt_client, TOPIC_CONTROL, 1);
    	    esp_mqtt_client_publish(mqtt_client, "finca/rele/conexion", "{\"c\":1}", 7, 1, 0);
    	    char payload_init[128];
    	    snprintf(payload_init, sizeof(payload_init), "{\"rele\":0,\"lat\":\"%s\",\"lon\":\"%s\"}", device_lat, device_lon);
    	    esp_mqtt_client_publish(mqtt_client, TOPIC_STATUS, payload_init, 0, 1, 0);
    	    break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Desconectado de AWS IoT");
            estado_actual = ESTADO_RECONECTANDO;
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "Topic: %.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "Data: %.*s", event->data_len, event->data);
            if (event->topic_len > 0 && strncmp(event->topic, TOPIC_CONTROL, event->topic_len) == 0) {
                char data[64] = {0};
                memcpy(data, event->data, event->data_len < 63 ? event->data_len : 63);
                if (strstr(data, "\"rele\":1")) set_rele(1);
                else if (strstr(data, "\"rele\":0")) set_rele(0);
            }
            break;
        case MQTT_EVENT_ERROR:
    	   ESP_LOGE(TAG, "Error MQTT tipo: %d", event->error_handle->error_type);
    	   ESP_LOGE(TAG, "Error ESP-TLS: %d", event->error_handle->esp_tls_last_esp_err);
    	   ESP_LOGE(TAG, "Error TLS stack: %d", event->error_handle->esp_tls_stack_err);
    	   estado_actual = ESTADO_RECONECTANDO;
    	   break;
    }
}

static void mqtt_init(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address = {.uri = "mqtts://" AWS_ENDPOINT ":8883"},
            .verification = {
                .certificate = (const char *)root_ca_pem_start,
                .certificate_len = root_ca_pem_end - root_ca_pem_start,
            },
        },
        .credentials = {
            .client_id = AWS_THING_NAME,
            .authentication = {
                .certificate = (const char *)certificate_pem_crt_start,
                .certificate_len = certificate_pem_crt_end - certificate_pem_crt_start,
                .key = (const char *)private_pem_key_start,
                .key_len = private_pem_key_end - private_pem_key_start,
            },
        },
        .session = {
	    .keepalive = 30,
            .last_will = {
                .topic = "finca/rele/conexion",
                .msg = "{\"c\":0}",
                .msg_len = 7,
                .qos = 1,
                .retain = 0,
            },
        },
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    // Configurar GPIOs
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_RELE) | (1ULL << LED_RELE) | (1ULL << LED_WIFI),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_RELE, 0);
    gpio_set_level(LED_RELE, 0);
    gpio_set_level(LED_WIFI, 0);

    // Configurar botón reset
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_RESET),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);

    xTaskCreate(tarea_led, "tarea_led", 2048, NULL, 5, NULL);
    xTaskCreate(tarea_boton, "tarea_boton", 2048, NULL, 4, NULL);

    estado_actual = ESTADO_INICIANDO;

    // Verificar si hay credenciales guardadas
    if (nvs_load_wifi()) {
        ESP_LOGI(TAG, "Credenciales encontradas, conectando a: %s", wifi_ssid);
        estado_actual = ESTADO_CONECTANDO_WIFI;
        wifi_init_sta();
        mqtt_init();
    } else {
        ESP_LOGI(TAG, "Sin credenciales, iniciando modo provisioning");
        iniciar_modo_ap();
    }
}