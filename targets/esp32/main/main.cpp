#include <fstream>
#include <istream>
#include <memory>

#include "AudioSinkI2S.h"
#include "AuthInfo.h"
#include "Authenticator.h"
#include "Session.h"
#include "bell/Logger.h"
#include "bell/http/Server.h"
#include "bell/mdns/Manager.h"
#include "bell/utils/Semaphore.h"
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
// DIN/I2C control lines needed. Reused directly from /desarrollo/git/cspot's
// own already hardware-verified components/cspot/Kconfig for this exact
// board, not re-derived.
constexpr gpio_num_t kI2sBclkGpio = GPIO_NUM_42;
constexpr gpio_num_t kI2sWsGpio = GPIO_NUM_2;
constexpr gpio_num_t kI2sDoutGpio = GPIO_NUM_41;
constexpr gpio_num_t kI2sMclkGpio = GPIO_NUM_0;  // boot-strap pin, safe once running

// Mirrors targets/cli/main.cpp's waitForZeroconfAuth() - same ZeroConf
// pairing flow, same bell::http::Server/bell::mdns API (portable, no
// ESP32-specific bits needed here).
void waitForZeroconfAuth(std::shared_ptr<cspot::AuthInfo> authInfo) {
  auto httpServer = std::make_shared<bell::http::Server>();
  auto authenticator = std::make_unique<cspot::Authenticator>();
  bell::Semaphore authSemaphore;

  httpServer->registerGet(
      "/spotify_handler",
      [&](const std::unique_ptr<bell::http::Reader>& requestReader,
          const std::unique_ptr<bell::http::Writer>& responseWriter,
          const auto& routeParams) {
        auto queryParams = *requestReader->getQueryParams();
        BELL_LOG(info, "Zeroconf", "Received GET Request");

        if (queryParams.find("action") != queryParams.end() &&
            queryParams["action"] == "getInfo") {
          auto zeroConfString = authenticator->buildZeroconfJSONResponse(
              authInfo->deviceName, authInfo->deviceId, "");
          (void)responseWriter->writeResponseWithBody(
              200, {{"Content-Type", "application/json"}}, zeroConfString);
        } else {
          (void)responseWriter->writeResponseWithBody(500, {},
                                                      "Invalid action");
        }
      });

  httpServer->registerPost(
      "/spotify_handler",
      [&](const std::unique_ptr<bell::http::Reader>& requestReader,
          const std::unique_ptr<bell::http::Writer>& responseWriter,
          const auto& routeParams) {
        auto bodyStr = *requestReader->getBodyStringView();
        tao::json::value responseJson;
        responseJson["status"] = 101;
        responseJson["statusString"] = "OK";
        responseJson["spotifyError"] = 0;

        auto responseString = tao::json::to_string(responseJson);
        (void)responseWriter->writeResponseWithBody(
            200, {{"Content-Type", "application/json"}}, responseString);

        auto res = authenticator->authenticateZeroconfString(authInfo->deviceId,
                                                             bodyStr);
        if (res) {
          BELL_LOG(info, "Zeroconf", "authenticated with spotify");
          authInfo->loginCredentials = *res;
        } else {
          BELL_LOG(error, "Zeroconf", "failed to authenticate with spotify");
        }

        authSemaphore.give();
      });

  (void)httpServer->listen(2139);
  auto service = bell::mdns::getDefaultManager()->advertise(
      authInfo->deviceName, "_spotify-connect._tcp", "", "", 2139,
      {{"VERSION", "1.0"}, {"CPath", "/spotify_handler"}, {"Stack", "SP"}});
  if (!service) {
    BELL_LOG(error, "Zeroconf", "mDNS advertise failed - device won't be "
                                "discoverable by the Spotify app");
  }

  authSemaphore.take();
}
}  // namespace

// Minimal real entry point for the Session/api engine (Stage C checkpoint -
// no audio wiring yet, Session.cpp's own TrackPlayer hookup is still
// commented out). Same shape as targets/cli/main.cpp, just with ESP32's
// NVS/SPIFFS/WiFi bootstrap instead of a plain file for session persistence.
class CSpotTask : public bell::Task {
 public:
  // espStackOnPsram=false: this task reads/writes /spiffs/session.json
  // (SPIFFS, backed by flash). ESP-IDF's flash operations briefly disable
  // the flash cache, which also makes PSRAM unreachable on this SoC -
  // esp_task_stack_is_sane_cache_disabled() (cache_utils.c) asserts the
  // calling task's own stack is in internal DRAM specifically because of
  // this, and a PSRAM-backed stack (bell::Task's own default) fails that
  // check the moment this task does its first flash read. Confirmed via a
  // real hardware crash, not assumed - see git history for the exact
  // backtrace (CSpotTask::runTask() -> std::ifstream -> SPIFFS -> flash).
  CSpotTask()
      : bell::Task("cspot", 32 * 1024, 0, bell::TaskCore::Core1,
                   /*espStackOnPsram=*/false) {
    startTask();
  }

  void runTask() override {
    auto authInfo = std::make_shared<cspot::AuthInfo>(DEVICE_NAME);

    std::ifstream sessionFile(sessionFilePath, std::ios::binary);
    if (sessionFile.is_open()) {
      std::string sessionString((std::istreambuf_iterator<char>(sessionFile)),
                                std::istreambuf_iterator<char>());
      sessionFile.close();
      if (!sessionString.empty()) {
        authInfo->assignDataFromJson(sessionString);
      }
    }

    if (!authInfo->loginCredentials.has_value()) {
      waitForZeroconfAuth(authInfo);
      if (!authInfo->loginCredentials.has_value() ||
          authInfo->loginCredentials->authData.empty()) {
        BELL_LOG(error, TAG, "No login credentials, halting");
        return;
      }

      std::string sessionString = authInfo->toJson();
      std::ofstream outFile(sessionFilePath, std::ios::binary);
      if (outFile.is_open()) {
        outFile << sessionString;
        outFile.close();
      }
    }

    cspot::AudioSinkI2S::Config sinkConfig{
        .port = I2S_NUM_0,
        .bclkPin = kI2sBclkGpio,
        .wsPin = kI2sWsGpio,
        .doutPin = kI2sDoutGpio,
        .mclkPin = kI2sMclkGpio,
        .monoOutput = true,
    };
    auto audioSink = std::make_shared<cspot::AudioSinkI2S>(sinkConfig);
    cspot::AudioOutputCallback audioCallback =
        [audioSink](tcb::span<const std::byte> pcm, const cspot::SpotifyId&) {
          audioSink->feedPCMFrames(
              reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size());
        };

    auto session = std::make_shared<cspot::Session>(authInfo, audioCallback);
    auto startRes = session->start();
    if (!startRes) {
      BELL_LOG(error, TAG, "Failed to start session: {}", startRes.error());
      return;
    }

    while (true) {
      session->runPoller();
    }
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
