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
#define BASE_URL                     "https://github-ota-api.herokuapp.com"
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
extern const char aws_s3_root_ca_cert_pem_start[] asm("_binary_aws_s3_root_ca_cert_pem_start");
extern const char aws_s3_root_ca_cert_pem_end[] asm("_binary_aws_s3_root_ca_cert_pem_end");
/* openssl s_client -showcerts -verify 5 -connect herokuapp.com:443 < /dev/null */
extern const char heroku_root_ca_cert_pem_start[] asm("_binary_heroku_root_ca_cert_pem_start");
extern const char heroku_root_ca_cert_pem_end[] asm("_binary_heroku_root_ca_cert_pem_end");

/* http receive buffer */
char tcHttpRcvBuffer[HTTP_APP_RX_BUFFER_SIZE];

esp_err_t _http_event_handler(esp_http_client_event_t *pstEvent)
{
  switch(pstEvent->event_id)
  {
  case HTTP_EVENT_ERROR:
    ESP_LOGI(TAG, "HTTP error");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGI(TAG, "HTTP connected to server");
    break;
  case HTTP_EVENT_HEADERS_SENT:
    ESP_LOGI(TAG, "All HTTP headers are sent to server");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGI(TAG, "Received HTTP header from server");
    printf("%.*s", pstEvent->data_len, (char*)pstEvent->data);
    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGI(TAG, "Received data from server, len=%d", pstEvent->data_len);
    if(!esp_http_client_is_chunked_response(pstEvent->client))
    {
      strncpy(tcHttpRcvBuffer, (char*)pstEvent->data, pstEvent->data_len);
    }
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGI(TAG, "HTTP session is finished");
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "HTTP connection is closed");
    break;
  }
  return ESP_OK;
}

char* get_download_url()
{
  int s32HttpCode;
  esp_err_t s32RetVal;
  char* pcDownloadUrl;
  cJSON *pstJsonObject;
  cJSON *pstJsonDownloadUrl;
  esp_http_client_handle_t pstClient;

  pcDownloadUrl = NULL;
  esp_http_client_config_t config =
  {
    .url = API_URL,
    .buffer_size = HTTP_INTERNAL_RX_BUFFER_SIZE,
    .event_handler = _http_event_handler,
    .cert_pem = heroku_root_ca_cert_pem_start,
  };
  pstClient = esp_http_client_init(&config);
  s32RetVal = esp_http_client_perform(pstClient);
  if(ESP_OK == s32RetVal)
  {
    ESP_LOGI(TAG, "Status = %d, content_length = %d",
             esp_http_client_get_status_code(pstClient),
             esp_http_client_get_content_length(pstClient));
    s32HttpCode = esp_http_client_get_status_code(pstClient);
    if(204 == s32HttpCode)
    {
      ESP_LOGI(TAG, "Device is already running the latest firmware");
    }
    else if(200 == s32HttpCode)
    {
      ESP_LOGI(TAG, "tcHttpRcvBuffer: %s\n", tcHttpRcvBuffer);
      /* parse the http json respose */
      pstJsonObject = cJSON_Parse(tcHttpRcvBuffer);
      if(pstJsonObject == NULL)
      {
        ESP_LOGW(TAG, "Response does not contain valid json, aborting...");
      }
      else
      {
        pstJsonDownloadUrl = cJSON_GetObjectItemCaseSensitive(pstJsonObject, "download_url");
        if(cJSON_IsString(pstJsonDownloadUrl) && (pstJsonDownloadUrl->valuestring != NULL))
        {
          pcDownloadUrl = pstJsonDownloadUrl->valuestring;
          ESP_LOGI(TAG, "download_url length: %d", strlen(pcDownloadUrl));
        }
        else
        {
          ESP_LOGW(TAG, "Unable to read the download_url, aborting...");
        }
      }
    }
    else
    {
      ESP_LOGW(TAG, "Failed to get URL with HTTP code: %d", s32HttpCode);
    }
  }
  esp_http_client_cleanup(pstClient);
  return pcDownloadUrl;
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
  char* pcDownloadUrl;

  while(1)
  {
    pcDownloadUrl = get_download_url();
    if(pcDownloadUrl != NULL)
    {
      ESP_LOGI(TAG, "download_url: %s", pcDownloadUrl);
      ESP_LOGI(TAG, "Downloading and installing new firmware");
      esp_http_client_config_t ota_client_config =
      {
        .url = pcDownloadUrl,
        .cert_pem = aws_s3_root_ca_cert_pem_start,
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
      ESP_LOGW(TAG, "Could not get download url");
    }
    vTaskDelay(30000 / portTICK_PERIOD_MS);
  }
}

void app_main()
{
  /* Block until connected to wifi */
  wifi_initialise();
  wifi_wait_connected();

  /* start the check update task */
  xTaskCreate(&check_update_task, "check_update_task", 8192, NULL, 5, NULL);
  /* start the app task */
  xTaskCreate(&app_task, "app_task", 2048, NULL, 5, NULL);
}
