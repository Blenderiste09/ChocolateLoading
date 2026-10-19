#define _CRT_SECURE_NO_WARNINGS
// Snapshot-aware TLSF heap. See arena.h for the snapshot model.
//
// Block layout (all sizes are multiples of 8, header included):
//   +0 prev_size  size of the previous physical block, valid only when F_PREVFREE is set
//   +4 sizef      size | flags
//   +8 payload    (free blocks: next/prev free-list links)
// The heap ends with a 0-sized "sentinel" header at ctl->top.
#include "arena.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

namespace arena {

uintptr_t g_lo = 1, g_hi = 1;   // [g_lo, g_hi) = heap span used by Contains()

namespace {

enum : uint32_t { F_FREE = 1, F_PREVFREE = 2, F_FROZEN = 4, F_MASK = 7 };
const uint32_t ALIGN = 8, HDR = 8, MIN_BLOCK = 16;
const uint32_t SL_LOG = 4, SL_COUNT = 1u << SL_LOG;
const uint32_t FL_SHIFT = SL_LOG + 3;        // first FL class starts at 128 bytes
const uint32_t SMALL = 1u << FL_SHIFT;
const uint32_t FL_COUNT = 26;
const uint32_t MAGIC = 0x434f4341;           // 'ACOC'
const size_t   COMMIT_STEP = 4u << 20;
const uint32_t GROW_MIN = 1u << 20;

struct Hdr {
    uint32_t prev_size;
    uint32_t sizef;
};
struct FreeLinks { Hdr* next; Hdr* prev; };

struct Control {
    uint32_t magic;
    uint32_t top;                       // address of the sentinel header
    uint32_t fl_bitmap;
    uint32_t sl_bitmap[FL_COUNT];
    Hdr*     heads[FL_COUNT][SL_COUNT];
};

bool g_oom = false;   // a VArray could not grow: the bookkeeping being built is incomplete

// Simple growable array outside the arena (VirtualAlloc backed, never uses the game heap).
template <class T> struct VArray {
    T* data = nullptr; size_t count = 0, cap = 0;
    bool push(const T& v) {
        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 4096;
            T* n = (T*)VirtualAlloc(nullptr, ncap * sizeof(T), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!n) { g_oom = true; return false; }
            if (data) { memcpy(n, data, count * sizeof(T)); VirtualFree(data, 0, MEM_RELEASE); }
            data = n; cap = ncap;
        }
        data[count++] = v;
        return true;
    }
    void clear() { count = 0; }
};

SRWLOCK      g_lock = SRWLOCK_INIT;
uintptr_t    g_base = 0, g_reserve_end = 0, g_commit_end = 0;
uintptr_t    g_heap_start = 0;
Control*     g_ctl = nullptr;
bool         g_snapshot = false;
uintptr_t    g_top_s = 0;               // sentinel address at snapshot time
VArray<Hdr*>   g_deferred;              // frozen blocks freed after the snapshot
VArray<void*>  g_deferred_old;          // non-arena blocks freed by routed code after the snapshot
VArray<void*>  g_pending_old;           // deferred old blocks that may now really be freed
VArray<Range>  g_free_s;                // free blocks at snapshot time (sorted)
VArray<Range>  g_zombies;               // scratch for CollectZombies
VArray<Range>  g_usable;                // scratch: post-snapshot usable ranges
OldFreeFn    g_old_free = nullptr;

inline uint32_t bsize(const Hdr* h) { return h->sizef & ~F_MASK; }
inline Hdr*     next_phys(Hdr* h) { return (Hdr*)((char*)h + bsize(h)); }
inline Hdr*     prev_phys(Hdr* h) { return (Hdr*)((char*)h - h->prev_size); }
inline FreeLinks* links(Hdr* h) { return (FreeLinks*)(h + 1); }
inline Hdr*     hdr_of(const void* p) { return (Hdr*)p - 1; }

inline int fls32(uint32_t v) { unsigned long i; return _BitScanReverse(&i, v) ? (int)i : -1; }
inline int ffs32(uint32_t v) { unsigned long i; return _BitScanForward(&i, v) ? (int)i : -1; }

inline void mapping(uint32_t size, uint32_t* fl, uint32_t* sl) {
    if (size < SMALL) { *fl = 0; *sl = size >> 3; return; }
    int f = fls32(size);
    *sl = (size >> (f - SL_LOG)) ^ SL_COUNT;
    *fl = (uint32_t)(f - (int)(FL_SHIFT - 1));
}

void insert_free(Hdr* b) {
    uint32_t fl, sl;
    mapping(bsize(b), &fl, &sl);
    Hdr* head = g_ctl->heads[fl][sl];
    links(b)->next = head;
    links(b)->prev = nullptr;
    if (head) links(head)->prev = b;
    g_ctl->heads[fl][sl] = b;
    g_ctl->fl_bitmap |= 1u << fl;
    g_ctl->sl_bitmap[fl] |= 1u << sl;
}

void remove_free(Hdr* b) {
    uint32_t fl, sl;
    mapping(bsize(b), &fl, &sl);
    FreeLinks* l = links(b);
    if (l->prev) links(l->prev)->next = l->next;
    else g_ctl->heads[fl][sl] = l->next;
    if (l->next) links(l->next)->prev = l->prev;
    if (!g_ctl->heads[fl][sl]) {
        g_ctl->sl_bitmap[fl] &= ~(1u << sl);
        if (!g_ctl->sl_bitmap[fl]) g_ctl->fl_bitmap &= ~(1u << fl);
    }
}

Hdr* find_free(uint32_t need) {
    uint32_t size = need;
    if (size >= SMALL) {
        uint32_t round = (1u << (fls32(size) - SL_LOG)) - 1;
        if (size > 0xFFFFFFFFu - round) return nullptr;
        size += round;
    }
    uint32_t fl, sl;
    mapping(size, &fl, &sl);
    if (fl >= FL_COUNT) return nullptr;
    uint32_t sl_map = g_ctl->sl_bitmap[fl] & (~0u << sl);
    if (!sl_map) {
        uint32_t fl_map = (fl + 1 < 32) ? (g_ctl->fl_bitmap & (~0u << (fl + 1))) : 0;
        if (!fl_map) return nullptr;
        fl = (uint32_t)ffs32(fl_map);
        sl_map = g_ctl->sl_bitmap[fl];
    }
    sl = (uint32_t)ffs32(sl_map);
    return g_ctl->heads[fl][sl];
}

bool ensure_commit(uintptr_t end) {
    if (end <= g_commit_end) return true;
    uintptr_t want = (end + COMMIT_STEP - 1) & ~(uintptr_t)(COMMIT_STEP - 1);
    if (want > g_reserve_end) want = g_reserve_end;
    if (want < end) return false;
    if (!VirtualAlloc((void*)g_commit_end, want - g_commit_end, MEM_COMMIT, PAGE_READWRITE)) return false;
    g_commit_end = want;
    return true;
}

// Extend the heap by at least `need` bytes at the top. Lock held.
bool grow(uint32_t need) {
    uintptr_t old_top = g_ctl->top;
    uintptr_t delta = ((uintptr_t)need + GROW_MIN + 0xFFFF) & ~(uintptr_t)0xFFFF;
    uintptr_t new_top = old_top + delta;
    if (new_top + HDR > g_reserve_end) {
        delta = ((g_reserve_end - HDR - old_top) & ~(uintptr_t)7);
        if (delta < need) return false;
        new_top = old_top + delta;
    }
    if (!ensure_commit(new_top + HDR)) return false;
    Hdr* b = (Hdr*)old_top;                     // old sentinel becomes a free block
    b->sizef = (uint32_t)delta | F_FREE | (b->sizef & F_PREVFREE);
    Hdr* s = (Hdr*)new_top;
    s->sizef = F_PREVFREE;
    s->prev_size = (uint32_t)delta;
    g_ctl->top = (uint32_t)new_top;
    g_hi = new_top;
    if (b->sizef & F_PREVFREE) {
        Hdr* p = prev_phys(b);
        remove_free(p);
        p->sizef = (bsize(p) + (uint32_t)delta) | F_FREE | (p->sizef & F_PREVFREE);
        s->prev_size = bsize(p);
        b = p;
    }
    insert_free(b);
    return true;
}

// Mark free block b (already removed from its list) as used with size `need`, splitting.
void take(Hdr* b, uint32_t need) {
    uint32_t s = bsize(b);
    uint32_t rem = s - need;
    uint32_t keep = b->sizef & F_PREVFREE;
    if (rem >= MIN_BLOCK) {
        b->sizef = need | keep;
        Hdr* r = (Hdr*)((char*)b + need);
        r->sizef = rem | F_FREE;
        Hdr* n = next_phys(r);
        n->prev_size = rem;
        n->sizef |= F_PREVFREE;
        insert_free(r);
    } else {
        b->sizef = s | keep;
        next_phys(b)->sizef &= ~F_PREVFREE;
    }
}

// Really free a used block (coalescing). Lock held.
void release(Hdr* b) {
    uint32_t s = bsize(b);
    Hdr* n = (Hdr*)((char*)b + s);
    if (b->sizef & F_PREVFREE) {
        Hdr* p = prev_phys(b);
        remove_free(p);
        s += bsize(p);
        b = p;
    }
    if (n->sizef & F_FREE) {
        remove_free(n);
        s += bsize(n);
    }
    b->sizef = s | F_FREE;
    Hdr* nn = (Hdr*)((char*)b + s);
    nn->prev_size = s;
    nn->sizef |= F_PREVFREE;
    insert_free(b);
}

inline uint32_t need_for(size_t n) {
    uint32_t need = (uint32_t)((n + HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1));
    return need < MIN_BLOCK ? MIN_BLOCK : need;
}

void* alloc_locked(size_t n) {
    if (n > 0x7FFF0000u) return nullptr;
    uint32_t need = need_for(n);
    Hdr* b = find_free(need);
    if (!b) {
        if (!grow(need)) return nullptr;
        b = find_free(need);
        if (!b) return nullptr;
    }
    remove_free(b);
    take(b, need);
    return b + 1;
}

void free_locked(void* p) {
    Hdr* b = hdr_of(p);
    if (b->sizef & F_FREE) return;              // double free: ignore
    if ((b->sizef & F_FROZEN) && g_snapshot) {
        g_deferred.push(b);
        return;
    }
    release(b);
}

// Builds g_usable = ranges that were free at the snapshot, plus the heap growth since.
void build_usable_ranges(uintptr_t cur_top) {
    g_usable.clear();
    for (size_t i = 0; i < g_free_s.count; i++) g_usable.push(g_free_s.data[i]);
    if (cur_top > g_top_s) {
        if (g_usable.count && g_usable.data[g_usable.count - 1].end == g_top_s)
            g_usable.data[g_usable.count - 1].end = cur_top;
        else
            g_usable.push(Range{ g_top_s, cur_top });
    }
}

void apply_deferred_locked() {
    for (size_t i = 0; i < g_deferred.count; i++) release(g_deferred.data[i]);
    g_deferred.clear();
    for (size_t i = 0; i < g_deferred_old.count; i++) g_pending_old.push(g_deferred_old.data[i]);
    g_deferred_old.clear();
}

} // namespace

bool Init(size_t reserve_bytes) {
    if (g_ctl) return true;
    reserve_bytes = (reserve_bytes + 0xFFFF) & ~(size_t)0xFFFF;
    void* p = VirtualAlloc(nullptr, reserve_bytes, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_READWRITE);
    if (!p) return false;
    g_base = (uintptr_t)p;
    g_reserve_end = g_base + reserve_bytes;
    g_commit_end = g_base;
    if (!ensure_commit(g_base + 65536)) return false;
    g_ctl = (Control*)g_base;
    memset(g_ctl, 0, sizeof(Control));
    g_ctl->magic = MAGIC;
    g_heap_start = (g_base + sizeof(Control) + 63) & ~(uintptr_t)63;
    // initial sentinel only; the first allocation grows the heap
    Hdr* s = (Hdr*)g_heap_start;
    s->prev_size = 0;
    s->sizef = 0;
    g_ctl->top = (uint32_t)g_heap_start;
    g_lo = g_heap_start;
    g_hi = g_heap_start;
    return true;
}

uintptr_t Base() { return g_base; }
uintptr_t ReservedEnd() { return g_reserve_end; }

void* Alloc(size_t size) {
    AcquireSRWLockExclusive(&g_lock);
    void* p = alloc_locked(size);
    ReleaseSRWLockExclusive(&g_lock);
    return p;
}

void Free(void* p) {
    if (!p) return;
    AcquireSRWLockExclusive(&g_lock);
    free_locked(p);
    ReleaseSRWLockExclusive(&g_lock);
}

size_t UsableSize(const void* p) {
    return bsize(hdr_of(p)) - HDR;
}

bool IsAllocated(const void* p) {
    return Contains(p) && !(hdr_of(p)->sizef & F_FREE);
}

void* Realloc(void* p, size_t n) {
    if (!p) return Alloc(n);
    Hdr* b = hdr_of(p);
    uint32_t cur = bsize(b) - HDR;
    if (n <= cur) return p;
    if (n > 0x7FFF0000u) return nullptr;
    uint32_t need = need_for(n);
    AcquireSRWLockExclusive(&g_lock);
    if (!(b->sizef & F_FROZEN)) {
        Hdr* nx = next_phys(b);
        if ((nx->sizef & F_FREE) && bsize(b) + bsize(nx) >= need) {
            uint32_t total = bsize(b) + bsize(nx);
            remove_free(nx);
            uint32_t keep = b->sizef & F_PREVFREE;
            uint32_t rem = total - need;
            if (rem >= MIN_BLOCK) {
                b->sizef = need | keep;
                Hdr* r = (Hdr*)((char*)b + need);
                r->sizef = rem | F_FREE;
                Hdr* nn = next_phys(r);
                nn->prev_size = rem;
                nn->sizef |= F_PREVFREE;
                insert_free(r);
            } else {
                b->sizef = total | keep;
                ((Hdr*)((char*)b + total))->sizef &= ~F_PREVFREE;
            }
            ReleaseSRWLockExclusive(&g_lock);
            return p;
        }
    }
    void* q = alloc_locked(n);
    if (q) {
        memcpy(q, p, cur);
        free_locked(p);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return q;
}

void Lock() { AcquireSRWLockExclusive(&g_lock); }
void Unlock() { ReleaseSRWLockExclusive(&g_lock); }
bool SnapshotActive() { return g_snapshot; }

static void walk_stats(Stats* st) {
    memset(st, 0, sizeof(*st));
    Hdr* h = (Hdr*)g_heap_start;
    Hdr* top = (Hdr*)(uintptr_t)g_ctl->top;
    while (h < top) {
        if (h->sizef & F_FREE) st->free_blocks++;
        else {
            st->used_blocks++;
            st->used_bytes += bsize(h);
            if (h->sizef & F_FROZEN) st->frozen_blocks++;
        }
        if (!bsize(h)) break;
        h = next_phys(h);
    }
    st->heap_bytes = g_ctl->top - g_heap_start;
    st->committed = g_commit_end - g_base;
    st->deferred = g_deferred.count;
}

Stats GetStats() {
    Stats st;
    AcquireSRWLockExclusive(&g_lock);
    walk_stats(&st);
    ReleaseSRWLockExclusive(&g_lock);
    return st;
}

bool Check(char* err, size_t errlen) {
    bool ok = true;
    AcquireSRWLockExclusive(&g_lock);
    Hdr* h = (Hdr*)g_heap_start;
    Hdr* top = (Hdr*)(uintptr_t)g_ctl->top;
    bool prev_free = false;
    uint32_t prev_sz = 0;
    size_t free_walk = 0;
    while (h < top) {
        uint32_t s = bsize(h);
        if (s < MIN_BLOCK || (uintptr_t)h + s > (uintptr_t)top) {
            _snprintf(err, errlen, "bad block size %u at %p", s, h); ok = false; break;
        }
        if (((h->sizef & F_PREVFREE) != 0) != prev_free) {
            _snprintf(err, errlen, "PREVFREE mismatch at %p", h); ok = false; break;
        }
        if (prev_free && h->prev_size != prev_sz) {
            _snprintf(err, errlen, "prev_size mismatch at %p", h); ok = false; break;
        }
        bool f = (h->sizef & F_FREE) != 0;
        if (f && prev_free) { _snprintf(err, errlen, "adjacent free blocks at %p", h); ok = false; break; }
        if (f) free_walk++;
        prev_free = f;
        prev_sz = s;
        h = next_phys(h);
    }
    if (ok && h != top) { _snprintf(err, errlen, "walk overran top"); ok = false; }
    if (ok && (((top->sizef & F_PREVFREE) != 0) != prev_free)) { _snprintf(err, errlen, "sentinel PREVFREE mismatch"); ok = false; }
    if (ok) {
        size_t free_lists = 0;
        for (uint32_t fl = 0; fl < FL_COUNT && ok; fl++)
            for (uint32_t sl = 0; sl < SL_COUNT && ok; sl++) {
                bool bit = (g_ctl->sl_bitmap[fl] >> sl) & 1;
                Hdr* b = g_ctl->heads[fl][sl];
                if (bit != (b != nullptr)) { _snprintf(err, errlen, "bitmap mismatch fl=%u sl=%u", fl, sl); ok = false; break; }
                Hdr* prev = nullptr;
                for (; b; b = links(b)->next) {
                    if (!(b->sizef & F_FREE)) { _snprintf(err, errlen, "used block in free list %p", b); ok = false; break; }
                    uint32_t f2, s2; mapping(bsize(b), &f2, &s2);
                    if (f2 != fl || s2 != sl) { _snprintf(err, errlen, "block in wrong list %p", b); ok = false; break; }
                    if (links(b)->prev != prev) { _snprintf(err, errlen, "bad prev link %p", b); ok = false; break; }
                    prev = b;
                    if (++free_lists > free_walk) { _snprintf(err, errlen, "free list cycle"); ok = false; break; }
                }
            }
        if (ok && free_lists != free_walk) { _snprintf(err, errlen, "free count %u vs lists %u", (unsigned)free_walk, (unsigned)free_lists); ok = false; }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (ok && errlen) err[0] = 0;
    return ok;
}

uint64_t ChecksumAllocated() {
    // FNV-1a over each allocated block's address, size and payload. Caller holds the lock.
    uint64_t h = 1469598103934665603ULL;
    Hdr* p = (Hdr*)g_heap_start;
    Hdr* top = (Hdr*)(uintptr_t)g_ctl->top;
    while (p < top && bsize(p)) {
        if (!(p->sizef & F_FREE)) {
            uint32_t s = bsize(p);
            uintptr_t a = (uintptr_t)p;
            const uint8_t* b = (const uint8_t*)&a;
            for (int i = 0; i < 4; i++) { h ^= b[i]; h *= 1099511628211ULL; }
            b = (const uint8_t*)&s;
            for (int i = 0; i < 4; i++) { h ^= b[i]; h *= 1099511628211ULL; }
            const uint8_t* pl = (const uint8_t*)(p + 1);
            uint32_t n = s - HDR;
            for (uint32_t i = 0; i < n; i++) { h ^= pl[i]; h *= 1099511628211ULL; }
        }
        p = next_phys(p);
    }
    return h;
}

uintptr_t BeginSnapshot() {
    apply_deferred_locked();
    g_oom = false;
    g_free_s.clear();
    Hdr* h = (Hdr*)g_heap_start;
    Hdr* top = (Hdr*)(uintptr_t)g_ctl->top;
    while (h < top && bsize(h)) {
        if (h->sizef & F_FREE) {
            h->sizef &= ~F_FROZEN;
            g_free_s.push(Range{ (uintptr_t)h, (uintptr_t)h + bsize(h) });
        } else {
            h->sizef |= F_FROZEN;
        }
        h = next_phys(h);
    }
    if (g_oom) { g_free_s.clear(); return 0; }   // incomplete free-range list: no snapshot
    g_top_s = g_ctl->top;
    g_snapshot = true;
    return g_top_s + HDR;
}

void EndSnapshot() {
    ResetWriteWatch((void*)g_base, g_commit_end - g_base);
}

void DropSnapshot() {
    apply_deferred_locked();
    g_snapshot = false;
    g_free_s.clear();
}

Range* CollectZombies(size_t* count) {
    g_oom = false;
    g_zombies.clear();
    uintptr_t cur_top = g_ctl->top;
    build_usable_ranges(cur_top);
    for (size_t i = 0; i < g_usable.count; i++) {
        Hdr* h = (Hdr*)g_usable.data[i].start;
        Hdr* end = (Hdr*)g_usable.data[i].end;
        while (h < end) {
            uint32_t s = bsize(h);
            if (!s) break;
            if (!(h->sizef & F_FREE) && !(h->sizef & F_FROZEN))
                g_zombies.push(Range{ (uintptr_t)h, (uintptr_t)h + s });
            h = (Hdr*)((char*)h + s);
        }
        if (h != end) { *count = (size_t)-1; return nullptr; }   // boundary mismatch: heap corrupted
    }
    if (g_oom) { *count = (size_t)-1; return nullptr; }          // incomplete lists: refuse to restore
    *count = g_zombies.count;
    return g_zombies.data;
}

size_t DirtyPages(uintptr_t start, uintptr_t end, uintptr_t* out, size_t max_out, bool* overflow) {
    ULONG_PTR count = max_out;
    DWORD gran = 0;
    *overflow = false;
    if (end > g_commit_end) end = g_commit_end;
    if (end <= start) return 0;
    if (GetWriteWatch(0, (void*)start, end - start, (PVOID*)out, &count, &gran) != 0) {
        *overflow = true;
        return 0;
    }
    if (count == max_out) *overflow = true;   // may be truncated: caller restores everything
    return count;
}

bool FinishRestore(size_t kept, char* err, size_t errlen) {
    if (kept > g_zombies.count) kept = g_zombies.count;
    // ctl now holds the snapshot's control block; rebuild free lists around the zombies.
    uintptr_t cur_top = 0;
    // The current top is the larger of the restored top (S) and the highest zombie/usable end.
    // build_usable_ranges needs the pre-restore top, which CollectZombies stored in g_usable.
    if (g_usable.count) cur_top = g_usable.data[g_usable.count - 1].end;
    if (cur_top < g_top_s) cur_top = g_top_s;
    memset(g_ctl->sl_bitmap, 0, sizeof(g_ctl->sl_bitmap));
    memset(g_ctl->heads, 0, sizeof(g_ctl->heads));
    g_ctl->fl_bitmap = 0;
    g_ctl->top = (uint32_t)cur_top;
    // g_usable may end at g_top_s (no growth): the sentinel is then the block at the range end.
    size_t zi = 0;
    for (size_t i = 0; i < g_usable.count; i++) {
        uintptr_t r0 = g_usable.data[i].start, r1 = g_usable.data[i].end;
        uintptr_t cursor = r0;
        bool prev_free = false;
        uint32_t prev_sz = 0;
        while (zi < kept && g_zombies.data[zi].start < r1) {
            Range z = g_zombies.data[zi++];
            if (z.start < cursor) { _snprintf(err, errlen, "zombie order"); return false; }
            if (z.start > cursor) {
                Hdr* g = (Hdr*)cursor;
                g->sizef = (uint32_t)(z.start - cursor) | F_FREE;
                insert_free(g);
                prev_free = true;
                prev_sz = (uint32_t)(z.start - cursor);
            }
            Hdr* zh = (Hdr*)z.start;
            zh->sizef = (uint32_t)(z.end - z.start) | (prev_free ? F_PREVFREE : 0);
            if (prev_free) zh->prev_size = prev_sz;
            prev_free = false;
            cursor = z.end;
        }
        if (cursor < r1) {
            Hdr* g = (Hdr*)cursor;
            g->sizef = (uint32_t)(r1 - cursor) | F_FREE;
            insert_free(g);
            prev_free = true;
            prev_sz = (uint32_t)(r1 - cursor);
        }
        Hdr* after = (Hdr*)r1;
        if (r1 == cur_top) after->sizef = 0;    // sentinel
        if (prev_free) { after->sizef |= F_PREVFREE; after->prev_size = prev_sz; }
        else after->sizef &= ~F_PREVFREE;
    }
    if (zi != kept) { _snprintf(err, errlen, "zombie outside usable ranges"); return false; }
    if (!g_usable.count || g_usable.data[g_usable.count - 1].end != cur_top) {
        // No usable range touches the top: the sentinel was restored from the image as-is.
        Hdr* s = (Hdr*)cur_top;
        s->sizef &= F_PREVFREE;
    }
    g_hi = cur_top;
    g_deferred.clear();
    g_deferred_old.clear();
    ResetWriteWatch((void*)g_base, g_commit_end - g_base);
    if (errlen) err[0] = 0;
    return true;
}

void HeapSpan(uintptr_t* lo, uintptr_t* hi) { *lo = g_heap_start; *hi = g_ctl->top; }

void DeferOldFree(void* p) {
    AcquireSRWLockExclusive(&g_lock);
    g_deferred_old.push(p);
    ReleaseSRWLockExclusive(&g_lock);
}

void SetOldFreeFn(OldFreeFn fn) { g_old_free = fn; }
size_t DeferredOldCount() { return g_deferred_old.count; }

void FlushPendingOld() {
    // The original allocator takes its own locks: never call this while threads are suspended.
    AcquireSRWLockExclusive(&g_lock);
    size_t n = g_pending_old.count;
    void** v = g_pending_old.data;
    g_pending_old.data = nullptr; g_pending_old.count = 0; g_pending_old.cap = 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (v) {
        if (g_old_free) for (size_t i = 0; i < n; i++) g_old_free(v[i]);
        VirtualFree(v, 0, MEM_RELEASE);
    }
}

} // namespace arena
