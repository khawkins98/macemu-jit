# M14 Findings — Cuda SR Interrupt Delivery Failure

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

**Diagnosis:** The timer delivers the interrupt correctly through the full VIA→PIC→NK
pipeline. The block is DOWNSTREAM: the NK's fallback external interrupt handler at
`[KDP+0x5b0]` (0x50325f00) doesn't know how to service VIA/Cuda interrupts and route
them to the 68k interrupt handler that would drive the Cuda byte exchange. This is the
registered-handler table gap (W2L-1 in the sheepshaver_glue comments): the NK has no
registered handler for IRQ source 0x19 (OPENPIC_IRQ_VIA_CUDA).

**Verdict: PARTIAL SUCCESS.** Timer-delayed delivery is the correct VIA-layer fix —
it delivers IFR_SR at the right time (after IER.SR enables). But packets=0 because
the interrupt routing from NK to 68k VIA handler is missing (a separate milestone's
scope — the NK registered-handler table / interrupt dispatch chain, not the Cuda
device model).

**Next:** The Cuda timer delivery is validated at the VIA/PIC layer. The remaining
blocker is NK→68k interrupt routing for the VIA source. Either:
(a) wire the NK registered-handler table entry for IRQ 0x19 (VIA/Cuda), or
(b) identify how Mac OS 9's Nanokernel Interrupt Manager (ENIM/IHT) registers
    the VIA handler during boot (it may need the Cuda protocol to be working
    first — a chicken-and-egg).

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
