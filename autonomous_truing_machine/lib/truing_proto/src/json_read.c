#include "truing_proto/json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    const char *end;
} cur_t;

static void skip_ws(cur_t *c)
{
    while (c->p < c->end) {
        const char ch = *c->p;
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            c->p++;
        } else {
            break;
        }
    }
}

static bool at(const cur_t *c, char ch)
{
    return c->p < c->end && *c->p == ch;
}

static int hex_val(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* On success `out` spans the contents BETWEEN the quotes and the cursor sits past
 * the closing quote. Raw control characters are rejected, as RFC 8259 requires. */
static bool parse_string(cur_t *c, const char **out, size_t *out_len)
{
    if (!at(c, '"')) {
        return false;
    }
    c->p++;
    const char *start = c->p;
    while (c->p < c->end) {
        const unsigned char ch = (unsigned char)*c->p;
        if (ch == '"') {
            if (out != NULL) {
                *out = start;
                *out_len = (size_t)(c->p - start);
            }
            c->p++;
            return true;
        }
        if (ch == '\\') {
            c->p++;
            if (c->p >= c->end) {
                return false;
            }
            const char esc = *c->p;
            if (esc == 'u') {
                if (c->end - c->p < 5) {
                    return false;
                }
                for (int i = 1; i <= 4; ++i) {
                    if (hex_val(c->p[i]) < 0) {
                        return false;
                    }
                }
                c->p += 5;
            } else if (esc == '"' || esc == '\\' || esc == '/' || esc == 'b' ||
                       esc == 'f' || esc == 'n' || esc == 'r' || esc == 't') {
                c->p++;
            } else {
                return false;
            }
            continue;
        }
        if (ch < 0x20u) {
            return false;
        }
        c->p++;
    }
    return false;
}

static bool parse_number(cur_t *c, double *out)
{
    const char *start = c->p;
    if (at(c, '-')) {
        c->p++;
    }
    if (c->p >= c->end) {
        return false;
    }
    if (*c->p == '0') {
        c->p++;
    } else if (*c->p >= '1' && *c->p <= '9') {
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            c->p++;
        }
    } else {
        return false;   /* a leading '.', '+', or "01" is not a JSON number */
    }
    if (at(c, '.')) {
        c->p++;
        if (c->p >= c->end || *c->p < '0' || *c->p > '9') {
            return false;
        }
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            c->p++;
        }
    }
    if (at(c, 'e') || at(c, 'E')) {
        c->p++;
        if (at(c, '+') || at(c, '-')) {
            c->p++;
        }
        if (c->p >= c->end || *c->p < '0' || *c->p > '9') {
            return false;
        }
        while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
            c->p++;
        }
    }
    if (out != NULL) {
        /* The grammar above is already validated, so this only has to convert. The C
         * standard starts a program in the "C" locale and this firmware never calls
         * setlocale(), so the separator strtod() expects is the period JSON mandates. */
        char tmp[48];
        const size_t n = (size_t)(c->p - start);
        if (n >= sizeof(tmp)) {
            return false;
        }
        memcpy(tmp, start, n);
        tmp[n] = '\0';
        *out = strtod(tmp, NULL);
    }
    return true;
}

static bool parse_literal(cur_t *c, const char *lit)
{
    const size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) {
        return false;
    }
    c->p += n;
    return true;
}

static bool parse_value(cur_t *c, unsigned depth, truing_json_value_t *out);

static bool parse_object(cur_t *c, unsigned depth)
{
    c->p++;   /* '{' */
    skip_ws(c);
    if (at(c, '}')) {
        c->p++;
        return true;
    }
    for (;;) {
        skip_ws(c);
        if (!parse_string(c, NULL, NULL)) {
            return false;
        }
        skip_ws(c);
        if (!at(c, ':')) {
            return false;
        }
        c->p++;
        skip_ws(c);
        if (!parse_value(c, depth + 1u, NULL)) {
            return false;
        }
        skip_ws(c);
        if (at(c, ',')) {
            c->p++;
            continue;
        }
        if (at(c, '}')) {
            c->p++;
            return true;
        }
        return false;
    }
}

static bool parse_array(cur_t *c, unsigned depth)
{
    c->p++;   /* '[' */
    skip_ws(c);
    if (at(c, ']')) {
        c->p++;
        return true;
    }
    for (;;) {
        skip_ws(c);
        if (!parse_value(c, depth + 1u, NULL)) {
            return false;
        }
        skip_ws(c);
        if (at(c, ',')) {
            c->p++;
            continue;
        }
        if (at(c, ']')) {
            c->p++;
            return true;
        }
        return false;
    }
}

/* Validates one value and advances past it, recording it in `out` when asked.
 * Depth is capped so a deeply nested frame cannot exhaust the task stack. */
static bool parse_value(cur_t *c, unsigned depth, truing_json_value_t *out)
{
    if (depth > TRUING_JSON_MAX_DEPTH || c->p >= c->end) {
        return false;
    }
    const char *start = c->p;
    truing_json_value_t v;
    memset(&v, 0, sizeof(v));

    switch (*c->p) {
    case '{':
        if (!parse_object(c, depth)) {
            return false;
        }
        v.type = TRUING_JSON_OBJECT;
        break;
    case '[':
        if (!parse_array(c, depth)) {
            return false;
        }
        v.type = TRUING_JSON_ARRAY;
        break;
    case '"':
        if (!parse_string(c, &v.start, &v.len)) {
            return false;
        }
        v.type = TRUING_JSON_STRING;
        if (out != NULL) {
            *out = v;
        }
        return true;   /* start/len already point between the quotes */
    case 't':
        if (!parse_literal(c, "true")) {
            return false;
        }
        v.type = TRUING_JSON_BOOL;
        v.boolean = true;
        break;
    case 'f':
        if (!parse_literal(c, "false")) {
            return false;
        }
        v.type = TRUING_JSON_BOOL;
        v.boolean = false;
        break;
    case 'n':
        if (!parse_literal(c, "null")) {
            return false;
        }
        v.type = TRUING_JSON_NULL;
        break;
    default:
        if (!parse_number(c, &v.number)) {
            return false;
        }
        v.type = TRUING_JSON_NUMBER;
        break;
    }
    v.start = start;
    v.len = (size_t)(c->p - start);
    if (out != NULL) {
        *out = v;
    }
    return true;
}

bool truing_json_doc_init(truing_json_doc_t *d, const char *json, size_t len)
{
    if (d == NULL) {
        return false;
    }
    memset(d, 0, sizeof(*d));
    if (json == NULL || len == 0u) {
        return false;
    }
    cur_t c = { json, json + len };
    skip_ws(&c);
    truing_json_value_t root;
    if (!parse_value(&c, 0u, &root) || root.type != TRUING_JSON_OBJECT) {
        return false;
    }
    skip_ws(&c);
    if (c.p != c.end) {
        return false;   /* trailing garbage: reject rather than parse a prefix */
    }
    d->json = json;
    d->len = len;
    d->valid = true;
    return true;
}

/* Decodes the next character of a string span into UTF-8.
 * Returns the byte count, 0 at the end of the span, or -1 on a malformed escape. */
static int decode_next(const char **p, const char *end, unsigned char out[4])
{
    if (*p >= end) {
        return 0;
    }
    unsigned char ch = (unsigned char)**p;
    if (ch != '\\') {
        (*p)++;
        out[0] = ch;
        return 1;
    }
    (*p)++;
    if (*p >= end) {
        return -1;
    }
    const char esc = **p;
    (*p)++;
    switch (esc) {
    case '"':  out[0] = '"';  return 1;
    case '\\': out[0] = '\\'; return 1;
    case '/':  out[0] = '/';  return 1;
    case 'b':  out[0] = '\b'; return 1;
    case 'f':  out[0] = '\f'; return 1;
    case 'n':  out[0] = '\n'; return 1;
    case 'r':  out[0] = '\r'; return 1;
    case 't':  out[0] = '\t'; return 1;
    case 'u':  break;
    default:   return -1;
    }
    if (end - *p < 4) {
        return -1;
    }
    uint32_t cp = 0u;
    for (int i = 0; i < 4; ++i) {
        const int h = hex_val((*p)[i]);
        if (h < 0) {
            return -1;
        }
        cp = (cp << 4) | (uint32_t)h;
    }
    *p += 4;
    if (cp >= 0xD800u && cp <= 0xDBFFu) {          /* high surrogate: a low one must follow */
        if (end - *p < 6 || (*p)[0] != '\\' || (*p)[1] != 'u') {
            return -1;
        }
        uint32_t lo = 0u;
        for (int i = 0; i < 4; ++i) {
            const int h = hex_val((*p)[2 + i]);
            if (h < 0) {
                return -1;
            }
            lo = (lo << 4) | (uint32_t)h;
        }
        if (lo < 0xDC00u || lo > 0xDFFFu) {
            return -1;
        }
        *p += 6;
        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
        return -1;                                  /* unpaired low surrogate */
    }
    if (cp < 0x80u) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (unsigned char)(0xC0u | (cp >> 6));
        out[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (unsigned char)(0xE0u | (cp >> 12));
        out[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = (unsigned char)(0xF0u | (cp >> 18));
    out[1] = (unsigned char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (unsigned char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static bool span_equals(const char *start, size_t len, const char *s)
{
    if (start == NULL || s == NULL) {
        return false;
    }
    const char *p = start;
    const char *end = start + len;
    const unsigned char *q = (const unsigned char *)s;
    for (;;) {
        unsigned char dec[4];
        const int n = decode_next(&p, end, dec);
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            return *q == '\0';
        }
        for (int i = 0; i < n; ++i) {
            if (*q == '\0' || *q != dec[i]) {
                return false;
            }
            q++;
        }
    }
}

truing_json_type_t truing_json_get(const truing_json_doc_t *d, const char *key, truing_json_value_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (d == NULL || !d->valid || key == NULL) {
        return TRUING_JSON_ABSENT;
    }
    cur_t c = { d->json, d->json + d->len };
    skip_ws(&c);
    if (!at(&c, '{')) {
        return TRUING_JSON_ABSENT;
    }
    c.p++;
    skip_ws(&c);
    if (at(&c, '}')) {
        return TRUING_JSON_ABSENT;
    }
    /* The document validated in doc_init(), so every step below is known to succeed;
     * the checks remain because this walker must also be safe if it is ever reached
     * with a document that did not. The FIRST match wins, so a duplicate key later in
     * the frame cannot override the value an earlier one already established. */
    for (;;) {
        skip_ws(&c);
        const char *k_start = NULL;
        size_t k_len = 0u;
        if (!parse_string(&c, &k_start, &k_len)) {
            return TRUING_JSON_ABSENT;
        }
        skip_ws(&c);
        if (!at(&c, ':')) {
            return TRUING_JSON_ABSENT;
        }
        c.p++;
        skip_ws(&c);
        truing_json_value_t v;
        if (!parse_value(&c, 1u, &v)) {
            return TRUING_JSON_ABSENT;
        }
        if (span_equals(k_start, k_len, key)) {
            if (out != NULL) {
                *out = v;
            }
            return v.type;
        }
        skip_ws(&c);
        if (at(&c, ',')) {
            c.p++;
            continue;
        }
        return TRUING_JSON_ABSENT;
    }
}

bool truing_json_str_equals(const truing_json_value_t *v, const char *s)
{
    if (v == NULL || v->type != TRUING_JSON_STRING) {
        return false;
    }
    return span_equals(v->start, v->len, s);
}

bool truing_json_copy_str(const truing_json_value_t *v, char *out, size_t cap)
{
    if (v == NULL || out == NULL || cap == 0u) {
        return false;
    }
    out[0] = '\0';
    if (v->type != TRUING_JSON_STRING) {
        return false;
    }
    const char *p = v->start;
    const char *end = v->start + v->len;
    size_t n = 0u;
    for (;;) {
        unsigned char dec[4];
        const int got = decode_next(&p, end, dec);
        if (got < 0) {
            out[0] = '\0';
            return false;
        }
        if (got == 0) {
            out[n] = '\0';
            return true;
        }
        if (n + (size_t)got + 1u > cap) {
            out[0] = '\0';
            return false;
        }
        for (int i = 0; i < got; ++i) {
            out[n++] = (char)dec[i];
        }
    }
}
