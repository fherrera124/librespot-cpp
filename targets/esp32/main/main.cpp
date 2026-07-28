#include <atomic>
#include <fstream>
#include <memory>

#include "AudioSinkI2S.h"
#include "AuthInfo.h"
#include "Authenticator.h"
#include "Session.h"
#include "Utils.h"
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
// DIN/I2C control lines needed.
constexpr gpio_num_t kI2sBclkGpio = GPIO_NUM_42;
constexpr gpio_num_t kI2sWsGpio = GPIO_NUM_2;
constexpr gpio_num_t kI2sDoutGpio = GPIO_NUM_41;
constexpr gpio_num_t kI2sMclkGpio = GPIO_NUM_0;  // boot-strap pin, safe once running

// Runs for the task's entire lifetime, independent of pairing/session
// state, so the device stays visible/selectable in the Spotify app's
// picker at all times. The returned Advertiser must be kept alive by the
// caller: its destructor tears down the mDNS announcement.
std::unique_ptr<bell::mdns::Advertiser> startZeroconfService(
    std::shared_ptr<cspot::AuthInfo> authInfo,
    std::shared_ptr<bell::http::Server> httpServer,
    cspot::Authenticator& authenticator, bell::Semaphore& authSemaphore,
    std::atomic<bool>& needsSessionRestart) {
  httpServer->registerGet(
      "/spotify_handler",
      [&authenticator, authInfo](
          const std::unique_ptr<bell::http::Reader>& requestReader,
          const std::unique_ptr<bell::http::Writer>& responseWriter,
          const auto& routeParams) {
        auto queryParams = *requestReader->getQueryParams();
        BELL_LOG(info, "Zeroconf", "Received GET Request");
        cspot::logHeapStatus("Zeroconf", "GET /spotify_handler entry");

        if (queryParams.find("action") != queryParams.end() &&
            queryParams["action"] == "getInfo") {
          auto zeroConfString = authenticator.buildZeroconfJSONResponse(
              authInfo->deviceName, authInfo->deviceId, "");
          (void)responseWriter->writeResponseWithBody(
              200, {{"Content-Type", "application/json"}}, zeroConfString);
        } else {
          (void)responseWriter->writeResponseWithBody(500, {},
                                                      "Invalid action");
        }
        cspot::logHeapStatus("Zeroconf", "GET /spotify_handler exit");
      });

  httpServer->registerPost(
      "/spotify_handler",
      [&authenticator, &authSemaphore, authInfo, &needsSessionRestart](
          const std::unique_ptr<bell::http::Reader>& requestReader,
          const std::unique_ptr<bell::http::Writer>& responseWriter,
          const auto& routeParams) {
        BELL_LOG(info, "Zeroconf", "Received POST Request");
        cspot::logHeapStatus("Zeroconf", "POST /spotify_handler entry");

        auto bodyStr = *requestReader->getBodyStringView();

        auto res = authenticator.authenticateZeroconfString(authInfo->deviceId,
                                                             bodyStr);
        if (res) {
          BELL_LOG(info, "Zeroconf", "authenticated with spotify");
          authInfo->loginCredentials = *res;

          tao::json::value responseJson;
          responseJson["status"] = 101;
          responseJson["statusString"] = "OK";
          responseJson["spotifyError"] = 0;
          (void)responseWriter->writeResponseWithBody(
              200, {{"Content-Type", "application/json"}},
              tao::json::to_string(responseJson));

          // Wakes runTask()'s loop to (re)build the Session with these
          // credentials, replacing whatever pairing (if any) is already
          // active. give() only fires on success so the loop never wakes
          // to rebuild against stale/unchanged credentials.
          needsSessionRestart.store(true);
          authSemaphore.give();
        } else {
          BELL_LOG(error, "Zeroconf", "failed to authenticate with spotify");
          (void)responseWriter->writeResponseWithBody(500, {}, "");
        }

        cspot::logHeapStatus("Zeroconf", "POST /spotify_handler exit");
      });

  (void)httpServer->listen(2139);
  auto service = bell::mdns::getDefaultManager()->advertise(
      authInfo->deviceName, "_spotify-connect._tcp", "", "", 2139,
      {{"VERSION", "1.0"}, {"CPath", "/spotify_handler"}, {"Stack", "SP"}});
  if (!service) {
    BELL_LOG(error, "Zeroconf", "mDNS advertise failed - device won't be "
                                "discoverable by the Spotify app");
    return nullptr;
  }
  return std::move(*service);
}
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

    std::ifstream sessionFile(sessionFilePath, std::ios::binary);
    if (sessionFile.is_open()) {
      std::string sessionString((std::istreambuf_iterator<char>(sessionFile)),
                                std::istreambuf_iterator<char>());
      sessionFile.close();
      if (!sessionString.empty()) {
        authInfo->assignDataFromJson(sessionString);
      }
    }
    authInfo->logDeviceIdOrigin();

    auto httpServer = std::make_shared<bell::http::Server>();
    cspot::Authenticator authenticator;
    bell::Semaphore authSemaphore;
    std::atomic<bool> needsSessionRestart{false};
    auto mdnsService = startZeroconfService(authInfo, httpServer, authenticator,
                                            authSemaphore, needsSessionRestart);

    // audioSink outlives every Session rebuild below.
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
    cspot::VolumeChangedCallback volumeCallback =
        [audioSink](uint16_t volume) { audioSink->volumeChanged(volume); };

    auto persistSession = [&authInfo]() {
      std::string sessionString = authInfo->toJson();
      std::ofstream outFile(sessionFilePath, std::ios::binary);
      if (outFile.is_open()) {
        outFile << sessionString;
        outFile.close();
      }
    };
    persistSession();

    // haveCredentialsToTry: skips the initial wait when a persisted
    // session was loaded above, so it's tried once before falling back to
    // waiting for a pairing.
    std::shared_ptr<cspot::Session> session;
    bool haveCredentialsToTry = authInfo->loginCredentials.has_value();
    while (true) {
      if (!haveCredentialsToTry) {
        authSemaphore.take();
        needsSessionRestart.store(false);

        if (!authInfo->loginCredentials.has_value() ||
            authInfo->loginCredentials->authData.empty()) {
          continue;
        }
      }
      haveCredentialsToTry = false;

      BELL_LOG(info, TAG, session ? "Rebuilding session after a fresh pairing"
                                  : "Starting session");
      if (session) {
        auto oldInactiveRes = session->putInactive();
        if (!oldInactiveRes) {
          BELL_LOG(warn, TAG, "putInactive on old session failed: {}",
                   oldInactiveRes.error());
        }
      }
      session.reset();
      session = std::make_shared<cspot::Session>(authInfo, audioCallback,
                                                  volumeCallback);
      auto startRes = session->start();
      if (!startRes) {
        BELL_LOG(error, TAG,
                 "Failed to start session: {} - waiting for another pairing "
                 "attempt",
                 startRes.error());
        continue;
      }
      persistSession();

      // Returns on a fresh pairing (needsSessionRestart) or a rejected
      // login (Session::credentialsRejected()) - AP/dealer transport
      // failures are retried internally and never surface here.
      session->runPoller(needsSessionRestart);

      if (session->credentialsRejected()) {
        BELL_LOG(error, TAG,
                 "Credentials rejected by server - clearing and waiting for "
                 "a new pairing");
        authInfo->loginCredentials.reset();
        persistSession();
      }
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
