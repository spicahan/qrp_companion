#pragma once
#include "FreeRTOS.h"
#include <cstddef>
struct FakeAudioQueue;
using QueueHandle_t = FakeAudioQueue *;
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t item_size);
QueueHandle_t xQueueCreateStatic(UBaseType_t count, UBaseType_t item_size,
                                uint8_t *storage, StaticQueue_t *control);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueReset(QueueHandle_t);
void vQueueDelete(QueueHandle_t);
