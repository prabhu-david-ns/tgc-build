#!/usr/bin/env python3
"""When does a garbage collector decide to run? (Trigger policies.)

Simulates allocation/free churn against three policies:

  P1. FIXED ALLOCATION THRESHOLD -- collect after N net allocations.
      This is CPython's generation-0 rule: the collector tracks
      "the number of object allocations and deallocations since the last
      collection. When the number of allocations minus the number of
      deallocations exceeds threshold0, collection starts."

  P2. GROWTH POLICY -- collect when live-object count exceeds N x the
      live count measured at the END of the previous sweep. The tiny-GC
      workhorse: work per byte collected stays roughly constant, and a
      heap that stops growing stops collecting.

  P3. GENERATIONAL IDEA AT A GLANCE -- most objects die young, so young
      objects are collected often/cheaply and survivors are promoted to
      older generations collected rarely. We emulate gen-0/gen-2 style
      counters with threshold0 and a promotion counter.

Run:  python3 trigger-policy.py
"""

import random
import sys
import time

# Reference: https://docs.python.org/3/library/gc.html#gc.set_threshold
#   "In order to decide when to run, the collector keeps track of the
#    number object allocations and deallocations since the last collection.
#    When the number of allocations minus the number of deallocations
#    exceeds threshold0, collection starts." Generations: survivors move
#    0 -> 1 -> 2; older generations are examined less often. Default is
#    (700, 10, 10).
#
# Reference: https://github.com/python/cpython/blob/main/InternalDocs/garbage_collector.md
#   "Generational garbage collection takes advantage of what is known as
#    the weak generational hypothesis: Most objects die young."
#   Also documents the full-collection damping heuristic:
#   long_lived_pending / long_lived_total > 25% before scanning gen 2,
#   which keeps total GC cost amortized-linear instead of quadratic.
#
# Reference: https://www.hboehm.info/gc/gcdescr.html (BDWGC)
#   Trigger-on-demand at allocation: if allocation since the last
#   collection is less than heap_size / GC_free_space_divisor, EXPAND the
#   heap instead of collecting, else collect -- "This ensures that the
#   amount of garbage collection work per allocated byte remains
#   constant." Our P2 growth policy is a simplified cousin: it compares
#   LIVE OBJECTS rather than bytes allocated.


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


class ChurnSim:
    """A toy mutator: allocates, drops, and reports liveness."""

    def __init__(self, steps=240, seed=11):
        self.rng = random.Random(seed)
        self.steps = steps
        self.live = []          # ids of currently-live fake objects
        self.next_id = 0
        self.total_allocs = 0
        self.net_since_gc = 0   # allocations - frees since last collection

    def step(self):
        """One mutator step; returns ('alloc'|'free', id)."""
        # 60% allocate, but always free something occasionally so churn
        # looks realistic (many short-lived objects).
        if self.live and self.rng.random() < 0.35:
            victim = self.live.pop(self.rng.randrange(len(self.live)))
            self.net_since_gc -= 1
            return "free", victim
        oid = self.next_id
        self.next_id += 1
        self.live.append(oid)
        self.total_allocs += 1
        self.net_since_gc += 1
        return "alloc", oid


def run_policy_p1(steps=120):
    section("P1 - Fixed allocation threshold (CPython gen-0 style)")
    sim = ChurnSim(steps)
    THRESHOLD0 = 25                      # like gc.set_threshold(25)
    collections = 0
    for i in range(sim.steps):
        op, oid = sim.step()
        if sim.net_since_gc > THRESHOLD0:
            freed = len([x for x in sim.live if x % 2 == 1])  # pretend ~half die
            sim.live = [x for x in sim.live if x % 2 == 0]
            sim.net_since_gc = 0
            collections += 1
            print(f"  [step {i:3d}] {op} #{oid}: net={sim.net_since_gc + freed + 1}"
                  f">{THRESHOLD0} => COLLECT "
                  f"(freed {freed}, {len(sim.live)} survive)")
    print(f"\n  >> {collections} collections in {steps} steps "
          f"(threshold fixed => steady cadence regardless of heap size).")
    return collections


def run_policy_p2(steps=160):
    section("P2 - Growth policy: live > N x live-at-last-sweep")
    sim = ChurnSim(steps)
    GROWTH_FACTOR = 2
    live_at_last_sweep = 1               # seed value
    collections = 0
    for i in range(sim.steps):
        op, oid = sim.step()
        if len(sim.live) > GROWTH_FACTOR * live_at_last_sweep:
            survivors = [x for x in sim.live if x % 3 != 2]
            freed = len(sim.live) - len(survivors)
            print(f"  [step {i:3d}] {op} #{oid}: live={len(sim.live)} > "
                  f"{GROWTH_FACTOR}*{live_at_last_sweep} => COLLECT "
                  f"(freed {freed})")
            live_at_last_sweep = max(1, len(survivors))
            sim.live = survivors
            collections += 1
        # Watch the policy NOT fire while the heap stays small:
        elif i in (5, 40, 80, 120):
            print(f"  [step {i:3d}] {op} #{oid}: live={len(sim.live)} <= "
                  f"{GROWTH_FACTOR}*{live_at_last_sweep} => no collection")
    print(f"\n  >> {collections} collections in {steps} steps; frequency")
    print("  >> adapts: small quiet heaps are never disturbed, growing")
    print("  >> heaps get swept just often enough to bound memory.")
    return collections


def run_policy_p3(steps=90):
    section("P3 - Generational sketch: young collected often, old rarely")
    young, old = [], []
    THRESHOLD0, PROMOTE_EVERY = 12, 4    # cf. defaults (700, 10, 10): older
    promotions = 0                        # gens are collected far less often
    rng = random.Random(3)
    for i in range(steps):
        oid = i
        young.append(oid)
        if rng.random() < 0.5 and young:
            young.pop(rng.randrange(len(young)))   # most die young...
        if len(young) > THRESHOLD0:
            survivors = [x for x in young if x % 4 != 3]     # ...a few don't
            promote = survivors[-PROMOTE_EVERY:]
            old.extend(promote)
            promotions += 1
            young = [x for x in survivors if x not in promote]
            print(f"  [step {i:3d}] young>{THRESHOLD0}: minor COLLECT; "
                  f"promote {promote} -> old ({len(old)} old now)")
    print(f"\n  >> {promotions} minor collections; the OLD generation was")
    print("  >> never scanned at all this run -- that's the point:")
    print("  >> pay-per-new-byte, ignore the stable old majority.")
    print("  >> (CPython adds a 25%-growth damper before touching gen 2.)")


def main():
    print("""Trigger policies researched:
  P1 fixed allocation threshold  |  P2 live-growth vs last sweep  |  P3 generational""")
    pause()
    run_policy_p1()
    pause()
    run_policy_p2()
    pause()
    run_policy_p3()
    section("WHAT OUR TINY C GC WILL USE (Spec 02 preview)")
    print("""  Policy P2 (collect when live_count > N x count_at_last_sweep):
    - one integer of state, no clocks, no byte counting;
    - self-tuning: dead-heavy heaps trigger immediately (cheap win),
      live-heavy heaps stretch intervals automatically;
    - pairs naturally with conservative mark-sweep, where 'live count'
      is exactly what sweep() already computes.""")


if __name__ == "__main__":
    main()
