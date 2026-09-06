# Democrt[AI]{SE} — TGC Series Index

Canonical index of the TGC series pieces (Month 01 of the candidates calendar — **L0+L1 foundations + 12 pieces**, 3/week).
**Every post in this series links back here.** This index lives in the public build repo so the
series links are publicly reachable (content repos stay private). `LinkedIn URL` is filled in by
**Prabhu after each manual post**; HTA cannot see LinkedIn.

**Series:** We are building tgc as part of the **Democrt[AI]{SE}** series — reliving the joy of
building classic software with a modest LLM.

| # | Piece | Status | Code / files | LinkedIn URL |
|---|-------|--------|--------------|--------------|
| L0 | Series introduction — "the safety net C never had" | ✍️ proposal pending | [repo root](https://github.com/prabhu-david-ns/tgc-build) | _fill after posting_ |
| L1 | What is garbage collection? (taxonomy) | ✍️ proposal pending | [repo root](https://github.com/prabhu-david-ns/tgc-build) | _fill after posting_ |
| 02 | Refcounting cannot free cycles (rewritten C-first) | ✍️ proposal pending | [prep/refcount-cycles.py](https://github.com/prabhu-david-ns/tgc-build/blob/main/prep/refcount-cycles.py) | _fill after posting_ |
| 03 | Mark and sweep on a toy heap (C-bridged) | ✍️ draft — video pending | [prep/mark-sweep-sim.py](https://github.com/prabhu-david-ns/tgc-build/blob/main/prep/mark-sweep-sim.py) | _fill after posting_ |
| 04 | Conservative collection & false positives (C-bridged) | ✍️ draft — video pending | [prep/conservative-scan.py](https://github.com/prabhu-david-ns/tgc-build/blob/main/prep/conservative-scan.py) | _fill after posting_ |
| 05 | Out-of-band metadata table | 📝 draft — video pending | [prep/out-of-band-metadata.py](https://github.com/prabhu-david-ns/tgc-build/blob/main/prep/out-of-band-metadata.py) | _fill after posting_ |
| 06 | Stack scanning — roots anchor | 📝 draft — video pending | [prep/stack-scanning.py](https://github.com/prabhu-david-ns/tgc-build/blob/main/prep/stack-scanning.py) | _fill after posting_ |
| 07 | setjmp / register-spill trick | 📝 draft — video pending | [tiny_gc.c](https://github.com/prabhu-david-ns/tgc-build/blob/main/tiny_gc.c) | _fill after posting_ |
| 08 | Range-filtered word scanning | 📝 draft — video pending | [prep/conservative-scan.py](https://github.com/prabhu-david-ns/tgc-build/blob/main/prep/conservative-scan.py) | _fill after posting_ |
| 09 | Leaf flags — when not to scan | 📝 draft — video pending | [tiny_gc.c](https://github.com/prabhu-david-ns/tgc-build/blob/main/tiny_gc.c) | _fill after posting_ |
| 10 | Trigger policies: pause/resume | 📝 draft — video pending | [prep/trigger-policy.py](https://github.com/prabhu-david-ns/tgc-build/blob/main/prep/trigger-policy.py) | _fill after posting_ |
| 11 | Destructors & sweep ordering | 📝 draft — video pending | [prep/destructors-weakrefs.py](https://github.com/prabhu-david-ns/tgc-build/blob/main/prep/destructors-weakrefs.py) | _fill after posting_ |
| 12 | realloc/free bookkeeping | 📝 draft — video pending | [tiny_gc.c](https://github.com/prabhu-david-ns/tgc-build/blob/main/tiny_gc.c) | _fill after posting_ |
| 13 | Finale — recap month 01 | 📝 draft — video pending | [repo root](https://github.com/prabhu-david-ns/tgc-build) | _fill after posting_ |

**Maintenance rule:** HTA creates/keeps this index current in the drafts PR; Prabhu fills the
`LinkedIn URL` column after each manual post. New posts reference the previous piece by name +
`<<prev-url>>` marker, and carry GitHub file links for everything they discuss.

**Series history:** renumbered 00-13 on 2026-09-06 (L0=00, L1=01, old 01-12 → 02-13) so the index
reads linearly (kilo L0-L3 → V4+ pattern). Candidates' original numbering lives in
`CANDIDATES/01_tgc/content/drafts/`.