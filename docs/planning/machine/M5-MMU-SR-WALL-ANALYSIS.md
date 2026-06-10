# M5/M3 groundwork — MMU/SR wall static analysis: the 0x50326050 frontier

> **Status:** 📋 Static-analysis memo (read-only investigation, no builds, no boots) ·
> **Created:** 2026-06-10 · **Branch:** `macos-arm64`
> **Inputs:** decompressed 9.0.1 ROM `/tmp/rom901_decompressed.bin` (capstone PPC BE, base
> `0x50000000`); `MACHINE-LAYER-PLAN.md` §2d/§2e; `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md`
> §1.6/§2.8/§3; `MMU-DEFERRAL-REDTEAM.md`; today's LEARNINGS NK-ceiling entry; code in
> `ppc-execute.cpp`, `ppc-jit.cpp`, `ppc-registers.hpp`, `main_unix.cpp`, `cpu_emulation.h`.
> **Caveat:** every register *value* below is reasoned from data flow, not observed — the
> emulator was not run. Values needing a live probe are listed in §7.

---

## 0. TL;DR — the verdict

The 0x50326050–0x50326068 wall is a **nanokernel MMU/segment-fault service routine** that
**writes and reads segment registers** (`mtsrin`/`mfsrin`), **toggles MSR[DR]** (data
translation on/off, `mtmsr`), and sets up **BATs** (`mtdbatu`/`mtdbatl`) around byte-reversed
loads. The faulting access is `lwbrx r26, r22, r26` at **0x50326068** (the doc's "0x200a0
below RAMBase" is correct; the precise EA is `r22 + 0x200a0`, not a bare `0x200a0` — RA is
`r22`, see §1).

**The load-bearing finding:** our CPU core **stores** BAT/SDR1/SPRG as real registers
(`ppc-execute.cpp:1316–1404`, `ppc-registers.hpp:258–266`) — the SPRG fix harvested from
Path A — but it **drops** `mtsr`/`mtsrin`/`mfsr`/`mfsrin` (segment registers) **and** `mtmsr`
(MSR), routing all of them through `execute_illegal` (`ppc-execute.cpp:60, 75, 101, 153`).
There is **no `sr[16]` field and no `msr` field** in `powerpc_registers`
(`ppc-registers.hpp` has only `sprg[4]`, `sdr1`, `bat[16]`). This handler both *writes* a
segment register (`mtsrin r24,r22`) and immediately *reads it back* (`mfsrin r21,r22`) and
gates its memory access on MSR[DR] — so it depends on exactly the two pieces of stored state
we do not keep.

**Rung verdict:** Rung 2 must be **extended from "SR/BAT stored state" to "SR + BAT + MSR
stored state"** (HANDOFF §2.8 already names rung 2 the *minimum prerequisite*; this memo
confirms it and widens its scope to SRs and MSR, which the existing BAT/SDR1/SPRG storage
does not cover). **Rung 2 is necessary but its sufficiency is unproven** — it hinges on
whether `0x200a0`-translated equals physical `0x200a0` (REDTEAM's question D), which is a
runtime fact about the SR/BAT the nanokernel programmed and **cannot be resolved
statically**. If identity → rung 2 + a low-memory mapping unblocks it; if non-identity →
rung 3/4 territory. Do **not** read this as "rung 2 alone crosses the wall."

---

## 1. The faulting instruction (precise)

```
0x50326050  a3740f88  lhz    r27, 0xf88(r20)     ← start of the reported SIGSEGV PC range
0x50326054  62f40010  ori    r20, r23, 0x10      ← r20 = saved-MSR | 0x10  (set MSR[DR]=1)
0x50326058  3f400002  lis    r26, 2              ┐
0x5032605c  635a00a0  ori    r26, r26, 0xa0      ┴ r26 = 0x000200a0  (an IMMEDIATE, built here)
0x50326060  7e800124  mtmsr  r20                 ← enable data translation
0x50326064  4c00012c  isync
0x50326068  7f56d42c  lwbrx  r26, r22, r26       ← FAULTS: load byte-reversed from (r22 + 0x200a0)
0x5032606c  575a053e  clrlwi r26, r26, 0x14      ← keep low 12 bits of the loaded value
0x50326070  281a0040  cmplwi r26, 0x40           ← compare against 0x40 / 0x41 (DSISR-like codes?)
```

Corrections to the prose in HANDOFF §2.8 / CHANGELOG / LEARNINGS (all minor, none change the
conclusion):

- **`0x200a0` is an immediate**, materialized by `lis r26,2; ori r26,r26,0xa0` at
  `0x50326058`/`0x5032605c`. It is *not* a low-memory pointer that was loaded. (Confirms the
  "hardcoded physical" wording.)
- The faulting op is **`lwbrx r26, r22, r26`** — `RA = r22`, not `0`. The effective address is
  `r22 + 0x200a0`. The "`lwbrx r26,0,r26`" form quoted in the task brief is wrong about RA.
  **EA = 0x200a0 only if `r22 ≈ 0`** — unverified (see §7).
- `r20` (used by the `lhz` at `0x50326050`) comes from `lwz r20, -0x20(r1)` at `0x50325f40`,
  where `r1 = KDP = 0x68ffe000` (§2). `r20 + 0xf18` is read at `0x50325f48` *without faulting*,
  and `r20 + 0xf88` at `0x50326050` is only `0x70` higher in the same host-mapped region —
  so the `lhz` does **not** fault; the first instruction in the range that *can* fault is the
  `lwbrx` into the unmapped hole. (The reported PC *range* spans 0x50326050–0x50326068 because
  that is the basic block; the fault is at its end.)

**Why byte-reversed (`lwbrx`) on a big-endian machine?** Two non-exclusive readings:
(a) it is reading a **little-endian-presented hardware register or descriptor** (Mac/CHRP I/O
and some PTEG/host-controller structures are accessed byte-swapped), or (b) it is an
endian-neutral fetch of a packed field whose low 12 bits (`clrlwi r26,r26,0x14`) are then
range-checked against `0x40`/`0x41`. Combined with the MSR[DR]-on bracketing, the routine is
**deliberately peeking at one address through translation** and inspecting a small code field.
Resolving (a) vs (b) needs the live value (§7) — and that resolution is what tells us whether
crossing the wall needs only stored state or a real mapping.

---

## 2. The containing routine and its dispatch context

`0x325c00–0x326160` is a **table of fixed-size (~0x40-byte) nanokernel supervisor service
routines**, each with an identical prologue and a shared `b 0x503254e0` epilogue. The routine
containing the fault begins at **0x50325f00**:

```
0x50325f00  mr     r8, r8           ┐ marker NOPs (mr rX,rX)
0x50325f04  mr     r9, r9           │
0x50325f08  addi   r8, r1, -0xb70   │ r1 = KDP; r8 → KDP-0xb70 (a lock/nesting word)
0x50325f0c  bl     0x50312700       ┴ acquire/log stub (lwarx/stwcx. on KDP-0xb70; reads KDP-0x340)
0x50325f18  bl     0x503238d4       ← register-save stub (stw r20..r27 → save area at r6+0x1a4..)
0x50325f1c  addi   r9, r1, -0x750
0x50325f24  mfspr  r30, 0x113       ← SPR 275 = SPRG3 (save caller's SPRG3)
0x50325f38  mtspr  0x113, r9        ← SPRG3 = KDP-0x750  (per-context scratch)
0x50325f3c  mfmsr  r23             ← r23 = entry MSR (saved; restored as "DR off" baseline later)
0x50325f40  lwz    r20, -0x20(r1)   ← r20 = *(KDP-0x20)   (a context/area pointer)
0x50325f44  lhz    r27, 0x910(r1)
0x50325f48  lwz    r22, 0xf18(r20)  ← r22 = *(r20+0xf18)  (the faulting-access base — see §1)
0x50325f54  rlwinm r24, r22, 6, ..  ┐
0x50325f5c  mfsrin r21, r22         │ read the SEGMENT REGISTER selected by r22 (save it)
0x50325f60  lwzx   r24, r25, r24    │ fetch a replacement SR value from a table at *(KDP-0x3fc)+0x30
0x50325f64  mtsrin r24, r22         ┴ WRITE that segment register
0x50325f68  isync
...        (the 0x50326050 sub-path described in §1) ...
0x503260fc  mtsrin r21, r22         ← RESTORE the original segment register (r21 saved at f5c)
0x50326100  isync
0x50326104  b      0x503254e0       ← common epilogue → 0x50312cb0 (exception-return path)
```

**Identifications (data-flow, high confidence):**

- **`r1 = KDP = 0x68ffe000`** — the per-CPU KernelData pointer. The prologue's lock stub does
  `mfspr r9, 0x110` (SPRG0) then `lwz r9, -0x340(r9)`; SPRG0 holds the KDP (`cpu_emulation.h:37
  KERNEL_DATA_BASE = 0x68ffe000`), and `KDP-0x340` is the same poison field implicated in the
  page-descriptor ceiling fix (LEARNINGS 2026-06-10). The common tail `0x50312cb0` does
  `mfspr r1, 0x110` (re-loads KDP) — confirming r1 is KDP throughout, **not** a normal stack.
- **This is the MMU/segment-fault service path**, one entry in a vectored table of nanokernel
  primitives reached via the dispatcher tail at `0x503254e0 → 0x50312cb0` (which restores CR
  via `mtcrf` and branches on `r7` flag bits — the classic nanokernel "return from exception"
  shape). Sibling routines in the same table do BAT manipulation (`0x50325e7c`:
  `mfdbatu/mfdbatl … mtdbatu/mtdbatl`) and segment-fault classification (`0x50325c3c`: the
  `mfsrin`/`mtsrin` + `mtmsr` + a cascade of `rlwinm.` tests building a fault code 1..7 in
  r28). The shared idiom across the whole table: **save SR → reprogram SR → toggle MSR[DR] →
  touch memory → restore SR/MSR → return a small code.**

The routine's *structure* (save SR, flip DR, probe one address, classify, restore) is exactly
a **data-storage / segment exception handler** walking or validating a mapping — PPC vector
0x300 (DSI) / 0x400 (ISI) class behavior, implemented as a nanokernel primitive rather than a
raw 0x300 vector entry (NewWorld routes hardware exceptions through the nanokernel's own
dispatch, which is why we never see a literal 0x00000300 entry).

---

## 3. Where does `0x200a0` point, and what is the routine doing with it?

- **0x200a0 is in an unmapped guest hole.** SheepShaver maps guest **0x0000–0x3000** as the
  Low Memory area (`main_unix.cpp:1604–1610`, `vm_mac_acquire_fixed(0, 0x3000)`), RAM at
  **RAMBase = 0x10000000** (`main_unix.cpp:192`), ROM at 0x50000000, KernelData at 0x68ffe000,
  and MMIO at 0xF3xxxxxx. The range **0x3000 – 0x0FFFFFFF is not backed** → a flat (V=P)
  access to `0x200a0` SIGSEGVs. That is the wall.
- **On real Core99 hardware, physical `0x200a0` is in the first 128 KB of DRAM** — the region
  the nanokernel uses for its own low-memory structures (interrupt/exception scratch, the
  per-CPU "hardware vector" mirror, and the 68k low-memory globals the OS later overlays).
  It is **not** the HTAB (the nanokernel sizes that high in RAM from SDR1, LEARNINGS) and
  **not** a 0xF-page MMIO register (those are at 0xF3xxxxxx here). The `lwbrx` + low-12-bit
  mask + compare-against-0x40/0x41 reads like a **status/type byte** out of a small fixed
  descriptor the nanokernel parked in low physical memory during cold-init — which under our
  build never got written there because the access that *would* write it also targets the
  unmapped hole (or was an `ignoresegv`-eaten store on the old path).
- **The MSR[DR]-on bracket is the tell.** The handler flips data translation **on** precisely
  for this one access (`ori r20,r23,0x10; mtmsr r20` … `lwbrx` … `mtmsr r23` to turn it off
  again). On real hardware, with the SR/BAT it just programmed, `0x200a0` *virtual* resolves
  to some *physical* page. **Our core ignores MSR[DR] entirely** (it is dropped, §4), so the
  access is taken as flat `0x200a0` regardless. Whether that flat interpretation is *correct*
  depends on whether the nanokernel's mapping for `0x200a0` is identity — the open question.

---

## 4. Environment-needs inventory (with code citations)

What this routine reads/writes from the supervisor environment, and what our core does today:

| Resource | Routine's use | Core today | Gap |
|---|---|---|---|
| **Segment registers (SR0–15)** | `mfsrin r21,r22` (save), `mtsrin r24,r22` (program), `mfsrin r21,r22` again, `mtsrin r21,r22` (restore) | `mtsr`/`mtsrin`/`mfsr`/`mfsrin` → `execute_illegal` (dropped/ignored): `ppc-execute.cpp:60` ("mtsr…→illegal-ignored"), `:75`, `:101` ("210=mtsr"), `:153`. **No `sr[16]` field** in `powerpc_registers` (`ppc-registers.hpp:258–266` has only `sprg/sdr1/bat`). | **HARD GAP.** The routine reads back the SR it just wrote (`mfsrin` after `mtsrin`); with writes dropped and reads returning garbage, the BAT/SR translation logic it drives is incoherent. |
| **MSR[DR] (bit 0x10) / MSR[IR]** | `mfmsr` (save), `ori …,0x10; mtmsr` (DR on), `mtmsr r23` (DR off) | `mfmsr` decoded (`execute_mfmsr`, `:1277`) but `mtmsr` (xo 146) → `execute_illegal` (`:167`, dropped). **No `msr` field** stored. | **HARD GAP for fidelity.** Core has no notion of translation on/off; the bracket is a no-op to us. Acceptable *only* if 0x200a0 is identity (then DR on/off is semantically irrelevant). |
| **BAT registers (IBAT/DBAT)** | sibling routine `0x50325e7c` does `mfdbatu/mtdbatu/mtdbatl`; this routine relies on BAT/SR state set up earlier | **STORED** as real regs: `ppc-execute.cpp:1338–1339, 1403–1404`; `ppc-registers.hpp:266 bat[16]`. | OK (read-back coherent), but **inert** — stored, never consulted for translation. |
| **SDR1 / SPRG0–3** | KDP via SPRG0; SPRG3 used as per-context scratch (`mtspr 0x113`) | **STORED**: `ppc-execute.cpp:1316–1317, 1330–1336, 1384–1397`; `ppc-registers.hpp:258, 264`. | OK — the SPRG fix already covers this. |
| **Physical low memory at ~0x200a0** | `lwbrx r26, r22, 0x200a0` (read), companion `stwx` writes at `0x50326028`/`0x50326094` to `r22 + 0x200b0` | guest 0x3000–0x0FFFFFFF **unmapped** (`main_unix.cpp:1604` maps only 0–0x3000; RAM at 0x10000000, `:192`). | **HARD GAP.** Even with SR/MSR stored, the *flat* access to 0x200a0 has nowhere to land. Needs a backing mapping (see §5/§6). |

**JIT side:** `ppc-jit.cpp` has **no** native cases for mtsr/mtsrin/mfsr/mfsrin/mtmsr (grep:
none) — they fall back to the interpreter, i.e. to the same `execute_illegal`. So the gap is
single-sourced in `ppc-execute.cpp`; fixing it there fixes both paths (with the standing
caveat that any new `powerpc_registers` field — `sr[16]`, `msr` — must be appended **last** and
needs a clean PPC recompile, per the stale-build footgun / MACHINE-LAYER-PLAN §2d).

---

## 5. M1 composition — does the minimal unblock collide with the bus?

No collision, and the fix is M1-shaped:

- **0x200a0 sits in the 0x3000–0x10000000 hole, immediately above the existing Low Memory
  mapping** (0–0x3000, `main_unix.cpp:1604`). MMIO bus regions are all at **0xF3xxxxxx**
  (`main_unix.cpp:1696` PROT_NONE reservation at `NATMEM_OFFSET+0xF3000000`; VIA at
  `0xF3016000`, `:1718`). **The hole and the bus do not overlap** — extending a scratch/low-RAM
  mapping to cover (say) physical `0x00000–0x20000` or `0x00000–0x100000` does not contend with
  the MMIO reservation or the NATMEM layout.
- This is precisely the **§2d "single special-cased low-memory mapping" carve-out class**
  (MACHINE-LAYER-PLAN §2d/§2e) — a *mapped* region kind, not softmmu, not a per-access probe.
  It composes with M1's `bus_read/write` model as either (i) an extension of the existing
  `vm_mac_acquire_fixed(0, …)` low-mem area to cover the needed physical low pages, or (ii) an
  M1 **aperture-kind** region backed by real scratch RAM. Either way: **zero hot-path cost,
  no 16 KB-page interaction, no W^X hazard** — the REDTEAM's killers (Claims 1–5) do not apply
  because nothing here emulates a *guest MMU via the host MMU*; we are just giving the flat
  address something to land on.
- The `is_mmio_or_managed`/range-check seam already exists in `main_unix.cpp` (`:988–1002`
  recognizes RAM/ROM/KernelData/HTAB ranges) — a low-mem scratch range slots into the same
  predicate.

---

## 6. Rung-ladder verdict + minimal-unblock recommendation

**Which rung unblocks this:** **Rung 2 (extended) + a low-memory mapping (§2d carve-out).**
Neither alone is sufficient; both are necessary.

1. **Rung 2 must grow to cover SRs and MSR**, not just the BAT/SDR1/SPRG it nominally names.
   - Add `uint32 sr[16];` and `uint32 msr;` to `powerpc_registers` (**appended last**).
   - Store `mtsr`/`mtsrin` into `sr[]`, return on `mfsr`/`mfsrin`; store `mtmsr` into `msr`,
     return on `mfmsr`. Same "store-the-write / return-the-read" pattern as the SPRG fix.
   - Cost: **S (hours)** for the storage; this is general PPC-correctness hygiene independent
     of New World. (HANDOFF §2.8 rung-2 row already calls this "the minimum prerequisite to
     advance past the 0x50326050 frontier" — this memo confirms and scopes it.)
2. **A backing mapping for the low-physical hole** so the `lwbrx`/`stwx` at ~0x200a0 lands.
   - Cheapest honest form: extend the low-mem mapping (or add an M1 scratch-RAM aperture)
     covering at least 0x00000–0x20100 (round to 0x00000–0x100000 for headroom).
   - Cost: **S (hours)**, M1-native (§5). This is the §2d carve-out, *not* rung 3+.

**Sufficiency is conditional — state it honestly:** Storing SR/MSR makes the handler's
read-backs *coherent* and gives the byte-reversed probe a place to land, so the routine will
**execute to completion** instead of SIGSEGV-ing. Whether the *value* it reads is **correct**
depends on whether `0x200a0`-virtual == `0x200a0`-physical under the SR/BAT the nanokernel
programmed (REDTEAM question D). 
- If **identity** (the V=P default, and what 9.0.4 demonstrably is): rung 2 + mapping crosses
  this wall cleanly; the DR on/off bracket is semantically a no-op and the flat read is right.
- If **non-identity** (a coarse mapping the handler depends on): the flat read returns the
  wrong page → rung 3 (pre-seed/HLE) or rung 4 (shadow-arena, ≥16 KB-aligned) — and *that*
  would be the first real datum flipping REDTEAM's "never unless proven" to "now."
- **This cannot be decided statically.** Do not budget for rung 3/4 yet; budget rung 2 +
  mapping, then *measure* (§7).

**Budget recommendation for M3/M5:**
- **M3/M5 must budget: ~1 day** — SR[16] + MSR stored state (S) + low-mem scratch mapping (S)
  + a clean PPC recompile + one diagnostic boot to observe the outcome.
- **Do NOT budget rung 3/4** until a probe shows a non-identity dependency. The deferral
  verdict (REDTEAM) stands; this wall is a *stored-state + mapping* wall, the cheap end of the
  ladder, exactly as §3.1's "pointer bug, not a translation bug" reframe predicts.
- **Sequencing vs M3:** the SR/MSR storage is independent of M3's exception-delivery rebuild
  and can land first as a correctness fix (it does not require `rfi`/SRR0-1). The full M3
  exception architecture is *not* a prerequisite for crossing this specific wall — this handler
  is reached and returns via the nanokernel's own dispatch (`0x503254e0 → 0x50312cb0`), not via
  a hardware 0x300 vector we must synthesize.

---

## 7. Unknowns remaining — probes a FUTURE sanctioned run should do

All are no-recompile probes (block-entry granularity). **Do not run these here** — they are
for a sanctioned boot session on the `newworld` profile. Named per the SS_PROBE_PC /
SS_SEED_MEM conventions (CLAUDE.md "PC Probes" / "Guest Memory Seeding").

1. **Confirm the faulting EA and the SR context** — the single most important probe; it
   resolves identity-vs-not (question D):
   ```
   SS_PROBE_PC=0x50326050:r20,r22,r23 ./SheepShaver --config <newworld diag prefs>
   ```
   - `r22` gives the faulting EA = `r22 + 0x200a0` (confirms whether EA ≈ 0x200a0).
   - `r23` is the saved MSR — check bit 0x10 (DR) and 0x20 (IR) to confirm translation state.
   - Cross with the SR programmed at 0x50325f64: add a probe at `0x50325f68:r21,r22,r24` to
     capture the saved SR (r21), the SR selector (r22), and the new SR value (r24).

2. **Is 0x200a0 identity?** With rung-2 storage landed, log the SR/BAT covering the segment of
   `r22+0x200a0` and compute VPN→RPN. If RPN of the translated 0x200a0 == 0x200a0 → identity →
   rung 2 + mapping suffices. Capture via watchpoint on the SR table and the BAT regs:
   ```
   SS_PROBE_PC=0x50326068:r22,r26  +  SS_JIT_WATCH_ADDR on the SR-table source (*(KDP-0x3fc)+0x30)
   ```

3. **What is supposed to be AT 0x200a0?** Once a low-mem mapping exists, watch the companion
   store path (`stwx` at 0x50326028 / 0x50326094, target `r22 + 0x200b0`) to see what the
   nanokernel writes into that low-physical descriptor — confirms whether it is a status byte,
   a vector mirror, or an MMIO-presented register. Probe:
   ```
   SS_PROBE_PC=0x50326028:r22,r27,r28  (the stwx-bracketed write)
   ```

4. **Does the handler get re-entered / loop?** This routine is a fault *handler*; if 0x200a0
   itself faults inside it (handler-in-handler), a single mapping fixes one level but a loop
   would signal a deeper nesting bug. Confirm with the visit counter in the `[PROBE]` output
   (logarithmic sampling) — a runaway visit count at 0x50326050 means re-entry.

5. **Sibling-routine coverage:** the same SR/MSR drop affects every routine in the
   0x325c00–0x326160 table (e.g. the BAT routine at 0x50325e7c, the classifier at 0x50325c3c).
   Once SR/MSR are stored, re-run the boot and watch for the *next* wall — likely one of these
   siblings or the DR-Emulator handoff at 0x5046f900 (HANDOFF §1.7 / §2.7.1). Budget for "this
   unblocks one of several coupled handlers," not "this is the last wall."

**Honest residue:** the value/meaning of the 0x200a0 descriptor, the identity-vs-non-identity
status of the nanokernel's mapping for it, and whether crossing this wall reveals a sibling
wall immediately behind it — all three are **untestable without a live boot** and gate the
final cost. The static analysis fixes the *mechanism* (SR/MSR dropped + hole unmapped) with
high confidence; the *sufficiency* is a one-boot measurement away.
