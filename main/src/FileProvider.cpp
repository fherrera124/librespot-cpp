#include "FileProvider.h"

#include <mutex>

#include "bell/Logger.h"
#include "bell/utils/Task.h"
#include "events/EventLoop.h"
#include "events/EventModels.h"

using namespace cspot;

class DefaultFileProvider : public FileProvider, bell::Task {
 public:
  DefaultFileProvider(std::shared_ptr<EventLoop> eventLoop,
                      std::shared_ptr<SpClient> spClient,
                      std::shared_ptr<ApClient> apClient);

  ~DefaultFileProvider() override;

  void provideTrack(const SpotifyId& trackId) override;

  // Cancels providing a track by its ID
  void cancel(const SpotifyId& trackId) override;

 private:
  const char* LOG_TAG = "FileProvider";

  std::shared_ptr<EventLoop> eventLoop;
  std::shared_ptr<SpClient> spClient;
  std::shared_ptr<ApClient> apClient;

  std::mutex providedFilesMutex;
  bell::Semaphore providedFileSemaphore;
  std::vector<ProvidedFile> currentlyProvidedFiles;

  std::mutex pendingAudioKeyFilesMutex;
  std::unordered_map<SpotifyId, ProvidedFile> pendingAudioKeyFiles;

  void taskLoop() override;

  void handleAudioKeyResponse(const AudioKeyResponse& response);
};

DefaultFileProvider::DefaultFileProvider(std::shared_ptr<EventLoop> eventLoop,
                                         std::shared_ptr<SpClient> spClient,
                                         std::shared_ptr<ApClient> apClient)
    : bell::Task("cspot_file_provider", 4 * 1024, false),
      eventLoop(std::move(eventLoop)),
      spClient(std::move(spClient)),
      apClient(std::move(apClient)) {

  startTask();

  this->eventLoop->registerHandler(
      EventLoop::EventType::AUDIO_KEY, [this](EventLoop::Event&& event) {
        auto ev = std::move(event);
        auto res = std::get<AudioKeyResponse>(ev.payload);

        BELL_LOG(info, LOG_TAG,
                 "Handling audio key response event for track ID: {}",
                 res.trackId.hexGid());
        // Handle audio key response event
        handleAudioKeyResponse(res);
      });
}

DefaultFileProvider::~DefaultFileProvider() {
  stopTask();
}

void DefaultFileProvider::provideTrack(const SpotifyId& trackId) {
  std::scoped_lock lock(providedFilesMutex);

  ProvidedFile file = {.itemId = trackId};
  currentlyProvidedFiles.push_back(file);

  // Notify semaphore of new file
  providedFileSemaphore.give();
}

void DefaultFileProvider::cancel(const SpotifyId& trackId) {
  std::scoped_lock lock(providedFilesMutex);

  auto it = std::remove_if(
      currentlyProvidedFiles.begin(), currentlyProvidedFiles.end(),
      [&trackId](const ProvidedFile& file) { return file.itemId == trackId; });

  if (it != currentlyProvidedFiles.end()) {
    currentlyProvidedFiles.erase(it, currentlyProvidedFiles.end());
  }
}

void DefaultFileProvider::taskLoop() {
  if (providedFileSemaphore.take(100)) {
    std::optional<ProvidedFile> file = std::nullopt;

    {
      std::scoped_lock lock(providedFilesMutex);
      // Has provided file
      if (currentlyProvidedFiles.empty()) {
        return;  // No files to take
      }

      file = currentlyProvidedFiles.front();

      // Erase front
      currentlyProvidedFiles.erase(currentlyProvidedFiles.begin());
    }

    // Step 1, fetch metadata
    // TODO: Fetch episode metadata
    auto metadataRes = spClient->trackMetadata(file->itemId);
    if (!metadataRes) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG, "Could not fetch track metadata, err={}",
               metadataRes.error());

      // Post failure
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    const auto& files = metadataRes->audioFiles;

    // Step 2, pick audio file & resolve cdn url
    auto selectedAudioFile = std::find_if(
        files.begin(), files.end(), [](const cspot_proto::AudioFile& file) {
          return file.format == AudioFormat_OGG_VORBIS_160;
        });

    if (selectedAudioFile == files.end()) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG, "Could not find suitable audio file");

      // Post failure
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    // Resolve CDN url
    auto cdnUrlRes =
        spClient->resolveStorageInteractive(selectedAudioFile->fileId);
    if (!cdnUrlRes) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG, "Could not resolve cdn url, err={}",
               cdnUrlRes.error());

      // Post failure
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    // Assign file info
    file->cdnUrl = *cdnUrlRes;
    file->fileId = selectedAudioFile->fileId;
    file->trackMetadata = *metadataRes;

    {
      std::scoped_lock lock(pendingAudioKeyFilesMutex);
      pendingAudioKeyFiles.insert({file->itemId, file.value()});

      // Request audio key from the ap
      auto requestRes = apClient->requestAudioKey(file->itemId, file->fileId);
      if (!requestRes) {
        // Post failure
        file->isError = true;
        BELL_LOG(info, LOG_TAG, "Could not request audio key, err={}",
                 requestRes.error());
        eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
        return;
      }
    }
  }
}

void DefaultFileProvider::handleAudioKeyResponse(
    const AudioKeyResponse& response) {
  std::scoped_lock lock(pendingAudioKeyFilesMutex);

  auto fileRes = pendingAudioKeyFiles.find(response.trackId);
  if (fileRes != pendingAudioKeyFiles.end()) {
    ProvidedFile file = pendingAudioKeyFiles[response.trackId];

    // Erase the result
    pendingAudioKeyFiles.erase(fileRes);

    // assign audio key
    file.decryptionKey = response.audioKey;

    file.isError = false;  // success

    // post result
    eventLoop->post(EventLoop::EventType::FILE_PROVIDED, file);
  }
}

std::unique_ptr<FileProvider> cspot::createDefaultFileProvider(
    std::shared_ptr<EventLoop> eventLoop, std::shared_ptr<SpClient> spClient,
    std::shared_ptr<ApClient> apClient) {
  return std::make_unique<DefaultFileProvider>(
      std::move(eventLoop), std::move(spClient), std::move(apClient));
}
