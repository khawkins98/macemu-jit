# M3a Task 0 — Live entry table + recon findings (9.0.1 staged NK)

> **Status:** ✅ Complete — 2026-06-10. Method: SS_PROBE_PC absolute-word dumps on live
> 9.0.1 newworld diagnostic boots (`/tmp/m2accept.prefs`), per the rev-2 two-anchor,
> probe-verified protocol (plan `2026-06-10-machine-layer-m3a.md` Task 0). Every value
> below is **probe-verified** unless marked inferred.

## The entry table

| Field | Value | Mode | Evidence |
|---|---|---|---|
| `interrupt_entry` | **`0x50412b1c`** | **KDP-SHIM** (see below) | 16 live words at 0x50412b0c–0x50412b48 byte-identical to static 0x312b0c–0x312b48 (`409b001c 92260024 …`) |
| `syscall_entry` | **0 (unresolved — descope active)** | `SS_EXC_SC=abort` default | Not resolved in Task 0; the SRR-capture+abort path carries the double-increment fix; revisit when a live `sc` fires |

## Findings

1. **Relocation delta = +0x100000 EXACTLY, byte-identical copy.** Both anchors agree:
   - interrupt entry: live `0x50412b1c..48` == static `0x312b1c..48`, all 16 words.
   - check_work: live `0x50426880..bc` == static `0x326880..bc`, all 16 words (incl. the
     relative `bl` word `4bfebe49` — relative branches survive copy unchanged; no immediate
     fixups observed in these windows).
   - The Wave-0 "+0x100004" was a heartbeat-sampled-PC-one-instruction-in artifact.
   - **Consequence:** static-dump RE remains valid for the staged kernel via `static + 0x100000`;
     live confirmation per new target is still cheap (one probe) and recommended.

2. **The real vector pages are EMPTY — DIRECT-VECTOR mode is out; KDP-SHIM confirmed.**
   16-word dumps at 0x100, 0x300, 0x500, 0x900: all zeros. The staged NK installed no
   architectural vector code (consistent with V=P + the nanokernel's own dispatch design).
   M3a Task 4 performs the KDP register-save shim before `ExcEnter`, as planned.

3. **The `[NW-INT]` silence is SOLVED: `XLM_IRQ_NEST` (@0x2818) = `0xFFFFFFFF`.**
   The tick thread's gate (`ReadMacInt32(XLM_IRQ_NEST) == 0`, main_unix.cpp:2216) is
   permanently false on this boot → `TriggerInterrupt` never fires from the tick thread →
   no `[NW-INT]`, and **the 60 Hz re-trigger safety net does NOT exist on this boot** —
   the EE-edge re-raise (plan Task 3) is load-bearing, exactly as the rev-2 C1 hypothesis
   predicted. `XLM_RUN_MODE` (@0x2810) = 0 (MODE_68K) — the MODE_NATIVE arm is unreachable
   today (its newworld fence stays; cheap insurance). XLM signature 'Baah' present @0x2800.

4. **KDP shim inputs are live:** `[KDP+0x65c]` = `0x68fff000` (the ECB — matches the
   `[NW-TRAMP] ECB=68fff000` boot line; the handler-prologue r6 target is sane).
   `[KDP+0x660]` = `0x00000000` (the r7 source — `interrupt()` rlwimi's bit 0 in, so the
   shim will pass r7=0x80000000; same value the paravirtual path would compute from this
   memory; recorded as a watch item for Task 7 outcome (b) analysis).

5. **Boot context for acceptance:** idle-loop spin PCs 0x50426884–0x50426b1c = staged
   check_work/idle cluster (static 0x326880-family + 0x100000), polling the real SCC at
   0xF3012000 (Wave-0 §8). MSR observed 0xf072 at the wall (EE=1); the spin's `mtmsr`
   writes are DR-toggles preserving EE — outcome-(c) (EE-masked) remains low-probability.

## Task 7 acceptance results (2026-06-10)

### The first real PPC exception ever delivered

```
[EXC] DEC delivered #1: restart=50429b40 srr1=0000f072 msr=00001040 -> entry=50412b1c
```

The KDP shim satisfied the handler ABI; the handler ran natively, reprogrammed DEC itself
(`mtspr_dec 3->4`), and exited via its r7-flag `blr` path as designed. Delivery at
`entry=0x50412b1c` matches the Task 0 probe-verified interrupt_entry (KDP-SHIM mode, above).

### Boot-A root cause and fix (commit ab8e5ac6)

The cold MSR fiction `0xf072` has `EE=1` from instruction zero. The first DEC expiry
therefore delivered into NK cold-init, whose state was: all registers zero, LR=0. The
handler ran correctly (shim ABI satisfied), then exited via the r7-flag `blr` path with
LR=0 → jumped to address 0 → `ignoreillegal` zero-page march → SIGSEGV at the 0x100000
mapping edge.

Architecturally, reset MSR has EE=0; the OS enables interrupts when ready. Fix: the
newworld trampoline now seeds MSR=0x7072 (the fiction minus EE bit). Verified live:
heartbeat shows `exc=0/1/0` (delivered=0, deferred_ee=1, deferred_depth=0) — the
cold-init expiry defers correctly, boot reaches the console spin intact.

### Acceptance reinterpretation — both delivery directions verified

The post-fix boot frontier is the NK **Thud debug console** (SPIKE-S3 §2.5), whose
**designed wake is a serial character, not a timer**. EE stays honestly masked at the
console prompt. The same behavior occurs with and without `SS_ROM_SKIP_JUMP68K`; on a
diskless, System-less diagnostic boot this is plausibly the NK's designed end state.

**Carry-forward (not failure):** M3a verified both directions of the delivery machinery —
delivery when EE permits, and deferral (with correct re-raise) when EE is masked.
"Idle loop wakes via real delivery" in the M3 row sense is gated on the boot proceeding
past the console (M6 PPC→68k handoff + M3b external-source wiring). Recorded explicitly
as a carry-forward, not a regression.

**Carry-forward additions (final-review follow-ups, 2026-06-10):**
1. The depth-deferral branch (`execute_depth != 1` -> deferred_depth) is **live-untested**
   (every boot shows `deferred_depth=0`); M3b's deliverability harness vector is its first
   real exercise.
2. **Post-close spike:** a disk-attached Mac OS 9 boot (with AND without
   `SS_ROM_SKIP_JUMP68K`) parks at the SAME console spin (comp=781, identical signature)
   - the console parking is NOT a diskless artifact; the frontier is the NK
   handoff/initial-task gap (couples to M6), not external interrupts. Full analysis:
   `M5A-BOOT-HANDOFF-ANALYSIS.md` (in progress). M3b must not be scoped on the assumption
   that external-interrupt wiring alone advances the boot.

**Planned debug knob dropped:** `SS_EXC_FORCE` (deliver once ignoring EE) was in the plan
but never implemented — the deferral evidence (exc=0/1/0 telemetry) came for free from the
heartbeat, making the knob moot. Noted as dropped.

### End-to-end machine-layer demonstration (commit a2dd1ff8)

`SS_SCC_RX_INJECT=25:0D` (inject one CR at T+25s) fed the console its designed wake signal.

Evidence (two consecutive two-heartbeat windows, same boot):
- **Pre-injection:** JIT compile counter frozen at 781 blocks (two heartbeats identical).
- **Post-injection:** compile counter 781 → 791 (two heartbeats, 10 new code blocks compiled
  and executed in direct response).
- Console processed the byte and returned to its prompt-wait loop.

This is an airtight A/B within one boot: M2 scheduler → M1 bus/backpatch → SCC Rx →
`check_work` → console. Every machine-layer milestone composing in one observable event.
M1's carried-forward consumer-(b) Rx-path coverage is closed.

## Probe recipes used (for reproduction)

```bash
# Run 1 — XLM + KDP + vector pages (5 probe PCs, 16 fields each):
SS_PROBE_PC='0x504268d0:[0x2810],[0x2814],[0x2818],[0x281c],[0x68ffe65c],[0x68ffe660],[0x2800],[0x2808];0x50426aec:[0x900],...;0x504268bc:[0x100],...;0x50426b1c:[0x300],...;0x50426884:[0x500],...'
# Run 2 — two-anchor delta verification:
SS_PROBE_PC='0x504268d0:[0x50412b1c],...,[0x50426880],...;0x50426aec:[0x50412b3c],...,[0x504268a0],...'
# Both: SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1, perl-alarm 20-25s, /tmp/m2accept.prefs
```
