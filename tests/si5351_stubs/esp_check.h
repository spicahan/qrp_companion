#pragma once
#include "esp_err.h"
#define ESP_RETURN_ON_FALSE(condition, error, tag, ...) do { \
    (void)(tag); if (!(condition)) return (error); \
} while (0)
#define ESP_RETURN_ON_ERROR(expression, tag, ...) do { \
    (void)(tag); const esp_err_t test_error = (expression); \
    if (test_error != ESP_OK) return test_error; \
} while (0)
