/* Licensed under the MIT License
   https://github.com/craigahobbs/bare-script-c/blob/main/LICENSE */

/*
 * A leak check for the runtime: interposed on malloc, it counts the blocks live when the process
 * exits and prints the count to stderr. A BareScript value carries a pointer in the payload of a
 * NaN-boxed word, which the leaks tool does not recognize as a reference, so it reports the
 * thread-local roots' strings as leaks on every run; this count is the check instead - equal to the
 * previous build's on the same script, or a reference was dropped or kept.
 *
 *   cc -O2 -dynamiclib -o liveblocks.dylib test/tools/liveblocks.c
 *   DYLD_INSERT_LIBRARIES=./liveblocks.dylib build/bare -c 'systemLog(1)'
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TABLE (1u << 22)

static void **table;
static size_t live;
static int busy;

static unsigned slotOf(void *ptr)
{
    uintptr_t x = (uintptr_t) ptr;
    x ^= x >> 17;
    x *= 0x9E3779B1u;
    x ^= x >> 15;
    return (unsigned) x & (TABLE - 1);
}

static void record(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    if (table == NULL) {
        table = calloc(TABLE, sizeof(void *));
    }
    unsigned slot = slotOf(ptr);
    while (table[slot] != NULL) {
        slot = (slot + 1) & (TABLE - 1);
    }
    table[slot] = ptr;
    live++;
}

static void forget(void *ptr)
{
    if (ptr == NULL || table == NULL) {
        return;
    }
    unsigned slot = slotOf(ptr);
    while (table[slot] != ptr) {
        if (table[slot] == NULL) {
            return;
        }
        slot = (slot + 1) & (TABLE - 1);
    }
    /* Backward-shift deletion keeps the probe sequences intact */
    unsigned hole = slot;
    for (unsigned next = (slot + 1) & (TABLE - 1); table[next] != NULL; next = (next + 1) & (TABLE - 1)) {
        unsigned home = slotOf(table[next]);
        bool moves = hole <= next ? (home <= hole || home > next) : (home <= hole && home > next);
        if (moves) {
            table[hole] = table[next];
            hole = next;
        }
    }
    table[hole] = NULL;
    live--;
}

#define INTERPOSE(replacement, replacee) \
    __attribute__((used)) static const struct { const void *replacement; const void *replacee; } interpose_##replacee \
        __attribute__((section("__DATA,__interpose"))) = {(const void *) (replacement), (const void *) (replacee)}

static void *liveMalloc(size_t size)
{
    void *ptr = malloc(size);
    if (busy++ == 0) {
        record(ptr);
    }
    busy--;
    return ptr;
}

static void *liveCalloc(size_t count, size_t size)
{
    void *ptr = calloc(count, size);
    if (busy++ == 0) {
        record(ptr);
    }
    busy--;
    return ptr;
}

static void *liveRealloc(void *old, size_t size)
{
    if (busy++ == 0) {
        forget(old);
    }
    void *ptr = realloc(old, size);
    if (busy == 1) {
        record(ptr);
    }
    busy--;
    return ptr;
}

static void liveFree(void *ptr)
{
    if (busy++ == 0) {
        forget(ptr);
    }
    free(ptr);
    busy--;
}

INTERPOSE(liveMalloc, malloc);
INTERPOSE(liveCalloc, calloc);
INTERPOSE(liveRealloc, realloc);
INTERPOSE(liveFree, free);

__attribute__((destructor)) static void report(void)
{
    fprintf(stderr, "liveblocks: %zu blocks live at exit\n", live);
}
