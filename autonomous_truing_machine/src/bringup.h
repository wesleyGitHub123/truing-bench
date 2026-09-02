/**
 * @file bringup.h
 * Phase 1a bring-up: platform verification (SPEC §4.1, §9.4.1), on-device core
 * self-test, and configuration persistence checks (SPEC §11.5).
 */
#ifndef TRUING_BRINGUP_H
#define TRUING_BRINGUP_H

#include <stdbool.h>

typedef struct {
    int  checks_passed;
    int  checks_failed;
    bool psram_ok;
    bool flash_ok;
    bool i2s_probe_ok;
    bool nvs_ok;
    bool core_selftest_ok;
    bool config_provisioned;    /* every configuration kind present and valid in NVS */
} truing_bringup_report_t;

void truing_bringup_run(truing_bringup_report_t *report);

#endif /* TRUING_BRINGUP_H */
