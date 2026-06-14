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
