#include "truing/survey_readings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void truing_survey_clear(truing_survey_t *s)
{
    if (s == NULL) {
        return;
    }
    s->seq++;   /* odd: a write is in progress */
    s->cycle = 0u;
    memset((void *)s->have, 0, sizeof(s->have));
    for (size_t i = 0; i < TRUING_SURVEY_MAX_SPOKES; ++i) {
        s->hz[i] = NAN;
        s->tension_n[i] = NAN;
    }
    s->seq++;
}

void truing_survey_put(truing_survey_t *s, uint8_t index, uint32_t cycle, float hz, float tension_n)
{
    if (s == NULL || index >= TRUING_SURVEY_MAX_SPOKES) {
        return;
    }
    s->seq++;
    s->cycle = cycle;
    s->have[index] = true;
    s->hz[index] = hz;
    s->tension_n[index] = tension_n;
    s->seq++;
}

static int put_num(char *p, size_t room, float v)
{
    return isfinite(v) ? snprintf(p, room, "%.3f", (double)v) : snprintf(p, room, "null");
}

size_t truing_survey_json(const truing_survey_t *s, char *buf, size_t cap)
{
    if (s == NULL || buf == NULL || cap == 0u) {
        return 0u;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        const uint32_t before = s->seq;
        if ((before & 1u) != 0u) {
            continue;   /* a write is in progress */
        }
        size_t n = 0u;
        int w = snprintf(buf, cap, "{\"cycle\":%lu,\"r\":[", (unsigned long)s->cycle);
        if (w < 0 || (size_t)w >= cap) {
            return 0u;
        }
        n = (size_t)w;
        bool first = true;
        for (size_t i = 0; i < TRUING_SURVEY_MAX_SPOKES; ++i) {
            if (!s->have[i]) {
                continue;
            }
            w = snprintf(buf + n, cap - n, "%s[%u,", first ? "" : ",", (unsigned)i);
            if (w < 0 || (size_t)w >= cap - n) {
                return 0u;
            }
            n += (size_t)w;
            w = put_num(buf + n, cap - n, s->hz[i]);
            if (w < 0 || (size_t)w >= cap - n) {
                return 0u;
            }
            n += (size_t)w;
            buf[n++] = ',';
            if (n >= cap) {
                return 0u;
            }
            w = put_num(buf + n, cap - n, s->tension_n[i]);
            if (w < 0 || (size_t)w >= cap - n) {
                return 0u;
            }
            n += (size_t)w;
            if (n + 2u >= cap) {
                return 0u;
            }
            buf[n++] = ']';
            first = false;
        }
        if (n + 3u > cap) {
            return 0u;
        }
        buf[n++] = ']';
        buf[n++] = '}';
        buf[n] = '\0';
        if (s->seq == before) {
            return n;
        }
    }
    return 0u;
}
