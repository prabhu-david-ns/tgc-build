#!/usr/bin/env python3
"""Per-object headers vs out-of-band metadata tables for a GC.

Simulates and compares the two ways a collector can remember facts
(size, mark bit, flags) about each heap block:

  A) IN-BAND: prepend a fixed header to every allocation.
     Metadata lives at (block_start + 0); payload follows the header.

  B) OUT-OF-BAND: allocate bare payloads; keep metadata in a table keyed
     by block address, e.g. {address: metadata} -- here a Python dict,
     in C typically an array of block descriptors indexed by address bits.

Run:  python3 out-of-band-metadata.py
"""

import ctypes
import struct
import sys
import time

# Reference: https://www.hboehm.info/gc/gcdescr.html
#   BDWGC keeps its bookkeeping OUT OF BAND: "Each object has an associated
#   mark bit" held in block-level header structures indexed by address, not
#   inside user data. The sweep phase walks these block headers first:
#   "This initial sweep pass touches only block headers, not the blocks."
#
# Reference: https://en.wikipedia.org/wiki/Tracing_garbage_collection
#   On per-object flags: mark-and-sweep has "the disadvantage of 'bloating'
#   objects by a small amount, as in, every object has a small hidden memory
#   cost because of the list/extra bit" -- that is the in-band header tax.
#
# TRADE-OFFS (discussed in comments below and printed at the end):
#
#   Header (+):
#     * zero lookup cost -- fields are one load at a constant offset;
#     * exact-size knowledge travels with the block, so a sweeper can walk
#       block -> next block sequentially without any side structure;
#     * metadata lifetime == object lifetime (freed together, no leaks).
#   Header (-):
#     * EVERY allocation pays overhead; tiny objects get relatively fatter,
#       and alignment padding can inflate it further;
#     * payload address != block address; every access must respect the
#       header offset.
#   Out-of-band (+):
#     * zero per-object cost -- payloads stay untouched;
#     * the table can hold arbitrarily rich data without changing blocks.
#   Out-of-band (-):
#     * every access is a keyed lookup (hash or index computation);
#     * the sweeper needs sizes from somewhere else to know block bounds;
#     * THE TABLE MUST NOT LIVE INSIDE THE COLLECTED HEAP or you get
#       recursion ("who collects the metadata?"). Real collectors malloc
#       their own structures outside scanned regions;
#     * stale entries after free are actively dangerous if addresses are
#       REUSED by later allocations -- entries must be deleted on free.

HEADER = struct.Struct("<QII")   # magic(u64) | size(u32) | flags(u32) = 16 B
MAGIC = 0x544F5947              # 'TOYG' -- detects metadata/payload mixups
FLAG_MARKED = 1


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


class HeaderedHeap:
    """Strategy A: [header | payload] in ONE buffer per object."""

    def __init__(self):
        self.blocks = []            # keeps buffers alive, in alloc order

    def allocate(self, payload: bytes, flags=0):
        buf = ctypes.create_string_buffer(HEADER.size + len(payload))
        HEADER.pack_into(buf, 0, MAGIC, len(payload), flags)
        buf[HEADER.size:HEADER.size + len(payload)] = payload
        addr = ctypes.addressof(buf)
        self.blocks.append(buf)
        return addr, buf

    @staticmethod
    def read_meta(addr, buf):
        # No table, no key computation: metadata is at a FIXED offset.
        return HEADER.unpack_from(buf, 0)

    def sweep_unmarked(self):
        freed = kept = 0
        survivors = []
        for buf in self.blocks:
            _, _, flags = HEADER.unpack_from(buf, 0)
            if flags & FLAG_MARKED:
                payload_len = len(buf) - HEADER.size   # exact-size knowledge
                HEADER.pack_into(buf, 0, MAGIC, payload_len,
                                 flags & ~FLAG_MARKED)  # clear mark bit
                kept += 1
                survivors.append(buf)
            else:
                freed += 1          # buffer dies WITH its metadata: nothing
                survivors.append(None)               # to clean up elsewhere
        self.blocks = [b for b in survivors if b is not None]
        return freed, kept


class OutOfBandHeap:
    """Strategy B: bare payloads + dict keyed by payload address."""

    def __init__(self):
        self.table = {}             # address -> {'size', 'flags'}
        self.payloads = []

    def allocate(self, payload: bytes, flags=0):
        buf = ctypes.create_string_buffer(len(payload))   # NO header bytes
        buf.raw = payload
        addr = ctypes.addressof(buf)
        self.table[addr] = {"size": len(payload), "flags": flags}
        self.payloads.append(buf)
        return addr

    @staticmethod
    def read_meta(addr, heap):
        # One hash lookup per access; cost grows with table discipline.
        return heap.table[addr]

    def sweep_unmarked(self):
        freed = kept = 0
        for addr in list(self.table):
            meta = self.table[addr]
            if meta["flags"] & FLAG_MARKED:
                meta["flags"] &= ~FLAG_MARKED
                kept += 1
            else:
                del self.table[addr]   # MUST remember this extra step...
                freed += 1             # ...forgetting leaks stale entries
        return freed, kept


def demo_layout():
    section("STEP 1 - Same logical object, two memory layouts")
    payload = b"hello GC world"

    hheap = HeaderedHeap()
    addr_h, buf_h = hheap.allocate(payload, FLAG_MARKED)
    magic, size, flags = HeaderedHeap.read_meta(addr_h, buf_h)
    print(f"  IN-BAND   : block {hex(addr_h)}, {HEADER.size}B header "
          f"+ {len(payload)}B payload")
    print(f"              header@+0  -> magic={hex(magic)} size={size} "
          f"flags={flags}")
    print(f"              payload@+{HEADER.size} = {buf_h[HEADER.size:].decode()}")

    oheap = OutOfBandHeap()
    addr_o = oheap.allocate(payload, FLAG_MARKED)
    meta = OutOfBandHeap.read_meta(addr_o, oheap)
    print(f"  OUT-OF-BND: payload {hex(addr_o)}, {len(payload)}B pure data,")
    print(f"              table[{hex(addr_o)}] -> {meta}")
    print(f"  overhead per object: header={HEADER.size}B vs table-entry="
          f"~40+B (dict entry, outside the block)")
    return hheap, oheap


def demo_sweep(hheap, oheap):
    section("STEP 2 - Sweep semantics differ")
    # First allocation was created MARKED in step 1; the second is born
    # unmarked in both strategies, so each sweep should free exactly one.
    hheap.allocate(b"victim-headered")   # flags = 0 (unmarked)
    oheap.allocate(b"victim-oob")        # flags = 0 (unmarked)

    fh, kh = hheap.sweep_unmarked()
    print(f"  IN-BAND    sweep: freed={fh} kept={kh}")
    print("     freeing the block frees header AND payload together.")
    fo, ko = oheap.sweep_unmarked()
    print(f"  OUT-OF-BND sweep: freed={fo} kept={ko}")
    print("     required an EXTRA delete of the table entry; forgetting")
    print("     it leaves stale metadata that poisons reused addresses.")

    # Show the mixup detector value of the magic field.
    section("STEP 3 - Header magic as corruption tripwire")
    magic, _, _ = HEADER.unpack_from(hheap.blocks[0], 0)
    ok = magic == MAGIC
    print(f"  magic check on surviving block: {ok} ({hex(magic)})")
    print("  In C, walking [header|payload] blocks sequentially REQUIRES")
    print("  trusting sizes; a magic field catches off-by-N overwrites.")


def demo_lookup_cost(buf_h, oheap, n=200_000):
    section("STEP 4 - Metadata access cost (Python-level proxy)")
    # Fair OOB loop: derive the key from the live buffer, like a real
    # sweeper that holds block pointers but must key by address.
    buf_o = oheap.payloads[0]
    t0 = time.perf_counter()
    for _ in range(n):
        HEADER.unpack_from(buf_h, 0)
    t_hdr = time.perf_counter() - t0

    t0 = time.perf_counter()
    for _ in range(n):
        oheap.table[ctypes.addressof(buf_o)]
    t_oob = time.perf_counter() - t0

    print(f"  {n:,} header reads : {t_hdr:.3f}s  "
          f"({t_hdr / n * 1e9:.0f} ns/op)")
    print(f"  {n:,} keyed lookups: {t_oob:.3f}s  "
          f"({t_oob / n * 1e9:.0f} ns/op)")
    print("""  CAUTION: this measures PYTHON overhead more than strategy --
  struct.unpack_from boxes a fresh tuple per call, while dict lookup
  returns an existing object. In C the comparison inverts decisively:
  header field = ONE load at a constant offset; out-of-band = hash/index
  computation plus a dependent load of cold table cache lines. Python
  cannot model that gap; trust the C reasoning, not these ns/op.""")


def main():
    hheap, oheap = demo_layout()
    pause()
    demo_sweep(hheap, oheap)
    pause()
    demo_lookup_cost(hheap.blocks[0], oheap)

    section("VERDICT FOR OUR TINY C COLLECTOR (Spec 02 preview)")
    print("""  We will use HEADERS prepended to each allocation:
    - the sweeper learns exact sizes while scanning (no side table),
    - mark bits ride with blocks (freed atomically with them),
    - conservative stack roots point at PAYLOADS; the scanner range-checks
      against payload spans, then steps back one header to find metadata.
  Cost accepted: a fixed per-allocation tax -- the classic trade where
  correctness/simplicity beats a few bytes per object.""")


if __name__ == "__main__":
    main()
