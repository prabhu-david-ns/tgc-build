#!/usr/bin/env python3
"""How a real conservative collector finds its roots on the stack.

Research points demonstrated (then simulated in Python, which cannot take
the address of a local variable):

  1. Stacks grow in ONE direction (downward on x86-64 Linux) from a deep,
     fixed "bottom".
  2. A collector captures that bottom ONCE ("stack anchor") at startup and,
     during collection, uses the address of a local variable inside its own
     scanning function as the other bound. Everything between is scanned
     conservatively.
  3. Registers are not on the stack -- setjmp() spills them into a jmp_buf
     so the scan sees register-held pointers too.

Run:  python3 stack-scanning.py
"""

import ctypes
import random
import struct
import sys

# Reference: https://en.wikipedia.org/wiki/Call_stack
#   A call stack is a region of memory holding one frame per active
#   function (locals, saved registers, return address), tracked by the
#   stack pointer; frames push/pop as functions call/return. On x86-64
#   Linux the stack grows DOWNWARD: newer frames live at LOWER addresses.
#
# Reference: https://github.com/bdwgc/bdwgc/blob/master/docs/gcdescr.md
#   Mark phase roots: the collector "views all static data areas, stacks
#   and registers as potentially containing pointers". The stack portion
#   it scans is bounded by two addresses: the stack BOTTOM captured at
#   collector initialization (deep end) and the CURRENT top of stack --
#   practically, the address of some local in GC code, e.g.
#       void gc_collect(void) {
#           jmp_buf regs;
#           char *top;
#           setjmp(regs);          // spill callee-saved registers
#           top = (char *)&regs;   // anything below here can't be ours
#           GC_push_all_stack(top, stack_bottom);
#       }
#
# Reference: https://pubs.opengroup.org/onlinepubs/9699919799/functions/setjmp.html
#   setjmp(env) saves "the calling environment" -- stack pointer, instruction
#   pointer and CALLEE-SAVED REGISTERS -- into env for later restore by
#   longjmp. For a GC this is exactly what we need not for restoring, but
#   because env (jmp_buf) is an automatic variable ON THE STACK: after
#   setjmp(), the register contents sit in scannable stack memory.
# Reference: https://www.hboehm.info/gc/04tutorial.pdf
#   "[The GC] pushes register contents onto the (GC-visible) stack."
#   Without this spill, a pointer living ONLY in a register at collection
#   time would be invisible to the scan and its object would be freed.
#
# HOW THE C VERSION DIFFERS FROM THIS SIMULATION (Spec 02 notes):
#   - Anchor: `stack_bottom` is a global captured once at GC init
#     (`GC_get_stack_base()`-style or passed down from main()); here it is
#     just index 0 of a fake array, fixed when the "program starts".
#   - Young bound: C takes `&local` INSIDE gc_collect(); the compiler then
#     cannot have any of our live locals above it. Python has no operator
#     to take addresses of locals -- hence we simulate with indices into an
#     array of fake slots.
#   - Registers: C calls setjmp() first; Python has nothing to spill.
#   - Direction: real code compares bounds and scans toward increasing or
#     decreasing addresses accordingly; we scan index order and note the
#     mapping (index grows = address DECREASES = newer frames).
#   - Alignment: both scan word-aligned words only (8 bytes on x86-64).

WORD = 8


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


class FakeAllocation:
    def __init__(self, tag, size):
        self.buffer = ctypes.create_string_buffer(size)
        self.start = ctypes.addressof(self.buffer)
        self.size = size
        self.tag = tag

    def contains(self, addr):
        return self.start <= addr < self.start + self.size


def main():
    section("STEP 1 - The heap the 'mutator' will point into")
    allocs = [FakeAllocation(tag, size)
              for tag, size in [("obj-a", 64), ("obj-b", 128),
                                ("obj-c", 48)]]
    for alloc in allocs:
        print(f"  {alloc.tag}: {hex(alloc.start)} .. "
              f"{hex(alloc.start + alloc.size)}")

    section("STEP 2 - Fake stack: array of word-sized slots")
    # Index 0 = deepest/oldest frame (captured once at 'startup').
    # Higher indices = younger frames; conceptually LOWER addresses on a
    # downward-growing stack (we store slots in age order for readability).
    STACK_SLOTS = 16
    rng = random.Random(7)
    stack = [rng.getrandbits(20) for _ in range(STACK_SLOTS)]  # junk ints
    stack_bottom_idx = 0                       # <-- the ANCHOR
    print(f"  stack_bottom (anchor) captured at startup: slot "
          f"{stack_bottom_idx}")
    print("  Real C: char *stack_bottom; captured once at GC init;")
    print("  every later collection reuses it. It never moves.")
    pause()

    section("STEP 3 - Mutator runs: locals (roots) land on the stack")
    # Pretend these three assignments happened inside user functions;
    # their values were spilled/written into stack slots.
    stack[3] = allocs[0].start                 # &obj-a held by a local var
    stack[5] = allocs[1].start                 # &obj-b likewise
    stack[6] = allocs[2].start + 8             # interior ptr into obj-c
    stack[9] = 0x1234                          # innocent integer
    current_frame_idx = 11                     # deepest live user frame now
    print(f"  planted roots in slots 3, 5, 6; junk elsewhere; "
          f"current frame at slot {current_frame_idx}")

    section("STEP 4 - Collection: scan [current_frame .. stack_bottom]")
    # C equivalent inside gc_collect(): top = &local; scan(top, bottom).
    # Our 'top' is the youngest slot we may legally read.
    found = []
    for idx in range(stack_bottom_idx, current_frame_idx + 1):
        word = stack[idx]
        hit = next((a for a in allocs if a.contains(word)), None)
        if hit:
            kind = "start" if word == hit.start else "interior"
            print(f"  slot {idx:2d}: 0x{word:016X} -> {hit.tag} [{kind}]")
            found.append(hit.tag)
        else:
            print(f"  slot {idx:2d}: 0x{word:016X} (not a pointer)")
    print(f"\n  root set discovered: {sorted(set(found))}")
    assert set(found) == {"obj-a", "obj-b", "obj-c"}
    pause()

    section("STEP 5 - Why the anchor must be captured EARLY")
    print("""  If instead we used 'the deepest frame ever' guessed at collect
  time, we could pick a bound BELOW live frames (missing roots => freed
  live objects => corruption) only if the guess is too SHALLOW; too deep
  merely costs extra scanning. Hence: capture generously at init
  (process stack base) -- safe side is always MORE scanning.
  The young bound is exact: taking &local in the scanner guarantees
  everything above it is dead scratch, never a live root.""")

    section("STEP 6 - Registers: the invisible half of the root set")
    print("""  At collection time, hot pointers live in registers, NOT on the
  stack. C solution: jmp_buf regs; setjmp(regs);
    -> callee-saved regs + sp/pc written into an AUTOMATIC buffer,
    -> which lies between our two scan bounds already,
    -> so the plain stack scan picks up register-held pointers for free.
  (BDWGC does this per-thread when it stops each thread.)
  Simulation shortcut: our fake 'registers' are just more slots in range.""")


if __name__ == "__main__":
    main()
