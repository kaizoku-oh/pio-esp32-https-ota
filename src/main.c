#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

#include "wifi_utils.h"

#define TAG                          "APP"
#define BASE_URL                     "http://github-ota-api.herokuapp.com"
#define ENDPOINT                     "/firmware/latest"
#define GITHUB_USERNAME              "kaizoku-oh"
#define GITHUB_REPOSITORY            "pio-esp32-https-ota"
#define DEVICE_CURRENT_FW_VERSION    APP_VERSION
#define HTTP_INTERNAL_TX_BUFFER_SIZE 1024
#define HTTP_INTERNAL_RX_BUFFER_SIZE 1024
#define HTTP_APP_RX_BUFFER_SIZE      1024

static const char* API_URL = BASE_URL ENDPOINT
                             "?github_username="GITHUB_USERNAME
                             "&github_repository="GITHUB_REPOSITORY
                             "&device_current_fw_version="DEVICE_CURRENT_FW_VERSION;

/* ca certificate */
/* openssl s_client -showcerts -verify 5 -connect s3.amazonaws.com:443 < /dev/null */
extern const char root_ca_cert_pem_start[] asm("_binary_root_ca_cert_pem_start");
extern const char root_ca_cert_pem_end[] asm("_binary_root_ca_cert_pem_end");

/* http receive buffer */
char http_rcv_buffer[HTTP_APP_RX_BUFFER_SIZE];

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
  switch(evt->event_id)
  {
  case HTTP_EVENT_ERROR:
    ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER");
    printf("%.*s", evt->data_len, (char*)evt->data);
    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    if(!esp_http_client_is_chunked_response(evt->client))
    {
      strncpy(http_rcv_buffer, (char*)evt->data, evt->data_len);
    }
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
    break;
  }
  return ESP_OK;
}

char* get_download_url()
{
  char* ret;

  ret = NULL;
  esp_http_client_config_t config =
  {
    .url = API_URL,
    .buffer_size = HTTP_INTERNAL_RX_BUFFER_SIZE,
    .event_handler = _http_event_handler,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t err = esp_http_client_perform(client);
  if(err == ESP_OK)
  {
    ESP_LOGI(TAG, "Status = %d, content_length = %d",
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));
    if(esp_http_client_get_status_code(client) == 204)
    {
      ESP_LOGI(TAG, "Device is already running the latest firmware");
    }
    else
    {
      ESP_LOGI(TAG, "http_rcv_buffer: %s\n", http_rcv_buffer);
      /* parse the http json respose */
      cJSON *json = cJSON_Parse(http_rcv_buffer);
      if(json == NULL)
      {
        ESP_LOGW(TAG, "Response does not contain valid json, aborting...");
      }
      else
      {
        cJSON *download_url = cJSON_GetObjectItemCaseSensitive(json, "download_url");
        if(cJSON_IsString(download_url) && (download_url->valuestring != NULL))
        {
          ret = download_url->valuestring;
          ESP_LOGI(TAG, "download_url length: %d", strlen(ret));
        }
        else
        {
          ESP_LOGW(TAG, "Unable to read the download_url, aborting...");
        }
      }
    }
  }
  esp_http_client_cleanup(client);
  return ret;
}

void app_task(void *pvParameter)
{
  while(1)
  {
    ESP_LOGI(TAG, "Device is running on firmware version %s", DEVICE_CURRENT_FW_VERSION);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

void check_update_task(void *pvParameter)
{
  char* download_url;

  while(1)
  {
    download_url = get_download_url();
    if(download_url != NULL)
    {
      ESP_LOGI(TAG, "download_url: %s", download_url);
      ESP_LOGI(TAG, "Downloading and installing new firmware");
      esp_http_client_config_t ota_client_config =
      {
        .url = download_url,
        .cert_pem = root_ca_cert_pem_start,
        .buffer_size = HTTP_INTERNAL_RX_BUFFER_SIZE,
        .buffer_size_tx = HTTP_INTERNAL_TX_BUFFER_SIZE,
      };
      esp_err_t ret = esp_https_ota(&ota_client_config);
      if (ret == ESP_OK)
      {
        ESP_LOGI(TAG, "OTA OK, restarting...");
        esp_restart();
      }
      else
      {
        ESP_LOGE(TAG, "OTA failed...");
      }
    }
    else
    {
      ESP_LOGW(TAG, "Could not get download_url");
    }
    vTaskDelay(30000 / portTICK_PERIOD_MS);
  }
}

void app_main()
{
  /* Block until connected to wifi */
  wifi_initialise();
  wifi_wait_connected();
  ESP_LOGI(TAG, "connected to wifi network");

  /* start the check update task */
  xTaskCreate(&check_update_task, "check_update_task", 8192, NULL, 5, NULL);
  /* start the app task */
  xTaskCreate(&app_task, "app_task", 2048, NULL, 5, NULL);
}
