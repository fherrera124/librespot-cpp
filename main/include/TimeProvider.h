#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "bell/utils/Task.h"

namespace cspot {

// Timestamp aligned with Spotify's clock, kept as an offset over a
// monotonic clock read - never mutates the OS system clock.
class TimeProvider : public bell::Task {
 public:
  TimeProvider();
  ~TimeProvider() override;

  // Payload is a 4-byte big-endian Unix seconds value.
  void syncWithPingPacket(const std::byte* data, size_t len);

  // Returns time-since-boot (not a real epoch value) until the first sync
  // completes.
  int64_t getSyncedTimestamp() const;

 protected:
  void runTask() override;

 private:
  std::atomic<int64_t> timestampDiffMs{0};

  static int64_t monotonicNowMs();
  void applyOffset(int64_t remoteEpochMs);
  bool queryNtp(int64_t& outEpochMs);
};

}  // namespace cspot
