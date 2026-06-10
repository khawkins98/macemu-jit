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

## Probe recipes used (for reproduction)

```bash
# Run 1 — XLM + KDP + vector pages (5 probe PCs, 16 fields each):
SS_PROBE_PC='0x504268d0:[0x2810],[0x2814],[0x2818],[0x281c],[0x68ffe65c],[0x68ffe660],[0x2800],[0x2808];0x50426aec:[0x900],...;0x504268bc:[0x100],...;0x50426b1c:[0x300],...;0x50426884:[0x500],...'
# Run 2 — two-anchor delta verification:
SS_PROBE_PC='0x504268d0:[0x50412b1c],...,[0x50426880],...;0x50426aec:[0x50412b3c],...,[0x504268a0],...'
# Both: SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1, perl-alarm 20-25s, /tmp/m2accept.prefs
```
