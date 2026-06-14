# M14 Findings — Cuda SR Interrupt Delivery Failure

> **CLOSED / banked RE — superseded by Operation NewSheep** (`docs/planning/newsheep/README.md`).
> The VERDICT below (the `[ALARM]` stall is a Cuda device-model IFR/IER bug, NOT a model-rejection
> gate) is still the canonical characterization of the expected post-NewSheep wall — cite it, not the
> retracted "model-rejection gate" framing. The branch's main aim is now the Trampoline producer.

> **VERDICT (2026-06-14):** The [ALARM] boot stall on `machine newworld` is NOT a
> model-rejection / Gestalt / System-file gate. It is a **Cuda device model bug** —
> `sr_int_pending` is never delivered to VIA IFR because `CudaSettle()` only runs on
> IFR reads, and the nanokernel polls IER instead (65,539 IER reads vs 2 IFR reads).
> The stall is ROM-independent, disk-independent, and profile-specific (`newworld` only).

---

## §1 — The wrong framing (inherited from M13)

M13's close-out hypothesized a "model-rejection / pre-System gate" — a Gestalt or
machine-ID check in the ROM that rejects the ROM/OS combo. The reasoning: the boot parks
~15s in, jDR freezes ~10s in, and the saved 68k PC at the autovector seam was `0x50034cae`.
SYSTEM-BOOT-GATES.md documents exactly this kind of gate for Mac OS 9.x System files.

**This framing was wrong.** The evidence below proves the stall is in the Cuda protocol
handshake, not a software gate.

## §2 — Cross-version evidence

Three newworld boots with different ROM/disk combos all stall identically:

| # | ROM | Disk | Profile | jDR count | Cuda packets | Result |
|---|-----|------|---------|-----------|-------------|--------|
| 1 | 9.0.1 | none | newworld | 2,862,996 | 0 | [ALARM] |
| 2 | 1.1 (1998) | none | newworld | 1,913,651 | 0 | [ALARM] |
| 3 | 9.0.1 | 8.6 disk | newworld | 2,745,406 | 0 | [ALARM] |
| 4 | 1.1 (1998) | 8.6 disk | paravirtual | — | — | boots to Finder |

- Runs 1–3: variable is `machine newworld`, not ROM or disk. Zero Cuda packets in all three.
- Run 4: same 1.1 ROM + same disk boots to Finder under paravirtual (ROM patches active,
  `cuda_init` HLE'd).

**Conclusion:** the stall is `machine newworld`-specific. ROM version and disk presence
are irrelevant.

## §3 — Root cause chain

1. **Newworld retires ROM patches.** The newworld profile marks `cuda_init` and `adb_init`
   ROM patches as retired (`[M3b] cuda_init ROM patch retired`). Guest code runs real Cuda
   init against the `dev_cuda` device model.

2. **Guest performs Cuda sync attention.** The guest's Cuda init writes ORB values
   `0x38→0x28→0x30→0x38` — a standard Cuda attention/sync handshake (TIP assert → TACK
   toggle → TIP negate cycle, active-LOW on ORB bits 5/4/3).

3. **Cuda model latches `sr_int_pending=1`.** During the attention handshake,
   `CudaORBWritten()` sets `sr_int_pending=1` to signal that a Cuda SR interrupt should
   be delivered to the VIA.

4. **`CudaSettle()` is the ONLY delivery surface.** The Cuda→VIA IFR delivery path is:
   ```
   CudaSettle() → returns IFR_SR if sr_int_pending → cuda_apply() sets ifr_latched |= IFR_SR
   ```
   `CudaSettle()` is called from exactly ONE site: `dev_via6522.cpp:301`, inside the
   `case R_IFR:` read handler.

5. **The NK polls IER, not IFR.** VIA register access stats from run 1:
   - `R_ORB`: 3,341 reads
   - `R_SR`: 3
   - `R_ACR`: 2
   - `R_IFR`: **2**
   - `R_IER`: **65,539**

   The NK's interrupt-polling loop reads IER (the *enable* register) to check which
   interrupts are enabled, but almost never reads IFR (the *flag* register where pending
   interrupts appear). With only 2 IFR reads in the entire boot, `CudaSettle()` runs
   at most twice — not enough to catch the `sr_int_pending` set during the attention
   handshake.

6. **Guest never sees Cuda acknowledgment.** Without `IFR_SR` being set, the guest's
   Cuda init routine never sees the interrupt that signals the device responded. It
   gives up after its timeout.

7. **68k init stalls → jDR freezes → NK spins.** The failed Cuda init causes downstream
   68k initialization to stall (likely ADB/input device init depends on Cuda). The DR's
   68k block counter freezes, and the NK spins indefinitely on its scheduling loop.

## §4 — Keystone smoke tests

Five throwaway hacks tested iteratively to validate the causal chain. Each killed a
hypothesis and narrowed the real problem.

### Smoke A: CudaSettle on IER reads

Added `cuda_apply(v, CudaSettle(v->cuda))` to the `R_IER` read case.

| | Baseline | Smoke A |
|---|---|---|
| jDR | 2.1M | 2.1M |
| IFR reads | 2 | 2 |
| Cuda packets | 0 | 0 |

**No change.** CudaSettle sets `ifr_latched |= IFR_SR`, but the guest reads IFR only
2 times total (both before the IER write that enables SR). The bit is latched but
never discovered via IFR polling at this stage.

### Smoke B: Eager ORB-edge delivery

`CudaORBWritten()` returns `take_pending(c)` directly (IFR_SR set at ORB write edge).

| | Baseline | Smoke B |
|---|---|---|
| jDR | 2.1M (frozen ~10s) | **1,261M** (running 124M/s) |
| jNK | 1,048M | **102K** |
| IFR reads | 2 | **15,002** |
| Cuda packets | 0 | 0 |
| [ALARM] | Yes | Yes |

**Profile change but [ALARM] persists.** Eager delivery unblocked the NK→68k
transition (jDR 600×↑). IFR reads jumped to 15K — the guest DOES read IFR once the
protocol advances past the sync stage. But `packets=0` — the CV-10 timing constraint
is violated (IFR_SR delivered before the guest's SR read), breaking the protocol.

### Smoke C: Countdown delay (3 VIA accesses)

`settle_countdown=3` in CudaDevice; ticked on every VIA read/write; delivers on zero.

**Result:** Delivered with `ier=0x00` and `ifr_latched=0x04`. SIGSEGV at `0x55590000`
(unrelated — unmodeled MacIO sub-block `0xf3018000`). The countdown fires too early
(3 VIA accesses ≈ before the guest's VIA init), so IER hasn't enabled SR interrupts yet.

### Smoke D: SR-read-triggered delivery

`CudaSRRead()` returns `CLEAR|RAISE` when `sr_int_pending` is set (event-based:
deliver after the guest reads the data byte from SR).

| | Baseline | Smoke D |
|---|---|---|
| IFR reads | 2 | 2 |
| ifr_latched at IFR read | 0x04 | **0x04** (SR-read delivery worked) |
| ier at IFR read | 0x00 | **0x00** |
| Cuda packets | 0 | 0 |

**IFR_SR IS latched** — the SR-read delivery works. But `ier=0x00` at both IFR reads:
the guest hasn't enabled SR interrupts yet. Even with IFR.2 set, the guest ignores it
(bit 7 master flag = 0 when no enabled interrupt is pending).

### Smoke D + IER write trace

Added `fprintf` to the IER write case. The guest's IER write sequence:

```
[SMOKE-IER] write 0x7f -> ier=0x00   (clear all — BEFORE the 2 IFR reads)
[SMOKE-IFR] read #1, ifr=0x04 ier=0x00  (IFR_SR set but SR not enabled)
[SMOKE-IFR] read #2, ifr=0x04 ier=0x00  (same)
[SMOKE-IER] write 0x7f -> ier=0x00   (clear all again)
[SMOKE-IER] write 0xa0 -> ier=0x20   (enable T2)
[SMOKE-IER] write 0x20 -> ier=0x00   (clear T2)
[SMOKE-IER] write 0x84 -> ier=0x04, ifr_latched=0x20  (enable SR — but IFR_SR is GONE)
```

**The smoking gun.** The guest enables IER.2 (SR interrupt) AFTER:
1. The 2 IFR reads (which saw IFR.2 but ignored it because ier=0x00)
2. An SR read that cleared `ifr_latched` bit 2

By the time `ier=0x04`, `ifr_latched=0x20` (T2 only) — IFR_SR was consumed and cleared.
The consume-once `sr_int_pending` model has no way to re-assert it.

### Smoke test conclusions

1. **The lazy-delivery structural diagnosis (§3) is CONFIRMED.** Smoke B proves it:
   eager delivery radically changes the execution profile (jDR 600×↑, IFR 2→15K).
2. **IFR-reading is state/timing-dependent, not "never."** Smoke B's 15K IFR reads
   show the guest polls IFR heavily once the protocol advances. The 2-read baseline
   reflects the protocol being stuck, not a polling design choice.
3. **The deeper bug is the consume-once model.** The guest's init sequence is:
   clear IER → Cuda attention → read IFR (ier=0 → ignore) → read SR (clears IFR.2) →
   enable IER.2 → wait for IFR.2. The consume-once `sr_int_pending` is spent before
   IER enables SR. On real hardware, the Cuda interrupt line is **level-triggered** —
   it stays asserted as long as TREQ is asserted (data pending), so IFR.2 re-latches
   immediately after any clear (SR read) as long as the condition persists.
4. **VIA `irq_fn` is NULL** (IRQ output unwired). Whether this matters depends on
   whether IFR polling suffices once delivery is level-correct. Not yet determined.

### Smoke E: Level-triggered CudaSettle (RAISE while treq_asserted) + IER-write settle

`CudaSettle` returns RAISE whenever `treq_asserted` (not consume-once); called on IER
writes too so enabling SR immediately latches IFR.2.

| | Baseline | Smoke E |
|---|---|---|
| jDR | 2.1M | **1,348M** (same as Smoke B profile) |
| IFR reads | 2 | **15,002** |
| Cuda packets | 0 | 0 |

**Same profile as Smoke B — still packets=0.** The IFR trace showed: `treq_asserted`
goes from 1 to 0 DURING the attention sync (ORB write `0x30` negates TREQ), so by the
time the guest enables IER.2, `treq_asserted=0` and CudaSettle returns NONE. The
attention TREQ is a PULSE, not a sustained level — `treq_asserted` is the wrong
predicate for level-triggered assertion.

### Smoke F: Non-consuming CudaSettle (sr_int_pending persists through IFR reads) + IER settle

`CudaSettle` returns RAISE while `sr_int_pending` without consuming it. `CudaSRRead`
consumes `sr_int_pending` (the acknowledgment point). Called on IER writes too.

| | Baseline | Smoke F |
|---|---|---|
| jDR | 2.1M | **1,348M** (same profile) |
| Cuda packets | 0 | 0 |

**Still packets=0.** The non-consuming `sr_int_pending` is consumed by the SR read
(which happens before the IER write). Same ordering problem as Smoke D.

### Smoke G: Persistent sr_int_pending (NOT cleared by SR reads, only by ORB edges)

`sr_int_pending` persists through SR reads — only new ORB edges overwrite it.
Non-consuming CudaSettle + IER-write settle.

| | Baseline | Smoke G |
|---|---|---|
| jDR | 2.1M | 2.1M (**BACK TO BASELINE**) |
| IFR reads | 2 | 2 |
| Cuda packets | 0 | 0 |

**Regression to baseline.** With `sr_int_pending` persistent through SR reads,
CudaSettle keeps re-latching IFR_SR after every SR read → the guest's SR-read cycle
loops forever (the CV-10 `0x9584` sync park). This confirms the CV-10 constraint: IFR_SR
MUST be cleared after the SR read and NOT immediately re-asserted, or the guest parks.

### Smoke test conclusions (revised after 8 tests)

1. **The lazy-delivery structural diagnosis (§3) is CONFIRMED.** Smoke B proves it:
   eager delivery radically changes the execution profile (jDR 600×↑, IFR 2→15K).
2. **IFR-reading is state/timing-dependent, not "never."** Smoke B's 15K IFR reads
   show the guest polls IFR heavily once the protocol advances. The 2-read baseline
   reflects the protocol being stuck, not a polling design choice.
3. **The bug is a timing model problem, not a simple model choice.** All three classes
   of delivery fail:
   - **Too early** (Smoke B, G): IFR_SR before/during SR read → CV-10 sync park
   - **Too late** (baseline, Smoke A): IFR_SR never arrives because IFR barely polled
   - **Right time but wrong IER state** (Smoke D, F): IFR_SR delivered after SR read
     but ier=0x00; consumed/cleared before IER enables SR
4. **TREQ is a pulse, not a level** during attention sync — `treq_asserted` is the
   wrong predicate for level-triggered assertion (Smoke E).
5. **The fix needs a TIMER** — the only way to deliver IFR_SR at the right moment
   (after the guest's SR read AND after IER enables SR) is a real time delay, matching
   QEMU's `cuda_delay_set_sr_int` with `sr_delay_ns = 20000` (20µs). The EventScheduler
   already exists and is wired to the VIA. The delay must be long enough for the guest
   to: (a) read SR, (b) enable IER.2, (c) start polling IFR.
6. **VIA `irq_fn` is NULL** (IRQ output unwired). Whether this matters depends on
   whether IFR polling suffices once delivery timing is correct. Defer until the timer
   smoke test resolves it.

## §4a — Fix direction (revised after 8 smoke tests)

The fix is **timer-delayed Cuda SR interrupt delivery** — the QEMU
`cuda_delay_set_sr_int` model. The EventScheduler is already bound to the VIA
(`VIABindScheduler`); the Cuda model needs a similar binding to schedule a one-shot
that fires ~20µs after each `sr_int_pending` is set, delivering IFR_SR at that point.

Three elements:
1. **Cuda→VIA callback or EventScheduler binding:** the Cuda model needs to schedule a
   one-shot timer. Either bind the EventScheduler directly, or use a VIA back-pointer
   so the Cuda can call `cuda_apply` outside the VIA read/write path.
2. **Timer-based delivery:** on `sr_int_pending=1`, schedule delivery ~20µs later (using
   `VIA_CLOCK_HZ = 783360`, that's ~16 VIA ticks). The timer callback sets
   `ifr_latched |= IFR_SR` and calls `via_update_irq`.
3. **VIA IRQ output wiring** — defer; test empirically after the timer works.

**The cheapest smoke test:** use the existing EventScheduler to schedule a one-shot from
`CudaORBWritten` (via a new Cuda→VIA seam), delivering IFR_SR after ~16 VIA ticks.
One boot. If `packets > 0`, the timer model is confirmed correct.

**Gate:** run the timer smoke test before building the proper gated implementation.

### Smoke H — timer-delayed delivery with IER-wait reschedule (2026-06-14)

**Hack:** `CudaBindTimerDelivery` wired CudaDevice → EventScheduler + VIA back-pointer.
Each `sr_int_pending=1` scheduled a one-shot ~20µs later. Callback ran under the bus
lock via `locked_call`; if `ier & 0x04` (IER.SR) was not yet enabled, reschedule for
another 20µs (loop until IER.SR on, then latch `ifr_latched |= IFR_SR` + `via_update_irq`).
Added `VIALatchIFRBits(VIA6522*, uint8_t)` to expose the latch+irq_update from outside
the VIA read/write path.

**Result:**
- `sr_timer=1` (one successful delivery), `packets=0`
- Timer DID fire correctly: `ifr=0x20 ier=0x04 irq_out=0` → `ifr=0x24 irq_out=1`
  (IFR_SR latched, IRQ summary transitioned 0→1)
- `nw_via_irq_edge` fired → PIC edge → NK EXT delivery:
  `[EXC] EXT delivered #1: restart=504dfe40 srr1=00009040 msr=00001040 -> entry=50314880`
- `host-irq: edges=1 consumed=1` — the full pipeline worked mechanically
- But `irq_fired=0`, `packets=0` — the NK EXT handler (`0x50314880`) dispatched
  but the Cuda protocol never advanced

**Diagnosis (CORRECTED + watchpoint evidence, 2026-06-14):**

Timer delivery is mechanically correct at the VIA layer (IFR_SR latched, irq_out 0→1,
PIC edge). But the ed0a "8/8" result was misleading — `SS_PROBE_68K` has a default cap
of 8 (edge-triggered, linear). Uncapped (`:64`), baseline+PIC saturates at **64/64 in
30s** from DEC autovector alone. The count is identical with or without Smoke H, so the
ed0a entries are all DEC — **Cuda EXT does NOT add handler entries at ed0a**.

This is confirmed by M13's static analysis of the NK fallback handler at `0x50325f00`:
it reads the PIC source, clears the pending bit, and returns to PPC — it **never sets
`cr2lt`** to trigger the DR's exception/autovector path. So the Cuda EXT is consumed
by the NK and never forwarded to the 68k world. This is a **per-source dispatch gap**
(distinct from the retracted M13 "registered-handler table" thesis — the handler runs,
it just doesn't forward EXT sources to the DR).

Additionally, watchpoint evidence confirms the **interrupt-source struct is never
populated**:
- `*(0x68ffefd0)` settles to pointer `0x68ff4f00` (briefly `0x68ffef00` during NK init)
- Both `0x68ff4f00` and `0x68ffef00` are **all zeros** for the entire 30s boot (watched
  at `+0x00`, `+0x04`, `+0x28` — zero at 1B+ observations, zero `[WATCH]` change events)
- Nobody writes to this struct: not the NK, not 68k ROM code, not any device model

**Verdict: PARTIAL SUCCESS.** Timer-delayed delivery is the correct VIA-layer fix.
Two remaining gaps:
1. **NK fallback doesn't forward EXT to DR** — the `0x50325f00` handler consumes the
   PIC source and returns; it never sets cr2lt/takes the DR exception path. The Cuda EXT
   reaches the NK but NOT the 68k world.
2. **Interrupt-source struct is unpopulated** — even if EXT reached ed0a, the struct at
   `0x68ff4f00` is empty so the 68k dispatcher would find nothing to service.

> **This is NOT the retracted M13 thesis.** M13 claimed "handler table not installed /
> handler never runs." The handler at 0x50325f00 DOES run and correctly reads the PIC.
> The gap is behavioral: it clears the source but doesn't forward to the DR. And the
> struct gap is a data-layer problem (who populates it?), not a registration problem.

### Hnfo struct probe at EXT handler entry (direct evidence, 2026-06-14)

`SS_PROBE_PC=0x50314880` (NK EXT handler entry) with `SS_NW_PIC=1`:

| Field | Address | Value | Meaning |
|---|---|---|---|
| hnfo_rec+0x14 | 0x68ff4f14 | **0x00000000** | Source table pointer = NIL |
| hnfo_rec+0x28 | 0x68ff4f28 | **0x00000000** | Pending bits = empty |
| hnfo_rec+0x70 | 0x68ff4f70 | 0x486e666f | `'Hnfo'` tag (set by trampoline) |
| hnfo_rec+0xa8 | 0x68ff4fa8 | **0x00000000** | Source-device table = NIL |

The VIA-IFR-RECON §5c table had `hnfo+0x28 = 0x80000000` from a 2026-06-12 session with
different machinery (M10 CGRP or Task-C HLE, since reverted). Current baseline: zero.

### Reconciliation with M13 stage-2-MISSING diagnosis

The M13 original diagnosis was a 3-stage chain:
1. **NK EXT → WORKS** (0x50314880 reached)
2. **NK→DR handoff → MISSING** (CGRP handler not registered → fallback at 0x50325f00
   consumes the PIC source and returns; never sets cr2lt for the DR)
3. **DR autovector → never fires** (for EXT; DEC fires via the published DEC handler)

The M13 RETRACTION ("ed0a fires 8/8 baseline → delivery works → don't re-chase") was
based on a **capped-probe artifact**: SS_PROBE_68K default cap is 8. Uncapped (`:64`),
baseline saturates at 64/64 in 30s — all DEC autovector ticks. Smoke H adds ZERO ed0a
entries. The DEC path (published handler → DR → cr2lt → 68k frame → [0x64]→ed08) is
healthy. The EXT path through the CGRP fallback does NOT reach the DR.

**The M13 stage-2-MISSING was correct.** The retraction overcorrected. The evidence:
- Uncapped ed0a probe: identical counts baseline vs Smoke-H → Cuda EXT adds zero
- Watchpoints: hnfo+0x28 all zeros → no pending source bits ever set
- Direct EXT-entry probe: hnfo+0x14 (source table) = NIL, hnfo+0x28 = 0
- Static (M13): CGRP+0x20 = 1 (< 2 → "no handler" per NK guard at 0x50325f00)
- M13 static: fallback reads PIC, clears source, returns via rfi — NEVER sets cr2lt

**What the CGRP handler would do** (M13 B.1–B.4 analysis, never retracted):
1. Scan the PIC source (the fallback already does this)
2. Set cr2lt in the DR's PPC state to trigger the between-instruction exception path
3. The DR then builds a genuine 68k frame (vector $64, saved SR/IPL) and vectors [0x64]→ed08
4. The 68k handler at ed08/ed0a reads `hnfo_rec+0x28` to find the pending source

Steps 2–4 require: (a) cr2lt to be set (stage-2), (b) hnfo+0x28 to be populated with the
correct source bit (data-layer). Both are the registered CGRP handler's job.

**Chicken-and-egg status:** The CGRP handler is installed by Mac OS's Interrupt Manager
during boot. The boot stalls at Cuda init (packets=0) BEFORE reaching Interrupt Manager
init. **DEC does NOT signal the DR** (see §7) — the ed0a entries attributed to "DEC
autovector" actually come from HandleInterrupt MODE_EMUL_OP Execute68k, not from the NK
DEC handler at all.

### §4b — IER/IFR timeline (path (b) precondition, 2026-06-14)

Temporary IER/IFR read/write diagnostics in `dev_via6522.cpp` (reverted after capture):

| # | Event | IER | IFR | Meaning |
|---|-------|-----|-----|---------|
| W1 | IER write `0x7F` | `0x00→0x00` | — | Clear all |
| R1-R2 | IFR read | — | **`0x04`** | IFR_SR already set (CudaSettle delivered on first IFR read) |
| W2 | IER write `0x7F` | `0x00→0x00` | — | Clear all again |
| W3 | IER write `0xA0` | `0x00→0x20` | — | SET T1 timer interrupt enable |
| R1-65536 | IER read (65536×) | `0x20` | `0x00→0x20` | T1 poll loop; IFR_T1 fires at ~#32768 |
| W4 | IER write `0x20` | `0x20→0x00` | — | CLEAR T1 |
| W5 | IER write `0x84` | `0x00→0x04` | — | **SET SR interrupt enable** |
| — | (ORB writes: Cuda handshake) | — | — | (no more IFR reads after this point) |

**Findings:**
1. The 65,539 IER reads are a **T1 timer wait loop** — NOT Cuda init polling. The guest
   reads IER (not IFR) 65,536 times while waiting for T1 to fire. This is a ROM timer
   calibration loop that runs BEFORE the Cuda attention handshake starts.

2. **SR is enabled in IER AFTER the T1 loop** (write #5: IER |= `0x04`). Then the guest
   starts the Cuda attention handshake (ORB writes). The guest expects an
   **interrupt-driven** SR response, not a polled one — **there are NO IFR reads after
   write #5**. The guest relies on the autovector interrupt chain (CGRP → DR cr2lt →
   68k handler → IFR read) to service the Cuda SR int.

3. **CudaSettle delivers on the first IFR read** (IFR reads #1-#2 show `0x04`), confirming
   the settle-on-IFR-read mechanism works. But this early delivery is consumed before the
   guest enables SR in IER and starts the Cuda handshake — it's a pre-init read, not part
   of the protocol.

**Verdict on path (b): RED HERRING.** The guest does not poll IFR for Cuda responses.
It relies on interrupt-driven delivery. Path (b) cannot break the chicken-and-egg.

**Status: PARKED (2026-06-14).** All viable paths collapse to the same crash-prone
forge class (seed KDP+0x674 + hnfo source tables from host). See §7 for the complete
analysis and the precise resume experiment.

## §5 — Bug found during investigation

### `SS_NW_TRAMPOLINE=0` still activates newworld

`machine_profile.cpp:80`:
```cpp
if (getenv("SS_NW_TRAMPOLINE") != NULL)  // checks existence, not value
```
Setting `SS_NW_TRAMPOLINE=0` still activates the newworld profile. Must use
`SS_MACHINE=paravirtual` to override. (This fully explains why early attempts to disable
newworld via `--env 'SS_NW_TRAMPOLINE=0'` through the slot wrapper failed — the wrapper's
env override mechanism itself is correct; macOS is last-wins for duplicate env keys.)

## §6 — Methodology notes

- **r24 ring interpretation:** `SS_DR_R24_RING=1` records `gpr(24)` at EVERY JIT block
  entry (ppc-cpu.cpp:2370), not just DR blocks. Tail entries during NK spin are stale r24
  values from the last DR exit, not genuine 68k PC visits. Must cross-reference with
  actual guest memory contents to distinguish.
- **VIA register access counts** (IFR=2, IER=65,539) come from the built-in
  `reg_reads[16]` per-register histogram in `dev_via6522.cpp` (M6a Wave 2 #4), emitted
  via `VIAFormatReadHistogram` in the `[VIA] reads:` diagnostic line. They are
  **non-load-bearing** for the conclusion — the structural code fact (CudaSettle called
  only on R_IFR reads, `dev_via6522.cpp:301`) carries it independently — but they are
  reproducible built-in counters, not throwaway instrumentation.
- **Paravirtual control boot required `SS_MACHINE=paravirtual`** due to the Bug 1
  existence check — `SS_NW_TRAMPOLINE=0` still activates newworld.
- **Ring analysis:** use `tools/ring-walk.py` for r24 ring analysis (the prescribed tool
  per LEARNINGS ring-confirm rule).

## §7 — DEC handler RE + ed0a misattribution + collapse to forge (2026-06-14)

### DEC handler does NOT signal the DR

The NK DEC published handler at `0x50313200` was fully disassembled. Its flow:
1. Save prologue (`bl 0x50313d40`) — sets `r7 = [KDP-0x10]` (NK context flags)
2. Timer body (`bl 0x50322eac`) — services NK timer queue, updates DEC
3. Return via `0x50312cb0` — checks `r7 & 0x30`; if set, dispatches to DR via `0x50312ab4`
4. Fast return via `0x503242a8` — restores CR fully (`mtcrf 0xff, r13`), rfi

**Probe result:** `r7 = 0x00a80000` at `0x50312cb0` → `r7 & 0x30 = 0` → the DR dispatch
path at `0x50312ab4` never fires. Confirmed: zero probe hits at `0x50312ab4`. The DEC
handler services timers and returns without signaling the DR.

### ed0a entries come from HandleInterrupt MODE_EMUL_OP

The 64/64 ed0a entries attributed to "DEC autovector" actually come from
`HandleInterrupt()` in `MODE_EMUL_OP` (sheepshaver_glue.cpp:3503–3522). When a VBL timer
fires during an EMUL_OP callback (host-side trap), `Execute68k()` runs a 68k proc that
jumps to vector `$64` (the level-1 autovector). These are synchronous nested 68k
executions, not NK interrupt dispatches.

Key evidence:
- **KDP+0x674 (CR mask) = 0** on NewWorld (probed at `0x68fff674`). The OldWorld CR
  injection mechanism (`r->cr |= cr_mask`) is not just fenced — it's empty.
- **HandleInterrupt MODE_68K** for NewWorld only bumps Ticks (line 3453). CR injection
  is fenced (line 3441: `if (!MachineProfileIsNewWorld())`). No cr2lt, no autovector.
- **DEC handler restores CR fully** (`mtcrf 0xff, r13` at `0x503244dc`) — no DR signaling.
- **MODE_EMUL_OP fires** during host trap callbacks (GetResource, InitGraf, etc.),
  producing the ed0a entries. MODE_EMUL_OP is safe for Execute68k (saves/restores all
  PPC state); MODE_68K is NOT (clobbers XLM_RUN_MODE).

### Guest is NOT in a tight wait loop

r24 ring (`SS_DR_R24_RING=1`, 30s diagnostic boot): 1,507,519 transitions, ending at
normal 68k initialization activity — vector table setup (`0x5000e0ee` loop), InsTime
calls (`0x50066e24: dc.w $a024`), A-line handler entry (`0x5000dfa2`). The DR is actively
running 68k init code when SIGTERM fires, not stuck in a spin loop.

The Cuda stall manifests as `syncs=1, packets=0` — the attention handshake started but
the SR response was never processed by the 68k Cuda interrupt handler. Other init code
continues running while Cuda-dependent paths (ADB, input) never complete.

### All paths collapse to the forge class

| Path | Mechanism | Why it fails |
|------|-----------|-------------|
| (c') Fix XLM_RUN_MODE + Execute68k($64) | Make Execute68k safe from MODE_68K | Handler reads hnfo+0x28=0, +0x14=NIL → no-ops |
| (b') Seed KDP+0x674 CR mask | Un-fence MODE_68K injection | KDP+0x674=0 (never initialized); seeding it = forge |
| (a') Host-side Cuda HLE | Bypass NK→DR chain | Guest is interrupt-driven (zero IFR reads after SR enable); no poll fallback to satisfy |
| Un-retire cuda_init patch | NOP the 68k Cuda init | Pattern misses 9.0.1 ROM (0x9be2 outside 0xa000..0x12000) |

Every path that could make the interrupt fire requires seeding the uninitialized NK
routing infrastructure from the host: KDP+0x674 (CR mask), hnfo+0x28 (pending bits),
hnfo+0x14 (source table). This is the M10-CGRP forge class (0xDEADBEEF crash).

### PARKED — resume experiment (if NewWorld 9.x becomes a committed target)

The forge is now well-informed (unlike the blind M10 attempt). The precise smoke test:

1. **Seed three locations at NW-trampoline-end** (via `SS_SEED_MEM` or host code):
   - `KDP+0x674` (0x68fff674) = cr2lt bit pattern (the CR mask HandleInterrupt injects)
   - `hnfo+0x28` (0x68ff4f28) = pending source bit for Cuda/VIA
   - `hnfo+0x14` (0x68ff4f14) = pointer to a minimal source handler table
2. **Un-fence MODE_68K CR injection** for NewWorld (remove the
   `if (!MachineProfileIsNewWorld())` guard at sheepshaver_glue.cpp:3441)
3. **Boot with Smoke-H timer delivery** (the proven VIA-layer fix)
4. **Gate:** `packets > 0` AND no 0xDEADBEEF crash
5. **If step 4 fails:** the forge values are wrong. RE the hnfo source table format
   from a working paravirtual boot (where IM init runs) to get correct values.

This is genuinely different from M10's blind forge — it targets three precisely-located
addresses with a clear causal chain, and the VIA-layer delivery (Smoke H) is already
proven correct. But it's still forge-class (writing NK data structures the host doesn't
own), so the crash risk is real.
