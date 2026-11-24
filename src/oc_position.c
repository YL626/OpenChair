#include "oc_position.h"

#include <errno.h>

#include "oc_store.h"     /* enum oc_origin */
#include "oc_vl6180x.h"   /* range error helpers */

static bool cfg_ok(const struct oc_position_cfg *c)
{
    return c->min_mm <= c->max_mm && c->max_spread_mm >= 0 && c->window_n >= 1 &&
           c->window_n <= OC_POSITION_WINDOW_MAX && c->max_age_ns > 0;
}

int oc_position_init(struct oc_position *p, const struct oc_position_cfg *cfg)
{
    if (p == nullptr || cfg == nullptr || !cfg_ok(cfg))
        return -EINVAL;
    *p = (struct oc_position){.cfg = *cfg};
    return 0;
}

static void clear_window(struct oc_position *p)
{
    p->head = 0;
    p->n = 0;
}

void oc_position_reset(struct oc_position *p)
{
    if (p == nullptr)
        return;
    clear_window(p);
    p->have_last = false;
    p->last_seq = 0;
    p->last_t_ns = 0;
    p->last_status = 0;
    p->last_origin = 0;
    p->last_aux = 0;
}

/* True if s continues the stream: the next seq (wrap-safe) and no step back
 * in time. */
static bool continues(const struct oc_position *p, const struct oc_sample *s)
{
    return (uint32_t)(s->seq - p->last_seq) == 1u && s->t_mono_ns >= p->last_t_ns;
}

int oc_position_push(struct oc_position *p, const struct oc_sample *s)
{
    if (p == nullptr || s == nullptr || oc_sample_validate(s) != 0 ||
        s->source != OC_SOURCE_DISTANCE)
        return -EINVAL;

    if (p->have_last && !continues(p, s)) {
        clear_window(p);
        p->discontinuities++;
    }
    p->have_last = true;
    p->last_seq = s->seq;
    p->last_t_ns = s->t_mono_ns;
    p->last_status = s->status;
    p->last_origin = s->origin;
    p->last_aux = s->aux;

    if (s->status != OC_ST_OK) {
        clear_window(p);
        return 0;
    }
    p->mm[p->head] = s->value;
    p->origin[p->head] = s->origin;
    p->head = (p->head + 1) % p->cfg.window_n;
    if (p->n < p->cfg.window_n)
        p->n++;
    return 0;
}

/* Insertion sort of at most OC_POSITION_WINDOW_MAX values. */
static void sort_small(int32_t *a, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        int32_t v = a[i];
        uint32_t j = i;
        for (; j > 0 && a[j - 1] > v; j--)
            a[j] = a[j - 1];
        a[j] = v;
    }
}

/* Median, spread and common origin of the window (n >= 1). The ring holds
 * exactly the newest n values, in slots 0..n-1 in some rotation, because it
 * is emptied rather than partially cleared. */
static void window_stats(const struct oc_position *p, struct oc_position_verdict *v)
{
    int32_t a[OC_POSITION_WINDOW_MAX];
    uint8_t origin = p->origin[0];
    for (uint32_t i = 0; i < p->n; i++) {
        a[i] = p->mm[i];
        if (p->origin[i] != origin)
            origin = OC_ORIGIN_MIXED;
    }
    sort_small(a, p->n);
    const uint32_t m = p->n / 2;
    if (p->n % 2 == 1) {
        v->median_mm = a[m];
    } else {   /* floor of the mean of the two middle values, without overflow */
        const int64_t sum = (int64_t)a[m - 1] + a[m];
        v->median_mm = (int32_t)(sum >= 0 ? sum / 2 : -((-sum + 1) / 2));
    }
    v->spread_mm = a[p->n - 1] - a[0];
    v->origin = origin;
}

int oc_position_eval(const struct oc_position *p, uint64_t now_ns,
                     struct oc_position_verdict *out)
{
    if (p == nullptr || out == nullptr)
        return -EINVAL;
    struct oc_position_verdict v = {.state = OC_POS_UNAVAILABLE, .origin = OC_ORIGIN_NONE};
    if (!p->have_last) {
        *out = v;
        return 0;
    }
    v.origin = p->last_origin;
    v.last_status = p->last_status;
    v.age_ns = now_ns >= p->last_t_ns ? now_ns - p->last_t_ns : 0;
    v.n = p->n;
    if (p->last_status == OC_ST_RANGE_ERR)
        v.range_error = oc_vl6180x_range_error((uint8_t)(p->last_aux & 0xFFu));

    if (v.age_ns > p->cfg.max_age_ns) {
        v.state = OC_POS_UNAVAILABLE;
    } else if (p->last_status == OC_ST_RANGE_ERR) {
        v.state = oc_vl6180x_range_error_is_system(v.range_error) ? OC_POS_FAULT
                                                                  : OC_POS_NO_TARGET;
    } else if (p->last_status != OC_ST_OK) {
        v.state = OC_POS_UNAVAILABLE;
    } else {
        window_stats(p, &v);
        if (p->n < p->cfg.window_n)
            v.state = OC_POS_SETTLING;
        else if (v.median_mm < p->cfg.min_mm || v.median_mm > p->cfg.max_mm)
            v.state = OC_POS_OUT_OF_RANGE;
        else if (v.spread_mm > p->cfg.max_spread_mm)
            v.state = OC_POS_MOVING;
        else
            v.state = OC_POS_STABLE;
    }
    *out = v;
    return 0;
}

const char *oc_position_state_name(uint8_t state)
{
    static const char *const names[] = {
        "?", "UNAVAILABLE", "FAULT", "NO_TARGET", "SETTLING",
        "OUT_OF_RANGE", "MOVING", "STABLE",
    };
    return state < sizeof names / sizeof names[0] ? names[state] : "?";
}
