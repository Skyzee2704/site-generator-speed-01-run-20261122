/* hmap.c — open-addressing string maps and sets over pool spans.
 * FNV-1a hash, linear probing, grow at >70% load. Used only for
 * lookups (slug cache, taken-slug sets, tag index), never iterated,
 * so the hash function and growth policy do not affect output. */
#include "zg.h"

/* Find the slot for a key: the slot holding it, or the first empty slot
 * after its probe run. `found` reports whether the key is present. */
static size_t dict_slot(Dict *d, Pool *p, const uint8_t *key, size_t klen, bool *found) {
    size_t mask = d->mask;
    size_t i = (size_t)(hash_bytes(key, klen) & mask);
    uint8_t *used = (uint8_t *)d->used.items;
    Sp *keys = (Sp *)d->keys.items;
    while (used && used[i]) {
        if (keys[i].len == klen && eq_bytes(p->base + keys[i].off, key, klen)) {
            *found = true;
            return i;
        }
        i = (i + 1) & mask;
    }
    *found = false;
    return i;
}

static void dict_rehash(Dict *d, Pool *p, size_t ncap) {
    Arr ok = d->keys, ov = d->vals, ou = d->used;
    arr_new(&d->keys, sizeof(Sp));
    arr_new(&d->vals, sizeof(Sp));
    arr_new(&d->used, sizeof(uint8_t));
    arr_grow(&d->keys, p, ncap);
    arr_grow(&d->vals, p, ncap);
    arr_grow(&d->used, p, ncap);
    d->keys.len = ncap; d->vals.len = ncap; d->used.len = ncap;
    fill_bytes(d->used.items, 0, ncap);
    d->mask = ncap - 1;
    d->len = 0;
    for (size_t i = 0; i < ou.len; i++) {
        if (!((uint8_t *)ou.items)[i]) continue;
        Sp *k = arr_at(&ok, i);
        Sp *v = arr_at(&ov, i);
        bool f;
        size_t slot = dict_slot(d, p, p->base + k->off, k->len, &f);
        *(Sp *)arr_at(&d->keys, slot) = *k;
        *(Sp *)arr_at(&d->vals, slot) = *v;
        ((uint8_t *)d->used.items)[slot] = 1;
        d->len++;
    }
}

void dict_new(Dict *d) {
    arr_new(&d->keys, sizeof(Sp));
    arr_new(&d->vals, sizeof(Sp));
    arr_new(&d->used, sizeof(uint8_t));
    d->mask = 0;
    d->len = 0;
}

bool dict_get(Dict *d, Pool *p, const uint8_t *key, size_t klen, Sp *out) {
    if (d->mask == 0) return false;
    bool found;
    size_t slot = dict_slot(d, p, key, klen, &found);
    if (!found) return false;
    *out = *(Sp *)arr_at(&d->vals, slot);
    return true;
}

void dict_put(Dict *d, Pool *p, Sp key, Sp val) {
    if (d->mask == 0) {
        size_t ncap = 32;
        arr_grow(&d->keys, p, ncap);
        arr_grow(&d->vals, p, ncap);
        arr_grow(&d->used, p, ncap);
        d->keys.len = ncap; d->vals.len = ncap; d->used.len = ncap;
        fill_bytes(d->used.items, 0, ncap);
        d->mask = ncap - 1;
    }
    if ((d->len + 1) * 10 > (d->mask + 1) * 7) dict_rehash(d, p, (d->mask + 1) * 2);
    bool found;
    size_t slot = dict_slot(d, p, p->base + key.off, key.len, &found);
    *(Sp *)arr_at(&d->keys, slot) = key;
    *(Sp *)arr_at(&d->vals, slot) = val;
    if (!found) {
        ((uint8_t *)d->used.items)[slot] = 1;
        d->len++;
    }
}

bool dict_has(Dict *d, Pool *p, const uint8_t *key, size_t klen) {
    Sp out;
    return dict_get(d, p, key, klen, &out);
}

static size_t set_slot(Set *s, Pool *p, const uint8_t *key, size_t klen, bool *found) {
    size_t mask = s->mask;
    size_t i = (size_t)(hash_bytes(key, klen) & mask);
    uint8_t *used = (uint8_t *)s->used.items;
    Sp *keys = (Sp *)s->keys.items;
    while (used && used[i]) {
        if (keys[i].len == klen && eq_bytes(p->base + keys[i].off, key, klen)) {
            *found = true;
            return i;
        }
        i = (i + 1) & mask;
    }
    *found = false;
    return i;
}

static void set_rehash(Set *s, Pool *p, size_t ncap) {
    Arr ok = s->keys, ou = s->used;
    arr_new(&s->keys, sizeof(Sp));
    arr_new(&s->used, sizeof(uint8_t));
    arr_grow(&s->keys, p, ncap);
    arr_grow(&s->used, p, ncap);
    s->keys.len = ncap; s->used.len = ncap;
    fill_bytes(s->used.items, 0, ncap);
    s->mask = ncap - 1;
    s->len = 0;
    for (size_t i = 0; i < ou.len; i++) {
        if (!((uint8_t *)ou.items)[i]) continue;
        Sp *k = arr_at(&ok, i);
        bool f;
        size_t slot = set_slot(s, p, p->base + k->off, k->len, &f);
        *(Sp *)arr_at(&s->keys, slot) = *k;
        ((uint8_t *)s->used.items)[slot] = 1;
        s->len++;
    }
}

void set_new(Set *s) {
    arr_new(&s->keys, sizeof(Sp));
    arr_new(&s->used, sizeof(uint8_t));
    s->mask = 0;
    s->len = 0;
}

/* Insert; true when the value was not present (newly inserted). */
bool set_add(Set *s, Pool *p, Sp val) {
    if (s->mask == 0) {
        size_t ncap = 32;
        arr_grow(&s->keys, p, ncap);
        arr_grow(&s->used, p, ncap);
        s->keys.len = ncap; s->used.len = ncap;
        fill_bytes(s->used.items, 0, ncap);
        s->mask = ncap - 1;
    }
    if ((s->len + 1) * 10 > (s->mask + 1) * 7) set_rehash(s, p, (s->mask + 1) * 2);
    bool found;
    size_t slot = set_slot(s, p, p->base + val.off, val.len, &found);
    if (found) return false;
    *(Sp *)arr_at(&s->keys, slot) = val;
    ((uint8_t *)s->used.items)[slot] = 1;
    s->len++;
    return true;
}

bool set_has(Set *s, Pool *p, const uint8_t *key, size_t klen) {
    if (s->mask == 0) return false;
    bool found;
    set_slot(s, p, key, klen, &found);
    return found;
}
