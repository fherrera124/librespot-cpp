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

// Mirrors librespot-cpp's TrackDataUtils::doRestrictionsApply() (a real,
// hardware-proven implementation in our other project) - true if the
// given country can NOT play a track/alternative with these restrictions.
bool doRestrictionsApply(const std::vector<cspot_proto::Restriction>& restrictions,
                         const std::string& country) {
  if (country.empty()) {
    // AP hasn't sent its CountryCode packet yet - can't evaluate, assume
    // playable rather than block everything.
    return false;
  }
  for (auto& restriction : restrictions) {
    if (!restriction.countriesAllowed.empty()) {
      return !countryListContains(restriction.countriesAllowed, country);
    }
    if (!restriction.countriesForbidden.empty()) {
      return countryListContains(restriction.countriesForbidden, country);
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
    // 4KB was never enough for this task's real work (HTTPS + protobuf
    // decode via spClient->trackMetadata()/resolveStorageInteractive()) -
    // it just never actually ran until Stage D wired StreamPlayer up to
    // call provideTrack() for real tracks. Confirmed via a real hardware
    // stack-overflow crash ("A stack overflow in task cspot_file_prov"),
    // same class of bug as StreamPlayer's own stack (StreamPlayer.cpp).
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

    // Some tracks aren't directly playable in this account's region -
    // the playable version lives under `alternative` instead (confirmed
    // on real hardware: several tracks in a real playlist all resolved
    // this way). Mirrors librespot-cpp's own fallback for the exact same
    // real Spotify behavior. Only restriction/alternative info comes from
    // trackMetadata() now - AudioFile entries are resolved separately
    // below via resolveAudioFiles(), keyed by whichever entity (original
    // or alternative) survives this check.
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

    // Step 2, fetch the actual playable files, pick one & resolve cdn url
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

    BELL_LOG(info, LOG_TAG, "Resolved CDN url for track {}: {}",
             file->itemId.uri, file->cdnUrl);

    {
      std::scoped_lock lock(pendingAudioKeyFilesMutex);
      // Keyed by effectiveTrackId (the alternative's gid, when one was
      // used) because that's what the AP protocol requires in the wire
      // request to match the requested fileId, and it's what comes back
      // unchanged as AudioKeyResponse::trackId (see ApClient::requestAudioKey/
      // apPacketHandler - the response is correlated by sequence number,
      // then echoes back whatever trackId was stored at request time).
      // file->itemId itself (the original queue track id, used by
      // StreamPlayer to key its own state) is untouched here.
      pendingAudioKeyFiles.insert({effectiveTrackId, file.value()});

      // Request audio key from the ap
      auto requestRes =
          apClient->requestAudioKey(effectiveTrackId, file->fileId);
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

    // assign audio key
    file.decryptionKey = response.audioKey;

    file.isError = false;  // success

    BELL_LOG(info, LOG_TAG, "File ready for track {}: cdnUrl={}, keyLen={}",
             file.itemId.uri, file.cdnUrl, file.decryptionKey.size());

    // post result
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
