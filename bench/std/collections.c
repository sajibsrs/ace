/* C reference for collections.ace: std/array's doubling growth and std/map's
 * hash, three-quarter load limit, linear probing, and backward-shift unset,
 * over the same malloc/realloc/calloc/free the Ace allocator lowers to.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static uint64_t now_ns(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void report(const char *name, uint64_t elapsed, uint64_t operations, uint64_t checksum) {
    printf("%s %llu %llu %llu\n", name, (unsigned long long)elapsed, (unsigned long long)operations,
           (unsigned long long)checksum);
}

typedef struct {
    uint64_t *items;
    size_t cap;
    size_t len;
} Array;

/* mem.grow: at least `needed`, doubling when that is larger. */
static bool array_reserve(Array *a, size_t needed) {
    if (a->cap >= needed) return true;
    size_t capacity = a->cap * 2;
    if (capacity < needed) capacity = needed;
    if (capacity == 0) capacity = 1;
    uint64_t *grown = realloc(a->items, capacity * sizeof *grown);
    if (!grown) return false;
    a->items = grown;
    a->cap = capacity;
    return true;
}

static bool array_push(Array *a, uint64_t value) {
    if (a->len == a->cap && !array_reserve(a, a->len + 1)) return false;
    a->items[a->len++] = value;
    return true;
}

static void array_wipe(Array *a) {
    free(a->items);
    a->items = NULL;
    a->cap = 0;
    a->len = 0;
}

typedef struct {
    int64_t key;
    int64_t value;
    bool used;
} Slot;

typedef struct {
    Slot *slots;
    size_t cap;
    size_t len;
} Map;

static uint64_t mix(uint64_t x) {
    x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
    return x ^ (x >> 31);
}

static size_t probe(const Slot *slots, size_t cap, int64_t key) {
    size_t mask = cap - 1;
    size_t at = (size_t)mix((uint64_t)key) & mask;
    while (slots[at].used && slots[at].key != key) at = (at + 1) & mask;
    return at;
}

static size_t capacity_for(size_t count) {
    size_t capacity = 8;
    while (capacity * 3 < count * 4) capacity *= 2;
    return capacity;
}

static bool map_rehash(Map *m, size_t capacity) {
    Slot *fresh = calloc(capacity, sizeof *fresh);
    if (!fresh) return false;
    for (size_t i = 0; i < m->cap; i++)
        if (m->slots[i].used) fresh[probe(fresh, capacity, m->slots[i].key)] = m->slots[i];
    free(m->slots);
    m->slots = fresh;
    m->cap = capacity;
    return true;
}

static bool map_set(Map *m, int64_t key, int64_t value) {
    if (m->len != 0) {
        size_t at = probe(m->slots, m->cap, key);
        if (m->slots[at].used) {
            m->slots[at].value = value;
            return true;
        }
    }
    if ((m->len + 1) * 4 > m->cap * 3 && !map_rehash(m, capacity_for(m->len + 1))) return false;
    size_t at = probe(m->slots, m->cap, key);
    m->slots[at] = (Slot){ key, value, true };
    m->len++;
    return true;
}

static bool map_get(const Map *m, int64_t key, int64_t *value) {
    if (m->len == 0) return false;
    size_t at = probe(m->slots, m->cap, key);
    if (!m->slots[at].used) return false;
    *value = m->slots[at].value;
    return true;
}

static bool map_unset(Map *m, int64_t key) {
    if (m->len == 0) return false;
    size_t mask = m->cap - 1;
    size_t hole = probe(m->slots, m->cap, key);
    if (!m->slots[hole].used) return false;
    size_t at = hole;
    for (;;) {
        at = (at + 1) & mask;
        if (!m->slots[at].used) break;
        size_t home = (size_t)mix((uint64_t)m->slots[at].key) & mask;
        if (((at + m->cap - home) & mask) >= ((at + m->cap - hole) & mask)) {
            m->slots[hole] = m->slots[at];
            hole = at;
        }
    }
    m->slots[hole].used = false;
    m->len--;
    return true;
}

static void map_wipe(Map *m) {
    free(m->slots);
    m->slots = NULL;
    m->cap = 0;
    m->len = 0;
}

int main(void) {
    const size_t count = 1000000, rounds = 8;
    Array items = { 0 };
    uint64_t push_time = 0, set_time = 0, get_time = 0, checksum = 0;

    for (size_t round = 0; round < rounds; round++) {
        uint64_t started = now_ns();
        for (size_t i = 0; i < count; i++)
            if (!array_push(&items, i)) return 1;
        push_time += now_ns() - started;
        started = now_ns();
        for (size_t i = 0; i < count; i++) items.items[i] = i * 3 + round;
        set_time += now_ns() - started;
        started = now_ns();
        uint64_t sum = 0;
        for (size_t i = 0; i < count; i++) sum += items.items[i];
        get_time += now_ns() - started;
        checksum += sum % 1000003 + items.len;
        array_wipe(&items);
    }
    report("array-push", push_time, count * rounds, checksum);
    report("array-set", set_time, count * rounds, checksum);
    report("array-get", get_time, count * rounds, checksum);

    const size_t keys = 16384, map_rounds = 64;
    uint64_t miss_time = 0, unset_time = 0;
    set_time = get_time = checksum = 0;

    for (size_t round = 0; round < map_rounds; round++) {
        Map table = { 0 };
        uint64_t started = now_ns();
        for (size_t i = 0; i < keys; i++)
            if (!map_set(&table, (int64_t)(i * 7), (int64_t)(i + round))) return 2;
        set_time += now_ns() - started;
        started = now_ns();
        for (size_t i = 0; i < keys; i++) {
            int64_t value;
            if (map_get(&table, (int64_t)(i * 7), &value)) checksum += (uint64_t)value;
        }
        get_time += now_ns() - started;
        started = now_ns();
        for (size_t i = 0; i < keys; i++) {
            int64_t value;
            if (!map_get(&table, (int64_t)(i * 7 + 3), &value)) checksum += 1;
        }
        miss_time += now_ns() - started;
        started = now_ns();
        for (size_t i = 0; i < keys; i++)
            if (map_unset(&table, (int64_t)(i * 7))) checksum += 2;
        unset_time += now_ns() - started;
        checksum += table.len;
        map_wipe(&table);
    }
    report("map-set", set_time, keys * map_rounds, checksum);
    report("map-get", get_time, keys * map_rounds, checksum);
    report("map-miss", miss_time, keys * map_rounds, checksum);
    report("map-unset", unset_time, keys * map_rounds, checksum);
    return 0;
}
