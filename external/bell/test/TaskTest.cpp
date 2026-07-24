#include <doctest/doctest.h>

#include "bell/utils/Task.h"
#include "bell/utils/Utils.h"

class TestTask : public bell::utils::Task {
 public:
  TestTask() : Task("Test Task", 1024), loopCounter(0) { startTask(); }

  // Override taskLoop to increment a counter
  void taskLoop() override {
    ++loopCounter;
    bell::utils::sleepMs(10);
  }

  // Function to get the count of how many times taskLoop was called
  int getLoopCounter() const { return loopCounter; }

 private:
  std::atomic<int> loopCounter;
};

TEST_CASE("bell::utils::Task tests") {
  TestTask task;

  SUBCASE("task is properly started and stopped") {
    // Allow some time for the task to run
    bell::utils::sleepMs(100);

    // Called at least once
    REQUIRE(task.getLoopCounter() > 0);

    // Stop the task and give it time to terminate
    task.stopTask();

    int loopCountAfterStop = task.getLoopCounter();
    bell::utils::sleepMs(100);

    // Ensure taskLoop is not called anymore after stopping
    REQUIRE(task.getLoopCounter() == loopCountAfterStop);
  }
}
