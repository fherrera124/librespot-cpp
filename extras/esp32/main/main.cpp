// Minimal ESP32 app for this component - WiFi bootstrap plus exactly the
// platform-specific pieces cspot::SpotifyConnectReceiver needs from a
// caller (an AudioSink and a config struct); everything else (ZeroConf
// pairing, session connect/auth/retry, the playback engine itself) lives
// in the component and never touches hardware directly. Same pattern as
// extras/cli/main.cpp (this repo, host/desktop) and cspot_connect.cpp
// (the separate `cspot` ESP32 app, which additionally layers an LVGL UI
// and a C callback API on top - intentionally not reproduced here, this
// is the minimum needed to actually hear audio on real hardware).
//
// No session persistence: like extras/cli, this re-pairs via ZeroConf on
// every boot. SpotifyConnectReceiver's own public API has no way to hand
// it a previously-saved LoginBlob (see LoginBlob::toJson()/loadJson() -
// the serialization exists, but the receiver's constructor never takes
// one), so skipping a boot's pairing dance would need that gap closed
// first - out of scope for this minimal example.

#include <memory>

#include "BellLogger.h"  // for setDefaultLogger/enableTimestampLogging
#include "PlainI2SAudioSink.h"
#include "PlaybackEvent.h"
#include "SpotifyConnectReceiver.h"
#include "TrackQueue.h"  // for TrackInfo (cspot::Event's TRACK_INFO payload)

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"

namespace {
const char* TAG = "main";

// CONFIG_CSPOT_I2S_MCLK_GPIO's own Kconfig default is -1 ("not wired") -
// PlainI2SAudioSink's Config wants I2S_GPIO_UNUSED for that case, not a
// gpio_num_t cast of -1.
gpio_num_t mclkPinOrUnused(int configuredGpio) {
  return configuredGpio < 0 ? I2S_GPIO_UNUSED
                            : static_cast<gpio_num_t>(configuredGpio);
}

void handleEngineEvent(std::unique_ptr<cspot::Event> event) {
  using EventType = cspot::EventType;
  switch (event->eventType) {
    case EventType::PLAY_PAUSE:
      ESP_LOGI(TAG, "%s", std::get<bool>(event->data) ? "Paused" : "Playing");
      break;
    case EventType::TRACK_INFO: {
      auto info = std::get<cspot::TrackInfo>(event->data);
      ESP_LOGI(TAG, "Now playing: %s - %s", info.name.c_str(),
               info.artist.c_str());
      break;
    }
    default:
      // VOLUME/DISC/NEXT/PREV/SEEK/DEPLETED/FLUSH/REPEAT_CONTEXT: no
      // logging-worthy counterpart, SpotifyConnectReceiver already did
      // whatever engine-side reaction they need.
      break;
  }
}

void handleConnectionStateChanged(bool connected) {
  ESP_LOGI(TAG, "%s", connected ? "Connected to Spotify"
                                : "Disconnected, awaiting a new pairing");
}
}  // namespace

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  // Blocks until connected (SSID/password: `idf.py menuconfig` -> Example
  // Connection Configuration, same as any ESP-IDF network example).
  ESP_ERROR_CHECK(example_connect());
  // Lower join/CDN-fetch latency at the cost of some power draw - a
  // mains-powered speaker, unlike example_connect()'s own battery-minded
  // default.
  esp_wifi_set_ps(WIFI_PS_NONE);

  bell::setDefaultLogger();
  bell::enableTimestampLogging();
  ESP_LOGI(TAG, "Connected to AP, starting Spotify Connect receiver");

  // JC3248W535 I2S pinout (onboard NS4168 mono class-D amp, I2S-only, no
  // DIN/I2C control lines needed) - see this project's own Kconfig if
  // targeting different hardware. GPIO0 (the default MCLK pin) is a
  // boot-strap pin: safe to drive once the board has already finished
  // booting, not before.
  PlainI2SAudioSink::Config sinkConfig;
  sinkConfig.port = I2S_NUM_0;
  sinkConfig.bclkPin = static_cast<gpio_num_t>(CONFIG_CSPOT_I2S_BCLK_GPIO);
  sinkConfig.wsPin = static_cast<gpio_num_t>(CONFIG_CSPOT_I2S_WS_GPIO);
  sinkConfig.doutPin = static_cast<gpio_num_t>(CONFIG_CSPOT_I2S_DOUT_GPIO);
  sinkConfig.mclkPin = mclkPinOrUnused(CONFIG_CSPOT_I2S_MCLK_GPIO);
  // Fixed, not a Kconfig option: matches this example's own target board
  // (single mono amp). Set false for a real stereo DAC.
  sinkConfig.monoOutput = true;
  auto audioSink = std::make_unique<PlainI2SAudioSink>(sinkConfig);

  cspot::SpotifyConnectReceiverConfig receiverConfig;
  receiverConfig.deviceName = CONFIG_CSPOT_DEVICE_NAME;
#if defined(CONFIG_CSPOT_QUALITY_320)
  receiverConfig.bitrate = 320;
#elif defined(CONFIG_CSPOT_QUALITY_96)
  receiverConfig.bitrate = 96;
#else
  receiverConfig.bitrate = 160;
#endif

  // static: app_main()'s own task is suspended, not destroyed, at the end
  // of this function (see vTaskSuspend() below) - this local would stay
  // alive regardless, but static makes that lifetime requirement explicit
  // rather than incidental, matching this repo's own feature/esp32-port
  // branch (targets/esp32/main/main.cpp) for the equivalent object.
  static auto receiver = std::make_unique<cspot::SpotifyConnectReceiver>(
      std::move(audioSink), receiverConfig, handleEngineEvent,
      handleConnectionStateChanged);

  if (!receiver->startTask()) {
    ESP_LOGE(TAG, "Failed to start Spotify Connect receiver task");
    return;
  }

  ESP_LOGI(TAG,
           "Spotify Connect receiver started - device name: \"%s\". Open "
           "the Spotify app on this network to pair.",
           receiverConfig.deviceName.c_str());

  vTaskSuspend(NULL);
}
