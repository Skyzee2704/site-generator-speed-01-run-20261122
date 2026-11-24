/* hmap.c — compact single-array open-addressing string maps and sets over pool spans.
 * Stores 32-bit non-zero hash fingerprints inline with keys/values so each probe
 * stays within a single cache line and skips pool key comparisons on mismatch. */
#include "zg.h"

static inline uint32_t hash_tag(uint64_t h) {
    return (uint32_t)(h >> 32) | 1u;
}

static void dict_alloc(Dict *d, Pool *p, size_t ncap) {
    DictEnt *ents = (DictEnt *)pool_alloc(p, ncap * sizeof(DictEnt));
    fill_bytes(ents, 0, ncap * sizeof(DictEnt));
    d->ents = ents;
    d->mask = ncap - 1;
    d->len = 0;
}

static void dict_rehash(Dict *d, Pool *p, size_t ncap) {
    DictEnt *old = d->ents;
    size_t ocap = d->mask ? d->mask + 1 : 0;
    dict_alloc(d, p, ncap);
    size_t mask = d->mask;
    DictEnt *ents = d->ents;
    for (size_t i = 0; i < ocap; i++) {
        if (!old[i].h32) continue;
        uint64_t h = hash_bytes(p->base + old[i].key.off, old[i].key.len);
        size_t s = (size_t)(h & mask);
        while (ents[s].h32) s = (s + 1) & mask;
        ents[s] = old[i];
        d->len++;
    }
}

void dict_new(Dict *d) {
    d->ents = 0;
    d->mask = 0;
    d->len = 0;
}

void dict_reserve(Dict *d, Pool *p, size_t expect) {
    size_t ncap = 32;
    while (ncap * 7 < expect * 10) ncap *= 2;
    if (d->mask + 1 >= ncap) return;
    if (d->mask == 0) dict_alloc(d, p, ncap);
    else dict_rehash(d, p, ncap);
}

bool dict_get(Dict *d, Pool *p, const uint8_t *key, size_t klen, Sp *out) {
    size_t mask = d->mask;
    if (__builtin_expect(mask == 0, 0)) return false;
    uint64_t h = hash_bytes(key, klen);
    uint32_t h32 = hash_tag(h);
    size_t i = (size_t)(h & mask);
    DictEnt *ents = d->ents;
    const uint8_t *base = p->base;
    for (;;) {
        uint32_t eh = ents[i].h32;
        if (!eh) return false;
        if (eh == h32 && ents[i].key.len == klen && eq_bytes(base + ents[i].key.off, key, klen)) {
            *out = ents[i].val;
            return true;
        }
        i = (i + 1) & mask;
    }
}

void dict_put(Dict *d, Pool *p, Sp key, Sp val) {
    if (__builtin_expect(d->mask == 0, 0)) dict_alloc(d, p, 32);
    else if (__builtin_expect((d->len + 1) * 10 > (d->mask + 1) * 7, 0)) dict_rehash(d, p, (d->mask + 1) * 2);
    const uint8_t *base = p->base;
    const uint8_t *kb = base + key.off;
    uint64_t h = hash_bytes(kb, key.len);
    uint32_t h32 = hash_tag(h);
    size_t mask = d->mask;
    size_t i = (size_t)(h & mask);
    DictEnt *ents = d->ents;
    for (;;) {
        uint32_t eh = ents[i].h32;
        if (!eh) {
            ents[i].h32 = h32;
            ents[i].key = key;
            ents[i].val = val;
            d->len++;
            return;
        }
        if (eh == h32 && ents[i].key.len == key.len && eq_bytes(base + ents[i].key.off, kb, key.len)) {
            ents[i].key = key;
            ents[i].val = val;
            return;
        }
        i = (i + 1) & mask;
    }
}

bool dict_has(Dict *d, Pool *p, const uint8_t *key, size_t klen) {
    Sp out;
    return dict_get(d, p, key, klen, &out);
}

static void set_alloc(Set *s, Pool *p, size_t ncap) {
    SetEnt *ents = (SetEnt *)pool_alloc(p, ncap * sizeof(SetEnt));
    fill_bytes(ents, 0, ncap * sizeof(SetEnt));
    s->ents = ents;
    s->mask = ncap - 1;
    s->len = 0;
}

static void set_rehash(Set *s, Pool *p, size_t ncap) {
    SetEnt *old = s->ents;
    size_t ocap = s->mask ? s->mask + 1 : 0;
    set_alloc(s, p, ncap);
    size_t mask = s->mask;
    SetEnt *ents = s->ents;
    for (size_t i = 0; i < ocap; i++) {
        if (!old[i].h32) continue;
        uint64_t h = hash_bytes(p->base + old[i].key.off, old[i].key.len);
        size_t slot = (size_t)(h & mask);
        while (ents[slot].h32) slot = (slot + 1) & mask;
        ents[slot] = old[i];
        s->len++;
    }
}

void set_new(Set *s) {
    s->ents = 0;
    s->mask = 0;
    s->len = 0;
}

void set_reserve(Set *s, Pool *p, size_t expect) {
    size_t ncap = 32;
    while (ncap * 7 < expect * 10) ncap *= 2;
    if (s->mask + 1 >= ncap) return;
    if (s->mask == 0) set_alloc(s, p, ncap);
    else set_rehash(s, p, ncap);
}

bool set_add(Set *s, Pool *p, Sp val) {
    if (__builtin_expect(s->mask == 0, 0)) set_alloc(s, p, 32);
    else if (__builtin_expect((s->len + 1) * 10 > (s->mask + 1) * 7, 0)) set_rehash(s, p, (s->mask + 1) * 2);
    const uint8_t *base = p->base;
    const uint8_t *kb = base + val.off;
    uint64_t h = hash_bytes(kb, val.len);
    uint32_t h32 = hash_tag(h);
    size_t mask = s->mask;
    size_t i = (size_t)(h & mask);
    SetEnt *ents = s->ents;
    for (;;) {
        uint32_t eh = ents[i].h32;
        if (!eh) {
            ents[i].h32 = h32;
            ents[i].key = val;
            s->len++;
            return true;
        }
        if (eh == h32 && ents[i].key.len == val.len && eq_bytes(base + ents[i].key.off, kb, val.len)) {
            return false;
        }
        i = (i + 1) & mask;
    }
}

bool set_has(Set *s, Pool *p, const uint8_t *key, size_t klen) {
    size_t mask = s->mask;
    if (__builtin_expect(mask == 0, 0)) return false;
    uint64_t h = hash_bytes(key, klen);
    uint32_t h32 = hash_tag(h);
    size_t i = (size_t)(h & mask);
    SetEnt *ents = s->ents;
    const uint8_t *base = p->base;
    for (;;) {
        uint32_t eh = ents[i].h32;
        if (!eh) return false;
        if (eh == h32 && ents[i].key.len == klen && eq_bytes(base + ents[i].key.off, key, klen)) {
            return true;
        }
        i = (i + 1) & mask;
    }
}
