#ifndef BELL_TASK_H
#define BELL_TASK_H

#include <atomic>
#include <mutex>
#include <string>

#ifdef ESP_PLATFORM
#include <esp_pthread.h>
#include <esp_task.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#elif _WIN32
#include <winsock2.h>
#else
#include <pthread.h>
#endif

#include "BellLogger.h"

namespace bell {
class Task {
 public:
  std::string TASK;
  int stackSize, core;
  bool runOnPSRAM;
  Task(std::string taskName, int stackSize, int priority, int core,
       bool runOnPSRAM = true) {
    this->TASK = taskName;
    this->stackSize = stackSize;
    this->core = core;
    this->runOnPSRAM = runOnPSRAM;
#ifdef ESP_PLATFORM
    this->xStack = NULL;
    this->priority = CONFIG_ESP32_PTHREAD_TASK_PRIO_DEFAULT + priority;
    if (this->priority <= ESP_TASK_PRIO_MIN)
      this->priority = ESP_TASK_PRIO_MIN + 1;
    if (runOnPSRAM) {
      this->xStack = (StackType_t*)heap_caps_malloc(
          this->stackSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (this->xStack == nullptr) {
        BELL_LOG(error, "Task",
                 "heap_caps_malloc failed allocating %d PSRAM bytes for "
                 "task '%s' stack",
                 this->stackSize, this->TASK.c_str());
      }
    }
#endif
  }
  virtual ~Task() {
#ifdef ESP_PLATFORM
    if (xStack)
      heap_caps_free(xStack);
#endif
  }

  bool startTask() {
#ifdef ESP_PLATFORM
    if (runOnPSRAM) {
      if (xStack == nullptr) {
        // Constructor's allocation already failed and logged - nothing
        // to create the task with.
        return false;
      }
      xTaskBuffer = (StaticTask_t*)heap_caps_malloc(
          sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (xTaskBuffer == nullptr) {
        BELL_LOG(error, "Task",
                 "heap_caps_malloc failed allocating the TCB for task '%s'",
                 this->TASK.c_str());
        return false;
      }
      return (xTaskCreateStaticPinnedToCore(
                  taskEntryFuncPSRAM, this->TASK.c_str(), this->stackSize, this,
                  this->priority, xStack, xTaskBuffer, this->core) != NULL);
    } else {
      BELL_LOG(info, "Task", "task on internal %s", this->TASK.c_str());
      esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
      cfg.stack_size = stackSize;
      cfg.inherit_cfg = true;
      cfg.thread_name = this->TASK.c_str();
      cfg.pin_to_core = core;
      cfg.prio = this->priority;
      esp_pthread_set_cfg(&cfg);
    }
#endif
#if _WIN32
    thread = CreateThread(NULL, stackSize,
                          (LPTHREAD_START_ROUTINE)taskEntryFunc, this, 0, NULL);
    return thread != NULL;
#else
    if (!pthread_create(&thread, NULL, taskEntryFunc, this)) {
        pthread_detach(thread);
        return true;
    }
    return false;
#endif
  }

  // Signals the task to stop and BLOCKS until its runTask() override has
  // actually returned - safe (and required) to call from a derived
  // destructor. Must be the FIRST thing that destructor does, before any
  // of the derived class's own members start being torn down: C++
  // destroys a derived object's members (and runs the rest of its
  // destructor body) before ~Task() ever runs, so by the time this base
  // class could try to wait on its own, runTask() may already be
  // executing against half-destroyed derived state. Calling this here,
  // manually, first, is what keeps that from happening - the base class
  // owns the synchronization primitive, but the ordering is still the
  // derived class's responsibility.
  void stopAndWait() {
    stopRequested = true;
    onStopRequested();
    std::scoped_lock lock(taskLifetimeMutex);
  }

 protected:
  virtual void runTask() = 0;

  // Override to wake up whatever runTask()'s own loop is blocked on (a
  // queue's wtpop, a semaphore's twait, a socket accept()) - called once
  // from stopAndWait(), on the calling thread, before it blocks waiting
  // for runTask() to actually return. Default no-op is fine for a loop
  // that already polls shouldStop() on a short enough timeout by itself.
  virtual void onStopRequested() {}

  // What runTask()'s own loop condition should poll, instead of each
  // derived class maintaining its own private atomic<bool> running/
  // isRunning for the same purpose.
  bool shouldStop() const { return stopRequested.load(); }

 private:
#if _WIN32
  HANDLE thread;
#else
  pthread_t thread;
#endif
#ifdef ESP_PLATFORM
  int priority;
  StaticTask_t* xTaskBuffer = nullptr;
  StackType_t* xStack;

  static void taskEntryFuncPSRAM(void* This) {
    Task* self = (Task*)This;
    {
      std::scoped_lock lock(self->taskLifetimeMutex);
      self->runTask();
    }

    // TCB are cleanup in IDLE task, so give it some time
    TimerHandle_t timer =
        xTimerCreate("cleanup", pdMS_TO_TICKS(5000), pdFALSE, self->xTaskBuffer,
                     [](TimerHandle_t xTimer) {
                       heap_caps_free(pvTimerGetTimerID(xTimer));
                       xTimerDelete(xTimer, portMAX_DELAY);
                     });
    xTimerStart(timer, portMAX_DELAY);

    vTaskDelete(NULL);
  }
#endif

  static void* taskEntryFunc(void* This) {
    Task* self = (Task*)This;
    std::scoped_lock lock(self->taskLifetimeMutex);
    self->runTask();
    return NULL;
  }

  std::atomic<bool> stopRequested{false};
  // Held by whichever task-entry function is currently running runTask()
  // for that call's whole duration - stopAndWait() acquiring this is what
  // blocks until runTask() has actually returned. One canonical copy
  // here, replacing what used to be a taskLifetimeMutex/runningMutex/
  // isRunningMutex hand-rolled separately in every derived class.
  std::mutex taskLifetimeMutex;
};
}  // namespace bell

#endif
