#pragma once

#include <condition_variable>
#include <mutex>

namespace bell::utils {
/// CPP20 has std::counting_semaphore, but this is a simple implementation for older compilers.
class Semaphore {
 public:
  explicit Semaphore(uint32_t initialCount = 0) : semCount(initialCount) {}

  /**
   * @brief Takes a semaphore, blocking until the count is greater than zero, or until the timeout expires.
   *
   * @param timeout Timeout in milliseconds. A value of 0 means no timeout.
   * @return true if the semaphore was taken, false if the timeout expired.
   */
  bool take(int timeout = 0) {
    std::unique_lock<std::mutex> lock(semMutex);

    if (timeout == 0) {
      semCondition.wait(lock, [this]() { return semCount > 0; });
      --semCount;
      return true;
    }

    auto timeoutDuration = std::chrono::milliseconds(timeout);
    if (semCondition.wait_for(lock, timeoutDuration,
                              [this]() { return semCount > 0; })) {
      --semCount;
      return true;
    }
    return false;  // Timed out
  }

  /**
   * @brief Resets the semaphore count to zero.
   */
  void reset() {
    std::lock_guard<std::mutex> lock(semMutex);
    semCount = 0;
  }

  /**
   * @brief Gives a semaphore, incrementing the count. If there are any threads waiting on the semaphore, one will be unblocked.
   */
  void give() {
    std::lock_guard<std::mutex> lock(semMutex);
    ++semCount;
    semCondition.notify_one();
  }

 private:
  // Mutex and condition variable for the semaphore
  std::mutex semMutex;
  std::condition_variable semCondition;
  uint32_t semCount;
};
}  // namespace bell::utils

namespace bell {
using Semaphore = utils::Semaphore;
}
