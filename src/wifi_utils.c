#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#define WIFI_MAXIMUM_RETRY 3
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "WIFI_UTILS";

static EventGroupHandle_t stWifiEventGroupe;
static int8_t u08RetryCount = 0;
ip_event_got_ip_t* pstIpEvent;
EventBits_t u32EventBits;

static void wifi_event_handler(void* pvArg,
                               esp_event_base_t pcEventBase,
                               int32_t s32EventId,
                               void* pvEventData)
{
  if((pcEventBase == WIFI_EVENT ) && (s32EventId == WIFI_EVENT_STA_START))
  {
    esp_wifi_connect();
  }
  else if((pcEventBase == WIFI_EVENT) && (s32EventId == WIFI_EVENT_STA_DISCONNECTED))
  {
    if(u08RetryCount < WIFI_MAXIMUM_RETRY)
    {
      esp_wifi_connect();
      u08RetryCount++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    }
    else
    {
      xEventGroupSetBits(stWifiEventGroupe, WIFI_FAIL_BIT);
    }
    ESP_LOGI(TAG,"connect to the AP fail");
  }
  else if((pcEventBase == IP_EVENT) && (s32EventId == IP_EVENT_STA_GOT_IP))
  {
    pstIpEvent = (ip_event_got_ip_t*) pvEventData;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&pstIpEvent->ip_info.ip));
    u08RetryCount = 0;
    xEventGroupSetBits(stWifiEventGroupe, WIFI_CONNECTED_BIT);
  }
}

void wifi_initialise(void)
{
  esp_err_t s32RetVal = nvs_flash_init();
  if((s32RetVal == ESP_ERR_NVS_NO_FREE_PAGES) || (s32RetVal == ESP_ERR_NVS_NEW_VERSION_FOUND))
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    s32RetVal = nvs_flash_init();
  }
  ESP_ERROR_CHECK(s32RetVal);
  stWifiEventGroupe = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,
                                             ESP_EVENT_ANY_ID,
                                             &wifi_event_handler,
                                             NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,
                                             IP_EVENT_STA_GOT_IP,
                                             &wifi_event_handler,
                                             NULL));
  wifi_config_t wifi_config =
  {
    .sta =
    {
      .ssid = WIFI_SSID,
      .password = WIFI_PASS,
      .threshold.authmode = WIFI_AUTH_WPA2_PSK,
      .pmf_cfg =
      {
        .capable = true,
        .required = false
      },
    },
  };
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
  ESP_ERROR_CHECK(esp_wifi_start() );
  ESP_LOGI(TAG, "wifi_initialise finished");
}

void wifi_wait_connected(void)
{
  ESP_LOGI(TAG, "Waiting for wifi connection");
  u32EventBits = xEventGroupWaitBits(stWifiEventGroupe,
                                     WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                     pdFALSE,
                                     pdFALSE,
                                     portMAX_DELAY);
  if(u32EventBits & WIFI_CONNECTED_BIT)
  {
    ESP_LOGI(TAG,
             "connected to ap SSID:%s password:%s",
             WIFI_SSID,
             WIFI_PASS);
  }
  else if(u32EventBits & WIFI_FAIL_BIT)
  {
  ESP_LOGI(TAG,
           "Failed to connect to SSID:%s, password:%s",
           WIFI_SSID,
           WIFI_PASS);
  }
  else
  {
    ESP_LOGE(TAG, "UNEXPECTED EVENT");
  }
  ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT,
                                               IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler));
  ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               &wifi_event_handler));
  vEventGroupDelete(stWifiEventGroupe);
}