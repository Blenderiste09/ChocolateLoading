// The objects a guard keeps alive for the snapshots (MP3 decoders, Bink videos...).
//
// Each object carries the set of snapshots that refer to it (a bit mask, one bit per snapshot slot) and
// whether the current timeline has it ("alive"). When the game destroys an object some snapshot still
// refers to, the destruction is deferred; an object that no snapshot refers to and that the current
// timeline does not have is really destroyed. A restore makes the restored snapshot's objects alive
// again and discards the others (Discard before the memory is restored, Revive after).
#pragma once
#include <windows.h>
#include "slotmask.h"

template <int N> class PinSet {
public:
    void Init() { InitializeCriticalSection(&cs_); }
    bool Overflowed() const { return overflow_; }

    // The game created p.
    void Created(void* p) {
        EnterCriticalSection(&cs_);
        int i = find(p);   // an address reused after a destruction we did not see: a new object
        if (i < 0 && n_ < N) i = n_++;
        if (i >= 0) e_[i] = Ent{ p, 0, true };
        else overflow_ = true;
        LeaveCriticalSection(&cs_);
    }

    // The game destroys p: true if it must be destroyed now, false if a snapshot still needs it.
    bool Destroyed(void* p) {
        EnterCriticalSection(&cs_);
        int i = find(p);
        bool now = i < 0 || !e_[i].mask;
        if (i >= 0) {
            if (now) e_[i] = e_[--n_];
            else e_[i].alive = false;
        }
        LeaveCriticalSection(&cs_);
        return now;
    }

    // A snapshot was taken in `slot`: it refers to every object the current timeline has.
    int Pin(int slot) {
        int pinned = 0;
        EnterCriticalSection(&cs_);
        for (int i = 0; i < n_; i++) if (e_[i].alive) { e_[i].mask |= SlotBit(slot); pinned++; }
        LeaveCriticalSection(&cs_);
        return pinned;
    }

    // Before restoring `slot`: the objects the restored timeline will not have are no longer alive; those
    // no snapshot refers to are destroyed (their owners, being discarded, are still intact).
    template <class Destroy> int Discard(int slot, Destroy destroy) {
        return remove_where([slot](Ent& x) {
            if (!(x.mask & SlotBit(slot))) x.alive = false;
            return !x.alive && !x.mask;
        }, destroy);
    }

    // After restoring `slot`: its objects are alive again.
    int Revive(int slot) {
        int revived = 0;
        EnterCriticalSection(&cs_);
        for (int i = 0; i < n_; i++)
            if ((e_[i].mask & SlotBit(slot)) && !e_[i].alive) { e_[i].alive = true; revived++; }
        LeaveCriticalSection(&cs_);
        return revived;
    }

    // The snapshot in `slot` is gone: destroy what nothing refers to any more.
    template <class Destroy> int Unpin(int slot, Destroy destroy) {
        return remove_where([slot](Ent& x) {
            x.mask &= ~SlotBit(slot);
            return !x.alive && !x.mask;
        }, destroy);
    }

private:
    struct Ent { void* p; SlotMask mask; bool alive; };
    Ent  e_[N];
    int  n_ = 0;
    bool overflow_ = false;
    CRITICAL_SECTION cs_;

    int find(void* p) const {
        for (int i = 0; i < n_; i++) if (e_[i].p == p) return i;
        return -1;
    }

    // Removes the entries for which pred (which may update them) returns true, then destroys them
    // outside the lock.
    template <class Pred, class Destroy> int remove_where(Pred pred, Destroy destroy) {
        void* doomed[N];
        int n = 0;
        EnterCriticalSection(&cs_);
        for (int i = 0; i < n_;)
            if (pred(e_[i])) { doomed[n++] = e_[i].p; e_[i] = e_[--n_]; }
            else i++;
        LeaveCriticalSection(&cs_);
        for (int i = 0; i < n; i++) destroy(doomed[i]);
        return n;
    }
};
