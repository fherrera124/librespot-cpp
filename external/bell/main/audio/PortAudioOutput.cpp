// #ifdef BELL_BACKEND_PORTAUDIO
// // Own header
// #include "bell/audio/PortAudioOutput.h"

// // Bell includes
// #include "bell/Logger.h"
// #include "bell/audio/Common.h"

// // Include the Portaudio library
// #include <fmt/format.h>
// #include <portaudio.h>

// using namespace bell;

// namespace {
// // Convert the bell audio format to a PortAudio sample format
// PaSampleFormat getPaSampleFormat(const bell::audio::Format& format) {
//   switch (format.getBitWidth()) {
//     case bell::audio::BitWidth::BW_16:
//       return paInt16;
//     case bell::audio::BitWidth::BW_24:
//       return paInt24;
//     case bell::audio::BitWidth::BW_32:
//       return paInt32;
//     default:
//       throw std::runtime_error("Unsupported bit width");
//   }
// }
// }  // namespace

// void audio::PortAudioOutput::configure(const std::any& outputConfig) {
//   std::scoped_lock lock(configMutex);

//   // Extract the Portaudio specific configuration
//   auto config = std::any_cast<PortAudioOutputConfig>(outputConfig);

//   if (!portaudioInitialized) {
//     // Initialize the Portaudio library
//     PaError err = Pa_Initialize();
//     if (err != paNoError) {
//       throw std::runtime_error("Failed to initialize Portaudio");
//     }
//   }

//   if (stream != nullptr) {
//     // Stop and close the current stream
//     Pa_StopStream(stream);
//     Pa_CloseStream(stream);
//     stream = nullptr;
//   }

//   // Setup the output stream
//   PaStreamParameters outputParams;
//   outputParams.device = config.deviceIndex;
//   outputParams.channelCount = config.audioFormat.getNumChannels();
//   outputParams.sampleFormat = getPaSampleFormat(config.audioFormat);
//   outputParams.suggestedLatency = config.suggestedLatency;
//   outputParams.hostApiSpecificStreamInfo = nullptr;

//   if (outputParams.device == -1) {
//     outputParams.device = Pa_GetDefaultOutputDevice();
//   }

//   BELL_LOG(debug, LOG_TAG, "Using PortAudio device {}",
//            Pa_GetDeviceInfo(outputParams.device)->name);

//   // Save the audio format
//   audioFormat = config.audioFormat;

//   // Open the output stream
//   PaError err = Pa_OpenStream(
//       &stream, nullptr, &outputParams, config.audioFormat.getSampleRateValue(),
//       config.framesPerBuffer, paClipOff, nullptr, nullptr);
//   if (err != paNoError) {
//     throw std::runtime_error("Failed to open Portaudio stream");
//   }

//   // Start the stream
//   err = Pa_StartStream(stream);
//   if (err != paNoError) {
//     throw std::runtime_error("Failed to start Portaudio stream");
//   }
// }

// uint32_t audio::PortAudioOutput::write(const uint8_t* pcmData, size_t length) {
//   std::scoped_lock lock(configMutex);

//   if (stream == nullptr) {
//     throw std::runtime_error("Portaudio stream is not initialized");
//   }

//   // Write the PCM data to the output stream
//   Pa_WriteStream(stream, pcmData, audioFormat.bytesToSamples(length));

//   return length;
// }

// audio::PortAudioOutput::~PortAudioOutput() {
//   std::scoped_lock lock(configMutex);

//   if (stream != nullptr) {
//     Pa_StopStream(stream);
//     Pa_CloseStream(stream);
//     Pa_Terminate();
//   }
// }

// #endif
