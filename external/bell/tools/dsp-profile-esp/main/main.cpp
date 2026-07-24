#include <cstdlib>
#include <string>
#include "bell/Logger.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

void testTask(void* arg) {
  BELL_LOG(info, "main", "Hello world!");
}

extern "C" void app_main(void) {
  bell::registerDefaultLogger();

  TaskHandle_t task;
  xTaskCreatePinnedToCore(&testTask, "test task", 1024 * 8, nullptr, 5, &task,
                          1);
  vTaskDelay(portMAX_DELAY);
}
