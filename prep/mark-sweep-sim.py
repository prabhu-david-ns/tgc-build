#!/usr/bin/env python3
"""Mark-and-sweep over a toy heap, simulated in pure Python.

Models the heap as a list of fake objects (id, refs, marked), a root set,
and implements mark() (iterative DFS with visited protection) and sweep().
Runs four scenarios: lone garbage freed, cyclic garbage freed, reachable
graphs (including reachable cycles) survive, repeated collections stable.

Run:  python3 mark-sweep-sim.py
"""

import sys
import time


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


class ToyObj:
    """Fake heap object: identity, outgoing reference ids, mark flag."""

    def __init__(self, oid, refs=None):
        self.id = oid
        self.refs = list(refs) if refs else []
        self.marked = False

    def __repr__(self):
        return f"#{self.id}(refs={self.refs}, marked={self.marked})"


class ToyHeap:
    """Allocation-order heap: dict id->ToyObj plus a root set."""

    def __init__(self):
        self.objects = {}   # id -> ToyObj
        self.order = []     # allocation order (sweep walks this)
        self.roots = set()  # root object ids
        self.next_id = 1

    def allocate(self, refs=None, rooted=False):
        oid = self.next_id
        self.next_id += 1
        self.objects[oid] = ToyObj(oid, refs)
        self.order.append(oid)
        if rooted:
            self.roots.add(oid)
        return oid

    # ------------------------------------------------------------------
    # MARK phase.
    # Reference: https://en.wikipedia.org/wiki/Tracing_garbage_collection
    #   "The first stage is the mark stage which does a tree traversal of
    #    the entire 'root set'... All objects that those objects point to,
    #    and so on, are marked as well."
    # Reference: https://www.hboehm.info/gc/gcdescr.html (BDWGC overview)
    #   "Mark phase. Marks all objects that can be reachable via chains of
    #    pointers from variables."
    # We use an explicit worklist instead of recursion so a deep or cyclic
    # graph cannot overflow the call stack; the `visited` set is the
    # cycle-protection: without it, marking R -> X -> Y -> X would loop.
    # ------------------------------------------------------------------
    def mark(self):
        worklist = [r for r in self.roots if r in self.objects]
        visited = set()
        while worklist:
            oid = worklist.pop()          # pop() => DFS; popleft-ish => BFS
            if oid in visited:
                continue                  # already marked: break the cycle
            visited.add(oid)
            obj = self.objects[oid]
            obj.marked = True
            for child in obj.refs:
                if child not in visited:
                    worklist.append(child)
        return len(visited)

    # ------------------------------------------------------------------
    # SWEEP phase.
    # Reference: https://en.wikipedia.org/wiki/Tracing_garbage_collection
    #   "In the second stage, the sweep stage, all memory is scanned from
    #    start to finish... those not marked as being 'in-use' are...
    #    freed." Survivors get their flag cleared "preparing for the next
    #    cycle".
    # ------------------------------------------------------------------
    def sweep(self):
        survivors, freed = [], []
        for oid in self.order:
            obj = self.objects[oid]
            if obj.marked:
                obj.marked = False        # clear flag for next collection
                survivors.append(oid)
            else:
                del self.objects[oid]     # return block to the free list
                freed.append(oid)
        self.order = survivors
        return freed


def collect(heap, label):
    print(f"\n>> COLLECT ({label})")
    n_marked = heap.mark()
    print(f"   mark: {n_marked} object(s) reachable from roots "
          f"{sorted(heap.roots)}")
    freed = heap.sweep()
    print(f"   sweep: freed {freed if freed else 'nothing'}")
    print(f"   live now: {heap.order}")
    return freed


def show(heap, title):
    print(f"\n{title}")
    print("   roots:", sorted(heap.roots))
    for oid in heap.order:
        obj = heap.objects[oid]
        tag = "ROOT" if oid in heap.roots else "    "
        print(f"   [{tag}] #{obj.id} -> {obj.refs}")


def main():
    section("SCENARIO 1 - Unreachable lone object is freed")
    heap = ToyHeap()
    a = heap.allocate(rooted=True)      # A is a root...
    b = heap.allocate()                 # ...and A -> B keeps B alive
    heap.objects[a].refs.append(b)
    lone = heap.allocate()              # L: nobody points at it
    show(heap, "Heap before:")
    collect(heap, "lone garbage")
    assert lone not in heap.objects and b in heap.objects
    print(">> Lone unreachable object freed; reachable pair untouched.")
    pause()

    section("SCENARIO 2 - Cyclic garbage is freed")
    x = heap.allocate()
    y = heap.allocate([x])
    heap.objects[x].refs.append(y)      # X <-> Y cycle, no path from roots
    show(heap, "Heap before (X <-> Y unreachable ring):")
    collect(heap, "cyclic garbage")
    assert x not in heap.objects and y not in heap.objects
    print(">> Mark never reached X/Y (no root path), sweep freed the ring.")
    print(">> This is exactly what refcounting could NOT do (see")
    print(">> refcount-cycles.py): internal edges don't confer liveness.")
    pause()

    section("SCENARIO 3 - Reachable cycle SURVIVES (visited protection)")
    heap2 = ToyHeap()
    r = heap2.allocate(rooted=True)
    p = heap2.allocate()
    q = heap2.allocate([p])
    heap2.objects[p].refs.append(q)     # P <-> Q ...
    heap2.objects[r].refs.append(p)     # ...and R -> P reaches the ring
    show(heap2, "Heap before (R -> P <-> Q):")
    collect(heap2, "reachable cycle")
    assert p in heap2.objects and q in heap2.objects
    print(">> Visited-set stopped re-walking P<->Q; both correctly survive.")
    pause()

    section("SCENARIO 4 - Repeated collections are stable (idempotent)")
    before_state = list(heap.order)
    results = []
    for i in range(3):
        freed_i = collect(heap, f"idempotency pass {i + 1}")
        results.append(list(freed_i))
    assert all(r == [] for r in results[1:])
    assert heap.order == before_state
    print("\n>> Passes 2 and 3 freed nothing new and live-set is unchanged:")
    print(">> mark-and-sweep reaches a fixed point once garbage is gone.")
    print(">> A collector that frees live data here would be corrupt.")


if __name__ == "__main__":
    main()
