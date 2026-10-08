#!/usr/bin/env python3
"""Exercise the real Mesa cache-growth function with a tracked allocator.

The GL probe tests the real device. This host check adds intrusive-list
invariants and deterministic allocation failure under ASan/UBSan.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('mesa', type=Path)
parser.add_argument('--source', type=Path, help='alternate v3d_bufmgr.c, e.g. baseline')
args = parser.parse_args()
source = (args.source or args.mesa / 'src/gallium/drivers/v3d/v3d_bufmgr.c').read_text()
begin = source.index('void\nv3d_bo_last_unreference_locked_timed(')
end = source.index('\nstatic struct v3d_bo *', begin)
function = source[begin:end]

prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "util/list.h"
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
struct v3d_bo_cache {
    struct list_head time_list;
    struct list_head *size_list;
    uint32_t size_list_size;
};
struct v3d_screen { struct v3d_bo_cache bo_cache; };
struct v3d_bo {
    struct v3d_screen *screen;
    uint32_t size;
    bool private;
    time_t free_time;
    const char *name;
    struct list_head size_list, time_list;
};
static bool dump_stats, fail_alloc;
static unsigned blocks, frees, allocations;
static size_t live_bytes;
static void *tracked_alloc(size_t size) {
    if (fail_alloc) return NULL;
    size_t *p = malloc(sizeof(size_t) + size);
    assert(p);
    *p = size;
    live_bytes += size;
    blocks++; allocations++;
    return p + 1;
}
#define ralloc_array(owner, type, count) ((type*)tracked_alloc(sizeof(type) * (count)))
static void ralloc_free(void *p) {
    if (!p) return;
    size_t *base = (size_t*)p - 1;
    live_bytes -= *base;
    blocks--;
    free(base);
}
static void v3d_bo_free(struct v3d_bo *bo) { frees++; free(bo); }
static void v3d_bo_dump_stats(struct v3d_screen *s) { (void)s; }
// Aging and GPU ioctls are covered by the native probe; retain all nodes here
// so growth has to repair nonempty, including multi-node, lists.
static void free_stale_bos(struct v3d_screen *s, time_t t) { (void)s; (void)t; }
'''

suffix = r'''
static struct v3d_bo *make_bo(struct v3d_screen *screen, unsigned pages) {
    struct v3d_bo *bo = calloc(1, sizeof(*bo));
    assert(bo);
    bo->screen = screen; bo->size = pages * 4096; bo->private = true;
    return bo;
}
static void verify(struct v3d_screen *s, unsigned expected) {
    unsigned count = 0, time_count = 0;
    for (unsigned i = 0; i < s->bo_cache.size_list_size; i++) {
        struct list_head *head = &s->bo_cache.size_list[i];
        assert(head->next->prev == head && head->prev->next == head);
        for (struct list_head *p = head->next; p != head; p = p->next) {
            assert(p->next->prev == p && p->prev->next == p);
            struct v3d_bo *bo = (struct v3d_bo*)((char*)p - offsetof(struct v3d_bo, size_list));
            assert(bo->size / 4096 - 1 == i);
            assert(++count <= expected);
        }
    }
    struct list_head *head = &s->bo_cache.time_list;
    for (struct list_head *p = head->next; p != head; p = p->next) {
        assert(p->next->prev == p && p->prev->next == p);
        assert(++time_count <= expected);
    }
    assert(count == expected && time_count == expected);
}
int main(void) {
    struct v3d_screen screen = {0};
    list_inithead(&screen.bo_cache.time_list);
    unsigned count = 0;
    for (unsigned pages = 1; pages <= 2048; pages++) {
        v3d_bo_last_unreference_locked_timed(make_bo(&screen, pages), 0);
        count++;
        if (pages % 13 == 0) {
            v3d_bo_last_unreference_locked_timed(make_bo(&screen, 1), 0);
            count++;
        }
        verify(&screen, count);
    }
    printf("tables=%u allocations=%u retained_bytes=%zu entries=%u\n",
        blocks, allocations, live_bytes, screen.bo_cache.size_list_size);
    fflush(stdout);
    // Exactly one size table owns all heads; growth should be amortized.
    assert(blocks == 1);
    assert(allocations <= 12);
    assert(live_bytes == screen.bo_cache.size_list_size * sizeof(struct list_head));
    unsigned capacity = screen.bo_cache.size_list_size;
    struct list_head *old = screen.bo_cache.size_list;
    fail_alloc = true;
    v3d_bo_last_unreference_locked_timed(make_bo(&screen, capacity + 1), 0);
    assert(frees == 1 && screen.bo_cache.size_list == old);
    assert(screen.bo_cache.size_list_size == capacity);
    verify(&screen, count);
    fail_alloc = false;
    v3d_bo_last_unreference_locked_timed(make_bo(&screen, capacity + 1), 0);
    verify(&screen, ++count);
    // Uncacheable BOs must bypass the table.
    struct v3d_bo *shared = make_bo(&screen, 1); shared->private = false;
    v3d_bo_last_unreference_locked_timed(shared, 0);
    assert(frees == 2);
    verify(&screen, count);
    struct list_head *head = &screen.bo_cache.time_list;
    while (!list_is_empty(head)) {
        struct v3d_bo *bo = (struct v3d_bo*)((char*)head->next - offsetof(struct v3d_bo, time_list));
        list_del(&bo->time_list); list_del(&bo->size_list);
        free(bo);
    }
    verify(&screen, 0);
    ralloc_free(screen.bo_cache.size_list);
    assert(blocks == 0 && live_bytes == 0);
    puts("PASS cache membership, bounded allocation, failed growth and cleanup");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='v3d-cache-test-', dir='/mnt/HaikuWork/tmp') as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(prefix + function + suffix)
    subprocess.run(['cc', '-std=c11', '-g', '-O1', '-fsanitize=address,undefined',
        '-fno-omit-frame-pointer', '-I' + str(args.mesa / 'src'),
        str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
