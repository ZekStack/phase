#pragma once
#include <cstddef>
#include <cstdint>
using BaseType_t = int;
using UBaseType_t = unsigned int;
using TickType_t = uint32_t;
using StackType_t = uint32_t;
using configSTACK_DEPTH_TYPE = uint32_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) static_cast<TickType_t>(ms)
#define tskNO_AFFINITY (-1)
#define INCLUDE_uxTaskGetStackHighWaterMark 1
