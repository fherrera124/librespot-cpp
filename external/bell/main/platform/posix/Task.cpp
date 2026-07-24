#include "bell/utils/Task.h"

#include <bell/Logger.h>
#include <pthread.h>
#include <algorithm>

using namespace bell::utils;

class Task::Impl {
 public:
  Impl(const std::string& /*taskName*/, int stackSize, int /*espPriority*/,
       TaskCore /*espTaskCore*/, bool /*espStackOnPsram*/)
      : stackSize(stackSize) {}
  ~Impl() {
    if (threadAttrInitialized) {
      pthread_attr_destroy(&threadAttr);
    }
  };

  // Delete copy constructor and copy assignment operator
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  bool startTask(Task* task) {
    if (threadAttrInitialized) {
      pthread_attr_destroy(&threadAttr);
      threadAttrInitialized = false;
    }
    int ret = pthread_attr_init(&threadAttr);

    if (ret > 0) {
      // Could not initialize the pthread attribute
      return false;
    }

    threadAttrInitialized = true;

    // Try to create a new pthread
    int err = pthread_create(&threadHandle, &threadAttr, threadEntryFunc, task);
    if (err == 0) {
      pthread_detach(threadHandle);
      return true;
    }

    BELL_LOG(error, "BellTask", "Failed to create thread. Error: {}",
             strerror(err));
    return false;
  }

  // Pthread thread entry function
  static void* threadEntryFunc(void* ctx) {
    static_cast<Task*>(ctx)->runTask();
    return nullptr;
  }

 private:
  pthread_t threadHandle = 0;
  pthread_attr_t threadAttr{};
  bool threadAttrInitialized = false;

  int stackSize;
};

// Task constructor and member methods
Task::Task(const std::string& taskName, int stackSize, int espPriority,
           TaskCore espTaskCore, bool espStackOnPsram)
    : pImpl(std::make_unique<Impl>(taskName, stackSize, espPriority,
                                   espTaskCore, espStackOnPsram)) {}

Task::~Task() {
  stopTask();
}

bool Task::startTask() {
  return pImpl->startTask(this);
}
