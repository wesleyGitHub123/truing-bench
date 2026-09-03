/**
 * @file json.h
 * Bounded, allocation-free JSON primitives for the SPEC §12 wire protocol.
 *
 * Two halves, deliberately minimal:
 *
 *   - a WRITER that appends to a caller-owned buffer and latches an overflow flag.
 *     Truncated output is never handed back as if it were valid: finish() returns
 *     false and the caller emits nothing. This matters because telemetry is
 *     best-effort (SPEC §12.2) — dropping a frame is correct, sending half of one
 *     is not.
 *
 *   - a READER that structurally validates one document and then looks up members
 *     of the top-level object. Command frames are flat, so no DOM, no allocation
 *     and no in-place mutation of the input: values are returned as spans into the
 *     caller's buffer.
 *
 * Recursion in the validator is depth-capped (TRUING_JSON_MAX_DEPTH) so a hostile
 * or corrupt frame cannot exhaust the task stack.
 *
 * NaN and infinity have no JSON representation. truing_json_f32() writes `null`
 * for a non-finite value, which is also what those fields mean in this firmware
 * (an unmeasured value, or a rotation the position authority does not vouch for).
 */
#ifndef TRUING_PROTO_JSON_H
#define TRUING_PROTO_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_JSON_MAX_DEPTH 8u

/* ---- Writer ---------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflow;                          /* latched; never cleared by a later append */
    uint8_t depth;
    bool   has_member[TRUING_JSON_MAX_DEPTH]; /* whether a comma is owed at this depth */
} truing_json_writer_t;

void truing_json_init(truing_json_writer_t *w, char *buf, size_t cap);

/* `key` is NULL for the root value and for array elements; non-NULL inside an object. */
void truing_json_obj_open(truing_json_writer_t *w, const char *key);
void truing_json_obj_close(truing_json_writer_t *w);
void truing_json_arr_open(truing_json_writer_t *w, const char *key);
void truing_json_arr_close(truing_json_writer_t *w);

void truing_json_str(truing_json_writer_t *w, const char *key, const char *value);
void truing_json_u32(truing_json_writer_t *w, const char *key, uint32_t value);
void truing_json_i32(truing_json_writer_t *w, const char *key, int32_t value);
void truing_json_bool(truing_json_writer_t *w, const char *key, bool value);
void truing_json_null(truing_json_writer_t *w, const char *key);
/* `decimals` in [0, 9]. Non-finite values are written as `null`. */
void truing_json_f32(truing_json_writer_t *w, const char *key, float value, unsigned decimals);
/* Lowercase hex of `n` bytes, as a JSON string. */
void truing_json_hex(truing_json_writer_t *w, const char *key, const uint8_t *bytes, size_t n);

/* NUL-terminates and reports the length. False if anything overflowed or a
 * container was left open — in which case the buffer contents are meaningless. */
bool truing_json_finish(truing_json_writer_t *w, size_t *len_out);

/* ---- Reader ---------------------------------------------------------------------- */

typedef enum {
    TRUING_JSON_ABSENT = 0,
    TRUING_JSON_STRING,
    TRUING_JSON_NUMBER,
    TRUING_JSON_BOOL,
    TRUING_JSON_NULL,
    TRUING_JSON_OBJECT,
    TRUING_JSON_ARRAY,
} truing_json_type_t;

typedef struct {
    truing_json_type_t type;
    const char        *start;   /* span into the caller's buffer; strings exclude the quotes */
    size_t             len;
    double             number;  /* NUMBER only */
    bool               boolean; /* BOOL only */
} truing_json_value_t;

typedef struct {
    const char *json;
    size_t      len;
    bool        valid;
} truing_json_doc_t;

/* Structurally validates the whole document and requires the root to be an object.
 * False for anything malformed, over-deep, or carrying trailing garbage. */
bool truing_json_doc_init(truing_json_doc_t *d, const char *json, size_t len);

/* Member of the top-level object. ABSENT when the key is not present. A duplicate
 * key resolves to the FIRST occurrence, so a later one cannot override an earlier. */
truing_json_type_t truing_json_get(const truing_json_doc_t *d, const char *key, truing_json_value_t *out);

/* Compares a STRING value against a C string, decoding escapes as it goes. */
bool truing_json_str_equals(const truing_json_value_t *v, const char *s);
/* Copies a STRING value out with escapes decoded. False if it does not fit. */
bool truing_json_copy_str(const truing_json_value_t *v, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_PROTO_JSON_H */
