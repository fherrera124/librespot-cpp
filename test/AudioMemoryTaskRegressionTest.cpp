#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "audio/CDNDataStream.h"
#include "bell/http/Reader.h"
#include "tracks/StreamPlayer.h"

namespace {
using namespace std::chrono_literals;
using Origin = bell::io::DataStream::SeekOrigin;

// Each response borrows a separate stream retained by the transport, so
// foreground reads and real PrefetchWorker requests can overlap safely.
class RangeTransport : public bell::http::Transport {
 public:
  explicit RangeTransport(size_t size) : plain(size), encrypted(size) {
    for (size_t i = 0; i < size; ++i) {
      plain[i] = static_cast<std::byte>((i * 13 + i / 251) % 256);
    }
    encrypted = plain;
    cspot::AesCtrCipher cipher(key);
    REQUIRE(cipher.decrypt(encrypted.data(), encrypted.size(), 0));
  }

  bell::Result<bell::HTTPResponse> execute(const bell::HTTPRequest& req) override {
    const auto& range = req.headers.at("Range");
    const auto dash = range.find('-');
    size_t start;
    size_t end;
    if (dash == 6) {
      auto length = std::stoull(range.substr(7));
      start = encrypted.size() > length ? encrypted.size() - length : 0;
      end = encrypted.size() - 1;
    } else {
      start = std::stoull(range.substr(6, dash - 6));
      end = std::min<size_t>(std::stoull(range.substr(dash + 1)),
                             encrypted.size() - 1);
    }
    const auto length = end - start + 1;
    std::string wire = "HTTP/1.1 206 Partial Content\r\nContent-Length: " +
                       std::to_string(length) + "\r\nContent-Range: bytes " +
                       std::to_string(start) + "-" + std::to_string(end) +
                       "/" + std::to_string(encrypted.size()) + "\r\n\r\n";
    wire.append(reinterpret_cast<const char*>(encrypted.data() + start), length);
    auto input = std::make_unique<std::istringstream>(std::move(wire));
    bell::http::Reader reader(bell::http::Direction::Response, input.get());
    auto result = reader.readHeaders();
    if (!result) return nonstd::make_unexpected(result.error());
    {
      std::lock_guard lock(mutex);
      responses.push_back(std::move(input));
    }
    cv.notify_all();
    return bell::HTTPResponse(std::move(reader));
  }

  bool waitForRequests(size_t count) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, 2s, [&] { return responses.size() >= count; });
  }

  const std::vector<std::byte> key = std::vector<std::byte>(16, std::byte{0x31});
  std::vector<std::byte> plain;

 private:
  std::vector<std::byte> encrypted;
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::unique_ptr<std::istringstream>> responses;
};

void checkBytes(cspot::CDNDataStream& stream, const RangeTransport& transport,
                size_t logicalStart, size_t length) {
  std::vector<std::byte> output(length);
  auto read = stream.read(output.data(), output.size());
  REQUIRE(read);
  REQUIRE(*read == length);
  CHECK(std::equal(output.begin(), output.end(),
                   transport.plain.begin() + 167 + logicalStart));
  CHECK(stream.position() == logicalStart + length);
}

class IdleFileProvider : public cspot::FileProvider {
 public:
  void provideTrack(const cspot::SpotifyId&) override {}
  void cancel(const cspot::SpotifyId&) override {}
};

class ObservedDecoder : public cspot::AudioDecoder {
 public:
  bell::Result<> openStream(const std::string&, const std::vector<std::byte>&,
                            const cspot::SpotifyId&, AudioFormat,
                            int64_t) override { return {}; }
  bell::Result<> openExternalStream(const std::string&, const cspot::SpotifyId&,
                                    int64_t) override { return {}; }
  bool isOpen() const override { ++checks; return true; }
  void resetStream() override { resets.give(); }
  bool isEOF() const override { return false; }
  bool isNearEnd() const override { return false; }
  void processPacket() override {
    ++packets;
    packetProduced.give();
    std::this_thread::sleep_for(1ms);
  }
  bell::Result<> seekToMs(int64_t position) override {
    lastSeek = position;
    seeks.give();
    return {};
  }
  mutable std::atomic<size_t> checks{0};
  std::atomic<size_t> packets{0};
  std::atomic<int64_t> lastSeek{0};
  bell::Semaphore seeks;
  bell::Semaphore resets;
  bell::Semaphore packetProduced;
};
}  // namespace

TEST_CASE("CDN shared blocks survive prefetch eviction, seeks and reopen") {
  auto transport = std::make_unique<RangeTransport>(14 * cspot::kCDNChunkSize);
  auto* data = transport.get();
  auto http = std::make_shared<bell::HTTPClient>(std::move(transport));
  auto worker = std::make_shared<cspot::PrefetchWorker>(http);
  // Exceed the nine-slot cache on purpose: prefetch evicts the block the
  // reader still holds, before it has consumed that block.
  cspot::CDNDataStream stream(http, worker, 10);
  REQUIRE(stream.open("https://example.invalid/audio", data->key));
  REQUIRE(stream.seek(0, Origin::Begin));
  REQUIRE(data->waitForRequests(11));
  checkBytes(stream, *data, 0, 97);
  REQUIRE(stream.seek(3, Origin::Begin));
  checkBytes(stream, *data, 3, 211);

  // New phase while the worker can still own the previous cache.
  REQUIRE(stream.seek(11 * cspot::kCDNChunkSize + 9, Origin::Begin));
  checkBytes(stream, *data, 11 * cspot::kCDNChunkSize + 9, 101);
  REQUIRE(stream.open("https://example.invalid/reopened", data->key));
  auto header = stream.readRawHeaderBytes(167);
  REQUIRE(header);
  CHECK(std::equal(header->begin(), header->end(), data->plain.begin()));
  REQUIRE(stream.seek(0, Origin::Begin));
  checkBytes(stream, *data, 0, 2 * cspot::kCDNChunkSize + 17);
}

TEST_CASE("CDN owned probe and final partial block preserve alignment and EOF") {
  auto transport = std::make_unique<RangeTransport>(2 * cspot::kCDNChunkSize + 128);
  auto* data = transport.get();
  auto http = std::make_shared<bell::HTTPClient>(std::move(transport));
  auto worker = std::make_shared<cspot::PrefetchWorker>(http);
  cspot::CDNDataStream stream(http, worker, 0);
  REQUIRE(stream.open("https://example.invalid/audio", data->key));
  REQUIRE(stream.seek(0, Origin::End));
  auto size = stream.size();
  REQUIRE(size);
  checkBytes(stream, *data, *size - cspot::kCDNChunkSize, cspot::kCDNChunkSize);
  std::byte byte;
  auto eof = stream.read(&byte, 1);
  REQUIRE(eof);
  CHECK(*eof == 0);
  REQUIRE(stream.seek(0, Origin::Begin));
  checkBytes(stream, *data, 0, *size);
}

TEST_CASE("Event loop wakes for concurrent and nested posts and stops idle") {
  auto loop = std::make_shared<cspot::EventLoop>();
  std::atomic<int> received{0};
  bell::Semaphore done;
  loop->registerHandler(cspot::EventLoop::EventType::PLAYER_SEEK,
      [&](cspot::EventLoop::Event&&) {
        if (++received == 400) {
          loop->post(cspot::EventLoop::EventType::PLAYER_FLUSH, std::monostate{});
        }
      });
  loop->registerHandler(cspot::EventLoop::EventType::PLAYER_FLUSH,
                       [&](cspot::EventLoop::Event&&) { done.give(); });
  std::vector<std::thread> producers;
  for (int i = 0; i < 4; ++i) {
    producers.emplace_back([&] {
      for (int j = 0; j < 100; ++j) {
        loop->post(cspot::EventLoop::EventType::PLAYER_SEEK, int64_t{j});
      }
    });
  }
  for (auto& producer : producers) producer.join();
  REQUIRE(done.take(2000));
  CHECK(received == 400);
  std::this_thread::sleep_for(50ms);
  const auto start = std::chrono::steady_clock::now();
  loop->stopTask();
  CHECK(std::chrono::steady_clock::now() - start < 500ms);
}

TEST_CASE("Paused player stays asleep and wakes for seek, play and flush") {
  auto loop = std::make_shared<cspot::EventLoop>();
  auto decoder = std::make_unique<ObservedDecoder>();
  auto* observed = decoder.get();
  cspot::StreamPlayer player(loop, std::make_unique<IdleFileProvider>(),
                              std::move(decoder), nullptr);
  // Synchronize with an operation performed on the player's own task.
  loop->post(cspot::EventLoop::EventType::PLAYER_SEEK, int64_t{123});
  REQUIRE(observed->seeks.take(2000));
  std::this_thread::sleep_for(30ms);
  auto checks = observed->checks.load();
  std::this_thread::sleep_for(250ms);
  CHECK(observed->checks == checks);
  CHECK(observed->packets == 0);

  for (int64_t position = 0; position < 32; ++position) {
    loop->post(cspot::EventLoop::EventType::PLAYER_SEEK, position);
    REQUIRE(observed->seeks.take(2000));
    CHECK(observed->lastSeek == position);
  }
  loop->post(cspot::EventLoop::EventType::PLAYER_PLAY,
             cspot::PlayPauseCommand{.shouldPlay = true});
  loop->post(cspot::EventLoop::EventType::PLAYER_SEEK, int64_t{456});
  REQUIRE(observed->seeks.take(2000));
  REQUIRE(observed->packetProduced.take(2000));
  loop->post(cspot::EventLoop::EventType::PLAYER_PLAY,
             cspot::PlayPauseCommand{.shouldPlay = false});
  loop->post(cspot::EventLoop::EventType::PLAYER_FLUSH, std::monostate{});
  REQUIRE(observed->resets.take(2000));
  std::this_thread::sleep_for(30ms);
  auto packets = observed->packets.load();
  std::this_thread::sleep_for(150ms);
  CHECK(observed->packets == packets);
  loop->stopTask();
  const auto start = std::chrono::steady_clock::now();
  player.stopTask();
  CHECK(std::chrono::steady_clock::now() - start < 500ms);
}

TEST_CASE("Idle file provider can be destroyed without a queued request") {
  for (int i = 0; i < 5; ++i) {
    auto loop = std::make_shared<cspot::EventLoop>();
    auto provider = cspot::createDefaultFileProvider(loop, nullptr, nullptr);
    std::this_thread::sleep_for(20ms);
    loop->stopTask();
    const auto start = std::chrono::steady_clock::now();
    provider.reset();
    CHECK(std::chrono::steady_clock::now() - start < 500ms);
  }
}
