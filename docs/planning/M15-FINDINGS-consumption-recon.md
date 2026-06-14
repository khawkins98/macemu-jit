# M15 — Consumption-Recon Findings

**Purpose:** This doc records the bounded recon (M15) that decides whether the existing
`SS_NW_IRQ_CONSUME` consumption path is one `SC#1=0x0d` divergence short of completing the
guest's Interrupt Manager (IM) init (→ "real" fix) or needs a host-side forge (→ "forge"
fallback). It is the findings record for spec
`docs/superpowers/specs/2026-06-14-m15-consumption-recon-design.md` and plan
`docs/superpowers/plans/2026-06-14-m15-consumption-recon.md`. **Task 0** below pins the two
load-bearing facts (the LIVE `hnfo` base and the consumption-path waypoint PCs) that every
later watch/probe depends on, so they are resolved live rather than trusted from prior
sessions.

---

## Task 0 — blocking answers

| Fact | Value | Source / note |
|---|---|---|
| **Live `hnfo` base** | `0x68ff4f00` | `[NW-TRAMP]` log: word at `[KDP+0xfd0]` (= `0x68ffefd0`) re-asserted guest-side to `0x68ff4f00`; the `'Hnfo'` record (id=0x3035) is allocated `@68ff4f00`. **MATCHES** the prior-session `0x68ff4f00`. (No transient `0x68ffef00` observed in this run — the trampoline sets it to the settled value directly.) |
| watch target `hnfo+0x00` | `68ff4f00` | absolute hex, no `0x` prefix (watch-addr format) |
| watch target `hnfo+0x14` (source table) | `68ff4f14` | " |
| watch target `hnfo+0x28` (pending bits) | `68ff4f28` | " |
| `KDP+0x674` (CR mask) | `68fff674` | static (KDP base `0x68ffe000` + `0x674`) |
| **post latch / target** | `0x68fff070` | NK writes `0x8001` here; `KDP+0x67c` is NK-set to `0x68fff070` at cold-init (`[NW-TRAMP]` log) |
| post-leg deferred-pair **staging stub** (Q-C3) | `ROM+0x2fd280` = guest `0x5032d280` | `IRQ_POST_PATCH_SPACE` (`rom_patches.cpp:71`), gated on `SS_NW_IRQ_CONSUME`, checked at the site |
| **drain** (world-restore) | `0x50324720` | deferred-post pair `[KDP-0x43c]/[KDP-0x440]` drained here (M8 carried fact) |
| **slot-4 `twi` PC** (DR entry-vector "Interrupt") | `0x5046e8d0` | M8 / INTERRUPT-INJECTION-RECON; the DR's slot-4 entry-vector twi |
| NK slot-4 service | `0x50314660` | `mtlr [KDP+0x5b0]; blr` (STATIC, 3 insns) |
| **68k level-1 handler entry** | `0x5000ec50` | level-1 @0xec50 → `via_int` @0xef2c; consumption probes `0x5000ec52` / `0x5000ef22` / `0x5000bbca` |
| NK EXT handler | `0x50314880` | `[KDP+0x374]` |
| NK syscall entry | `0x50314ac0` | `[KDP+0x390]` |
| NK CGRP fallback (consumes PIC, never sets cr2lt) | `0x50325f00` | M13/M14 finding |

### Env incantation that arms the consumption path

```
SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1
```

- `SS_NW_IRQ_CONSUME` (default OFF) arms the consumption machinery; `SS_NW_PIC` (default OFF,
  flip HELD) is **required** to actually deliver EXT to the chain — default boots (no
  `SS_NW_PIC`) deliver EXT at level 0 (a no-op post), so the consumption chain is unreachable
  by design without it. This is the M8 acceptance cluster.
- **Smoke-H is NOT a standing env flag.** Verified from source: there is no `SS_*` gate for
  it (the only `SS_NW_*` flags in `machine/` + `main_unix.cpp` are `SS_NW_HOST_IRQ`,
  `SS_NW_IRQ_CONSUME`, `SS_NW_PIC`, `SS_NW_PIC_FORCE`, `SS_NW_TRAMPOLINE`). "Smoke-H" was an
  **M14 experimental hack** (`CudaBindTimerDelivery` + `VIALatchIFRBits`, see
  `docs/planning/M14-FINDINGS-cuda-delivery.md` §"Smoke H") that is **not present in the
  current tree** (`grep CudaBindTimerDelivery` → no hits). So do not add a Smoke-H gate to
  the consumption-path boots; if timer-driven delivery is needed it must be re-introduced as
  code, not toggled. The standing delivery rail under `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1` is the
  host-IRQ → PIC edge → NK EXT path (confirmed firing below).

### Boot budget note

≤~6 boots total for M15. Used 4 in Task 0 (see below). **Stop-when** = stall pinned AND the
struct-write question answered.

### Verbatim evidence

Live `hnfo` base (boot `m15-hnfo-pic`, `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`):

```
[NW-TRAMP] W2 vector stop stubs (bra.s *): illegal[0x10]=0x50429c00 aline[0x28]=0x50429c10 fline[0x2c]=0x50429c20; [KDP+0xfd0]=0x68ff4f00 ('Hnfo') re-asserted guest-side; [KDP+0x67c] left for NK cold-init (NK sets=0x68fff070)
[NW-TRAMP] W2 'Hnfo' record @68ff4f00 ([KDP+0xfd0]), id=0x3035, scratch=68ff5000 (machine-detect data path, memo §2 Option A)
```

Delivery rail confirmed firing (same boot):

```
[EXC] SC delivered #1: r0=0000003f r1=103ffb50 lr=500cf108 -> entry=50314ac0
[EXC] EXT delivered #1: restart=50318018 srr1=00009040 msr=00001040 -> entry=50314880
```

(Note the known `SC#1` divergence: under `SS_NW_IRQ_CONSUME=1` **without** `SS_NW_PIC`, the
boot delivers `SC delivered #1: r0=0000000d` and then traps at `pc=0x5046e324` — the
`SC#1=0x0d` shape this milestone must adjudicate. With `SS_NW_PIC=1` the first SC is
`r0=0x3f`.)

### Instrument caveat discovered (Task 0)

`SS_PROBE_PC` produced **zero** `[PROBE …]` output at the NK handler addresses
(`0x50314880`, `0x50314ac0`) across all boots, even though `EXT delivered #1 -> entry=50314880`
and `SC delivered #1 -> entry=50314ac0` both fired. These NK handlers are reached via the
**exception/syscall vector dispatch**, which bypasses the JIT block-entry probe hook that
`SS_PROBE_PC` lives in. **Implication for later tasks:** do not rely on `SS_PROBE_PC` at
exception-vector entry PCs; use the `[EXC] … delivered` log lines (always-on) for delivery
evidence and `SS_JIT_WATCH_ADDR` (needs `SS_JIT_TRACE_RING=1`) for the `hnfo`/post struct
writes. The live `hnfo` base is taken from the always-on `[NW-TRAMP]` trampoline log, which is
authoritative for `[KDP+0xfd0]`.

---

## Boot A — consumption-path stall point

**RUNDIR:** `/tmp/ss-slots/slot0/runs/20260614-145702.43261`
(arm: `SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_DR_R24_RING=1 SS_TERM_DUMP=1`, timeout 45s,
exit 133 / SIGTRAP — non-deterministic, NOT a regression per M15 ground rules).

### ⚠️ Wrapper env-split correction (affects every M15 boot)
`ss-slot-boot.sh --env` splits its argument on **whitespace**, not on `;` (line 70:
`read -r -a _kv <<<"$2"`; help text line 15 says "space-separated"). A semicolon-joined
`--env 'SS_NW_PIC=1;SS_NW_IRQ_CONSUME=1;...'` is taken as a **single** token: it sets
`SS_NW_PIC` to the garbage value `1;SS_NW_IRQ_CONSUME=1;...` (still truthy → PIC arms) and
**never sets** `SS_NW_IRQ_CONSUME` or `SS_DR_R24_RING`. The first attempt this way printed
`[NW-PROG config] PIC=1 CONSUME=0` and produced no ring. **Use space-separated `--env` for
all remaining M15 boots.** The numbers below are from the corrected (space-separated) boot.

### Delivery evidence (durable `[EXC]` / `[IRQ-CONSUME]` log lines, verbatim)
```
[EXC] EXT pending ASSERTED (host-irq latch, edge #1)
[PIC-HOST] lowmem level byte [0x3f3f]=1 survived to edge #1 (trampoline staging intact)
[EXC] EXT pending ASSERTED (edge #1)
[IRQ-CONSUME] EE edge deferred at pc=50318014 (stub 50318000-5031801c)
[IRQ-CONSUME] deferred edge fired at pc=504a8608 (held=0)
[EXC] EXT delivered #1: restart=504a8608 srr1=00009040 msr=00001040 -> entry=50314880
[EXC] EXT pending deasserted (edge #2)
[EXC] PROGRAM delivered #5: srr0=5046e8d0 word=0fff0004 slot=4 r1=17ffe70c lr=5046c4f4 -> entry=50314700
[IRQ-CONSUME] EE edge deferred at pc=50318014 (stub 50318000-5031801c)   (x3)
[IRQ-CONSUME] deferred edge fired at pc=50325fd0 (held=0)                (x3)
[EXC] host-irq: edges=1 consumed=1 deasserts=0 pending=0
```
`[DR68K] first instruction: r24=0x00000000 ppc_block=0x50310000 — 68k DR emulator started`
(68k DR started at PC 0, i.e. cold reset entry only).

### Waypoint reach table
| Waypoint | PC | Reached? | Evidence |
|---|---|---|---|
| post latch | `0x68fff070` | (data addr; n/a in ring) | — |
| EE-defer stub | `0x50318000–1c` | **YES** | `[IRQ-CONSUME] EE edge deferred at pc=50318014` |
| NK EXT handler | `0x50314880` | **YES** | `[EXC] EXT delivered #1 -> entry=50314880` (log; vector-dispatch, not in ring) |
| deferred-edge fire (1st) | `0x504a8608` | **YES** | `[IRQ-CONSUME] deferred edge fired at pc=504a8608` |
| slot-4 `twi` | `0x5046e8d0` | **YES** | `[EXC] PROGRAM delivered #5: srr0=5046e8d0 ... slot=4` |
| FE1F PROGRAM surface | `0x50314700` | **YES** | PROGRAM#5 `-> entry=50314700` |
| deferred-edge fire (post-twi) | `0x50325fd0` (CGRP fallback `0x50325f00` region) | **YES** | `[IRQ-CONSUME] deferred edge fired at pc=50325fd0` (x3) |
| **NK slot-4 service** | `0x50314660` | **NO** | absent from log; never appears |
| **68k L1 handler (via_int 0xef2c)** | `0x5000ec50` | **NO — ring-confirmed** | `--find-pc 5000ec50: 0 hits` (1.13M-entry r24 ring); only early 68k PCs `5000002a/2c/b6` present |

### Pinned stall point
- **LAST waypoint reached:** the slot-4 `twi` at **`0x5046e8d0`** (delivered as PROGRAM#5,
  `slot=4`, log-confirmed) and the NK EXT handler `0x50314880` (EXT delivered #1). After the
  twi, the deferred EXT edges re-fire at **`0x50325fd0`** — inside the **CGRP fallback region
  `0x50325f00`**, NOT the intended NK slot-4 service.
- **FIRST waypoint NOT reached:** the **NK slot-4 service `0x50314660`** — and consequently the
  **68k level-1 handler `0x5000ec50`** (via_int `0xef2c`). The consumed edge is routed into the
  CGRP fallback (`0x50325fd0`) instead of the slot-4 service path, so it never crosses into the
  68k world.
- **68k L1 handler `0x5000ec50` reached? NO** — ring-confirmed (0 hits across 1,130,134 r24-ring
  entries; the only 68k PCs in the ring are the cold-reset `5000002a/2c/b6`, not the L1 handler).
- **`EXT delivered` present? YES** (`#1 -> entry=50314880`). **`host-irq: edges=1 consumed=1
  deasserts=0 pending=0`** — exactly one edge, consumed once at the NK EXT level.
- **`irq_fired`:** no `[NW-PROG irq]` summary line in this run (SIGTRAP/133 did not invoke the
  SS_TERM_DUMP→exit atexit summary), but the ring-confirmed absence of `0x5000ec50` is the
  stronger, direct evidence that the interrupt was **never delivered into the 68k Interrupt
  Manager** (`irq_fired` would be 0).

### Confidence
The "NOT reached" verdict for the 68k L1 handler `0x5000ec50` is **ring-confirmed**
(`--find-pc` over the full 1.13M-entry r24 ring), not probe-absent-only. The NK-side
waypoints that DID fire are **log-confirmed** via always-on `[EXC]`/`[IRQ-CONSUME]` lines
(they are vector-dispatch entries and correctly do not appear in the block-entry ring, per
the Task-0 instrument caveat). Net: the Cuda EXT interrupt is consumed at the NK EXT level
and even drives the slot-4 `twi`, but the deferred edge lands in the CGRP fallback
(`0x50325fd0`) rather than the NK slot-4 service (`0x50314660`), so it never reaches the
68k level-1 handler — the consumption path is **stalled between the slot-4 `twi`
(`0x5046e8d0`) and the NK slot-4 service (`0x50314660`)**.
