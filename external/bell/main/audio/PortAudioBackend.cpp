
#ifdef BELL_BACKEND_PORTAUDIO

#include "bell/audio/Backend.h"

// Own includes
#include "bell/Logger.h"
#include "bell/Result.h"
#include "bell/audio/Common.h"

// Library includes
#include <portaudio.h>

using namespace bell::audio;

namespace {
// Map bell sample format to PortAudio sample format
bell::Result<PaSampleFormat> getSampleFormat(const SampleFormat& bitWidth) {
  switch (bitWidth) {
    case bell::SampleFormat::S16:
      return paInt16;
    case bell::SampleFormat::S24:
      return paInt24;
    case bell::SampleFormat::S32:
      return paInt32;
    default:
      return bell::make_unexpected_errc<PaSampleFormat>(
          std::errc::invalid_argument);
  }
}
}  // namespace

class PortAudioStream : public Backend::Stream {
 public:
  PortAudioStream() = default;

  ~PortAudioStream() override {
    if (streamPtr) {
      PaError err = Pa_CloseStream(streamPtr);
      if (err != paNoError) {
        BELL_LOG(error, LOG_TAG, "Failed to close PortAudio stream");
      }
    }
  }

  // Remove the copy constructor and assignment operator
  PortAudioStream(const PortAudioStream&) = delete;
  PortAudioStream& operator=(const PortAudioStream&) = delete;

  bell::Result<> initFromParams(const Backend::Device& device,
                                Backend::StreamType streamType,
                                const bell::AudioFormat& format,
                                uint32_t bufferFrames,
                                Backend::AudioCallback callback,
                                const std::any& backendSpecificOptions) {
    (void)backendSpecificOptions;

    // Assign stream parameters
    this->sourceDevice = device;
    this->streamFormat = format;
    this->streamType = streamType;

    PaStreamParameters inputParameters;
    PaStreamParameters outputParameters;

    PaError err = Pa_OpenStream(&streamPtr, &inputParameters, &outputParameters,
                                format.getSampleRateValue(), bufferFrames,
                                paNoFlag, paCallbackShim, nullptr);
    if (err != paNoError) {
      BELL_LOG(error, LOG_TAG, "Failed to open PortAudio stream");
      // return bell::make_unexpected_errc<PaError>(err);
    }

    return {};
  }

  bell::Result<> start() override {
    return {};  // TODO: impl
  }

  bell::Result<> stop() override {
    return {};  // TODO: impl
  }

  bool isOpen() const override { return false; }

  bool isStarted() const override { return false; }

  bell::AudioFormat getFormat() const override { return {}; }

  float getLatency() const override { return 0.0f; }

  Backend::StreamType getType() const override {
    return Backend::StreamType::Duplex;
  }

 private:
  const char* LOG_TAG = "PortAudioStream";

  PaStream* streamPtr = nullptr;
  Backend::StreamType streamType = Backend::StreamType::Duplex;
  bell::AudioFormat streamFormat;
  Backend::Device sourceDevice;

  static int paCallbackShim(const void* inputBuffer, void* outputBuffer,
                            unsigned long framesPerBuffer,
                            const PaStreamCallbackTimeInfo* timeInfo,
                            PaStreamCallbackFlags statusFlags, void* userData) {
    return paContinue;
  }
};

class PortAudioBackend : public Backend {
 public:
  PortAudioBackend() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
      throw std::runtime_error("Failed to initialize PortAudio");
    }
  }

  ~PortAudioBackend() override {
    PaError err = Pa_Terminate();
    if (err != paNoError) {
      BELL_LOG(error, LOG_TAG, "Failed to terminate PortAudio");
    }
  }

  // Remove the copy constructor and assignment operator
  PortAudioBackend(const PortAudioBackend&) = delete;
  PortAudioBackend& operator=(const PortAudioBackend&) = delete;

  std::vector<Device> getOutputDevices() override {
    auto allDevices = enumerateDevices();

    // Erase all non-output devices
    allDevices.erase(std::remove_if(allDevices.begin(), allDevices.end(),
                                    [](const Device& device) {
                                      return device.maxOutputChannels == 0;
                                    }),
                     allDevices.end());

    return allDevices;
  }

  std::vector<Device> getInputDevices() override {
    auto allDevices = enumerateDevices();

    // Erase all non-input devices
    allDevices.erase(std::remove_if(allDevices.begin(), allDevices.end(),
                                    [](const Device& device) {
                                      return device.maxInputChannels == 0;
                                    }),
                     allDevices.end());

    return allDevices;
  }

  bell::Result<Device> getDefaultOutputDevice() override {
    for (auto& device : enumerateDevices()) {
      if (device.isDefaultOutput) {
        return device;
      }
    }
    return bell::make_unexpected_errc<Device>(std::errc::no_such_device);
  }

  bell::Result<Device> getDefaultInputDevice() override {
    for (auto& device : enumerateDevices()) {
      if (device.isDefaultInput) {
        return device;
      }
    }
    return bell::make_unexpected_errc<Device>(std::errc::no_such_device);
  }

  bell::Result<std::unique_ptr<Stream>> openStream(
      const Device& device, StreamType streamType,
      const bell::AudioFormat& format, uint32_t bufferFrames,
      AudioCallback callback, const std::any& backendSpecificOptions) override {
    auto stream = std::make_unique<PortAudioStream>();

    auto res = stream->initFromParams(device, streamType, format, bufferFrames,
                                      callback, backendSpecificOptions);
    if (!res) {
      return tl::make_unexpected(res.error());
    }

    return stream;
  }

 private:
  const char* LOG_TAG = "PortAudioBackend";

  // Enumerates available portaudio devices
  static std::vector<Device> enumerateDevices() {
    std::vector<Device> devices{};

    int numDevices = Pa_GetDeviceCount();
    int defaultInputDevice = Pa_GetDefaultInputDevice();
    int defaultOutputDevice = Pa_GetDefaultOutputDevice();

    for (int i = 0; i < numDevices; ++i) {
      const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
      if (info) {
        devices.push_back(Device(
            i, info->name, i == defaultInputDevice, i == defaultOutputDevice,
            info->maxInputChannels, info->maxOutputChannels, info));
      }
    }

    return devices;
  }
};

#endif
