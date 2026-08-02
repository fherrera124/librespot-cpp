#include <memory>

#include "AudioSinkI2S.h"
#include "AuthInfo.h"
#include "ConnectReceiver.h"
#include "bell/Logger.h"
#include "bell/utils/Task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"

#define DEVICE_NAME CONFIG_CSPOT_DEVICE_NAME

namespace {
const char* TAG = "cspot";
const char* sessionFilePath = "/spiffs/session.json";

// JC3248W535 I2S pinout - onboard NS4168 mono class-D amp, I2S-only, no
// DIN/I2C control lines needed.
constexpr gpio_num_t kI2sBclkGpio = GPIO_NUM_42;
constexpr gpio_num_t kI2sWsGpio = GPIO_NUM_2;
constexpr gpio_num_t kI2sDoutGpio = GPIO_NUM_41;
constexpr gpio_num_t kI2sMclkGpio = GPIO_NUM_0;  // boot-strap pin, safe once running
}  // namespace

class CSpotTask : public bell::Task {
 public:
  // espStackOnPsram=false: this task does flash I/O (SPIFFS session.json),
  // which briefly disables the flash cache and makes PSRAM unreachable -
  // a PSRAM-backed stack crashes on the first flash read.
  CSpotTask()
      : bell::Task("cspot", 32 * 1024, 0, bell::TaskCore::Core1,
                   /*espStackOnPsram=*/false) {
    startTask();
  }

  void runTask() override {
    auto authInfo = std::make_shared<cspot::AuthInfo>(DEVICE_NAME);

    // audioSink outlives every Session rebuild inside ConnectReceiver::run().
    cspot::AudioSinkI2S::Config sinkConfig{
        .port = I2S_NUM_0,
        .bclkPin = kI2sBclkGpio,
        .wsPin = kI2sWsGpio,
        .doutPin = kI2sDoutGpio,
        .mclkPin = kI2sMclkGpio,
        .monoOutput = true,
    };
    auto audioSink = std::make_shared<cspot::AudioSinkI2S>(sinkConfig);

    cspot::ConnectReceiver(authInfo, sessionFilePath, audioSink).run();
  }
};

namespace {
void init_spiffs() {
  esp_vfs_spiffs_conf_t conf = {.base_path = "/spiffs",
                                .partition_label = NULL,
                                .max_files = 5,
                                .format_if_mount_failed = true};

  esp_err_t ret = esp_vfs_spiffs_register(&conf);

  if (ret != ESP_OK) {
    if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount or format filesystem");
    } else if (ret == ESP_ERR_NOT_FOUND) {
      ESP_LOGE(TAG, "Failed to find SPIFFS partition");
    } else {
      ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
    }
    return;
  }

  size_t total = 0, used = 0;
  ret = esp_spiffs_info(conf.partition_label, &total, &used);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)",
             esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
  }
}

}  // namespace

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  init_spiffs();

  esp_wifi_set_ps(WIFI_PS_NONE);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_ERROR_CHECK(example_connect());

  ESP_LOGI(TAG, "Connected to AP, starting Spotify Connect receiver");
  bell::registerDefaultLogger();

  static auto task = std::make_unique<CSpotTask>();
  vTaskSuspend(NULL);
}
