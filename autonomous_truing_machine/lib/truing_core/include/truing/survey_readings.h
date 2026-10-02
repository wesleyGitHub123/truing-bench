/**
 * @file survey_readings.h
 * The latest tension reading of each spoke in the running (or just finished) session, kept so a page that
 * connects part-way through a tension survey can show the whole table. Telemetry is never replayed to a
 * late client, so without this a phone that joins at spoke 14 shows 18 of 32 spokes.
 *
 * One writer (the demo task, as each MEASUREMENT_RESULT passes through it) and any number of readers (the
 * HTTP task): a sequence counter, odd while a write is in progress, lets a reader retry rather than copy a
 * half-written entry. Display only: nothing here feeds the solver or the wheel state.
 */
#ifndef TRUING_SURVEY_READINGS_H
#define TRUING_SURVEY_READINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_SURVEY_MAX_SPOKES 32u
/* Worst case for the JSON: 32 entries of "[31,1234.567,12345.678]," plus the header. */
#define TRUING_SURVEY_JSON_MAX 1280u

typedef struct {
    volatile uint32_t seq;
    uint32_t          cycle;
    bool              have[TRUING_SURVEY_MAX_SPOKES];
    float             hz[TRUING_SURVEY_MAX_SPOKES];        /* NaN: no frequency */
    float             tension_n[TRUING_SURVEY_MAX_SPOKES]; /* NaN: no tension */
} truing_survey_t;

void truing_survey_clear(truing_survey_t *s);
/* Record the latest reading for `index` (a later reading for the same spoke replaces it). */
void truing_survey_put(truing_survey_t *s, uint8_t index, uint32_t cycle, float hz, float tension_n);
/* {"cycle":C,"r":[[index,hz|null,tension|null],...]} in ascending spoke order. Returns the length, or 0 when
 * `cap` is too small or the table kept changing under the reader (the caller answers an error and the page
 * asks again). */
size_t truing_survey_json(const truing_survey_t *s, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_SURVEY_READINGS_H */
