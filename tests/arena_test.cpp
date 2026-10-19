// Stress test for the snapshot-aware arena (no game needed).
// Model: "game state" = table of live blocks owned by the domain (reverted on restore, like the
// real game state). "outside refs" = blocks allocated after the snapshot that something outside
// the domain keeps (they must survive restores as zombies).
#include "../src/arena.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <algorithm>

struct Obj { unsigned char* p; uint32_t size; uint32_t seed; };

static uint32_t rng_state = 12345;
static uint32_t rnd() { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static void fill(Obj& o) { for (uint32_t i = 0; i < o.size; i++) o.p[i] = (unsigned char)(o.seed + i * 31); }
static bool verify(const Obj& o) {
    for (uint32_t i = 0; i < o.size; i++) if (o.p[i] != (unsigned char)(o.seed + i * 31)) return false;
    return true;
}
static uint32_t rand_size() {
    uint32_t r = rnd() % 100;
    if (r < 70) return 1 + rnd() % 128;
    if (r < 95) return 1 + rnd() % 4096;
    if (r < 99) return 1 + rnd() % 200000;
    return 1 + rnd() % 3000000;
}

// The domain state lives *inside the arena* (like the game's): a vector allocated in the arena.
struct State { Obj* objs; uint32_t count, cap; };
static State* g_state;

static void state_push(const Obj& o) {
    if (g_state->count == g_state->cap) {
        uint32_t ncap = g_state->cap ? g_state->cap * 2 : 256;
        g_state->objs = (Obj*)arena::Realloc(g_state->objs, ncap * sizeof(Obj));
        g_state->cap = ncap;
    }
    g_state->objs[g_state->count++] = o;
}

static void check_heap(const char* where) {
    char err[256];
    if (!arena::Check(err, sizeof(err))) { printf("HEAP CHECK FAILED (%s): %s\n", where, err); exit(1); }
}

static void random_ops(int n, std::vector<Obj>* outside, bool allow_outside) {
    for (int i = 0; i < n; i++) {
        uint32_t r = rnd() % 100;
        if (r < 45 || g_state->count < 50) {
            Obj o; o.size = rand_size(); o.seed = rnd();
            o.p = (unsigned char*)arena::Alloc(o.size);
            if (!o.p) { printf("alloc failed\n"); exit(1); }
            fill(o);
            if (allow_outside && outside && (rnd() % 20) == 0) outside->push_back(o);
            else state_push(o);
        } else if (r < 85) {
            uint32_t k = rnd() % g_state->count;
            Obj o = g_state->objs[k];
            if (!verify(o)) { printf("corruption before free\n"); exit(1); }
            arena::Free(o.p);
            g_state->objs[k] = g_state->objs[--g_state->count];
        } else if (r < 97) {
            uint32_t k = rnd() % g_state->count;
            Obj& o = g_state->objs[k];
            if (!verify(o)) { printf("corruption before realloc\n"); exit(1); }
            uint32_t ns = o.size + rnd() % 3000;
            unsigned char* np = (unsigned char*)arena::Realloc(o.p, ns);
            if (!np) { printf("realloc failed\n"); exit(1); }
            o.p = np;
            for (uint32_t j = o.size; j < ns; j++) np[j] = (unsigned char)(o.seed + j * 31);
            o.size = ns;
        } else if (outside && !outside->empty()) {
            // outside code frees one of its blocks
            size_t k = rnd() % outside->size();
            if (!verify((*outside)[k])) { printf("outside corruption\n"); exit(1); }
            arena::Free((*outside)[k].p);
            (*outside)[k] = outside->back(); outside->pop_back();
        }
    }
}

struct Image { unsigned char* data; uintptr_t end; };

static Image take_snapshot() {
    arena::Lock();
    uintptr_t end = arena::BeginSnapshot();
    size_t len = end - arena::Base();
    Image img;
    img.data = (unsigned char*)VirtualAlloc(nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memcpy(img.data, (void*)arena::Base(), len);
    img.end = end;
    arena::EndSnapshot();
    arena::Unlock();
    arena::FlushPendingOld();
    return img;
}

static void restore(const Image& img, bool use_dirty, const std::vector<Obj>& outside) {
    arena::Lock();
    size_t nz = 0;
    arena::Range* z = arena::CollectZombies(&nz);
    if (nz == (size_t)-1) { printf("CollectZombies: corrupted\n"); exit(1); }
    // keep only the zombies still referenced from outside (simulates a perfect reference scan)
    std::vector<uintptr_t> refs;
    for (auto& o : outside) refs.push_back((uintptr_t)o.p - 8);
    std::sort(refs.begin(), refs.end());
    size_t kept = 0;
    for (size_t i = 0; i < nz; i++)
        if (std::binary_search(refs.begin(), refs.end(), z[i].start)) z[kept++] = z[i];
    std::vector<arena::Range> zs(z, z + kept);
    uintptr_t base = arena::Base();
    auto copy_range = [&](uintptr_t a, uintptr_t b) {
        // copy [a,b) from image, skipping zombies
        size_t lo = 0, hi = zs.size();
        while (lo < hi) { size_t mid = (lo + hi) / 2; if (zs[mid].end <= a) lo = mid + 1; else hi = mid; }
        uintptr_t cur = a;
        for (size_t i = lo; i < zs.size() && zs[i].start < b && cur < b; i++) {
            if (zs[i].start > cur) memcpy((void*)cur, img.data + (cur - base), zs[i].start - cur);
            if (zs[i].end > cur) cur = zs[i].end;
        }
        if (cur < b) memcpy((void*)cur, img.data + (cur - base), b - cur);
    };
    size_t restored_pages = 0;
    if (use_dirty) {
        static uintptr_t pages[1 << 18];
        bool overflow = false;
        size_t n = arena::DirtyPages(base, img.end, pages, 1 << 18, &overflow);
        if (overflow) copy_range(base, img.end);
        else for (size_t i = 0; i < n; i++) {
            uintptr_t a = pages[i], b = a + 4096;
            if (b > img.end) b = img.end;
            copy_range(a, b);
        }
        restored_pages = overflow ? (img.end - base) / 4096 : n;
    } else {
        copy_range(base, img.end);
    }
    char err[256];
    if (!arena::FinishRestore(kept, err, sizeof(err))) { printf("FinishRestore: %s\n", err); exit(1); }
    arena::Unlock();
    printf("  restore: %u zombies (%u kept), %u pages\n", (unsigned)nz, (unsigned)kept, (unsigned)restored_pages);
}

int main() {
    if (!arena::Init(1024u << 20)) { printf("init failed\n"); return 1; }
    g_state = (State*)arena::Alloc(sizeof(State));
    memset(g_state, 0, sizeof(State));

    std::vector<Obj> outside;   // zombies: survive restores
    random_ops(200000, nullptr, false);
    check_heap("initial");

    for (int round = 0; round < 6; round++) {
        Image img = take_snapshot();
        // model copy of the domain state at snapshot time
        std::vector<Obj> at_s(g_state->objs, g_state->objs + g_state->count);
        check_heap("after snapshot");
        printf("round %d: snapshot with %u objects, image %u KB\n", round, g_state->count,
               (unsigned)((img.end - arena::Base()) >> 10));
        for (int cycle = 0; cycle < 5; cycle++) {
            size_t outside_before = outside.size();
            random_ops(50000 + rnd() % 50000, &outside, true);
            check_heap("before restore");
            restore(img, cycle % 2 == 0, outside);
            check_heap("after restore");
            // the domain state must equal the snapshot again
            if (g_state->count != at_s.size()) { printf("state count mismatch\n"); return 1; }
            for (size_t i = 0; i < at_s.size(); i++) {
                const Obj& o = g_state->objs[i];
                if (o.p != at_s[i].p || o.size != at_s[i].size || !verify(o)) { printf("state object %u not restored\n", (unsigned)i); return 1; }
            }
            for (auto& o : outside) if (!verify(o)) { printf("zombie corrupted\n"); return 1; }
            printf("  cycle %d ok (outside %u -> %u)\n", cycle, (unsigned)outside_before, (unsigned)outside.size());
        }
        // play on after the last restore, then take a new snapshot next round
        random_ops(30000, &outside, true);
        check_heap("end of round");
        VirtualFree(img.data, 0, MEM_RELEASE);
    }
    arena::Stats st = arena::GetStats();
    printf("final: heap %u KB, used %u KB in %u blocks, %u free blocks, %u outside zombies\n",
           (unsigned)(st.heap_bytes >> 10), (unsigned)(st.used_bytes >> 10), (unsigned)st.used_blocks,
           (unsigned)st.free_blocks, (unsigned)outside.size());
    // speed test
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    std::vector<void*> v; v.reserve(1000000);
    for (int i = 0; i < 1000000; i++) v.push_back(arena::Alloc(16 + (i % 200)));
    for (int i = 0; i < 1000000; i++) arena::Free(v[i]);
    QueryPerformanceCounter(&t1);
    printf("1M alloc+free: %.1f ms\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    printf("ALL OK\n");
    return 0;
}
