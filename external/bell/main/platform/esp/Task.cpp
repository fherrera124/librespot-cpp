#include "bell/utils/Task.h"

// Library includes
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/timers.h>
#include "sdkconfig.h"

using namespace bell::utils;

class Task::Impl {
 public:
  Impl(std::string taskName, int stackSize, int espPriority,
       TaskCore espTaskCore, bool espStackOnPsram)
      : stackSize(stackSize),
        espTaskCore(espTaskCore),
        espStackOnPsram(espStackOnPsram),
        espPriority(espPriority),
        taskName(std::move(taskName)) {

    if (this->espStackOnPsram) {
      xStack = static_cast<StackType_t*>(heap_caps_malloc(
          this->stackSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

      xTaskBuffer = static_cast<StaticTask_t*>(heap_caps_malloc(
          sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
  }
  ~Impl() {
    if (taskPtr != nullptr) {
      taskPtr->stopTask();
    }

    if (xStack) {
      heap_caps_free(xStack);

      // Create a cleanup timer for PSRAM task TCB
      auto* timerHandle =
          xTimerCreate("TaskCleanupTimer", pdMS_TO_TICKS(5000), pdFALSE,
                       xTaskBuffer, [](TimerHandle_t timer) {
                         heap_caps_free(pvTimerGetTimerID(timer));
                         xTimerDelete(timer, portMAX_DELAY);
                       });
      xTimerStart(timerHandle, portMAX_DELAY);
    }
  };

  // Delete copy constructor and copy assignment operator
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  // Task entry point
  void taskEntryPoint() {
    if (taskPtr != nullptr) {
      taskPtr->runTask();
    }
  }

  // Task entry point shim, used to call the mmber method
  static void taskEntryPointShim(void* task) {
    Task::Impl* taskPtr = static_cast<Task::Impl*>(task);
    taskPtr->taskEntryPoint();

    vTaskDelete(NULL);
  }

  bool startTask(Task* task) {
    taskPtr = task;
    if (espStackOnPsram) {
      // Create the task with previously allocated stack on PSRAM
      xTaskHandle = xTaskCreateStaticPinnedToCore(
          taskEntryPointShim, this->taskName.c_str(), this->stackSize, this,
          this->espPriority + CONFIG_PTHREAD_TASK_PRIO_DEFAULT, xStack,
          xTaskBuffer, getFreeRTOSTaskCore());
    } else {
      // Create the task with default stack allocation
      if (xTaskCreatePinnedToCore(
              taskEntryPointShim, this->taskName.c_str(), this->stackSize, this,
              this->espPriority + CONFIG_PTHREAD_TASK_PRIO_DEFAULT,
              &xTaskHandle, getFreeRTOSTaskCore()) != pdPASS) {
        xTaskHandle = nullptr;
      }
    }

    return xTaskHandle != nullptr;
  }

 private:
  int stackSize = 0;
  TaskCore espTaskCore;
  bool espStackOnPsram = false;
  int espPriority;
  std::string taskName;

  StaticTask_t* xTaskBuffer;
  StackType_t* xStack;
  TaskHandle_t xTaskHandle;
  Task* taskPtr = nullptr;

  // Returns the FreeRTOS task core
  BaseType_t getFreeRTOSTaskCore() {
    switch (espTaskCore) {
      case TaskCore::Core0:
        return 0;
      case TaskCore::Core1:
        return 1;
      case TaskCore::CoreAny:
        return tskNO_AFFINITY;
    }

    return tskNO_AFFINITY;
  }
};

// Task constructor and member methods
Task::Task(const std::string& taskName, int stackSize, int espPriority,
           TaskCore espTaskCore, bool espStackOnPsram)
    : pImpl(std::make_unique<Impl>(taskName, stackSize, espPriority,
                                   espTaskCore, espStackOnPsram)) {}

Task::~Task() {}

bool Task::startTask() {
  return pImpl->startTask(this);
}
