#include "truing_proto/json.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void truing_json_init(truing_json_writer_t *w, char *buf, size_t cap)
{
    if (w == NULL) {
        return;
    }
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    /* One byte is always reserved for the terminating NUL. */
    if (buf == NULL || cap == 0u) {
        w->overflow = true;
    }
}

static void put(truing_json_writer_t *w, char c)
{
    if (w->overflow) {
        return;
    }
    if (w->len + 1u >= w->cap) {   /* +1 keeps room for the NUL */
        w->overflow = true;
        return;
    }
    w->buf[w->len++] = c;
}

static void put_raw(truing_json_writer_t *w, const char *s)
{
    while (*s != '\0' && !w->overflow) {
        put(w, *s++);
    }
}

/* Escapes per RFC 8259: the two mandatory characters, the short forms, and \u00XX
 * for the remaining control characters. Bytes >= 0x80 are passed through, so a
 * caller handing in valid UTF-8 gets valid UTF-8 out. */
static void put_escaped(truing_json_writer_t *w, const char *s)
{
    static const char *const k_hex = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0' && !w->overflow; ++p) {
        const unsigned char c = *p;
        switch (c) {
        case '"':  put_raw(w, "\\\""); break;
        case '\\': put_raw(w, "\\\\"); break;
        case '\b': put_raw(w, "\\b");  break;
        case '\f': put_raw(w, "\\f");  break;
        case '\n': put_raw(w, "\\n");  break;
        case '\r': put_raw(w, "\\r");  break;
        case '\t': put_raw(w, "\\t");  break;
        default:
            if (c < 0x20u) {
                put_raw(w, "\\u00");
                put(w, k_hex[(c >> 4) & 0x0Fu]);
                put(w, k_hex[c & 0x0Fu]);
            } else {
                put(w, (char)c);
            }
            break;
        }
    }
}

/* Comma, then the key. Every value writer starts here. */
static void prefix(truing_json_writer_t *w, const char *key)
{
    if (w == NULL || w->overflow) {
        return;
    }
    if (w->depth > 0u) {
        if (w->has_member[w->depth - 1u]) {
            put(w, ',');
        }
        w->has_member[w->depth - 1u] = true;
    }
    if (key != NULL) {
        put(w, '"');
        put_escaped(w, key);
        put_raw(w, "\":");
    }
}

static void container_open(truing_json_writer_t *w, const char *key, char brace)
{
    if (w == NULL || w->overflow) {
        return;
    }
    if (w->depth >= TRUING_JSON_MAX_DEPTH) {
        w->overflow = true;    /* structural failure, not merely a full buffer */
        return;
    }
    prefix(w, key);
    put(w, brace);
    w->has_member[w->depth] = false;
    w->depth++;
}

static void container_close(truing_json_writer_t *w, char brace)
{
    if (w == NULL || w->overflow) {
        return;
    }
    if (w->depth == 0u) {
        w->overflow = true;
        return;
    }
    w->depth--;
    put(w, brace);
}

void truing_json_obj_open(truing_json_writer_t *w, const char *key)  { container_open(w, key, '{'); }
void truing_json_obj_close(truing_json_writer_t *w)                  { container_close(w, '}'); }
void truing_json_arr_open(truing_json_writer_t *w, const char *key)  { container_open(w, key, '['); }
void truing_json_arr_close(truing_json_writer_t *w)                  { container_close(w, ']'); }

void truing_json_str(truing_json_writer_t *w, const char *key, const char *value)
{
    if (w == NULL || w->overflow) {
        return;
    }
    if (value == NULL) {
        truing_json_null(w, key);
        return;
    }
    prefix(w, key);
    put(w, '"');
    put_escaped(w, value);
    put(w, '"');
}

void truing_json_bool(truing_json_writer_t *w, const char *key, bool value)
{
    if (w == NULL || w->overflow) {
        return;
    }
    prefix(w, key);
    put_raw(w, value ? "true" : "false");
}

void truing_json_null(truing_json_writer_t *w, const char *key)
{
    if (w == NULL || w->overflow) {
        return;
    }
    prefix(w, key);
    put_raw(w, "null");
}

/* Written by hand rather than through snprintf: no format-string cost, no dependence
 * on how the C library renders integers, and no risk of a truncating conversion. */
static void put_u64(truing_json_writer_t *w, uint64_t v)
{
    char tmp[21];
    unsigned n = 0u;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u && n < sizeof(tmp));
    while (n > 0u) {
        put(w, tmp[--n]);
    }
}

void truing_json_u32(truing_json_writer_t *w, const char *key, uint32_t value)
{
    if (w == NULL || w->overflow) {
        return;
    }
    prefix(w, key);
    put_u64(w, (uint64_t)value);
}

void truing_json_i32(truing_json_writer_t *w, const char *key, int32_t value)
{
    if (w == NULL || w->overflow) {
        return;
    }
    prefix(w, key);
    uint64_t mag;
    if (value < 0) {
        put(w, '-');
        mag = (uint64_t)(-(int64_t)value);   /* via int64 so INT32_MIN negates safely */
    } else {
        mag = (uint64_t)value;
    }
    put_u64(w, mag);
}

void truing_json_f32(truing_json_writer_t *w, const char *key, float value, unsigned decimals)
{
    if (w == NULL || w->overflow) {
        return;
    }
    /* JSON has no NaN or Infinity (RFC 8259 §6). In this firmware a non-finite float
     * means "no value here" — an unmeasured reading, or a rotation the position
     * authority does not vouch for — and `null` says exactly that on the wire. */
    if (!isfinite((double)value)) {
        truing_json_null(w, key);
        return;
    }
    if (decimals > 9u) {
        decimals = 9u;
    }
    char tmp[64];
    const int n = snprintf(tmp, sizeof(tmp), "%.*f", (int)decimals, (double)value);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) {
        truing_json_null(w, key);
        return;
    }
    /* The C standard puts the program in the "C" locale until setlocale() is called and
     * this firmware never calls it, so the separator is a period. The sweep below costs
     * nothing and makes that a checked property rather than an assumption; any character
     * a JSON number may not contain turns the value into `null` instead of corrupt output. */
    for (int i = 0; i < n; ++i) {
        if (tmp[i] == ',') {
            tmp[i] = '.';
        } else if (tmp[i] != '-' && tmp[i] != '.' && (tmp[i] < '0' || tmp[i] > '9')) {
            truing_json_null(w, key);
            return;
        }
    }
    prefix(w, key);
    put_raw(w, tmp);
}

void truing_json_hex(truing_json_writer_t *w, const char *key, const uint8_t *bytes, size_t n)
{
    static const char *const k_hex = "0123456789abcdef";
    if (w == NULL || w->overflow) {
        return;
    }
    if (bytes == NULL) {
        truing_json_null(w, key);
        return;
    }
    prefix(w, key);
    put(w, '"');
    for (size_t i = 0u; i < n && !w->overflow; ++i) {
        put(w, k_hex[(bytes[i] >> 4) & 0x0Fu]);
        put(w, k_hex[bytes[i] & 0x0Fu]);
    }
    put(w, '"');
}

bool truing_json_finish(truing_json_writer_t *w, size_t *len_out)
{
    if (w == NULL || w->buf == NULL || w->cap == 0u) {
        return false;
    }
    if (w->overflow || w->depth != 0u) {
        w->buf[0] = '\0';
        if (len_out != NULL) {
            *len_out = 0u;
        }
        return false;
    }
    w->buf[w->len] = '\0';
    if (len_out != NULL) {
        *len_out = w->len;
    }
    return true;
}
