#include "FileProvider.h"

#include <mutex>

#include "bell/Logger.h"
#include "bell/utils/Task.h"
#include "events/EventLoop.h"
#include "events/EventModels.h"

using namespace cspot;

namespace {
bool countryListContains(const std::string& countryList,
                         const std::string& country) {
  for (size_t i = 0; i + 1 < countryList.size(); i += 2) {
    if (countryList[i] == country[0] && countryList[i + 1] == country[1]) {
      return true;
    }
  }
  return false;
}

// Country restriction is a oneof on the wire, so presence (which field was
// set), not emptiness, is what selects the branch. countries_allowed
// present but empty means "allowed nowhere" - true even though the string
// itself is empty. True if the given country can NOT play a track/
// alternative with these restrictions.
bool doRestrictionsApply(const std::vector<cspot_proto::Restriction>& restrictions,
                         const std::string& country) {
  if (country.empty()) {
    // AP hasn't sent its CountryCode packet yet - can't evaluate, assume
    // playable rather than block everything.
    return false;
  }
  for (auto& restriction : restrictions) {
    if (restriction.countriesAllowed.hasValue) {
      if (restriction.countriesAllowed.value.empty()) {
        return true;
      }
      return !countryListContains(restriction.countriesAllowed.value, country);
    }
    if (restriction.countriesForbidden.hasValue) {
      return countryListContains(restriction.countriesForbidden.value, country);
    }
  }
  return false;
}
}  // namespace

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
    : bell::Task("cspot_file_provider", 32 * 1024, false),
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
      if (currentlyProvidedFiles.empty()) {
        return;
      }

      file = currentlyProvidedFiles.front();

      currentlyProvidedFiles.erase(currentlyProvidedFiles.begin());
    }

    // TODO: Fetch episode metadata
    auto metadataRes = spClient->trackMetadata(file->itemId);
    if (!metadataRes) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG, "Could not fetch track metadata, err={}",
               metadataRes.error());

      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    const std::string& countryCode = apClient->getCountryCode();
    SpotifyId effectiveTrackId = file->itemId;
    bool hasPlayableEntity = true;

    if (doRestrictionsApply(metadataRes->restrictions, countryCode)) {
      hasPlayableEntity = false;
      for (auto& alt : metadataRes->alternativeTracks) {
        if (!doRestrictionsApply(alt.restrictions, countryCode)) {
          effectiveTrackId = SpotifyId(SpotifyIdType::Track, alt.gid);
          hasPlayableEntity = true;
          break;
        }
      }
    }

    if (!hasPlayableEntity) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG,
               "Track {} is restricted in {} with no playable alternative",
               file->itemId.uri, countryCode);
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    auto filesRes = spClient->resolveAudioFiles(effectiveTrackId.uri);
    if (!filesRes) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG, "Could not resolve audio files, err={}",
               filesRes.error());
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    auto& files = *filesRes;
    auto selectedAudioFile = std::find_if(
        files.begin(), files.end(), [](const cspot_proto::AudioFile& f) {
          return f.format == AudioFormat_OGG_VORBIS_160;
        });

    if (selectedAudioFile == files.end()) {
      file->isError = true;
      std::string formatsSeen;
      for (const auto& f : files) {
        formatsSeen += std::to_string(static_cast<int>(f.format)) + " ";
      }
      BELL_LOG(info, LOG_TAG,
               "Could not find suitable audio file, {} files available, "
               "formats: {}",
               files.size(), formatsSeen);

      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    auto cdnUrlRes =
        spClient->resolveStorageInteractive(selectedAudioFile->fileId);
    if (!cdnUrlRes) {
      file->isError = true;
      BELL_LOG(info, LOG_TAG, "Could not resolve cdn url, err={}",
               cdnUrlRes.error());

      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, *file);
      return;
    }

    file->cdnUrl = *cdnUrlRes;
    file->fileId = selectedAudioFile->fileId;
    file->trackMetadata = *metadataRes;

    BELL_LOG(info, LOG_TAG, "Resolved CDN url for track {}: {}",
             file->itemId.uri, file->cdnUrl);

    {
      std::scoped_lock lock(pendingAudioKeyFilesMutex);
      // Keyed by effectiveTrackId (not file->itemId) - that's what
      // requestAudioKey() sends below, and what AudioKeyResponse::trackId
      // echoes back for correlation (see ApClient::requestAudioKey).
      pendingAudioKeyFiles.insert({effectiveTrackId, file.value()});

      auto requestRes =
          apClient->requestAudioKey(effectiveTrackId, file->fileId);
      if (!requestRes) {
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

    pendingAudioKeyFiles.erase(fileRes);

    if (!response.success) {
      // An AudioKeyResponseError carries a short error code in place of
      // the key (2 bytes, not 16) - treating it as a real key regardless
      // of response.success (as this code used to) fed garbage into
      // mbedtls_aes_setkey_enc downstream, reproduced on real hardware as
      // "Failed to set AES key" retried forever for the affected track.
      file.isError = true;
      BELL_LOG(info, LOG_TAG, "Audio key request denied for track {}",
               file.itemId.uri);
      eventLoop->post(EventLoop::EventType::FILE_PROVIDED, file);
      return;
    }

    file.decryptionKey = response.audioKey;

    file.isError = false;  // success

    BELL_LOG(info, LOG_TAG, "File ready for track {}: cdnUrl={}, keyLen={}",
             file.itemId.uri, file.cdnUrl, file.decryptionKey.size());

    eventLoop->post(EventLoop::EventType::FILE_PROVIDED, file);
  } else {
    BELL_LOG(warn, LOG_TAG,
             "Audio key response for {} matched no pending request "
             "(already cancelled/superseded?)",
             response.trackId.hexGid());
  }
}

std::unique_ptr<FileProvider> cspot::createDefaultFileProvider(
    std::shared_ptr<EventLoop> eventLoop, std::shared_ptr<SpClient> spClient,
    std::shared_ptr<ApClient> apClient) {
  return std::make_unique<DefaultFileProvider>(
      std::move(eventLoop), std::move(spClient), std::move(apClient));
}
