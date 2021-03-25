#include <string.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <freertos/timers.h>
#include <cJSON.h>

#include "wifi_manager.h"

#define TAG                          "APP"

#define OTA_TASK_STACK_SIZE          8192
#define OTA_TASK_PRIORITY            5
#define OTA_QUEUE_ITEMS_COUNT        5
#define OTA_TIMER_TYPE_PERIODIC      pdTRUE
#define OTA_TIMER_PERIOD_MS          30000

#define APP_TASK_STACK_SIZE          2048
#define APP_TASK_PRIORITY            5

#define BASE_URL                     "https://github-ota-api.herokuapp.com"
#define ENDPOINT                     "/firmware/latest"
#define GITHUB_USERNAME              "kaizoku-oh"
#define GITHUB_REPOSITORY            "pio-esp32-https-ota"
#define DEVICE_CURRENT_FW_VERSION    APP_VERSION

#define HTTP_INTERNAL_TX_BUFFER_SIZE 1024
#define HTTP_INTERNAL_RX_BUFFER_SIZE 1024
#define HTTP_APP_RX_BUFFER_SIZE      1024

typedef enum
{
  OTA_START = 0,
}ota_event_t;

static const char* API_URL = BASE_URL ENDPOINT
                             "?github_username="GITHUB_USERNAME
                             "&github_repository="GITHUB_REPOSITORY
                             "&device_current_fw_version="DEVICE_CURRENT_FW_VERSION;

/* $ openssl s_client -showcerts -verify 5 -connect s3.amazonaws.com:443 < /dev/null */
extern const char tcAwsS3RootCaCertPemStart[] asm("_binary_aws_s3_root_ca_cert_pem_start");
extern const char tcAwsS3RootCaCertPemEnd[] asm("_binary_aws_s3_root_ca_cert_pem_end");
/* $ openssl s_client -showcerts -verify 5 -connect herokuapp.com:443 < /dev/null */
extern const char tcHerokuRootCaCertPemStart[] asm("_binary_heroku_root_ca_cert_pem_start");
extern const char tcHerokuRootCaCertPemEnd[] asm("_binary_heroku_root_ca_cert_pem_end");

typedef struct
{
  TimerHandle_t stOtaTimer;                      /**< Software timer to trigger update task */
  QueueHandle_t stOtaQueue;                      /**< Queue handler to receive ota events   */
  char tcHttpRcvBuffer[HTTP_APP_RX_BUFFER_SIZE]; /**< HTTP receive buffer                   */
}app_main_t;

static app_main_t APPi_stMain;

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
      strncpy(APPi_stMain.tcHttpRcvBuffer, (char*)pstEvent->data, pstEvent->data_len);
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

char* get_download_url(void)
{
  int s32HttpCode;
  char *pcDownloadUrl;
  cJSON *pstJsonObject;
  cJSON *pstJsonDownloadUrl;
  esp_http_client_handle_t pstClient;

  pcDownloadUrl = NULL;
  esp_http_client_config_t config =
  {
    .url = API_URL,
    .buffer_size = HTTP_INTERNAL_RX_BUFFER_SIZE,
    .event_handler = _http_event_handler,
    .cert_pem = tcHerokuRootCaCertPemStart,
  };
  pstClient = esp_http_client_init(&config);
  if(ESP_OK == esp_http_client_perform(pstClient))
  {
    ESP_LOGI(TAG,
             "Status = %d, content_length = %d",
             esp_http_client_get_status_code(pstClient),
             esp_http_client_get_content_length(pstClient));
    s32HttpCode = esp_http_client_get_status_code(pstClient);
    if(204 == s32HttpCode)
    {
      ESP_LOGI(TAG, "Device is already running the latest firmware");
    }
    else if(200 == s32HttpCode)
    {
      ESP_LOGI(TAG, "tcHttpRcvBuffer: %s\n", APPi_stMain.tcHttpRcvBuffer);
      /* parse the http json respose */
      pstJsonObject = cJSON_Parse(APPi_stMain.tcHttpRcvBuffer);
      if(!pstJsonObject)
      {
        ESP_LOGW(TAG, "Response does not contain valid json, aborting...");
      }
      else
      {
        pstJsonDownloadUrl = cJSON_GetObjectItemCaseSensitive(pstJsonObject, "download_url");
        if(cJSON_IsString(pstJsonDownloadUrl) && (pstJsonDownloadUrl->valuestring))
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

void ota_task(void *pvParameter)
{
  char *pcDownloadUrl;
  ota_event_t eOtaEvent;

  while(1)
  {
    if(pdPASS == xQueueReceive(APPi_stMain.stOtaQueue, &eOtaEvent, portMAX_DELAY))
    {
      switch(eOtaEvent)
      {
      case OTA_START:
        pcDownloadUrl = get_download_url();
        if(pcDownloadUrl)
        {
          ESP_LOGI(TAG, "download_url: %s", pcDownloadUrl);
          ESP_LOGI(TAG, "Downloading and installing new firmware");
          esp_http_client_config_t ota_client_config =
          {
            .url = pcDownloadUrl,
            .cert_pem = tcAwsS3RootCaCertPemStart,
            .buffer_size = HTTP_INTERNAL_RX_BUFFER_SIZE,
            .buffer_size_tx = HTTP_INTERNAL_TX_BUFFER_SIZE,
          };
          if(ESP_OK == esp_https_ota(&ota_client_config))
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
        break;
      default:
        ESP_LOGW(TAG, "Unknow OTA event");
        break;
      }
    }
  }
}

void wifi_manager_sta_got_ip_cb(void *pvParameter)
{
  ip_event_got_ip_t *pstIPAddr;
  char tcIpString[sizeof("xxx.xxx.xxx.xxx")];

  pstIPAddr = (ip_event_got_ip_t*)pvParameter;
  /* transform IP to human readable string */
  esp_ip4addr_ntoa(&pstIPAddr->ip_info.ip, tcIpString, IP4ADDR_STRLEN_MAX);
  ESP_LOGI(TAG, "I have a connection and my IP is %s!", tcIpString);
  /* start the ota timer */
  xTimerStart(APPi_stMain.stOtaTimer, (TickType_t)0);
}

void wifi_manager_sta_discon_cb(void *pvParameter)
{
  wifi_event_sta_disconnected_t *pstWifiDiscon;

  pstWifiDiscon = (wifi_event_sta_disconnected_t*)pvParameter;
  ESP_LOGW(TAG, "Station disconnected with reason code: %d", pstWifiDiscon->reason);
  /* stop the ota timer */
  xTimerStop(APPi_stMain.stOtaTimer, (TickType_t)0);
}

void timer_ota_cb(TimerHandle_t stTimerHandle)
{
  ota_event_t eOtaEvent;

  ESP_LOGI(TAG, "It's time to check for an update");
  eOtaEvent = OTA_START;
  xQueueSend(APPi_stMain.stOtaQueue, &eOtaEvent, portMAX_DELAY);
}

void app_main(void)
{
  /* start the wifi manager */
  wifi_manager_start();
  /* register a callback to be called when station is connected to AP */
  wifi_manager_set_callback(WM_EVENT_STA_GOT_IP, &wifi_manager_sta_got_ip_cb);
  /* register a callback to be called when station is disconnected from AP */
  wifi_manager_set_callback(WM_EVENT_STA_DISCONNECTED, &wifi_manager_sta_discon_cb);

  /* start the app task */
  xTaskCreate(&app_task, "app", APP_TASK_STACK_SIZE, NULL, APP_TASK_PRIORITY, NULL);
  /* create queue to receive ota events */
  APPi_stMain.stOtaQueue = xQueueCreate(OTA_QUEUE_ITEMS_COUNT, sizeof(ota_event_t));
  /* start the check update task */
  xTaskCreate(&ota_task, "ota", OTA_TASK_STACK_SIZE, NULL, OTA_TASK_PRIORITY, NULL);
  /* create a timer to trigger the update task peridically */
  APPi_stMain.stOtaTimer = xTimerCreate("ota",
                                        pdMS_TO_TICKS(OTA_TIMER_PERIOD_MS),
                                        OTA_TIMER_TYPE_PERIODIC,
                                        (void *)0,
                                        timer_ota_cb);
}
