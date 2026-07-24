#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace bell::utils {
// Enumeration of task cores, used on Espressif platforms
enum class TaskCore {
  Core0 = 0,
  Core1 = 1,
  CoreAny = -1,
};

class Task {
 public:
  /**
   * @brief Default constructor for the Task base class
   * @param taskName The name of the task
   * @param stackSize The size of the task stack
   * @param espPriority The priority of the task, Espressif platforms only
   * @param espTaskCore The core to run the task on, Espressif platforms only
   * @param espStackOnPsram Whether to allocate the stack on PSRAM, Espressif platforms only.
   */
  Task(const std::string& taskName, int stackSize, int espPriority = 0,
       TaskCore espTaskCore = TaskCore::CoreAny, bool espStackOnPsram = true);
  virtual ~Task();

  // Delete copy constructor and copy assignment operator
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  // @brief Attempts to stop execution of the task
  void stopTask() {
    // Attempt to stop the task
    taskRunning = false;
    std::scoped_lock lock(taskRunningMutex);
  }

 protected:
  // Default task runner implementation, can be overridden by derived classes
  virtual void runTask() {
    // Set the taskRunning flag
    std::scoped_lock lock(taskRunningMutex);
    taskRunning = true;

    while (taskRunning) {
      taskLoop();
    }
  }

  /**
   * @brief The task loop function. This function is called repeatedly by the runTask method.
   * @remark This method should be implemented by the derived class to perform the task's work, unless a custom runTask method is provided.
   */
  virtual void taskLoop(){};

  // @brief Starts the task's execution. This method is implemented per-platform.
  bool startTask();

  // Used to keep track of the task state during runTask execution
  std::mutex taskRunningMutex;
  std::atomic<bool> taskRunning = false;

 private:
  class Impl;
  std::unique_ptr<Impl> pImpl;
};

}  // namespace bell::utils

namespace bell {
using Task = utils::Task;
using TaskCore = utils::TaskCore;
}  // namespace bell
