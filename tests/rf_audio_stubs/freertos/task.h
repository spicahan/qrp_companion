#pragma once
#include "FreeRTOS.h"
using TaskHandle_t = void *;
using TaskFunction_t = void (*)(void *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char *, uint32_t, void *,
                                 UBaseType_t, TaskHandle_t *, BaseType_t);
void vTaskDelay(TickType_t);
void vTaskDelete(TaskHandle_t);
TaskHandle_t xTaskGetCurrentTaskHandle();
