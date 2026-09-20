#!/usr/bin/env python3
"""Conservative garbage collection: treating every word as a potential pointer.

Research demo for "conservative" scanning as used by the
Boehm-Demers-Weiser (BDWGC) collector:

  - Allocate several REAL buffers with ctypes and record their addresses.
  - Pack a fake stack frame (a byte buffer) word-by-word with a mix of real
    addresses, planted false positives, boundary cases, and random words.
  - Scan the frame word-by-word, range-filtering each word against the
    recorded allocations -- exactly how a conservative collector decides
    which words are pointers.

Run:  python3 conservative-scan.py
"""

import ctypes
import random
import struct
import sys
import time

WORD = 8  # 64-bit machine


# Reference: https://www.hboehm.info/gc/gcdescr.html
#   "Often the collector has no real information about the location of
#    pointer variables in the heap, so it views all static data areas,
#    stacks and registers as potentially containing pointers. Any bit
#    patterns that represent addresses inside heap objects managed by the
#    collector are viewed as pointers."
#
# Reference: https://www.hboehm.info/gc/04tutorial.pdf (BDWGC tutorial)
#   "Conservative collectors handle pointer location uncertainty:
#    If it might be a pointer it's treated as a pointer... May lead to
#    accidental retention of garbage objects."
#
# Why is it called CONSERVATIVE? It never risks freeing a live object: when
# in doubt, it keeps. The cost of being wrong is bounded:
#   - FALSE POSITIVE (random bits / an integer that looks like a heap
#     address): the block is retained even though it is garbage.
#     Retention is wasteful but SAFE.
#   - There is no "false negative" danger: any true pointer-looking word is
#     kept, so live data can never be freed while a reference exists.
#   - Retention cannot corrupt memory because a conservative collector is
#     NON-MOVING: "Objects with ambiguous references are not moved. And we
#     never move any objects." (04tutorial.pdf). Since blocks stay put and
#     the scan only *adds* liveness, the worst case is a bigger heap.
# Reference: https://www.hboehm.info/gc/bounds.html -- Boehm shows the extra
#   space retained by misidentified pointers is bounded relative to live
#   memory for typical programs.


def rule(ch="-", width=72):
    print(ch * width)


def section(title):
    print()
    rule("=")
    print(title)
    rule("=")


def pause(seconds=0.8):
    if sys.stdout.isatty():
        time.sleep(seconds)


class Allocation:
    """A real malloc'd block behind a ctypes string buffer."""

    def __init__(self, tag, size):
        # Reference: https://docs.python.org/3/library/ctypes.html
        # create_string_buffer() allocates writable memory via the C
        # allocator; its address is fixed for the object's lifetime.
        self.buffer = ctypes.create_string_buffer(size)
        self.start = ctypes.addressof(self.buffer)  # real address
        self.size = size
        self.tag = tag

    def contains(self, addr):
        return self.start <= addr < self.start + self.size

    def __repr__(self):
        return f"<{self.tag} @ {hex(self.start)} .. {hex(self.start + self.size)}>"


def classify(word, allocs):
    """Range-filter one candidate word against known allocations."""
    if word == 0:
        return None, "zero word"
    for alloc in allocs:
        if alloc.contains(word):
            kind = "start-pointer" if word == alloc.start else "interior"
            return alloc, kind
    return None, "no allocation contains this address"


def main():
    section("STEP 1 - Allocate real buffers and record their addresses")
    allocs = [Allocation(tag, size)
              for tag, size in [("obj-a", 64), ("obj-b", 128),
                                ("obj-c", 32), ("obj-d", 96),
                                ("obj-e", 48), ("obj-f", 80)]]
    for alloc in allocs:
        print(f"  {alloc.tag}: {hex(alloc.start)} .. "
              f"{hex(alloc.start + alloc.size)} ({alloc.size} B)")
    print(">> These ranges define 'what looks like a pointer' below.")
    pause()

    section("STEP 2 - Build a fake stack frame full of candidate words")
    rng = random.Random(0xC0FFEE)          # seeded => reproducible demo
    frame = bytearray(12 * WORD)

    def put(idx, value, note):
        struct.pack_into("<Q", frame, idx * WORD, value)
        notes[idx] = note

    notes = {}
    put(0, allocs[0].start,                    "true root: &obj-a")
    put(1, allocs[1].start,                    "true root: &obj-b")
    put(2, allocs[2].start + 16,               "true root: interior into obj-c")
    put(3, allocs[3].start + allocs[3].size + 8, "just PAST end of obj-d")
    put(4, rng.getrandbits(64),                "random 64-bit noise #1")
    put(5, rng.getrandbits(64),                "random 64-bit noise #2")
    put(6, allocs[4].start,                    "PLANTED FALSE POSITIVE:")
    print("           word 6 numerically equals obj-e's address but is")
    print("           'just an integer' (e.g. a hash or counter) -- the")
    print("           scanner cannot tell and must retain obj-e.")
    put(7, 42,                                 "small integer (typical scalar)")
    put(8, allocs[5].start,                    "true root: &obj-f")
    put(9, rng.getrandbits(64),                "random 64-bit noise #3")
    put(10, allocs[0].start,                   "same root stored twice")
    put(11, 0xDEADBEEF,                        "sentinel constant, points nowhere")
    print(f"  packed {len(frame) // WORD} aligned 64-bit words")
    pause()

    section("STEP 3 - Conservative scan: every word is guilty until proven innocent")
    # A real collector scans only word-aligned offsets; so do we.
    hits = []
    for i in range(len(frame) // WORD):
        (word,) = struct.unpack_from("<Q", frame, i * WORD)
        alloc, kind = classify(word, allocs)
        verdict = (f"POINTER -> {alloc.tag} [{kind}]"
                   if alloc else "not a pointer")
        expect = notes[i].split(":")[0]
        flagged = alloc is not None
        marker = ""
        if "FALSE POSITIVE" in notes[i] and flagged:
            marker = "   <-- RETAINED despite being garbage"
        elif not flagged and "PAST end" in notes[i]:
            marker = "   <-- correctly rejected (outside range)"
        print(f"  word {i:2d}: 0x{word:016X}  {verdict}{marker}")
        if flagged:
            hits.append((i, alloc))
    pause()

    section("RESULTS - What conservatism bought and cost us")
    found = sorted({a.tag for _, a in hits})
    print(f"  words treated as pointers : {len(hits)}")
    print(f"  distinct objects retained : {found}")
    print("  - obj-e survives ONLY because of a coincidental integer")
    print("    (false positive => retention, never corruption).")
    print("  - past-end address and small ints rejected by range filter;")
    print("    64-bit random noise essentially never lands inside a heap")
    print("    range (~heap_size / 2^64 probability per word).")
    print("  - Interior pointers retained obj-c: fine for a non-moving")
    print("    collector that scans from block start to block end.")
    print(">> Conclusion: safe-by-default, precise-enough, and the reason")
    print(">> a tiny C GC can skip type info entirely -- at the price of")
    print(">> occasionally keeping dead blocks alive.")


if __name__ == "__main__":
    main()
