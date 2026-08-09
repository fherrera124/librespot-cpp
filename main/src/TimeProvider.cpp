#include "TimeProvider.h"

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <string>

#include "bell/Logger.h"
#include "bell/net/IpAddress.h"
#include "bell/net/UDPSocket.h"

using namespace cspot;

namespace {
const char* LOG_TAG = "TimeProvider";
const char* kNtpServer = "pool.ntp.org";
const uint16_t kNtpPort = 123;
const int kNtpTimeoutMs = 3000;
// NTP epoch (1900-01-01) to Unix epoch (1970-01-01), in seconds.
const int64_t kNtpToUnixEpochSeconds = 2208988800LL;

std::string formatEpochMs(int64_t epochMs) {
  std::time_t seconds = static_cast<std::time_t>(epochMs / 1000);
  std::tm tmBuf{};
  gmtime_r(&seconds, &tmBuf);
  std::array<char, 24> buf{};
  std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M:%S", &tmBuf);
  return std::string(buf.data()) + " UTC";
}
}  // namespace

TimeProvider::TimeProvider() : Task("cspot_time_provider", 4096) {
  startTask();
}

TimeProvider::~TimeProvider() {
  stopTask();
}

int64_t TimeProvider::monotonicNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void TimeProvider::applyOffset(int64_t remoteEpochMs) {
  timestampDiffMs.store(remoteEpochMs - monotonicNowMs(),
                        std::memory_order_relaxed);
}

int64_t TimeProvider::getSyncedTimestamp() const {
  return monotonicNowMs() + timestampDiffMs.load(std::memory_order_relaxed);
}

void TimeProvider::syncWithPingPacket(const std::byte* data, size_t len) {
  if (len < sizeof(uint32_t)) {
    return;
  }
  uint32_t remoteSeconds;
  std::memcpy(&remoteSeconds, data, sizeof(remoteSeconds));
  remoteSeconds = ntohl(remoteSeconds);
  int64_t epochMs = static_cast<int64_t>(remoteSeconds) * 1000;
  applyOffset(epochMs);
  BELL_LOG(debug, LOG_TAG, "Time offset refined from AP ping: {}",
           formatEpochMs(epochMs));
}

bool TimeProvider::queryNtp(int64_t& outEpochMs) {
  auto addrRes = bell::net::IpAddress::resolveDomain(kNtpServer, SOCK_DGRAM);
  if (!addrRes) {
    BELL_LOG(warn, LOG_TAG, "Could not resolve {}: {}", kNtpServer,
             addrRes.error());
    return false;
  }
  addrRes->setPort(kNtpPort);

  bell::net::UDPSocket sock;
  auto fdRes = sock.createFd(addrRes->getFamily());
  if (!fdRes) {
    BELL_LOG(warn, LOG_TAG, "Could not create NTP socket: {}", fdRes.error());
    return false;
  }
  (void)sock.setReceiveTimeout(kNtpTimeoutMs);

  // Minimal NTP client request: everything zeroed except LI=0, VN=4, Mode=3
  // (client) in the first byte - all we need to get a Transmit Timestamp
  // back.
  std::array<std::byte, 48> packet{};
  packet[0] = std::byte{0x23};

  auto sendRes = sock.sendto(packet.data(), packet.size(), *addrRes);
  if (!sendRes) {
    BELL_LOG(warn, LOG_TAG, "Failed to send NTP request: {}",
             sendRes.error());
    return false;
  }

  std::array<std::byte, 48> response{};
  auto recvRes = sock.recvfrom(response.data(), response.size(), *addrRes);
  if (!recvRes || *recvRes < response.size()) {
    BELL_LOG(warn, LOG_TAG, "NTP request timed out or got a short response");
    return false;
  }

  // Transmit Timestamp: seconds since the NTP epoch, big-endian, at
  // offset 40 of the response.
  uint32_t transmitSeconds;
  std::memcpy(&transmitSeconds, &response[40], sizeof(transmitSeconds));
  transmitSeconds = ntohl(transmitSeconds);

  outEpochMs =
      (static_cast<int64_t>(transmitSeconds) - kNtpToUnixEpochSeconds) * 1000;
  return true;
}

void TimeProvider::runTask() {
  int64_t epochMs;
  if (queryNtp(epochMs)) {
    applyOffset(epochMs);
    BELL_LOG(info, LOG_TAG, "Synced time via NTP: {}", formatEpochMs(epochMs));
  } else {
    BELL_LOG(warn, LOG_TAG,
             "NTP sync failed, will rely on the Spotify AP ping instead");
  }
}
