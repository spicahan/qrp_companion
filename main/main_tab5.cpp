#include "pal.h"
#include "app.h"
#include "uac_host.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" void app_main(void)
{
    xTaskCreatePinnedToCore(
        [](void *) {
            pal::init(1280, 720);
            app::init();

            // Start USB Audio Class host (spawns its own tasks)
            uac_host_start();

            for (;;) { app::tick(); vTaskDelay(1); }
        },
        "main_task", 32768, nullptr, 1, nullptr, 1);
}
