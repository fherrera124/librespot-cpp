#include <doctest/doctest.h>
#include <future>
#include <mutex>
#include <tao/json.hpp>
#include "connect/ConnectStateHandler.h"

namespace {
using namespace cspot;
using namespace std::chrono_literals;
const std::string track = "spotify:track:2zWayiqyrdrqZTAAqnUM8y";
const std::string oldContext = "spotify:album:2DpwYA58Qs00nRl59i4eiL";
const std::string slowContext = "spotify:album:5sc8Hhp7hBwDbtsg4aGVcf";
const std::string newContext = "spotify:album:6JV1qSUJh03F3XnNSKX2CY";

struct Gate {
  bell::Semaphore entered, release;
  bool fail = false;
};

// Real queue semantics, with a controlled delay at the context-load boundary.
class DelayedQueue : public TrackQueueHandler {
 public:
  DelayedQueue(std::shared_ptr<EventLoop> loop, std::shared_ptr<Gate> gate)
      : loop(loop), gate(gate), delegate(createDefaultTrackQueueHandler(nullptr, loop)) {}
  std::unique_ptr<TrackQueueHandler> createContextLoader() const override {
    return std::make_unique<DelayedQueue>(loop, gate);
  }
  bell::Result<> loadContext(const std::string& uri,
      std::optional<std::string> trackUri, std::optional<std::string> uid,
      std::optional<uint32_t> index,
      const std::vector<cspot_proto::ContextPage>& pages) override {
    if (uri == slowContext) {
      gate->entered.give();
      gate->release.take();
      if (gate->fail) return bell::make_unexpected_errc(std::errc::io_error);
    }
    return delegate->loadContext(uri, trackUri, uid, index, pages);
  }
  void setQueue(const std::vector<cspot_proto::ContextTrack>& q) override { delegate->setQueue(q); }
  void setPlayingQueue(bool q) override { delegate->setPlayingQueue(q); }
  void addToQueue(const cspot_proto::ContextTrack& q) override { delegate->addToQueue(q); }
  void reorderQueue(const std::vector<cspot_proto::ContextTrack>& a,
                    const std::vector<cspot_proto::ContextTrack>& b) override { delegate->reorderQueue(a, b); }
  std::optional<cspot_proto::ProvidedTrack> currentTrack() override { return delegate->currentTrack(); }
  std::optional<cspot_proto::ContextIndex> currentContextIndex() override { return delegate->currentContextIndex(); }
  bell::Result<TrackAdvanceResult> skipToNextTrack(const std::string& a, const std::string& b) override {
    return delegate->skipToNextTrack(a, b);
  }
  bell::Result<> skipToPreviousTrack(const std::string& uri) override { return delegate->skipToPreviousTrack(uri); }
  bell::Result<> enableShuffle(bool enabled) override { return delegate->enableShuffle(enabled); }
  tcb::span<cspot_proto::ProvidedTrack> nextTracks() override { return delegate->nextTracks(); }
  tcb::span<cspot_proto::ProvidedTrack> previousTracks() override { return delegate->previousTracks(); }
  void updateTrackWindows(bool force) override { delegate->updateTrackWindows(force); }
  void clearContext() override { delegate->clearContext(); }
 private:
  std::shared_ptr<EventLoop> loop;
  std::shared_ptr<Gate> gate;
  std::unique_ptr<TrackQueueHandler> delegate;
};

struct Published {
  uint32_t command;
  std::string context, sender;
  bool paused;
  int64_t position;
};

class RecordingClient : public SpClient {
 public:
  bell::Result<> putConnectStateRaw(std::vector<std::byte> body,
      const std::string&, const std::string&) override {
    cspot_proto::PutStateRequest state{};
    if (!nanopb_helper::decodeFromVector(state, body)) {
      return bell::make_unexpected_errc(std::errc::bad_message);
    }
    auto& ps = state.device.playerState;
    {
      std::lock_guard lock(mutex);
      published.push_back({state.lastCommandMessageId, ps.contextUri,
                           ps.playOrigin.deviceIdentifier, ps.isPaused,
                           ps.positionAsOfTimestamp});
    }
    changed.notify_all();
    return {};
  }
  bool waitFor(const std::function<bool(const Published&)>& match) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, 1s, [&] {
      return std::any_of(published.begin(), published.end(), match);
    });
  }
  std::mutex mutex;
  std::condition_variable changed;
  std::vector<Published> published;

  bell::Result<> putConnectState(cspot_proto::PutStateRequest&, const std::string&, const std::string&) override { return {}; }
  bell::Result<> putInactive(const std::string&, const std::string&) override { return {}; }
  bell::Result<bell::HTTPResponse> contextResolve(const std::string&) override { return bell::make_unexpected_errc<bell::HTTPResponse>(std::errc::io_error); }
  bell::Result<bell::HTTPResponse> contextAutoplayResolve(cspot_proto::AutoplayContextRequest&) override { return bell::make_unexpected_errc<bell::HTTPResponse>(std::errc::io_error); }
  bell::Result<bell::HTTPResponse> rawRequest(const std::string&) override { return bell::make_unexpected_errc<bell::HTTPResponse>(std::errc::io_error); }
  bell::Result<cspot_proto::Track> trackMetadata(const SpotifyId&) override { return cspot_proto::Track{}; }
  bell::Result<cspot_proto::Episode> episodeMetadata(const SpotifyId&) override { return cspot_proto::Episode{}; }
  bell::Result<cspot_proto::SelectedListContent> resolvePlaylistContent(const SpotifyId&) override { return cspot_proto::SelectedListContent{}; }
  bell::Result<std::string> resolveStorageInteractive(const std::vector<std::byte>&, bool) override { return std::string{}; }
  bell::Result<std::vector<cspot_proto::AudioFile>> resolveAudioFiles(const std::string&) override { return std::vector<cspot_proto::AudioFile>{}; }
};

tao::json::value play(const std::string& uri, uint64_t id) {
  return tao::json::value{{"payload", {
      {"message_id", id}, {"sent_by_device_id", "controller"},
      {"command", {{"endpoint", "play"}, {"options", tao::json::empty_object},
                   {"context", {{"uri", uri}, {"pages", tao::json::value::array({
                       {{"tracks", tao::json::value::array({{{"uri", track}}})}}
                   })}}}}}
  }}};
}

struct Fixture {
  std::shared_ptr<EventLoop> loop = std::make_shared<EventLoop>();
  std::shared_ptr<Gate> gate = std::make_shared<Gate>();
  std::shared_ptr<RecordingClient> client = std::make_shared<RecordingClient>();
  std::shared_ptr<TimeProvider> clock = std::make_shared<TimeProvider>();
  std::unique_ptr<ConnectStateHandler> handler = std::make_unique<ConnectStateHandler>(
      loop, std::make_shared<AuthInfo>("test"), client, clock,
      std::make_unique<DelayedQueue>(loop, gate));
  ~Fixture() { loop->stopTask(); handler.reset(); }
  void start() {
    auto message = play(oldContext, 1);
    REQUIRE(handler->handlePlayerCommand(message));
    REQUIRE(client->waitFor([](const auto& p) { return p.command == 1; }));
  }
};

// Always release a blocked load before unwinding a failed assertion.
struct Pending {
  Fixture& f;
  std::future<bell::Result<>> result;
  Pending(Fixture& f, tao::json::value message) : f(f), result(std::async(
      std::launch::async, [&f, message = std::move(message)]() mutable {
        return f.handler->handlePlayerCommand(message);
      })) {}
  ~Pending() { f.gate->release.give(); if (result.valid()) result.wait(); }
};
}  // namespace

TEST_CASE("Context loading permits audio updates and PUTs of the previous state") {
  Fixture f;
  f.start();
  Pending pending(f, play(slowContext, 2));
  REQUIRE(f.gate->entered.take(1000));
  auto update = std::async(std::launch::async, [&] {
    PlayerStateUpdate state{};
    state.isPlaying = true;
    state.positionAsOfTimestamp = 123;
    state.timestamp = f.clock->getSyncedTimestamp();
    f.handler->onPlayerStateUpdate(state);
  });
  bool ready = update.wait_for(500ms) == std::future_status::ready;
  if (!ready) f.gate->release.give();
  update.get();
  REQUIRE(ready);
  CHECK(f.client->waitFor([](const auto& p) {
    return p.context == oldContext && p.command == 1 && p.position == 123;
  }));
  f.gate->release.give();
  REQUIRE(pending.result.get());
  CHECK(f.client->waitFor([](const auto& p) {
    return p.context == slowContext && p.command == 2 && p.sender == "controller";
  }));
}

TEST_CASE("A newer command prevents an older context load from committing") {
  Fixture f;
  f.start();
  Pending pending(f, play(slowContext, 2));
  REQUIRE(f.gate->entered.take(1000));
  bool pause = false;
  SUBCASE("local pause") { pause = true; }
  SUBCASE("newer play") {}
  auto newer = std::async(std::launch::async, [&] {
    if (pause) return f.handler->requestPlayPause(false);
    auto message = play(newContext, 3);
    return bool(f.handler->handlePlayerCommand(message));
  });
  bool ready = newer.wait_for(500ms) == std::future_status::ready;
  if (!ready) f.gate->release.give();
  CHECK(newer.get());
  REQUIRE(ready);
  f.gate->release.give();
  auto result = pending.result.get();
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(f.client->waitFor([&](const auto& p) {
    return pause ? p.context == oldContext && p.paused && p.command == 1
                 : p.context == newContext && p.command == 3;
  }));
}

TEST_CASE("Failed context loading preserves the active state and command acknowledgement") {
  Fixture f;
  f.start();
  f.gate->fail = true;
  Pending pending(f, play(slowContext, 2));
  REQUIRE(f.gate->entered.take(1000));
  f.gate->release.give();
  REQUIRE_FALSE(pending.result.get());
  REQUIRE(f.handler->requestPlayPause(false));
  CHECK(f.client->waitFor([](const auto& p) {
    return p.context == oldContext && p.command == 1 && p.paused;
  }));
}
