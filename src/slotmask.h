// Snapshot slots as bits: each guard keeps, per object, the mask of the snapshots (slots) that pin it.
#pragma once
#include <stdint.h>

const int MAX_SNAPSHOT_SLOTS = 64;   // = snapshot::MAX_SNAPSHOTS, at most the bits of SlotMask
typedef uint64_t SlotMask;

inline SlotMask SlotBit(int slot) { return (SlotMask)1 << slot; }
inline int SlotCount(SlotMask m) { int n = 0; for (; m; m &= m - 1) n++; return n; }
