/* Acoustic subsystem SYNTHETIC implementation (SPEC §14.5): scripted per-spoke
 * outcomes so the workflow runs with no hardware. Every estimate it produces is
 * `suspect` / PROVISIONAL_MODE_ID with mode_identity presumed_fundamental, exactly
 * as a real Stage 1 implementation must while layer 3 is interim (SPEC §4.4.1). */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing_hal/acoustic_if.h"
#include "truing_hal/records.h"

static void synthetic_measure(truing_acoustic_if_t *self, uint8_t spoke_id,
                              const truing_wheel_class_config_t *wheel_geometry,
                              uint8_t cycle_index, truing_tension_estimate_t *out)
{
    (void)wheel_geometry;   /* a damping ritual targeting a neighbour would use it (SPEC §9.1) */
    truing_acoustic_synthetic_ctx_t *ctx = (truing_acoustic_synthetic_ctx_t *)self->ctx;
    if (out == NULL) {
        return;
    }
    if (ctx == NULL) {
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_NOT_IMPLEMENTED, cycle_index, 0u, self->source_impl);
        return;
    }
    ctx->calls++;
    const uint32_t now = truing_clock_now_ms(&ctx->clock);

    if (ctx->cancel_requested) {
        /* SPEC §7.4 / §12.3: return at the next safe point; the incomplete measurement is discarded. */
        ctx->cancel_requested = false;
        ctx->cancelled_calls++;
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_CANCELLED, cycle_index, now, self->source_impl);
        return;
    }
    if (spoke_id >= ctx->n_spokes) {
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_VALUE_OUT_OF_RANGE, cycle_index, now, self->source_impl);
        return;
    }

    memset(out, 0, sizeof(*out));
    out->meta.cycle_index = cycle_index;
    out->meta.timestamp_ms = now;
    out->meta.source_impl = self->source_impl;
    out->model_name = ctx->model_name;
    out->model_version = ctx->model_version;
    out->sigma_n = NAN;

    const truing_status_t scripted = ctx->fail_status[spoke_id];
    if (scripted != TRUING_STATUS_UNSET) {
        /* Scripted failure: status by cause (SPEC §7.4 table), value absent. */
        out->meta.status = scripted;
        out->meta.reason_code = ctx->fail_reason[spoke_id];
        out->tension_n = NAN;
        out->frequency.selected_frequency_hz = NAN;
        out->frequency.snr_db = NAN;
        return;
    }

    truing_frequency_measurement_t *f = &out->frequency;
    f->meta.status = TRUING_STATUS_SUSPECT;
    f->meta.reason_code = TRUING_REASON_PROVISIONAL_MODE_ID;
    f->meta.cycle_index = cycle_index;
    f->meta.timestamp_ms = now;
    f->meta.source_impl = self->source_impl;
    f->selected_frequency_hz = ctx->frequency_hz[spoke_id];
    f->mode_identity = TRUING_MODE_ID_PRESUMED_FUNDAMENTAL;
    f->n_candidates = 1u;
    f->candidates[0].frequency_hz = ctx->frequency_hz[spoke_id];
    f->candidates[0].magnitude_db = 0.0f;
    f->candidates[0].prominence_db = ctx->snr_db;
    f->snr_db = ctx->snr_db;
    f->selection_rule_version = ctx->selection_rule_version;

    out->meta.status = TRUING_STATUS_SUSPECT;
    out->meta.reason_code = TRUING_REASON_PROVISIONAL_MODE_ID;
    out->tension_n = ctx->tension_n[spoke_id];
}

static void synthetic_cancel(truing_acoustic_if_t *self)
{
    truing_acoustic_synthetic_ctx_t *ctx = (truing_acoustic_synthetic_ctx_t *)self->ctx;
    if (ctx != NULL) {
        ctx->cancel_requested = true;
    }
}

static void synthetic_reset_session(truing_acoustic_if_t *self)
{
    truing_acoustic_synthetic_ctx_t *ctx = (truing_acoustic_synthetic_ctx_t *)self->ctx;
    if (ctx != NULL) {
        /* A cancel that no measurement consumed must not answer the next session's first
         * spoke (SPEC §7.4 / §12.3). The call counters are observability, left untouched. */
        ctx->cancel_requested = false;
    }
}

void truing_acoustic_synthetic_init(truing_acoustic_if_t *self, truing_acoustic_synthetic_ctx_t *ctx,
                                    truing_clock_if_t clock, uint8_t n_spokes, truing_tension_model_t model_name,
                                    uint16_t model_version)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->clock = clock;
    ctx->n_spokes = n_spokes <= TRUING_MAX_SPOKES ? n_spokes : (uint8_t)TRUING_MAX_SPOKES;
    ctx->model_name = model_name;
    ctx->model_version = model_version;
    ctx->selection_rule_version = 0u;
    ctx->snr_db = NAN;   /* caller scripts it; an unscripted spoke fails validation rather than lying */
    for (uint8_t i = 0; i < TRUING_MAX_SPOKES; ++i) {
        ctx->tension_n[i] = NAN;
        ctx->frequency_hz[i] = NAN;
    }
    self->impl_name = "acoustic_synthetic";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->measure_spoke_tension = synthetic_measure;
    self->request_cancel = synthetic_cancel;
    self->reset_session = synthetic_reset_session;
    self->ready = NULL;
    self->ctx = ctx;
}

void truing_acoustic_synthetic_set_spoke(truing_acoustic_synthetic_ctx_t *ctx, uint8_t spoke, float tension_n, float frequency_hz)
{
    if (ctx == NULL || spoke >= TRUING_MAX_SPOKES) {
        return;
    }
    ctx->tension_n[spoke] = tension_n;
    ctx->frequency_hz[spoke] = frequency_hz;
    ctx->fail_status[spoke] = TRUING_STATUS_UNSET;
    ctx->fail_reason[spoke] = TRUING_REASON_NONE;
}

void truing_acoustic_synthetic_set_failure(truing_acoustic_synthetic_ctx_t *ctx, uint8_t spoke, truing_status_t status, truing_reason_t reason)
{
    if (ctx == NULL || spoke >= TRUING_MAX_SPOKES) {
        return;
    }
    ctx->fail_status[spoke] = status;
    ctx->fail_reason[spoke] = reason;
}
