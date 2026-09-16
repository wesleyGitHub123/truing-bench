/* T6 evidence for 88b6b8b's claim: does the excitation pulse stop stretching under scheduler
 * load once its falling edge is set by a gptimer ISR instead of vTaskDelay?
 *
 * Compiles and fires the REAL src/pluck_gpio.c directly -- not a reimplementation -- on a spare
 * GPIO, with no orchestrator, no session, and no PRESENT flip (that stays gated on B2). Two
 * "load" tasks run on core 0 at the same priorities the real firmware's httpd (5) and wire_tx
 * (3) use, each alternating a ~3 ms busy-spin with a brief yield -- several such bursts land
 * inside every single 20 ms pulse, denser and more adversarial than the real tasks' actual
 * duty cycle, on purpose. The fire loop itself runs at priority 2, matching where pluck_gpio's
 * fire() is actually called from in orch_demo.
 *
 * Before this fix, fire() ended the pulse with vTaskDelay then gpio_set_level(0): a task at
 * priority 5 or 3 running when the delay expired would hold the CPU and stretch the pulse by
 * however long it stayed busy. With two such tasks permanently spinning here, that stretch
 * would be continuous and severe. If the fix works, none of it should show up in the measured
 * widths at all, because the falling edge no longer depends on this task ever running again.
 *
 * Pass/fail is the B1a T6 exit criterion from the campaign plan: |measured - commanded| <= 0.5 ms
 * on every fire. Prints one line per fire and a summary; nothing here claims a station is ready
 * -- that is what PRESENT means, and this harness never touches it. */
#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pluck_gpio.h"

#define TEST_GPIO       9u      /* LEFT's pin -- same pin, same driver, no PRESENT dependency */
#define PULSE_MS        20.0f   /* representative of the campaign's fixture excitation (20 ms) */
#define N_FIRES         200     /* matches B2-E3's magnitude */
#define GAP_MS           50u    /* between fires, so the load tasks are always mid-spin at the moment of firing */
#define TOLERANCE_US   500      /* B1a T6 exit criterion: |measured - commanded| <= 0.5 ms */

#define LOAD_HIGH_PRIO   5      /* same priority as httpd in the real firmware */
#define LOAD_LOW_PRIO    3      /* same priority as wire_tx in the real firmware */
#define FIRE_PRIO        2      /* same priority pluck_gpio's fire() is actually called from */

static const char *TAG = "pluck_timing_check";

/* Busy bursts at httpd's and wire_tx's real priorities, separated by a brief yield. A true
 * infinite busy-loop at these priorities would starve everything below them forever, including
 * the idle task -- which trips the watchdog and also means fire_task never runs at all, so
 * nothing gets measured. The yield keeps this a stress test rather than a hang, while still
 * presenting frequent high-priority interruption relative to a ~20 ms pulse: several bursts
 * land inside every single fire. */
static void load_task(void *arg)
{
    (void)arg;
    volatile uint32_t sink = 0;
    while (1) {
        const int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 3000) {   /* busy-spin ~3 ms */
            sink += 1u;
        }
        vTaskDelay(1);   /* yield at least one tick -- lets idle and fire_task run */
    }
}

static void fire_task(void *arg)
{
    (void)arg;
    truing_pluck_if_t pluck;
    truing_pluck_gpio_ctx_t ctx;
    if (!truing_pluck_gpio_init(&pluck, &ctx, TEST_GPIO)) {
        ESP_LOGE(TAG, "truing_pluck_gpio_init FAILED -- cannot run the check");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "init ok -- GPIO %u, impl \"%s\". Load tasks are spinning at prio %d and %d. "
                  "Firing %d pulses at %d ms commanded, %d ms apart.", TEST_GPIO, pluck.impl_name,
             LOAD_HIGH_PRIO, LOAD_LOW_PRIO, N_FIRES, (int)PULSE_MS, (int)GAP_MS);

    uint32_t worst_delta_us = 0u;
    uint32_t n_over_tolerance = 0u;
    uint32_t n_failed_fires = 0u;
    uint64_t sum_measured_us = 0u;

    for (int i = 1; i <= N_FIRES; i++) {
        if (!pluck.fire(&pluck, PULSE_MS)) {
            n_failed_fires++;
            ESP_LOGE(TAG, "fire %d/%d FAILED", i, N_FIRES);
            vTaskDelay(pdMS_TO_TICKS(GAP_MS));
            continue;
        }
        truing_pluck_fire_report_t rpt;
        if (!pluck.fire_report(&pluck, &rpt)) {
            ESP_LOGE(TAG, "fire %d/%d succeeded but fire_report FAILED -- can't measure it", i, N_FIRES);
            vTaskDelay(pdMS_TO_TICKS(GAP_MS));
            continue;
        }
        const int32_t commanded_us = (int32_t)(PULSE_MS * 1000.0f);
        const int32_t delta_us = (int32_t)rpt.pulse_us_measured - commanded_us;
        const uint32_t abs_delta_us = (uint32_t)(delta_us < 0 ? -delta_us : delta_us);
        sum_measured_us += rpt.pulse_us_measured;
        if (abs_delta_us > worst_delta_us) {
            worst_delta_us = abs_delta_us;
        }
        if (abs_delta_us > TOLERANCE_US) {
            n_over_tolerance++;
            ESP_LOGW(TAG, "fire %d/%d: measured %u us, commanded %d us, delta %d us -- OVER TOLERANCE", i,
                      N_FIRES, (unsigned)rpt.pulse_us_measured, (int)commanded_us, (int)delta_us);
        } else if (i % 20 == 0 || i == 1) {
            ESP_LOGI(TAG, "fire %d/%d: measured %u us, delta %d us, hardware_timed=%d", i, N_FIRES,
                     (unsigned)rpt.pulse_us_measured, (int)delta_us, (int)rpt.hardware_timed);
        }
        vTaskDelay(pdMS_TO_TICKS(GAP_MS));
    }

    const uint32_t n_measured = (uint32_t)N_FIRES - n_failed_fires;
    ESP_LOGI(TAG, "=== summary: %d fires, %u failed, %u measured, worst |delta| %u us, "
                  "%u over the %d us tolerance, mean measured %u us ===", N_FIRES, (unsigned)n_failed_fires,
             (unsigned)n_measured, (unsigned)worst_delta_us, (unsigned)n_over_tolerance, TOLERANCE_US,
             n_measured > 0u ? (unsigned)(sum_measured_us / n_measured) : 0u);
    ESP_LOGI(TAG, "%s", (n_failed_fires == 0u && n_over_tolerance == 0u) ? "PASS" : "FAIL");
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "pluck_timing_check -- T6 evidence for 88b6b8b, real pluck_gpio.c, GPIO %u, no PRESENT",
             TEST_GPIO);
    xTaskCreatePinnedToCore(load_task, "load_high", 1024, NULL, LOAD_HIGH_PRIO, NULL, 0);
    xTaskCreatePinnedToCore(load_task, "load_low", 1024, NULL, LOAD_LOW_PRIO, NULL, 0);
    xTaskCreatePinnedToCore(fire_task, "fire", 4096, NULL, FIRE_PRIO, NULL, 0);
}
