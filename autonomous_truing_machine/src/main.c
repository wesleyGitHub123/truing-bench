/* Application entry point.
 *
 * Phase 1a: run the bring-up (platform verification, core self-test, configuration
 * persistence report) and idle with a heartbeat. The orchestrator (Phase 1d) will
 * own the state machine on core 0; the audio path (Phase 1f/2) is pinned to core 1
 * (SPEC 4.5). Nothing here reaches into core logic beyond calling it (P3). */
#include "board/board_profile.h"
#include "bringup.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "firmware_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

/* Logging cadence for the idle heartbeat; not a configuration quantity. */
#define HEARTBEAT_PERIOD_MS 5000u

static void heartbeat_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
        ESP_LOGI(TAG, "heartbeat up=%lus free_internal=%u free_spiram=%u min_free_internal=%u",
                 (unsigned long)(esp_timer_get_time() / 1000000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "truing firmware %s on %s", TRUING_FIRMWARE_VERSION, truing_board_name());
    truing_bringup_report_t report;
    truing_bringup_run(&report);
    ESP_LOGI(TAG, "bring-up done (passed=%d failed=%d); orchestrator is Phase 1d -- idling on core 0",
             report.checks_passed, report.checks_failed);
    xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 4096, NULL, tskIDLE_PRIORITY + 1, NULL, 0);
}
