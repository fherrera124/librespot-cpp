#pragma once

// Standard includes
#include <any>
#include <memory>

// Own includes
#include "bell/Result.h"
#include "bell/audio/Common.h"

namespace bell::audio {

/**
 * @brief Base class for audio output backends
 */
class Backend {
 public:
  Backend() = default;
  virtual ~Backend() = default;

  // Represents a single audio backend device
  struct Device {
    uint32_t id;
    std::string name;

    bool isDefaultInput{};
    bool isDefaultOutput{};
    int maxInputChannels{};
    int maxOutputChannels{};
    std::any backendSpecificContext;
  };

  // Represents the direction of the audio stream.
  enum class StreamType : std::uint8_t {
    Output,
    Input,
    Duplex  // Both input and output
  };

  class Stream {
   public:
    virtual ~Stream() = default;
    Stream() = default;

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    /**
     * @brief Starts the audio stream, which begins invoking the callback.
     * @return A std::error_code if starting the stream fails.
     */
    virtual bell::Result<> start() = 0;

    /**
     * @brief Stops the audio stream, pausing the callback.
     * @return A std::error_code if stopping the stream fails.
     */
    virtual bell::Result<> stop() = 0;

    /**
     * @brief Checks if a stream is currently open.
     */
    virtual bool isOpen() const = 0;

    /**
     * @brief Checks if the stream is currently running (i.e., started and not stopped).
     */
    virtual bool isStarted() const = 0;

    /**
     * @brief Gets the format of the audio stream.
     */
    virtual AudioFormat getFormat() const = 0;

    /**
     * @brief Gets the actual latency of the stream in frames.
     */
    virtual float getLatency() const = 0;

    /**
     * @brief Gets the type of the audio stream.
     */
    virtual StreamType getType() const = 0;
  };

  using AudioCallback = std::function<void(
      void* outputBuffer, const void* inputBuffer, unsigned int frameCount)>;

  // Delete copy constructor and copy assignment operator
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  /**
   * @brief Gets a list of all available audio output devices.
   * @return A vector of AudioDevice objects.
   */
  virtual std::vector<Device> getOutputDevices() = 0;

  /**
   * @brief Gets a list of all available audio input devices.
   * @return A vector of AudioDevice objects.
   */
  virtual std::vector<Device> getInputDevices() = 0;

  /**
   * @brief Gets the default output device as determined by the system.
   * @return The default AudioDevice.
   */
  virtual bell::Result<Device> getDefaultOutputDevice() = 0;

  /**
   * @brief Gets the default output device as determined by the system.
   * @return The default AudioDevice.
   */
  virtual bell::Result<Device> getDefaultInputDevice() = 0;

  /**
   * @brief Opens an audio stream with the specified parameters.
   *
   * @param streamType Type of audio stream (input / output)
   * @param format desired audio format of the stream
   * @param bufferFrames The desired number of frames for the internal buffer. Affects latency.
   * @param callback The user-defined function to be called for audio processing.
   * @param backendSpecificOptions A type-erased object containing options for a specific backend.
   * The caller is responsible for passing a struct that the target backend understands.
   * @return A std::error_code if opening the stream fails.
   */
  virtual bell::Result<std::unique_ptr<Stream>> openStream(
      const Device& device, StreamType streamType, const AudioFormat& format,
      uint32_t bufferFrames, AudioCallback callback,
      const std::any& backendSpecificOptions = {}) = 0;
};

Backend* getDefaultAudioBackend();

Backend* getPortaudioBackend();
}  // namespace bell::audio

namespace bell {
// Type aliases for the audio output backend
using AudioBackend = audio::Backend;
using AudioBackendDevice = audio::Backend::Device;
using AudioBackendStreamType = audio::Backend::StreamType;
using AudioBackendStream = audio::Backend::Stream;
using AudioBackendCallback = audio::Backend::AudioCallback;
}  // namespace bell
