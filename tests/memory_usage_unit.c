/*
 * memory_usage_unit.c
 *
 * Verifies roaring_bitmap_memory_usage and roaring64_bitmap_memory_usage by
 * installing a custom memory hook that tracks every byte currently allocated
 * through the roaring allocator, then comparing the live byte count against
 * the reported value.
 *
 * After freeing each bitmap the live byte count must return to zero, which
 * proves that every allocation is both counted and freed.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <roaring/memory.h>
#include <roaring/roaring.h>
#include <roaring/roaring64.h>

#ifdef __cplusplus
using namespace roaring::internal;
#endif

#include "test.h"

/* -------------------------------------------------------------------------
 * Tracking allocator
 * Each roaring_malloc / roaring_realloc allocation prepends a size_t header
 * that records the requested size so that roaring_free can subtract it.
 * roaring_aligned_malloc stores two size_t fields (requested size + offset
 * from the raw allocation) right before the returned aligned pointer.
 * ------------------------------------------------------------------------- */

static size_t g_tracked = 0;

static void *ta_malloc(size_t n) {
    size_t *p = (size_t *)malloc(n + sizeof(size_t));
    if (!p) return NULL;
    *p = n;
    g_tracked += n;
    return p + 1;
}

static void *ta_realloc(void *ptr, size_t n) {
    if (!ptr) return ta_malloc(n);
    size_t *old = (size_t *)ptr - 1;
    size_t old_n = *old;
    size_t *p = (size_t *)realloc(old, n + sizeof(size_t));
    if (!p) return NULL;
    *p = n;
    g_tracked -= old_n;
    g_tracked += n;
    return p + 1;
}

static void *ta_calloc(size_t count, size_t n) {
    void *p = ta_malloc(count * n);
    if (p) memset(p, 0, count * n);
    return p;
}

static void ta_free(void *ptr) {
    if (!ptr) return;
    size_t *p = (size_t *)ptr - 1;
    g_tracked -= *p;
    free(p);
}

/*
 * Layout for aligned allocations:
 *   [ raw malloc'd block ]
 *   [ ... padding ... ][ offset(size_t) ][ size(size_t) ][ aligned user data ]
 *
 * "offset" is the distance in bytes from raw to the aligned address.
 * "size"   is the requested byte count.
 * Both fields sit immediately before the returned aligned pointer.
 */
static void *ta_aligned_malloc(size_t align, size_t n) {
    size_t hdr = 2 * sizeof(size_t);
    void *raw = malloc(n + align + hdr);
    if (!raw) return NULL;
    uintptr_t a = ((uintptr_t)raw + hdr + align - 1) & ~(align - 1);
    size_t *p = (size_t *)a;
    p[-1] = n;                        /* requested size   */
    p[-2] = a - (uintptr_t)raw;      /* offset from raw  */
    g_tracked += n;
    return (void *)a;
}

static void ta_aligned_free(void *ptr) {
    if (!ptr) return;
    size_t *p = (size_t *)ptr;
    size_t n      = p[-1];
    size_t offset = p[-2];
    void *raw = (void *)((uintptr_t)ptr - offset);
    g_tracked -= n;
    free(raw);
}

static void install_tracking_allocator(void) {
    roaring_memory_t h = {
        .malloc         = ta_malloc,
        .realloc        = ta_realloc,
        .calloc         = ta_calloc,
        .free           = ta_free,
        .aligned_malloc = ta_aligned_malloc,
        .aligned_free   = ta_aligned_free,
    };
    roaring_init_memory_hook(h);
}

/* Helper: fail with a useful message when sizes differ */
#define assert_mem_equal(tracked, reported)                               \
    do {                                                                  \
        size_t _t = (tracked), _r = (reported);                          \
        if (_t != _r) {                                                   \
            print_error("tracked=%zu  reported=%zu  diff=%zd\n",         \
                        _t, _r, (ssize_t)_r - (ssize_t)_t);             \
        }                                                                 \
        assert_true(_t == _r);                                            \
    } while (0)

/* -------------------------------------------------------------------------
 * 32-bit tests
 * ------------------------------------------------------------------------- */

DEFINE_TEST(memory_usage_32_empty) {
    g_tracked = 0;
    roaring_bitmap_t *r = roaring_bitmap_create();
    assert_mem_equal(g_tracked, roaring_bitmap_memory_usage(r));
    roaring_bitmap_free(r);
    assert_int_equal(g_tracked, 0); /* all memory returned */
}

DEFINE_TEST(memory_usage_32_array_container) {
    g_tracked = 0;
    roaring_bitmap_t *r = roaring_bitmap_create();
    for (uint32_t i = 0; i < 100; i++) roaring_bitmap_add(r, i);
    assert_mem_equal(g_tracked, roaring_bitmap_memory_usage(r));
    roaring_bitmap_free(r);
    assert_int_equal(g_tracked, 0);
}

DEFINE_TEST(memory_usage_32_bitset_container) {
    g_tracked = 0;
    roaring_bitmap_t *r = roaring_bitmap_create();
    /* > DEFAULT_MAX_SIZE (4096) values forces conversion to bitset */
    for (uint32_t i = 0; i < 5000; i++) roaring_bitmap_add(r, i);
    assert_mem_equal(g_tracked, roaring_bitmap_memory_usage(r));
    roaring_bitmap_free(r);
    assert_int_equal(g_tracked, 0);
}

DEFINE_TEST(memory_usage_32_run_container) {
    g_tracked = 0;
    roaring_bitmap_t *r = roaring_bitmap_create();
    for (uint32_t i = 0; i < 5000; i++) roaring_bitmap_add(r, i);
    roaring_bitmap_run_optimize(r);
    assert_mem_equal(g_tracked, roaring_bitmap_memory_usage(r));
    roaring_bitmap_free(r);
    assert_int_equal(g_tracked, 0);
}

DEFINE_TEST(memory_usage_32_multiple_containers) {
    g_tracked = 0;
    roaring_bitmap_t *r = roaring_bitmap_create();
    /* Three containers: keys 0, 1, 2 (high 16 bits differ) */
    for (uint32_t k = 0; k < 3; k++) {
        for (uint32_t i = 0; i < 50; i++) {
            roaring_bitmap_add(r, (k << 16) | i);
        }
    }
    assert_mem_equal(g_tracked, roaring_bitmap_memory_usage(r));
    roaring_bitmap_free(r);
    assert_int_equal(g_tracked, 0);
}

/* -------------------------------------------------------------------------
 * 64-bit tests
 * ------------------------------------------------------------------------- */

DEFINE_TEST(memory_usage_64_empty) {
    g_tracked = 0;
    roaring64_bitmap_t *r = roaring64_bitmap_create();
    assert_mem_equal(g_tracked, roaring64_bitmap_memory_usage(r));
    roaring64_bitmap_free(r);
    assert_int_equal(g_tracked, 0);
}

DEFINE_TEST(memory_usage_64_array_container) {
    g_tracked = 0;
    roaring64_bitmap_t *r = roaring64_bitmap_create();
    for (uint64_t i = 0; i < 100; i++) roaring64_bitmap_add(r, i);
    assert_mem_equal(g_tracked, roaring64_bitmap_memory_usage(r));
    roaring64_bitmap_free(r);
    assert_int_equal(g_tracked, 0);
}

DEFINE_TEST(memory_usage_64_two_buckets) {
    g_tracked = 0;
    roaring64_bitmap_t *r = roaring64_bitmap_create();
    /* Two separate high-48-bit buckets → two containers, multiple ART nodes */
    for (uint64_t i = 0; i < 100; i++) roaring64_bitmap_add(r, i);
    for (uint64_t i = 0; i < 100; i++)
        roaring64_bitmap_add(r, (UINT64_C(1) << 32) | i);
    assert_mem_equal(g_tracked, roaring64_bitmap_memory_usage(r));
    roaring64_bitmap_free(r);
    assert_int_equal(g_tracked, 0);
}

DEFINE_TEST(memory_usage_64_bitset_container) {
    g_tracked = 0;
    roaring64_bitmap_t *r = roaring64_bitmap_create();
    for (uint64_t i = 0; i < 5000; i++) roaring64_bitmap_add(r, i);
    assert_mem_equal(g_tracked, roaring64_bitmap_memory_usage(r));
    roaring64_bitmap_free(r);
    assert_int_equal(g_tracked, 0);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void) {
    install_tracking_allocator();

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(memory_usage_32_empty),
        cmocka_unit_test(memory_usage_32_array_container),
        cmocka_unit_test(memory_usage_32_bitset_container),
        cmocka_unit_test(memory_usage_32_run_container),
        cmocka_unit_test(memory_usage_32_multiple_containers),
        cmocka_unit_test(memory_usage_64_empty),
        cmocka_unit_test(memory_usage_64_array_container),
        cmocka_unit_test(memory_usage_64_two_buckets),
        cmocka_unit_test(memory_usage_64_bitset_container),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
