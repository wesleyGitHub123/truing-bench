/* Phase 1e bring-up: the SPEC §12 wire protocol on the actual board.
 *
 * Byte-exact frame comparison, because the failure this section exists to catch is a
 * silent one: a newlib built without floating-point formatting emits no digits for a
 * "%f" conversion, so every measurement in every frame would quietly become nothing
 * while the frame stayed valid JSON. */
#include "bringup_proto.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "truing_proto/json.h"
#include "truing_proto/wire.h"

static const char *TAG = "bringup";

typedef struct {
    int *pass;
    int *fail;
} tally_t;

static void check(tally_t *t, bool ok, const char *what)
{
    if (ok) {
        (*t->pass)++;
        ESP_LOGI(TAG, "  PASS  %s", what);
    } else {
        (*t->fail)++;
        ESP_LOGE(TAG, "  FAIL  %s", what);
    }
}

static bool encodes_to(const char *actual, const char *expect, const char *what, tally_t *t)
{
    const bool ok = (actual != NULL) && (strcmp(actual, expect) == 0);
    check(t, ok, what);
    if (!ok) {
        ESP_LOGE(TAG, "        expected %s", expect);
        ESP_LOGE(TAG, "        actual   %s", actual != NULL ? actual : "(none)");
    }
    return ok;
}

void truing_bringup_proto_section(int *pass, int *fail, bool *ok_out)
{
    tally_t t = { pass, fail };
    const int fail_before = *fail;
    char buf[TRUING_WIRE_STATE_BUF];

    ESP_LOGI(TAG, "--- wire protocol (SPEC 12) ---");

    /* 1. Float formatting. The one target-dependent step in the encoder, checked to
     *    the digit. If newlib cannot format a float this is where it shows. */
    {
        truing_json_writer_t w;
        truing_json_init(&w, buf, sizeof(buf));
        truing_json_obj_open(&w, NULL);
        truing_json_f32(&w, "a", 1.25f, 3u);
        truing_json_f32(&w, "b", -0.125f, 4u);
        truing_json_f32(&w, "c", 1100.5f, 2u);
        truing_json_f32(&w, "d", 42.0f, 0u);
        truing_json_f32(&w, "nan", NAN, 3u);
        truing_json_obj_close(&w);
        size_t len = 0u;
        const bool fin = truing_json_finish(&w, &len);
        check(&t, fin, "JSON writer completes a float frame on target");
        (void)encodes_to(fin ? buf : NULL,
                         "{\"a\":1.250,\"b\":-0.1250,\"c\":1100.50,\"d\":42,\"nan\":null}",
                         "float formatting is byte-exact with the host (newlib prints digits)", &t);
    }

    /* 2. Number parsing on the way back in, through the target's strtod(). */
    {
        truing_wire_command_t c;
        const truing_wire_error_t e = truing_wire_decode_command(
            "{\"cmd\":\"SUBMIT_RUNOUT\",\"wait_id\":3,\"lateral_mm\":0.31,\"radial_mm\":-0.125}", 0u, &c);
        const bool ok = (e == TRUING_WIRE_OK) &&
                        (c.intent.wait_id == 3u) &&
                        (fabsf(c.intent.payload.runout.lateral_mm - 0.31f) < 1e-6f) &&
                        (fabsf(c.intent.payload.runout.radial_mm + 0.125f) < 1e-9f);
        check(&t, ok, "decoded runout values parse to the same floats as on the host");
    }

    /* 3. The SPEC 12.3 wait_id wire contract, on the board that enforces it. */
    {
        truing_wire_command_t c;
        check(&t, truing_wire_decode_command("{\"cmd\":\"CONFIRM_POSITIONED\"}", 0u, &c) ==
                      TRUING_WIRE_ERR_MISSING_WAIT_ID,
              "confirmation without wait_id is rejected (SPEC 12.3)");
        check(&t, truing_wire_decode_command("{\"cmd\":\"ABORT\",\"wait_id\":1}", 0u, &c) ==
                      TRUING_WIRE_ERR_UNEXPECTED_WAIT_ID,
              "ABORT carrying a wait_id is rejected (SPEC 12.3)");
        check(&t, truing_wire_decode_command("{\"cmd\":\"ABORT\"}", 0u, &c) == TRUING_WIRE_OK &&
                      c.intent.type == TRUING_INTENT_ABORT,
              "ABORT decodes in every state it may be sent from");
    }

    /* 4. A current-state snapshot round-trips: the frame a reconnecting UI needs in
     *    order to learn the wait_id it must echo (SPEC 12.2). Checked by re-reading the
     *    frame rather than against a literal, so this stays a test of the target's float
     *    and string handling and not a copy of the encoder's field order. */
    {
        truing_orch_snapshot_t s;
        memset(&s, 0, sizeof(s));
        s.state = TRUING_STATE_WAIT_FOR_OPERATOR;
        s.waiting = true;
        s.active_wait.wait_id = 7u;
        s.active_wait.kind = TRUING_WAIT_APPLY_ADJUSTMENT;
        s.active_wait.expected_intent = TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE;
        s.active_wait.station = TRUING_STATION_ADJUSTMENT;
        s.active_wait.target_index = 5u;
        s.active_wait.display_turns_rev = 0.25f;
        s.cycle_index = 1u;
        s.cycles_run = 1u;
        s.session_active = true;
        const size_t len = truing_wire_encode_state(&s, buf, sizeof(buf));

        truing_json_doc_t d;
        bool ok = (len > 0u) && truing_json_doc_init(&d, buf, len);
        truing_json_value_t v;
        ok = ok && (truing_json_get(&d, "current_state", &v) == TRUING_JSON_STRING) &&
             truing_json_str_equals(&v, "WAIT_FOR_OPERATOR");
        /* The wait must arrive as an object carrying a usable id, not as null. */
        ok = ok && (truing_json_get(&d, "active_wait", &v) == TRUING_JSON_OBJECT);
        ok = ok && (strstr(buf, "\"wait_id\":7") != NULL);
        ok = ok && (strstr(buf, "\"expected_intent\":\"CONFIRM_ADJUSTMENT_DONE\"") != NULL);
        /* SPEC 10A: a positioning prompt names the feature AND its station. */
        ok = ok && (strstr(buf, "\"station\":\"adjustment\"") != NULL);
        ok = ok && (strstr(buf, "\"target_index\":5") != NULL);
        /* The turns the operator is being asked to apply must survive as a real number. */
        ok = ok && (strstr(buf, "\"display_turns_rev\":0.2500") != NULL);
        check(&t, ok, "current-state snapshot round-trips with its wait_id and prompt (SPEC 12.2)");
        if (!ok) {
            ESP_LOGE(TAG, "        frame %s", len > 0u ? buf : "(none)");
        }
    }

    /* 5. Provenance is the largest frame; confirm the declared budget holds a full
     *    36-spoke wheel on target, where the buffer is real memory. */
    {
        static truing_cycle_provenance_t p;      /* static: too large for this task's stack */
        static char big[TRUING_WIRE_PROVENANCE_BUF];
        memset(&p, 0, sizeof(p));
        p.generating_fingerprint.set = true;
        p.wheel_summary.n_spokes = 36u;
        p.wheel_summary.n_rim_angles = 36u;
        p.plan.valid = true;
        p.plan.n_spokes = 36u;
        for (unsigned i = 0u; i < 36u; ++i) {
            p.spoke_status[i] = TRUING_STATUS_UNAVAILABLE;
            p.spoke_reason[i] = TRUING_REASON_REQUIRES_ARTIFACT_REGENERATION;
            p.runout_status[i] = TRUING_STATUS_UNAVAILABLE;
            p.runout_reason[i] = TRUING_REASON_TENSION_NOT_VERIFICATION_GRADE;
            p.plan.turns_rev[i] = -1.2345f;
        }
        const size_t len = truing_wire_encode_provenance(&p, big, sizeof(big));
        check(&t, len > 0u && len < sizeof(big),
              "GET_CURRENT_CYCLE_PROVENANCE fits its budget for a 36-spoke wheel");
        truing_json_doc_t d;
        check(&t, len > 0u && truing_json_doc_init(&d, big, len),
              "provenance frame re-parses as valid JSON on target");
        ESP_LOGI(TAG, "        provenance frame %u bytes of %u budget", (unsigned)len, (unsigned)sizeof(big));
    }

    if (ok_out != NULL) {
        *ok_out = (*fail == fail_before);
    }
}
