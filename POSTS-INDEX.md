# Democrt[AI]{SE} — TGC Series Index

Canonical index of the TGC series pieces — rebuilding a tiny conservative garbage collector in C, 3 posts a week.
**Every post in this series links back here.** This index lives in the public build repo so the
series links are publicly reachable (content repos stay private). `LinkedIn URL` is filled in by
**Prabhu after each manual post**; HTA cannot see LinkedIn.

**Series:** We are building tgc as part of the **Democrt[AI]{SE}** series — reliving the joy of
building classic software with a modest LLM.

Reading order: F0 → F1 → L0 → 02…13 (L1 absorbed by F1, superseded).

| # | Piece | Status | Code / files | LinkedIn URL |
|---|-------|--------|--------------|--------------|
| F0 | Heap vs stack + manual memory management | 🟠 review — draft PR | [repo root](https://github.com/prabhu-david-ns/tgc-build) | _fill after posting_ |
| F1 | Where GC lives + what it is (Java/Go/CPython + taxonomy) | 🟠 review — draft PR | [prep/refcount-cycles.py](https://github.com/prabhu-david-ns/tgc-build/blob/main/prep/refcount-cycles.py) · [.specs/01](https://github.com/prabhu-david-ns/tgc-build/blob/main/.specs/01-prep-gc-concepts.md) | _fill after posting_ |
| L0 | Series introduction — "the safety net C never had" | 🟠 review — draft PR | [repo root](https://github.com/prabhu-david-ns/tgc-build) | _fill after posting_ |
| 02 | Refcounting cannot free cycles | ✅ ready — next batch | [prep/refcount-cycles.py](https://github.com/prabhu-david-ns/tgc-build/blob/main/prep/refcount-cycles.py) | _fill after posting_ |
| 03 | Mark and sweep on a toy heap | ✅ ready — next batch | prep study lands with its week | _fill after posting_ |
| 04 | Conservative collection & false positives | ✅ ready — next batch | prep study lands with its week | _fill after posting_ |
| 05 | Out-of-band metadata table | ✅ ready — next batch | prep study lands with its week | _fill after posting_ |
| 06 | Stack scanning — roots anchor | ✅ ready — next batch | prep study lands with its week | _fill after posting_ |
| 07 | setjmp / register-spill trick | ✅ ready — next batch | mechanics diagram — C core lands in build week | _fill after posting_ |
| 08 | Range-filtered word scanning | ✅ ready — next batch | mechanics diagram — C core lands in build week | _fill after posting_ |
| 09 | Leaf flags — when not to scan | ✅ ready — next batch | mechanics diagram — C core lands in build week | _fill after posting_ |
| 10 | Trigger policies: pause/resume | ✅ ready — next batch | prep study lands with its week | _fill after posting_ |
| 11 | Destructors & sweep ordering | ✅ ready — next batch | prep study lands with its week | _fill after posting_ |
| 12 | realloc/free bookkeeping | ✅ ready — next batch | C core lands in build week | _fill after posting_ |
| 13 | Finale — recap + how close we got | ✅ ready — build week | C core lands in build week | _fill after posting_ |
| L1 | What is garbage collection? (taxonomy) | 🗄️ superseded by F1 — not posted | — | — |

**Maintenance rule:** HTA keeps this index current in the drafts PR; Prabhu fills the
`LinkedIn URL` column after each manual post. New posts reference the previous piece by name +
`<<prev-url>>` marker, and carry GitHub file links for everything they discuss — files land in
this repo with their week's staging PR, before the week's posts go out.
