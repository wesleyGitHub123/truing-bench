#include "truing_proto/wire.h"

#include <float.h>
#include <math.h>
#include <string.h>

#include "truing/params.h"
#include "truing_proto/json.h"

/* The wire vocabulary IS the enum's own string table. Deriving the reverse lookup from
 * truing_intent_type_str() / truing_param_str() rather than repeating the names here
 * means the two can never drift apart: adding an intent or a parameter extends the
 * protocol automatically, and renaming one renames it on the wire too. */
static truing_intent_type_t intent_from_json(const truing_json_value_t *v)
{
    for (unsigned i = 1u; i < (unsigned)TRUING_INTENT__COUNT; ++i) {
        if (truing_json_str_equals(v, truing_intent_type_str((truing_intent_type_t)i))) {
            return (truing_intent_type_t)i;
        }
    }
    return TRUING_INTENT_UNSET;
}

static truing_param_id_t param_from_json(const truing_json_value_t *v)
{
    for (unsigned i = 1u; i < (unsigned)TRUING_PARAM__COUNT; ++i) {
        if (truing_json_str_equals(v, truing_param_str((truing_param_id_t)i))) {
            return (truing_param_id_t)i;
        }
    }
    return TRUING_PARAM_UNSET;
}

/* SPEC §12.3: the three confirmations are the intents that answer a wait, and the
 * only ones that carry a wait_id. */
static bool answers_a_wait(truing_intent_type_t t)
{
    return t == TRUING_INTENT_CONFIRM_POSITIONED ||
           t == TRUING_INTENT_SUBMIT_RUNOUT ||
           t == TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE;
}

static bool is_integral(double d)
{
    return floor(d) == d;
}

static bool num_u32(const truing_json_value_t *v, uint32_t *out)
{
    if (v->type != TRUING_JSON_NUMBER || !is_integral(v->number) ||
        v->number < 0.0 || v->number > 4294967295.0) {
        return false;
    }
    *out = (uint32_t)v->number;
    return true;
}

static bool num_i32(const truing_json_value_t *v, int32_t *out)
{
    if (v->type != TRUING_JSON_NUMBER || !is_integral(v->number) ||
        v->number < -2147483648.0 || v->number > 2147483647.0) {
        return false;
    }
    *out = (int32_t)v->number;
    return true;
}

static bool num_float(const truing_json_value_t *v, float *out)
{
    if (v->type != TRUING_JSON_NUMBER) {
        return false;
    }
    /* The JSON grammar admits magnitudes a float cannot hold; those are a range error,
     * not a silent infinity in a measurement field. */
    const double d = v->number;
    if (!isfinite(d) || fabs(d) > (double)FLT_MAX) {
        return false;
    }
    *out = (float)d;
    return true;
}

/* Abandons a partially-decoded command, keeping only `seq`. The client needs that
 * back to tell WHICH of its frames was refused, and everything else would be a
 * half-filled command that no caller should see. */
static truing_wire_error_t reject(truing_wire_command_t *out, truing_wire_error_t e)
{
    const bool has_seq = out->has_seq;
    const uint32_t seq = out->seq;
    memset(out, 0, sizeof(*out));
    out->has_seq = has_seq;
    out->seq = seq;
    return e;
}

/* Reads a required number field. */
static truing_wire_error_t need_float(const truing_json_doc_t *d, const char *key, float *out)
{
    truing_json_value_t v;
    const truing_json_type_t t = truing_json_get(d, key, &v);
    if (t == TRUING_JSON_ABSENT) {
        return TRUING_WIRE_ERR_MISSING_FIELD;
    }
    if (t != TRUING_JSON_NUMBER) {
        return TRUING_WIRE_ERR_FIELD_TYPE;
    }
    return num_float(&v, out) ? TRUING_WIRE_OK : TRUING_WIRE_ERR_FIELD_RANGE;
}

truing_wire_error_t truing_wire_decode_command(const char *json, size_t len, truing_wire_command_t *out)
{
    if (out == NULL) {
        return TRUING_WIRE_ERR_MALFORMED;
    }
    memset(out, 0, sizeof(*out));
    if (json == NULL) {
        return TRUING_WIRE_ERR_MALFORMED;
    }
    if (len == 0u) {
        len = strlen(json);
    }
    if (len > TRUING_WIRE_COMMAND_MAX) {
        return TRUING_WIRE_ERR_TOO_LONG;
    }

    truing_json_doc_t doc;
    if (!truing_json_doc_init(&doc, json, len)) {
        return TRUING_WIRE_ERR_MALFORMED;
    }

    truing_json_value_t v;
    if (truing_json_get(&doc, "cmd", &v) == TRUING_JSON_ABSENT) {
        return TRUING_WIRE_ERR_MISSING_FIELD;
    }
    if (v.type != TRUING_JSON_STRING) {
        return TRUING_WIRE_ERR_FIELD_TYPE;
    }
    const truing_json_value_t cmd = v;

    /* Optional correlation number. Purely an echo for the client's benefit: it carries
     * no delivery guarantee and the firmware never reasons about it (SPEC §12.2). */
    truing_json_value_t seq;
    const truing_json_type_t seq_t = truing_json_get(&doc, "seq", &seq);
    if (seq_t != TRUING_JSON_ABSENT) {
        if (seq_t != TRUING_JSON_NUMBER) {
            return TRUING_WIRE_ERR_FIELD_TYPE;
        }
        if (!num_u32(&seq, &out->seq)) {
            return TRUING_WIRE_ERR_FIELD_RANGE;
        }
        out->has_seq = true;
    }

    /* The two queries first: they are not intents and have no payload. */
    if (truing_json_str_equals(&cmd, "GET_CURRENT_STATE")) {
        out->kind = TRUING_WIRE_CMD_GET_CURRENT_STATE;
        return TRUING_WIRE_OK;
    }
    if (truing_json_str_equals(&cmd, "GET_CURRENT_CYCLE_PROVENANCE")) {
        out->kind = TRUING_WIRE_CMD_GET_CURRENT_CYCLE_PROVENANCE;
        return TRUING_WIRE_OK;
    }

    const truing_intent_type_t type = intent_from_json(&cmd);
    if (type == TRUING_INTENT_UNSET) {
        return TRUING_WIRE_ERR_UNKNOWN_COMMAND;
    }

    /* SPEC §12.3 wait_id, enforced structurally. A value that is present but wrong is
     * NOT judged here — only the orchestrator knows which wait is active, and it answers
     * with STALE_INTENT. Zero is rejected alongside an omitted field because zero is the
     * "not answering a wait" sentinel in truing_intent_t: it identifies no wait, so
     * accepting it would put a value into the intent that contradicts the command. */
    truing_json_value_t wid;
    const truing_json_type_t wid_t = truing_json_get(&doc, "wait_id", &wid);
    if (answers_a_wait(type)) {
        if (wid_t == TRUING_JSON_ABSENT) {
            return TRUING_WIRE_ERR_MISSING_WAIT_ID;
        }
        if (wid_t != TRUING_JSON_NUMBER) {
            return TRUING_WIRE_ERR_FIELD_TYPE;
        }
        uint32_t wait_id = 0u;
        if (!num_u32(&wid, &wait_id)) {
            return TRUING_WIRE_ERR_FIELD_RANGE;
        }
        if (wait_id == 0u) {
            return TRUING_WIRE_ERR_MISSING_WAIT_ID;
        }
        out->intent.wait_id = wait_id;
    } else if (wid_t != TRUING_JSON_ABSENT) {
        return TRUING_WIRE_ERR_UNEXPECTED_WAIT_ID;
    }

    out->kind = TRUING_WIRE_CMD_INTENT;
    out->intent.type = type;

    switch (type) {
    case TRUING_INTENT_SET_PARAMETER: {
        truing_json_value_t p;
        const truing_json_type_t pt = truing_json_get(&doc, "param", &p);
        if (pt == TRUING_JSON_ABSENT) {
            return reject(out, TRUING_WIRE_ERR_MISSING_FIELD);
        }
        if (pt != TRUING_JSON_STRING) {
            return reject(out, TRUING_WIRE_ERR_FIELD_TYPE);
        }
        const truing_param_id_t id = param_from_json(&p);
        if (id == TRUING_PARAM_UNSET) {
            /* Includes host-only model-preparation values, which SPEC §12.3.1 puts
             * outside the command surface entirely: they have no id here, so they are
             * unaddressable rather than refused. */
            return reject(out, TRUING_WIRE_ERR_UNKNOWN_PARAMETER);
        }
        float value = 0.0f;
        const truing_wire_error_t e = need_float(&doc, "value", &value);
        if (e != TRUING_WIRE_OK) {
            return reject(out, e);
        }
        out->intent.payload.set_parameter.id = id;
        out->intent.payload.set_parameter.value = value;
        break;
    }
    case TRUING_INTENT_SUBMIT_RUNOUT: {
        float lateral = 0.0f;
        float radial = 0.0f;
        truing_wire_error_t e = need_float(&doc, "lateral_mm", &lateral);
        if (e == TRUING_WIRE_OK) {
            e = need_float(&doc, "radial_mm", &radial);
        }
        if (e != TRUING_WIRE_OK) {
            return reject(out, e);
        }
        out->intent.payload.runout.lateral_mm = lateral;
        out->intent.payload.runout.radial_mm = radial;
        break;
    }
    case TRUING_INTENT_DEBUG: {
        truing_json_value_t c;
        const truing_json_type_t ct = truing_json_get(&doc, "code", &c);
        if (ct == TRUING_JSON_ABSENT) {
            return reject(out, TRUING_WIRE_ERR_MISSING_FIELD);
        }
        if (ct != TRUING_JSON_NUMBER) {
            return reject(out, TRUING_WIRE_ERR_FIELD_TYPE);
        }
        uint32_t code = 0u;
        if (!num_u32(&c, &code) || code > 0xFFFFu) {
            return reject(out, TRUING_WIRE_ERR_FIELD_RANGE);
        }
        int32_t arg = 0;
        truing_json_value_t a;
        const truing_json_type_t at = truing_json_get(&doc, "arg", &a);
        if (at != TRUING_JSON_ABSENT) {
            if (at != TRUING_JSON_NUMBER) {
                return reject(out, TRUING_WIRE_ERR_FIELD_TYPE);
            }
            if (!num_i32(&a, &arg)) {
                return reject(out, TRUING_WIRE_ERR_FIELD_RANGE);
            }
        }
        out->intent.payload.debug.code = (uint16_t)code;
        out->intent.payload.debug.arg = arg;
        break;
    }
    default:
        break;   /* START_TRUING, ABORT and the two confirmations carry no payload */
    }
    return TRUING_WIRE_OK;
}

const char *truing_wire_error_str(truing_wire_error_t e)
{
    switch (e) {
    case TRUING_WIRE_OK:                     return "OK";
    case TRUING_WIRE_ERR_TOO_LONG:           return "TOO_LONG";
    case TRUING_WIRE_ERR_MALFORMED:          return "MALFORMED";
    case TRUING_WIRE_ERR_UNKNOWN_COMMAND:    return "UNKNOWN_COMMAND";
    case TRUING_WIRE_ERR_MISSING_WAIT_ID:    return "MISSING_WAIT_ID";
    case TRUING_WIRE_ERR_UNEXPECTED_WAIT_ID: return "UNEXPECTED_WAIT_ID";
    case TRUING_WIRE_ERR_UNKNOWN_PARAMETER:  return "UNKNOWN_PARAMETER";
    case TRUING_WIRE_ERR_MISSING_FIELD:      return "MISSING_FIELD";
    case TRUING_WIRE_ERR_FIELD_TYPE:         return "FIELD_TYPE";
    case TRUING_WIRE_ERR_FIELD_RANGE:        return "FIELD_RANGE";
    default:                                 return "?";
    }
}

const char *truing_wire_cmd_kind_str(truing_wire_cmd_kind_t k)
{
    switch (k) {
    case TRUING_WIRE_CMD_UNSET:                        return "UNSET";
    case TRUING_WIRE_CMD_INTENT:                       return "INTENT";
    case TRUING_WIRE_CMD_GET_CURRENT_STATE:            return "GET_CURRENT_STATE";
    case TRUING_WIRE_CMD_GET_CURRENT_CYCLE_PROVENANCE: return "GET_CURRENT_CYCLE_PROVENANCE";
    default:                                           return "?";
    }
}
