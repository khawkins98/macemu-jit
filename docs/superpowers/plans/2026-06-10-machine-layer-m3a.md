# Machine Layer M3a — Exception core + direct-entry delivery (the big rock, first half) — rev 2

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the first half of MACHINE-LAYER-PLAN M3 (§2d): a real PPC exception model on the newworld profile — MSR semantics (EE consulted at delivery; full SRR1→MSR restore on `rfi`), `sc` as a real exception (eliminating the double-increment class), DEC-expiry **delivery** (consuming M2's condition latch), the deliverability rule (no delivery inside nested executes), and the **direct-entry** dispatch experiment — culminating in the live acceptance: **the NK idle loop (where the Wave 0 boot now spins, polling the real SCC) wakes via real DEC delivery on the 9.0.1 diagnostic boot.** OpenPIC + Cuda + external-source wiring + the full nested-execute-path retirement are M3b. Paravirtual stays byte-identical throughout.

**Architecture:** A pure module `exc_core` (`src/machine/`) owns the OEA exception-transition math with the **concrete PEM masks** (rev 2: `MSR_CLEAR_MASK = 0x0004EF32` on entry — POW/EE/PR/FP/FE0/SE/BE/FE1/IR/DR/RI cleared, ME/IP preserved; `SRR1 = msr & 0x0000FFFF`; `RFI_MSR_MASK = 0x0000FF73`). Thin profile-gated seams consume it: `execute_syscall`/`execute_rfi` in `ppc-execute.cpp` (with `sc` reclassified `CFLOW_TRAP` — rev 2 F1: as `CFLOW_NORMAL`, an absolute-PC `sc` would corrupt decoded interpreter blocks), and a delivery branch in `check_spcflags`' HANDLE arm that takes a pending DEC exception **in place** (mutate live PC/MSR/SRR and continue — the dispatcher re-derives from `pc()`, code-verified) when `execute_depth == 1` (rev 2 F4: the existing core counter — no new counter; it also covers the fifth nested site `execute_ppc` the recon missed) and `MSR[EE]` permits. Deferred delivery re-raises at the **EE 0→1 edges** (`mtmsr`/`rfi` interpreter handlers — both interpreter-only, so they always end JIT blocks; rev 2 F2/M1: a sticky TRIGGER would collapse chaining for the whole EE-masked window and is rejected). The delivery hook performs the **KDP register-save shim** before entry (rev 2 #1: the NK handler prologue stores through r6 = `[KDP+0x65c]` in its first instructions — the contract is structurally mandatory, not an iteration; M3a's upgrade over `interrupt()` is honest values: real restart PC in r10/r12, real composed SRR1 in r11) — unless Task 0 finds a real populated vector page (rev 2 C2), in which case the genuine vector entry (clean SRR-only contract) is preferred. Entry targets come from Task 0's two-anchor, immediate-masked signature scan (rev 2 #4: the relocation delta is NOT established — observed arithmetic is +0x100004, not +0x100000).

**Tech Stack:** C++ interpreter slow paths + glue, pure clang++ machine-module tests, SS_PROBE_PC live recon, autoconf build. No JIT codegen changes (`sc`/`rfi`/`mtmsr` all fall back; the aarch64 JIT file is `src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` — rev 2: not the similarly-named `src/cpu/ppc/ppc-jit.cpp`).

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/M3-INTERRUPT-RECON.md` | Today's interrupt path verbatim; MSR/SRR/rfi/sc state; poll contract; direct-entry ABI; ranked decisions. **Line anchors drifted ~9–22 lines — rev 2 F7 corrected table below.** |
| `docs/planning/MACHINE-LAYER-PLAN.md` §2d/§2g + M3 row + §9 stop-rule | The design contract |
| `docs/planning/machine/M5-MMU-SR-WALL-ANALYSIS.md` §8 | The live testbed + relocation finding + probe word-dump recipe |
| `docs/planning/machine/M3-PIC-CUDA-DONOR-STUDY.md` §7 | M3b scope fence |
| `docs/planning/machine/M3A-ENTRY-TABLE.md` (Task 0 output) | The resolved live entry table — written by this plan |

## Codebase facts (verified by the rev-2 red team, 2026-06-10)

- **Corrected line anchors (rev 2 F7):** `check_spcflags` = `ppc-cpu.cpp:1424–1468`; poll sites = :2251, :2410, :2467 (+legacy :1521, inert on aarch64); `interrupt()` = glue:633–706; `execute_68k` = :709; `execute_macos_code` = :805; `execute_emul_op` = :333; `execute_ppc` = :858–871 (**the fifth nested-execute site**, via `get_resource`/`NATIVE_GET_RESOURCE` :878–896/:498–508); `TriggerInterrupt` = :1863; `HandleInterrupt` = :1875–2008; `[NW-INT]` block = :1913–1941 (**already gated** on `nw_tramp = ROMType==NEWWORLD && MachineProfileIsNewWorld()` at :1914 — paravirtual can never reach it; outright deletion is safe; legacy `SS_NW_TRAMPOLINE=1` maps to newworld and loses it, intended); `execute_syscall` = ppc-execute.cpp:1118–1126; `execute_rfi` = :1577–1580; sc JIT fallback = aarch64 ppc-jit.cpp:3692–3693.
- **THE dispatcher re-derives from `pc()` after `check_spcflags` returns true** (rev 2, code-verified — the make-or-break fact): JIT path re-looks-up `fn` from `pc()` (ppc-cpu.cpp:2258–2261, fallthrough :2329–2331); decode-cache and per-insn loops likewise (:2432/2436, :2447). **In-place delivery works.** The JIT poll stores the **block-start PC** before any body code (emit at chain entry, ppc-jit.cpp:1368–1385, placement :5596–5618) — the stored PC is always a not-yet-executed block ⇒ valid SRR0.
- **`sc` is `CFLOW_NORMAL`** (`ppc-decode.cpp:880–883`) and decoded interpreter blocks run every entry unconditionally (ppc-cpu.cpp:1599, 2400–2408) — **rev 2 F1 (CRITICAL): an absolute-PC `sc` must be reclassified `CFLOW_TRAP`** (profile-safe: cflow only shortens block formation, never changes semantics; harness unaffected). The JIT side is already safe (fallback ends the block, :5762–5773). `rfi` is already `CFLOW_BRANCH` (ppc-decode.cpp:513–517) — MSR restore has zero block-formation implication.
- **Absolute-PC writes by fallback handlers survive the JIT bridge** (rev 2, verified): `ppc_jit_interp_one` (ppc-cpu.cpp:946–954) lets the handler own PC advancement; the fallback ends the compiled block. SRR0=pc+4 for `sc` is correct in both engines (handlers see pc() = the sc address).
- **`execute_depth`** (`ppc-cpu.hpp:250`, inc/dec at ppc-cpu.cpp:1508/2474 around **every** `execute()`) — rev 2 F4: use `execute_depth == 1` as the deliverability gate (we are inside the outermost execute when `check_spcflags` runs). Covers all five nested sites incl. `execute_ppc`. **No new counter; no new `powerpc_registers` fields anywhere in M3a.**
- **`HandleInterrupt`'s MODE_68K arm mutates live state beyond the [NW-INT] block** (rev 2 F3/M2): `WriteMacInt16(KDP+0x67c, 1)` at :1895 and the `r->cr` mask injection at :1897–1899 are paravirtual fake-delivery machinery written back through `interrupt_copy` — on newworld they would corrupt guest CR mid-NK-execution. The keep-set on newworld is ONLY the `Ticks` increment (:1906) + the 1 Hz time update. **`INTERRUPTS_IN_NATIVE_MODE` is compiled in** (glue:140); the MODE_NATIVE arm (:1944–1965) calls `interrupt(ROMBase+0x312b1c)` — the stale static entry — and must be fenced on newworld.
- **0xf072 decode (rev 2 #2 — the acceptance's load-bearing bit):** `0xf072 = EE|PR|FP|ME|IP|IR|DR|RI` — **EE = 1**. The Wave-0 wall probe saw MSR=0xf072 live; the idle loop's `mtmsr` writes are mfmsr-derived DR-toggles that preserve EE. Outcome-(c) (EE always off) is low-probability. Oddity to preserve, not fix: 0xf072 claims PR=1 (user mode) while the NK runs supervisor — a legacy fiction that now propagates through real SRR1/rfi; harmless (PR unenforced), documented in exc_core.h.
- **The `[NW-INT]` SILENCE mystery (rev 2 C1 — Task 0 must explain before Tasks 2/5):** Wave-0 logs contain zero `[NW-INT]` lines although statically the tick→TRIGGER→HANDLE→tick-50 chain should fire within ~1 s. Most plausible: **the XLM globals page (XLM_RUN_MODE @0x2810, XLM_IRQ_NEST @0x2818) sits inside Wave 0's newworld low-mem extension — memory the staged NK now also owns**; whatever the NK wrote at 0x2818 (≠0) gates the tick thread's `TriggerInterrupt` off (main_unix.cpp:2216–2219). Unverified — probe it.
- **The real vector page is now mapped guest memory (rev 2 C2):** post-Wave-0, physical 0x100/0x300/0x500/0x900 are inside the low-mem extension. If the staged NK installed genuine vector code there, direct-entry can target the **architectural vector** (clean SRR-only contract — no KDP ABI emulation needed). Task 0 dumps them.
- **Watchdogs:** `[ALARM]`/`[STALL]` are log-only (`emul_op.cpp` ss_boot_stall_check; `SS_BOOT_STALL_SECS=0` disables). The boot-killer is the `perl alarm` wrapper → SIGALRM skips atexit dumps (rev 2 M5): **`[EXC]` telemetry must ride the periodic `[HB]` heartbeat**, and Task 7 must also capture the M1 MMIO region counters the same way (the M5 §8 carry-forward).
- **Source discrimination (rev 2 M3):** the TRIGGER/HANDLE spcflag is a shared doorbell. DEC-pending = `VirtClockDECPending` latch; VIA-pending = `InterruptFlags` (main_unix.cpp:2359, sticky until guest-consumed). The delivery branch consumes ONLY the DEC latch and never touches `InterruptFlags`; with both pending, DEC delivers and the VIA flag waits for the next 60 Hz re-trigger (≤16 ms — acceptable, recorded). `check_spcflags` tests HANDLE before TRIGGER ⇒ a kick takes two poll passes (fine; optionally drain on TRIGGER too).
- **DR-emulator blocks have no entry poll** (aarch64 ppc-jit.cpp:5614–5618) — delivery inside chained DR loops waits for the between-block dispatcher check. Irrelevant to the diagnostic boot; recorded in Task 4's contract comment.
- **Wave-0 srr0-zeroing regression concern: retired** (rev 2 m3) — paravirtual e2e passed post-change; the garbage-jump lottery predates Wave 0 and the NK writes SRR0 before rfi.

## File map

| File | Status | Responsibility |
|---|---|---|
| `SheepShaver/src/include/exc_core.h` / `src/machine/exc_core.cpp` / `test_exc_core.cpp` | create | OEA transition math with the concrete masks; entry table struct; deliverability predicate |
| `SheepShaver/src/machine/Makefile` + `.gitignore` | modify | 9th standalone test |
| `docs/planning/machine/M3A-ENTRY-TABLE.md` | create (Task 0) | Live entry table + XLM/vector-page findings |
| `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-decode.cpp` | modify | `sc` → `CFLOW_TRAP` |
| `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp` | modify | `execute_rfi` (newworld full restore + EE-edge re-raise), `execute_syscall` (newworld real exception, `SS_EXC_SC` knob), `execute_mtmsr` (EE-edge re-raise) |
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` | modify | Delivery hook (KDP shim + ExcEnter), entry table, `[NW-INT]` deletion, HandleInterrupt newworld fence (incl. MODE_NATIVE arm), telemetry |
| `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp` | modify | `check_spcflags` HANDLE-arm newworld branch → glue hook (before interrupt_copy/HandleInterrupt) |
| `SheepShaver/src/Unix/main_unix.cpp` | modify | DEC latch → `TriggerInterrupt()` kick (newworld); `[EXC]` in heartbeat; Makefile.in SRCS += exc_core.cpp |
| docs (M3 row split note, recon addendum, CHANGELOG, **DIAGNOSTICS.md** — rev 2 m5) | modify | Bookkeeping |

**Parallelization:** Task 0 first (its outputs gate Tasks 3–4). Then Task 1 (exc_core, pure) ∥ Task 2 (glue fences — no exc_core dep). Tasks 3–5 sequential. Task 6 gates, Task 7 acceptance, Task 8 docs (draftable in parallel with 7).

**Standing rules:** M2/Wave-0 rules (batch inner loop / legacy authoritative; ask-first boots; per-task commits; grep the **aarch64** ppc-jit.cpp before promoting any stub). Stop-rule: if direct-entry bogs down after Task 0's facts are in, ship the M3a floor (sc/rfi fixes + depth rule + formalized current mechanism) and reassess.

---

### Task 0: Live recon — entry table, XLM mystery, vector page (user-coordinated probe boots)

All probes are register/absolute-word dumps at block entry (Wave-0 recipe); no code changes. Boots use `/tmp/m2accept.prefs` (recreate recipe in the Wave-0 plan).

- [ ] **Step 1 — the `[NW-INT]` silence + XLM ownership (rev 2 C1):** probe `[0x2810]` (XLM_RUN_MODE), `[0x2818]` (XLM_IRQ_NEST), and neighbors at the idle-loop PC. Explain why the tick chain never fires on this boot (expected: NK-owned values gate `XLM_IRQ_NEST != 0`, or run-mode ≠ MODE_68K). Record the implications for Task 5's fence reasoning.
- [ ] **Step 2 — the real vector page (rev 2 C2, highest-value):** 16-word dumps at 0x100, 0x300, 0x500, 0x900. If the staged NK installed genuine vector stubs (non-zero plausible PPC code), capstone-decode them: do they consume SRR0/SRR1 architecturally and branch into the staged kernel? If YES → the entry table's interrupt target is **the 0x900 vector itself** and the KDP shim in Task 4 is bypassed (clean contract — record as DIRECT-VECTOR mode). If zeros/garbage → KDP-shim mode confirmed.
- [ ] **Step 3 — relocation delta, properly (rev 2 #4):** the observed arithmetic is `0x426884 − 0x326880 = +0x100004` — NOT a clean +0x100000, and heartbeat-sampled PCs can't anchor a delta. Method: take ≥16-word static signatures at TWO anchors (check_work head 0x326880 AND the interrupt entry 0x312b1c), probe word-dump candidate deltas (+0x100000, +0x100004, bisect as needed), and compare **capstone mnemonic streams with immediate fields masked** (the staged copy is relocated — absolute `lis/ori/addis` immediates may be fixed up; raw-word comparison false-negatives). Both anchors must agree on one delta; disagreement ⇒ per-symbol resolution, record both addresses.
- [ ] **Step 4 — KDP context block liveness:** absolute dumps of `0x68ffe65c`/`0x68ffe660` (the `interrupt()` ABI's r6/r7 sources) during the spin — non-zero and sane ⇒ the KDP shim has live inputs.
- [ ] **Step 5 — sc handler entry:** resolve via the same two-anchor method from the static vector-dispatch family (`0x503254e0 → 0x50312cb0` tail) + delta; if not confidently resolvable, record the descope (Task 3's `SS_EXC_SC` knob covers it).
- [ ] **Step 6 — write `M3A-ENTRY-TABLE.md`:** resolved entries (interrupt target + mode DIRECT-VECTOR|KDP-SHIM, sc entry or descope), the delta finding, XLM observations, KDP liveness — each value tagged probe-verified vs inferred. Commit.

### Task 1: `exc_core` pure module (standalone-tested) — parallel with Task 2

API (machine-module idiom; the masks are LAW — rev 2 #3, written against the PEM):

```cpp
// MSR bits (OEA): EE=0x8000 PR=0x4000 FP=0x2000 ME=0x1000 FE0=0x800 SE=0x400
// BE=0x200 FE1=0x100 IP=0x40 IR=0x20 DR=0x10 RI=0x2 POW=0x40000
#define EXC_MSR_CLEAR_MASK 0x0004EF32u   // cleared on entry: POW|EE|PR|FP|FE0|SE|BE|FE1|IR|DR|RI (ME, IP preserved)
#define EXC_SRR1_KEEP_MASK 0x0000FFFFu   // SRR1 = msr & this (bits 1-4,10-15 of the high half zeroed)
#define EXC_RFI_MSR_MASK   0x0000FF73u   // rfi: MSR = (SRR1 & this) | (MSR & ~this); POW/ILE never restored

enum ExcClass { EXC_DECREMENTER, EXC_EXTERNAL, EXC_SYSCALL };
struct ExcEntryTable { uint32_t interrupt_entry; uint32_t syscall_entry; /* 0 = unresolved */ };
struct ExcTransition { uint32_t srr0, srr1, msr, pc; };
#define EXC_PC_UNRESOLVED 0xFFFFFFFFu    // returned pc when the class's table entry is 0

// cur_pc_restart: DEC/EXT = the not-yet-executed restart PC (block start at the poll);
// SC = the address of the sc instruction itself — ExcEnter adds the +4 (pin the ownership).
ExcTransition ExcEnter(uint32_t cur_pc_restart, uint32_t cur_msr, ExcClass cls, const ExcEntryTable *tbl);
void ExcRfi(uint32_t srr0, uint32_t srr1, uint32_t cur_msr, uint32_t *out_pc, uint32_t *out_msr);
static inline bool ExcDeliverable(uint32_t msr) { return (msr & 0x8000) != 0; }
```

Notes the header documents: DEC/EXT/SC share ONE MSR transform (per-class differences are SRR0 semantics + target only — rev 2 #7: don't invent mask differences); the 0xf072 PR=1 legacy fiction (preserved, not fixed); POW pre-exception state is architecturally lost (rfi doesn't restore it — one sentence, no code); LE/ILE degenerate (big-endian Mac).

TDD — the rev-2 #9 load-bearing test set verbatim (anti-vacuity rule: every mask assertion uses an input where a wrong mask changes the answer):
1. `ExcDeliverable`: 0xf072→true; 0x7072→false; 0x8000→true.
2. Entry MSR transform: 0xFFFFFFFF → `~0x0004EF32` result (catches over- AND under-clearing + ME/IP preservation); 0xf072 → **0x1040**.
3. SRR1: 0xFFFFFFFF → **0x0000FFFF**; 0xf072 → **0xf072** (the honest composition reproduces the legacy fake byte-for-byte — the boot-real equivalence datum).
4. SRR0: DEC/EXT → restart PC verbatim; SC → sc_addr+4.
5. Targets: DEC/EXT → interrupt_entry; SC → syscall_entry; syscall_entry==0 → pc == EXC_PC_UNRESOLVED exactly.
6. ExcRfi both directions: SRR1=0xFFFFFFFF,MSR=0 → **0x0000FF73**; SRR1=0,MSR=0xFFFFFFFF → **0xFFFF008C**; SRR0|3 → pc == SRR0 & ~3.
7. Round-trip: M ∈ {0xf072, 0x9032, alternating-pattern} → ExcEnter then ExcRfi restores pc/msr exactly; plus the documented-loss case (POW=0x40000 not restored — asserted, not untested).

Makefile recipe + .gitignore + `make -C src/machine test` 9/9. Commit.

### Task 2: Glue fences + [NW-INT] deletion (parallel with Task 1)

- [ ] **Delete the `[NW-INT]` tick-50 block** (glue:1913–1941) outright — rev 2 m1: it is already newworld-gated (`nw_tramp`), paravirtual never reached it, deletion's reachable-set is exactly the newworld injection hack M3a replaces. Commit message records this (no "paravirtual keeps it" claim).
- [ ] **Fence HandleInterrupt for newworld (rev 2 F3/M2/F5):** on `MachineProfileIsNewWorld()`, the MODE_68K arm keeps ONLY the `Ticks` increment + 1 Hz time update; the `WriteMacInt16(KDP+0x67c,1)` and the `r->cr` mask injection are fenced off (paravirtual fake-delivery machinery — on newworld they corrupt live guest state). The **MODE_NATIVE arm** (compiled in, glue:1944–1965) is fenced on newworld entirely (it nest-executes into the stale static entry).
- [ ] Note (rev 2 F4): NO new depth counter — Task 4 uses `execute_depth`. This task is fences only.
- [ ] Gates: build + batch + legacy test-jit 353/353 (all changes newworld-gated or unreachable-on-paravirtual). Commit.

### Task 3: `sc` + `rfi` real semantics (newworld-gated, via exc_core)

- [ ] **`ppc-decode.cpp`: `sc` cflow → `CFLOW_TRAP`** (rev 2 F1 — REQUIRED before the absolute-PC change; profile-safe, semantics-free, harness-unaffected).
- [ ] `execute_rfi`: newworld → `ExcRfi(...)` applying pc+msr; **plus the EE-edge re-raise** (rev 2 F2): `if (newworld && !(old_msr & EE) && (new_msr & EE) && VirtClockDECPending(...)) trigger_interrupt();`. Paravirtual → existing pc-only, byte-identical.
- [ ] `execute_mtmsr`: append the same EE-edge re-raise (newworld-gated; the handler is interpreter-only so it always ends JIT blocks — the re-poll happens naturally right after).
- [ ] `execute_syscall` (SHEEPSHAVER arm): newworld → `ExcEnter(pc(), msr, EXC_SYSCALL, &g_exc_entry_table)`; if `pc == EXC_PC_UNRESOLVED`: behavior per **`SS_EXC_SC=abort|legacy`** (default `abort` — rev 2 #5: the legacy no-op was never a harmless skip, it manufactured stale-CR control flow (HANDOFF §1.7.2); abort-with-SRR-capture is diagnosable; `legacy` is the no-rebuild fallback if an unexpected early `sc` masks the acceptance signal). Apply the transition absolutely; no `increment_pc`. Paravirtual untouched.
- [ ] Entry table `g_exc_entry_table` in glue, filled from Task 0 (`SS_EXC_ENTRY=0xINT[,0xSC]` env override for no-rebuild iteration).
- [ ] Makefile.in SRCS += `../machine/exc_core.cpp` + config.status. Gates: build, batch+legacy 353/353, machine suite 9/9. Commit.

### Task 4: The delivery hook — DEC delivery at the block-boundary poll

- [ ] **Kick:** `vclk_dec_arm`'s scheduler lambda calls `TriggerInterrupt()` after `VirtClockDECExpire` (newworld-gated; same atomic spcflags assertion the tick thread uses). The lazy `ReadDEC` path already runs on the CPU thread — add the same kick after a lazy latch.
- [ ] **Delivery branch** in `check_spcflags`' HANDLE arm (ppc-cpu.cpp:1431-region), BEFORE `processing_interrupt`/`interrupt_copy`/`HandleInterrupt`, after the EXEC_RETURN check: `if (MachineProfileIsNewWorld())` → call the glue hook. The hook: `if (VirtClockDECPending() && ExcDeliverable(regs().msr) && execute_depth == 1)`: clear the latch; **perform the KDP register-save shim** (rev 2 #1 — mandatory unless Task 0 found DIRECT-VECTOR mode): the `interrupt()` ABI writes (KDP+0x004/+0x018, the +0x65c block +0x13c..0x16c, r1=KernelDataAddr, r7=[KDP+0x660] rlwimi'd, r13=CR) with M3a's honest upgrades — **r10/r12 = the real restart PC** (not a trampoline) and **r11 = the real composed SRR1** (not 0xf072 — though for the boot-real case they are byte-equal, Task 1 test 3); then `ExcEnter(restart_pc, msr, EXC_DECREMENTER, table)` applied to LIVE regs; count `[EXC]` telemetry; **return true immediately** (rev 2 F3 — never fall through to HandleInterrupt in the same call). `SS_EXC_BARE=1` skips the KDP shim (the bounded direct-entry experiment knob). Deferred (EE off or depth>1): leave the latch set, count deferred-EE/deferred-depth telemetry, NO sticky flags (the EE-edge re-raise in Task 3 + the nested-execute-return recheck below re-raise) — and for depth-deferral, add the recheck at the nested-`execute()` returns (the four glue sites): `if (newworld && latch) trigger_interrupt();`.
- [ ] **Source discrimination comment** (rev 2 M3): this branch consumes only the DEC latch; `InterruptFlags`/VIA stays HandleInterrupt's (fenced per Task 2) until M3b's PIC.
- [ ] **Telemetry**: `[EXC]` counters (delivered/deferred-EE/deferred-depth per class) — **emitted in the periodic `[HB]` heartbeat** (rev 2 M5: SIGALRM skips atexit) + the crash-path dump; also append the M1 MMIO region counters to the heartbeat once (the M5 §8 carry-forward capture).
- [ ] Contract comments: restart PC = block-start (not-yet-executed, JIT poll contract verified); DR-emulator blocks poll only between blocks (rev 2 F8); XLM_IRQ_NEST deliberately ignored in favor of real MSR[EE] (revisit if Task 7 shows the NK manipulating it instead).
- [ ] Gates: build, batch+legacy 353/353, machine suite 9/9, paravirtual inertness greps. Commit.

### Task 5: Tick-thread interplay on newworld (scope-fenced, informed by Task 0 Step 1)

With Task 2's fences in place, document and verify what actually runs on the newworld diagnostic boot: the tick thread's trigger (gated by NK-owned XLM_IRQ_NEST — Task 0's finding), the HandleInterrupt keep-set (Ticks/Time only), and the delivery branch as the sole real delivery path. If Task 0 found the tick chain dead (C1 hypothesis), record that the 60 Hz re-trigger safety net does NOT exist on this boot — the EE-edge re-raise is load-bearing — and assert that in Task 7's telemetry expectations. Gates + commit (may be a docs/comments-only task if Task 0 confirmed the hypothesis).

### Task 6: Non-regression checkpoint

- [ ] Machine suite 9/9; batch + legacy test-jit 353/353; test-opcodes; rom-harness build; e2e-test; paravirtual `make e2e` (coordinate). Inertness greps: no `[EXC]` lines on paravirtual runs.
- [ ] **Deliverability-rule assertion** (rev 2 M4 — the §2d DoD item): a scripted newworld check that an interrupt raised while `execute_depth > 1` is deferred — telemetry-based: boot with `SS_PROBE_PC`-free config, assert `[EXC]` deferred-depth counter > 0 over a boot that runs EMUL_OPs (the ROM-patch EMUL_OPs fire during the diagnostic boot), OR a targeted unit-level assertion if the boot path can't demonstrate it — record which. (Full harness vector lands with M3b's PIC, named there.)

### Task 7: Acceptance — the idle loop wakes (user-coordinated boots)

- [ ] **Boot A:** 9.0.1 newworld diagnostic boot. Assert from the heartbeat `[EXC]` line: DEC deliveries > 0; the 0x504268xx spin **breaks** (comp resumes / new hot PCs / stage advances); paravirtual untouched. Outcome classes: (a) spin breaks, new frontier recorded — the M3a win even if a new wall appears; (b) delivery fires, handler crashes — record PC/context, iterate via `SS_EXC_ENTRY`/`SS_EXC_BARE`/`SS_EXC_SC` (no rebuilds); (c) delivery never fires — capture per-cause telemetry (deferred-EE vs deferred-depth vs latch-never-set) + **`SS_EXC_FORCE=1`** (rev 2 m4: debug knob, deliver once ignoring EE) to distinguish gating bugs from EE-masked-by-design.
- [ ] **Boot B:** probes at the entry target + the rfi site; verify SRR0/SRR1 round-trip in vivo; capture the MMIO region counters (M1 carry-forward closure).
- [ ] Record everything in M3A-ENTRY-TABLE.md; carry-forwards explicit.

### Task 8: Docs + bookkeeping

- [ ] MACHINE-LAYER-PLAN M3 row: split note — M3a landed; **M3b remaining: OpenPIC, Cuda + minimal ADB stub, external-source wiring, SDL_PumpEvents relocation, tm_task/via_int/cuda_init/via_init patch retirements, RTC, the deliverability harness vector, and the COMPLETION of the nested-execute interrupt-path retirement** (rev 2 m6: M3a stops calling `interrupt()` for DEC but the path remains; don't claim full retirement).
- [ ] **`SheepShaver/docs/DIAGNOSTICS.md`** (rev 2 m5): `[EXC]` heartbeat-line format, `SS_EXC_ENTRY`/`SS_EXC_SC`/`SS_EXC_BARE`/`SS_EXC_FORCE`.
- [ ] CHANGELOG (commits + acceptance numbers); LEARNINGS if non-obvious; recon addendum finalized; cross-tracker sweep (grep M3); ROM-PATCH-AUDIT: no retirements in M3a (confirmed — all M3-disposition patches are M3b). Commit.

---

## Self-review record

- §2d M3a-half coverage: MSR semantics ✓ (T1 masks + T4 gate + T3 EE-edges); sc real exception ✓ (T3 + CFLOW fix + SS_EXC_SC); rfi full restore ✓; DEC delivery consuming M2's latch ✓; deliverability rule ✓ (execute_depth gate + deferral telemetry + T6 assertion); direct-entry experiment ✓ (entry table + DIRECT-VECTOR vs KDP-SHIM modes + SS_EXC_BARE/SS_EXC_ENTRY knobs); [NW-INT] deleted ✓; acceptance = the M3 row's idle-loop wake ✓. M3b list explicit (T8) — nothing silently dropped (rev 2 M4/m6 fixed).
- Honest unknowns routed to Task 0: XLM ownership/silence, vector-page contents, relocation delta (off-by-4 acknowledged), KDP liveness, sc entry.
- The KDP shim is in-scope NOW (rev 2 #1), not an iteration; DIRECT-VECTOR is the upside surprise, not the baseline.

## Rev 2 corrections (red-team round, 2026-06-10 — 3 parallel code-verified reviewers)

**CRITICAL:** F1 `sc` CFLOW_NORMAL → decoded-block corruption with absolute-PC sc → reclassify CFLOW_TRAP (Task 3). F2/M1 deferred re-raise was circular/perf-fatal as sketched (cleared flags = no next poll; sticky TRIGGER = chaining collapse at 15M blocks/s) → EE-edge re-raise at mtmsr/rfi (interpreter-only, always end JIT blocks) + nested-return recheck. #1 the KDP register-save contract is structurally mandatory (the NK prologue stores through r6 in its first instructions — HANDOFF §2 evidence); bare entry = silent guest-memory corruption → shim promoted into Task 4 with honest values (real restart PC, real SRR1); SS_EXC_BARE keeps the experiment. C1 the [NW-INT] silence on Wave-0 boots is unexplained and the XLM globals now sit inside NK-owned mapped memory → Task 0 probes 0x2810/0x2818. C2 the real vector page (0x100/0x300/0x500/0x900) is now mapped and possibly NK-populated → Task 0 dumps it; DIRECT-VECTOR mode preferred if real.

**MAJOR:** verified the make-or-break dispatcher fact (re-derives from pc() — delivery works); F4 use existing `execute_depth` (fifth nested site `execute_ppc` found; recon's 4-site counter would misfire); F3/M2/F5 HandleInterrupt fence boundary = cr-mask injection + KDP+0x67c + the compiled-in MODE_NATIVE arm, not just [NW-INT]; #2 0xf072 has EE=1 (favorable; PR=1 fiction documented); #3 concrete PEM masks (0x0004EF32 / 0x0000FFFF / 0x0000FF73; FP IS cleared; ExcEnter(0xf072)→0x1040, SRR1→0xf072 byte-equal to the legacy fake); #4 relocation delta off-by-4 → two-anchor immediate-masked scan; M3 source discrimination written down; M4 deliverability DoD assertion added (T6); M5 telemetry rides the heartbeat (SIGALRM skips atexit) + M1 counter capture; #5 SS_EXC_SC knob.

**MINOR (folded):** depth-convention wording unified (execute_depth==1); per-class-mask test trap defused (shared transform + per-class SRR0/target deltas); POW/LE doc sentence; the full anti-vacuity test set pasted into Task 1; m1 [NW-INT] already newworld-gated (deletion safe, narrative fixed); m3 Wave-0 srr0 regression concern retired (paravirtual e2e passed); m4 SS_EXC_FORCE knob; m5 DIAGNOSTICS.md in Task 8; m6 split-note honesty; F7 line-anchor table; F8 DR-block poll note; aarch64-vs-ppc ppc-jit.cpp path correction.

**Verified-OK highlights:** dispatcher pc()-re-derivation at every resume path; restart-PC contract (poll at chain entry, pre-body); absolute-PC fallback survival through the JIT bridge (rfi precedent); rfi already CFLOW_BRANCH; sc SRR0=+4 correct in both engines; DEC kick seam sound; EXEC_RETURN ordering safe; [ALARM]/[STALL] log-only; machine suite count (8→9) and harness count (353) correct; MSR bit constants corroborated in-repo; recon ABI transcription accurate.
