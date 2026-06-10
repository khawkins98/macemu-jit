# LEARNINGS — macOS ARM64 JIT work

Running log of non-obvious things learned while working on this fork.
Newest entries at the top of each section. Review at the start of each session.

---
**⭐ BOTTOM LINE — AltiVec (read this first; the entries below are the journey, incl. one wrong turn).**
Real-app AltiVec WORKS end-to-end as of 2026-06-07. The AArch64 JIT always compiles PPC AltiVec→ARM64
NEON (validated by `make test-jit`, 349/349). The only gap was *detection*: under the OldWorld 1.1 ROM
the guest never registers the `'ppcf'` gestalt, so apps ran scalar. Fix = **opt-in `altivec` pref**
(`prefs_items.cpp`; `SS_FORCE_ALTIVEC` env = dev override) → `emul_op.cpp force_altivec_idle_service`
registers `'ppcf'` via `_NewGestalt $A3AD` with the vector-feature mask **`0x10` = `1<<gestaltPowerPC`
`HasVectorInstructions` (bit 4 — NOT 0x40, that's the 64-bit bit; the 0x40 typo caused a multi-hour
wrong "FC ignores gestalt" detour)**. Verified: AltiVec Fractal Carbon detects AltiVec and runs its
vector kernel through the JIT (`SS_JIT_PROFILE` → `[JIT-COMPILED-MIX] AltiVec=160`). Opt-in/default-off
because 8.6/9.0 here don't VR-context-switch (single-app-safe). Caveats + roadmap: ROADMAP §B5;
`docs/planning/sheepshaver-research/ALTIVEC-DETECTION-RESEARCH.md`.
---

## 2026-06-10 (latest) — Batch harness: score-only equivalence passes vacuously when both modes share a bug

When adding batch mode to the opcode harness, the first implementation omitted the
FPR/VR/FPSCR/vrsave reset between vectors. Both legacy mode (per-process) and the broken
batch mode produced the same stale floating-point and vector register state — so their
REGDUMPs matched and `score=100` in both modes. A score-only equivalence check would have
declared the modes identical and shipped the bug.

**The lesson:** if both modes share a defect identically (same stale state, same wrong
output), comparing scores is vacuous — you need to compare REGDUMP *content* byte-for-byte.
The batch equivalence proof was rerun after adding FPR/VR/FPSCR/vrsave reset and confirmed
byte-identical content for all 350 vectors in both interp and JIT modes, plus a deliberate
corruption test to confirm the diff logic is live (`pass=349` on the corrupted vector).

**Rule for any future "equivalent to the reference" claim:** content equivalence is the bar,
not score equivalence. Score equivalence is necessary but not sufficient.

---

## 2026-06-10 (latest) — NK-boot ceiling root-caused; ignoresegv was masking a whole fault class

### Path A's nanokernel progress was partly an ignoresegv illusion

The M1 carry-forward ceiling (fidelity-profile boot dies at `pc=0x503123fc`,
`ea=0xffffffff`) was root-caused by a post-M1 spike (2026-06-10, commit d8932203). The
nanokernel page-descriptor build loop (ROM 0x3123a8–0x312424) reads its trip-count cap
from `KDP+0x6b4`. That field is un-seeded, so the cap was 0, which clamps the loop to ~16k
iterations. The stride-8 pointer walk at `KDP+0x80` overran the ~64 valid page-descriptor
entries into `0xFFFFFFFF` poison at `KDP+0x340` → faulting `stw r30,0(r8)` at
`0x5031240c`.

**The critical finding:** under the pre-M0 paravirtual path, `ignoresegv` was silently
skipping those faulting stores for the entire page-init stage. The trampoline-time seeding
was byte-identical before and after M0 (DEC and `[KDP-0x900]` were both checked and ruled
out as the seeding source). The fidelity profile's abort-loudly design (no ignoresegv on
`machine newworld`) exposed the fault immediately — working exactly as intended.

**Methodology rule — any pre-M0 "reached stage X" claim under ignoresegv needs
re-verification on the fidelity profile.** Any un-seeded field that the paravirtual path
never checked (because ignoresegv silently ate the fault) is a potential hidden wall.

**Fix:** trampoline-time seeding of `KDP+0x6b4` gets clobbered by the NK's own cold-init
zeroing. The correct fix is a ROM instruction patch (in `rom_patches.cpp`, gated on
`g_rom_904_lenient` + newworld profile): `lwz r8,0x6b4(r1)` at ROM 0x3123ac is replaced
with `lis r8,<ceil(phys_pages/0x10000)>` (cap=65536 pages for 256 MB RAM). Note: the fix
commit's subject line says "in NW trampoline" — the actual fix lives in `rom_patches.cpp`.

**New frontier:** boot now advances one full stage further, then SIGSEGV at
`0x50326050–0x50326068` — `lwbrx` of hardcoded physical address `0x200a0` (below RAMBase)
inside the NK's MMU/segment-fault handler (`mtdbatl/mtdbatu`, `mtsrin`, `mtmsr`
translation-toggling, byte-reversed PTE accesses). This is the genuine SR/BAT/supervisor-
environment wall — M3/M5 territory in the Machine Layer plan. The spike's stop-rule fired
correctly: the wall is now documented and root-caused, but crossing it requires real
exception-delivery infrastructure, not a one-liner.

Probe logs for this investigation: `/tmp/nk-probe.out`, `/tmp/nk-run3.out` (ephemeral).

---

## 2026-06-10 — Machine Layer day: pivot → architecture → spikes → M0 landed

The full arc in one day: strategic pivot (Path A/B → **Machine Layer**, `docs/planning/MACHINE-LAYER-PLAN.md`),
2 adversarial review rounds (21 findings, §8), 3 de-risking spikes (`docs/planning/spikes/`), M0
implemented subagent-driven in a worktree, merged back. Tag **`pre-machine-layer`** (=46e497d8) is
the retreat point. Non-obvious learnings:

### Device identities flip-flopped a THIRD time — r18=VIA, r19=SCC
Spike S3 disassembled the actual stall loop: **0xF3016000 is the VIA 6522** (matches our own
AddrMap patch!), 0xF3012000 the SCC. The earlier "r18=SCC ch A" claim (below, and in pre-rewrite
SYSTEM-BOOT-GATES §5) was wrong. Rule reaffirmed: never label a device base without disassembling
the consumer — we have now mislabeled SCC/VIA in three separate sessions.

### Gate 2 ($63) is a CFM boot-fragment audit, not a model check
Spike S1 (QEMU mac99 oracle): the 0x7E24 probe Gestalt-selects a checklist then verifies CFM boot
fragments (DebugLib/InterfaceLib/...) that only **parcels ROMs** provide. Unpatched 9.2.1 boots to
Finder on the 9.0.1 ROM under QEMU. Identity patches never had a chance; 9.2-on-1.1 is structurally
impossible. The "version words" in the gate doc were resource IDs.

### Mach-exception MMIO is viable but ~8.5 µs/fault — backpatch is mandatory for hot sites
Spike S2 (spikes/s2-mach-fault-decode/): PROT_NONE trap → decode JIT-form LDR → thread_set_state
inject → resume works first-try, identically from MAP_JIT pages, zero entitlement friction. But
~10^4× a mapped access: a MHz-rate poll through the fault path is unusable — hot MMIO sites must be
JIT-backpatched to direct bus calls.

### SS_TEST_HEX bypasses init — don't use test-opcodes to verify startup logs; also masks MMIO false-passes
The SS_TEST_HEX early-exit returns before PrefsInit, so `make test-opcodes` can never show startup
lines like [MACHINE]. Verify init-time behavior with a <6s isolated-config run
(`perl -e 'alarm 6; exec ...'` — plain `timeout` is not on this box).

**M1 extension:** SS_TEST_HEX also bypasses MMIO bus registration, so any test vector whose
opcode happens to store/load at a device-range address (0xF3xxxxxx) will NOT fault into the bus —
it will either hit unmapped memory (Mach-fault eaten by sigsegv) or, on paravirtual, never reach
bus code at all. A vector that "passes" under `make test-jit` with an MMIO-range operand proves
nothing about the bus dispatch path. Bus correctness requires a proper boot path or an isolated
bus unit test — not a single-opcode harness run.

### Mach-O strong-overrides-weak-symbol pattern for standalone JIT unit tests (M1)
`test_mmio_machfault.cpp` needed the JIT's generic thunk pointer
(`g_mmio_backpatch_thunk`) without linking the full JIT. The solution: declare it `__attribute__((weak))`
in the emulator build (the no-op default) and provide a `__attribute__((visibility("default")))` strong
definition in the test binary. Clang/ld64 resolves strong over weak at link time — the test binary
gets its own definition, the emulator binary gets the real thunk. Rule: **weak stubs + strong test
overrides** is the correct pattern for any JIT-internal state a unit test needs to observe without
linking the full codegen engine. Do NOT use `extern` declarations that require linking the real object.

### M0 behavior change: SS_NW_TRAMPOLINE=1 now implies the fidelity profile
The deprecated alias selects `machine newworld`, which ALSO disables the legacy serial-skip hacks
and ignoresegv. Pre-M0 diagnostic behavior is only recoverable via the `pre-machine-layer` tag.
Reconciliation note: DEPRECATED-SCAFFOLDING-INVENTORY.md.

---

## 2026-06-10 — System file gate anatomy; DSAT; binary search on disk; SCC stall identified

> **⚠️ CORRECTED 2026-06-10 (same day):** two claims below are superseded — (1) r18 is the
> **VIA 6522**, not "SCC channel A" (SPIKE-S3); (2) the stall's root cause is **A-line vector
> ($28) corruption from a Memory Manager free-list bug**, not SCC polling — the serial monitor
> is where the corrupted vector *lands* (SYSTEM-BOOT-GATES §5 rewrite, commit 46e497d8).
> Gate anatomy/DSAT/binary-search methodology below remains valid.

### Post-splash stall is SCC hardware polling in ROM serial init

SS_PROBE_PC at the HOT-PC (0x50484038) revealed the 68k PC stabilizes at 0x500cc998 — ROM
code polling the SCC (Serial Communications Controller). Key register state at stall:

| Register | Value | Meaning |
|----------|-------|---------|
| r24 | 0x500cc998 | 68k PC (ROM offset 0xcc998) |
| r18 | 0xF3016000 | SCC channel A hardware address |
| r19 | 0xF3012000 | SCC channel B hardware address |
| r29 | 0x50484038 | DR handler table address |
| r30 | 0x50460000 | DR code base |
| r31 | 0x68FFF000 | ECB |

68k code at 0x500cc998: `btst.b #$11, d7; beq.b $500cc9c0` — tests SCC status, loops back.
Also `btst.b #$0, $2(a3)` at 0x500cc99c reads the SCC status register directly.

The stall is System 9.2.1's serial initialization polling SCC hardware that SheepShaver
doesn't fully emulate. The v1.1 ROM's serial init code was adequate for 9.0.4 but 9.2.1's
init sequence hits a polling path that never returns.

**ISO discriminator confirms ROM-structural**: both HD and ISO boot stall at the same SCC
polling point (identical HOT-PC, identical register state). The stall is not disk-related.

Options: improve SCC emulation in serial.cpp, patch ROM serial init at 0x500cc998, or
intercept the SCC I/O address in the memory map.

### Gate 2a ($66 at 0x03CA) fires on CD boot but NOT HD boot

ISO boot revealed a 4th gate: `btst #2,$0B20; beq.s +6; moveq #$66,d0; _SysError` at
boot id=3 offset 0x03CA. On HD boot the bit test passes (bit is clear, gate does not fire).
On CD boot the bit is set and the gate fires, displaying the $66 "won't work on this model"
dialog. This makes the full gate count FOUR for CD boot, THREE for HD boot.

### Mac OS 9.2.1 has THREE boot-time compatibility gates, not two (HD); FOUR for CD

Previous entry (2026-06-09) identified two barriers. The actual anatomy of the System file's
`boot` resource id=3 (55,048 bytes of 68k code, the main startup sequence) reveals three
distinct gates — all in the same resource, all using `_SysError` (A9C9 trap):

| Gate | Error code | DSAT text | boot id=3 offset | Mechanism |
|------|-----------|-----------|-------------------|-----------|
| 1 | (installer) | — | — | CFM launch-time check in Installer app (not in System) |
| 2 | $63 | "The System file on this startup disk may be damaged" | 0x0404 | `jsr` to subroutine at ~0x7E24, test result, `moveq #$63,d0; _SysError` |
| 3 | $76 | "...only functions on the original media, not if copied to another drive" | 0x70a4 | `tst.b; bne.s +10; moveq #$76,d0; _SysError` |

Gates 2 and 3 are independent — each fires regardless of the other. NOP-ing both (4 bytes
total on disk: replace A9C9 with 4E71 at each offset) lets boot proceed to the Mac OS 9.2
splash screen. A post-splash stall remains (separate issue — likely deeper ROM/System init
mismatch).

The DSAT error-code-to-text mapping was the key breakthrough: once we parsed the DSAT
resource header, we could identify $76 as the "original media" code and confirm it has
exactly one call site in boot id=3. Previous attempts failed because we were binary-searching
A9C9 calls by _offset_ without knowing which _error code_ to target.

**Note on error $63:** the DSAT text for $63 is "System file may be damaged," NOT the
"won't work on this Macintosh model" text (which is error $66). The actual dialog shown
uses $63 but displays as a model rejection. This may mean the dialog text is overridden by
the subroutine at ~0x7E24 before _SysError renders it, or the DSAT lookup has a fallback.
The empirical observation is clear: NOP at 0x0404 suppresses the model-rejection dialog.

### DSAT (Deep System Alert Table) resource format

DSAT id=0 in the System file maps `_SysError` error codes to dialog text. Format:

```
Bytes 0-1:    uint16 count (number of header entries)
Bytes 2+:     header entries, 14 bytes each:
                uint16  error_code
                12 bytes  {unknown fields, includes a b1XX text-marker ref}
After header: text entries, each:
                uint16  marker (0xb1XX)
                6 bytes  display params (length, coordinates?)
                variable-length text (null-terminated, '/' = line break)
```

The header's `b1XX` field links to the matching text entry. Parsing: iterate header entries,
extract (error_code, b1XX marker), then scan text section for matching markers.

Error codes found in the 9.2.1 DSAT (22 entries):
$7fff=default ("Sorry, a system error occurred"), $63, $66, $68, $69, $74, $76, $78, plus
14 more. Full mapping in `docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md`.

### Binary search methodology for disk-level patching

Finding which A9C9 call triggers a specific dialog required a multi-step approach:

1. **NOP all A9C9 on entire disk** (1354 instances) → confirms dialog is mediated by A9C9
2. **Restore by resource** — selectively restore A9C9 bytes in specific boot resources to
   narrow which resource contains the trigger
3. **Catalog error codes** — disassemble the `moveq #$NN,d0` preceding each A9C9 to build
   an (offset → error_code) map
4. **Parse DSAT** — find which error code maps to the target dialog text
5. **NOP the specific site(s)** with that error code

Pitfall: step 3 alone wasn't enough because some A9C9 calls use dynamic error codes (table
lookups via `move.b (a0,d0.w),d0` — see boot id=3 offset 0x5d5c). The DSAT parse (step 4)
was necessary to close the loop.

Pitfall: HFS disks contain MULTIPLE copies of each resource (allocation artifacts). Only the
"live" copy matters. The live boot id=3 starts at disk offset 0x18f39748 on pathB_upgrade.dsk.
Other copies at different offsets have different internal layouts and patching them has no effect.

### The post-splash stall is SCC serial polling (identified)

After bypassing both gates, boot shows the Mac OS 9.2 splash (Happy Mac + "Mac OS 9.2") but
stalls in a tight loop (~2M blocks/s, no new compilations, HOT-PC at 0x50484038). SS_PROBE_PC
revealed the 68k PC at 0x500cc998 — ROM serial init code polling SCC hardware at 0xF3012000
(channel B) and 0xF3016000 (channel A). The stall is NOT gate-related (persists with ALL A9C9
NOP'd) and NOT disk-related (identical on HD and ISO boot). It's SCC emulation incomplete for
9.2.1's serial init requirements.

---

## 2026-06-09 part 2 — DR Emulator entry wall falls; stop-rule revised

**The DR Emulator entry wall fell to a TRIVIAL fix**: promoting 4 existing KDP field writes
from a nested diagnostic guard (`SS_NW_SYNTH_ENTRY`) to the outer `SS_NW_TRAMPOLINE` block.
No new code needed — the writes existed but were unreachable on Path A.

**The `jump68k` redirect ALREADY WORKS for NewWorld** via `parcels_rfi_dat` pattern (Path A:
`mtctr;bctr;nop`). The `SS_ROM_SKIP_JUMP68K` env var was a diagnostic bypass that PREVENTED
it from running. Dropping that flag lets the full dispatch chain work.

**`patch_68k_emul()` runs for all ROM types** and writes the DR Emulator entry stub at
ROM+0x36f900. Not gated on ROM type.

**Boot now reaches DR Emulator** (region 50460000 compiles), then crashes at PC=0x72bf0000 —
nanokernel register-restore path loads garbage from ECB. Next wall: dispatch table or
cold-start init setup in `patch_68k_emul()`.

**Stop-rule revised**: The original closure ("remaining work = 84-shim byte-pattern porting")
was wrong. The DR Emulator wall fell without any byte-pattern porting. New discipline: keep
advancing as long as each wall costs <1 day and the forcing function yields insights.

Methodology validation: the "promote existing code" pattern keeps recurring — scaffolding
written for one diagnostic path turns out to be exactly what the forward path needs, just
gated too tightly.

---

## 2026-06-09 — SegMap spike confirms CreateAreasFromPageMap theory; NW forcing function closed

**KDP+0x80 corruption is real but bypassable**: The SegMap pointers at KDP+0x80 ARE corrupted
during the page-init loop (correct value 0x68FFE920 after NKInit copy, corrupted to 0x0000FFFF
before CAFPM entry). The exact store was not identified — likely an indirect store through a
runtime pointer in the page-init function (0x503214fc). Bypassed by writing fresh data
immediately before CAFPM via a PPC stub.

**KDP-0x900 = VIA base, not a work queue**: Previous comment in sheepshaver_glue.cpp said
"NOT a work queue." Confirmed by RE: it's the 6522 VIA physical base address. The nanokernel's
SchIdleTask polls VIA IFR (offset +2) for timer-1 interrupt. A fake mapped page with IFR bit 0
set lets the idle loop exit.

**Stop-rule fires correctly at DR Emulator boundary**: The 68k handoff requires
reverse-engineering NW-specific entry points and porting 84 HLE shims
(PATCH-68K-SHIM-INVENTORY.md) — pure ROM-specific work with no general emulator payoff. The
lateral-thinking audit confirmed: DR Emulator is the same PPC code as OldWorld (just at a
different ROM offset), so exercising it adds no new JIT coverage. The supervisor-mode
correctness vein (SPRG, fctiw, SDR1, HTAB, page-descriptors, SegMap, VIA) is mined.

**Methodology validated**: The "spike before full implementation" approach (option 2 from the
prior session) worked perfectly — 34 PPC instructions, 1 hour to write+test, conclusive
hypothesis confirmation. Compare to the multi-hour corruption hunt that preceded it. The
advisor's "stop hunting, the spike doesn't need it" guidance was correct.

**Encoding verification by disassembly is essential**: Every hand-assembled PPC instruction in
the stub was verified by dumping the patched ROM and running capstone. No encoding errors. This
is the methodology: write, dump, disassemble, test. Don't trust hand-math alone.

---

## 2026-06-09 — Parcels handoff is scheduler-dispatched; redirect correct but unreachable

**Path A redirect works at the RE level** — the rfi block at ROM+0x3126cc (mtsrr0;mtsrr1;rfi
→ mtctr;bctr;nop) is correctly patched and test-jit passes 350/350. But it's **never reached**:
the parcels nanokernel doesn't jump to 0x3126cc sequentially from init. It dispatches the 68k
handoff via the **blue-task scheduler** (ready queues at KDP-0x9f0/-0x9d0/-0x9b0/-0x990).

Evidence: `SS_LOG_FIRST_BLOCKS=100000` shows 396 unique blocks, NONE in 0x312200-0x3126ff. The
init flow goes 0x310000 → 0x3121d4 → 0x322xxx (deep init) → idle loop at 0x5032751C. The handoff
block only fires when a boot task is enqueued in the NominalReadyQ — which is done by the
**Trampoline bootloader** that runs on real hardware BEFORE the nanokernel.

SheepShaver bypasses the Trampoline entirely (it enters the nanokernel directly), so the ready
queues are empty and the scheduler never dispatches the handoff. Same pattern as SPRG0: the
emulator skips a hardware-setup phase that seeds the runtime.

**[KDP-0x900] is VIA base, not a work queue.** Previously set to 1 thinking it was a dispatch flag.
The nanokernel's Thud console (0x3263fc) loads this and if non-zero, enters CUDA/VIA I/O that hangs
without real hardware. Setting to 0 = skip VIA. This was the cause of the comp=629 CUDA-loop hang.

**Stop-rule fires:** no general emulator fixes banked — all changes (redirect, VIA fix, I/O poll
hoist, lenient gate) are parcels-specific. Next wall: Trampoline/scheduler emulation to populate
boot-task queues.

---

## 2026-06-08 — Path B (SS_NW_SYNTH_ENTRY) is a dead end; Path A is the forward path

**Path B** (skip nanokernel, enter DR Emulator directly) hits a fundamental wall at the first
Mixed-Mode transition. The parcels ROM's 68k init dispatches PPC modules via `_MixedModeMagic`
(F-line 0xFFC0), which requires the **nanokernel's F-line handler** to inspect the routine
descriptor and mode-switch to PPC. Without a nanokernel, 0xFFC0 traps to VBR+0x2C → RTE →
re-executes 0xFFC0 → infinite loop. Implementing a synthetic Mixed-Mode Manager would be
open-ended (recurs on every native transition throughout boot/runtime) with no prior art.

**Path A** (jump68k redirect in the nanokernel) is the forward path: direct prior art at
`rom_patches.cpp` ~1006 (`lwz r3,EmulatorData; lwz r4,opcode-tbl; mtctr; bctr`), bounded
scope (one patch site), and the nanokernel already boots under our JIT (proven via
`SS_ROM_SKIP_JUMP68K=1` — 160s+ stable idle, 0% crash). Candidate redirect sites in the
parcels ROM: 0x3126dc (clearest SRR0/SRR1 setup before rfi).

**What transfers from Path B to Path A:** KDP field seeding (ECB, decode loop, dtable,
UserModeMSR), exception vector stubs, run_diags fallback, all DR Emulator knowledge. The
cold-start dispatch ROM patch at 0x310000 is Path-B-only. Code is annotated for eventual
removal.

---

## 2026-06-08 — DR Emulator internals decoded + r24 corruption investigation

**DR Emulator decode loop (from decompressed 9.0.1 parcels ROM disassembly).**
Handler address formula: `handler = 0x50480000 + opcode * 8`, computed by
`rlwimi r29, r27, 3, 0xD, 0x1C` (MB=13, ME=28, mask `0x0007FFF8`). Initial misread was MB=6 —
produced wrong addresses like `0x500277D0`. Five decode-loop variants identified:
(1) standard (0x366080: `lhau/rlwimi/mtlr/lhau/bgelr cr2`),
(2) JMP continuation (0x367C64: adds `mtctr r23 + bgtctr cr1`),
(3) BRA.L continuation (0x367CA4: `lwz+lhaux` for 32-bit displacement),
(4) Bcc taken (0x367F60),
(5) Bcc not-taken (0x367F40).

**CR1.GT = interrupt-pending flag.** Set via `crand cr1gt, cr1un, cr6eq` at 0x5036D928 (requires
CR1.UN set first). Cold-start clears all CR1 via `mtcrf 0x7f, r0(=0)`. Only FP Rc=1
instructions set CR1 in PPC — integer Rc=1 sets CR0 (common misidentification; see methodology
note below).

**ROM mirror:** ROM[0x300000..0x400000] copied to [0x400000..0x500000] at runtime. PC-relative
branches in mirrored code resolve to +0x100000 offset addresses.

**ECB handler table:** 151 entries at ECB+0x800 to ECB+0xA5C, built from halfword lookups at
ROM+0x3DC44, OR'd with r30=0x50360000.

**r24 corruption — bgtctr cr1 hypothesis DISPROVEN.** Probe at PPC address 0 never fired
(0 visits). No instruction sets CR1.GT in the early 68k init path — cold-start clears CR1,
and the only setter (`crand cr1gt, cr1un, cr6eq`) requires CR1.UN which nothing sets after
the clear. So `bgtctr cr1` never fires during early init. Wasted investigation time on this
theory.

**Corruption window localized.** r24 valid at ~0x5000AD8A (decode loop visit ~10), then goes
wild to `0xE000280C`. r1 (68k SP) stuck at 0x28 (in exception vector table). First BRA.L
works correctly — MOVEA.W handler probe confirmed r24=0x5000AA12 (0xAA10 + 2 from decode
advance).

**~~Prime suspect: SR/BAT/MMU translation~~ — DISPROVEN. Actual root cause: missing HLE shims.**
The 9.0.1 parcels ROM has a **machine-init module dispatcher** at 0xAD7C (new code — the 1.1
ROM has FPU detection at the same address). It walks a table at ROM+0xE184; entry[0] points
to 0x502FD140, which is a **data header** (not code). First word 0xFFC0 = F-line trap →
exception vectors through `[VBR+0x2C]=0` (uninitialized) → execution at address 0 → zeros
decode as ORI.B → recursive A-line/F-line exceptions → r24 sweeps to 0xE000xxxx through
garbage stack frames. This is NOT a JIT/register/MMU bug — it's the expected consequence of
running parcels 68k init without `patch_68k()` HLE shims (which set up exception vectors +
intercept init before the dispatcher runs). **Fix = Phase 2 HLE porting or exception-vector
stubs.**

**Investigation efficiency note:** the SR/BAT/MMU hypothesis cost several probe runs before
being disproven. The actual diagnostic was **tracing the 68k instruction flow** (disassembly +
per-handler probes at computed addresses `0x50480000 + opcode*8`), not register-state
inspection. When the DR Emulator is involved, trace the 68k instruction stream, not the PPC
registers.

**⚠️ Methodology: integer Rc=1 sets CR0, NOT CR1.** Only floating-point Rc=1 sets CR1. An
initial analysis script falsely flagged integer Rc=1 (e.g. `subf.`, `subfe.`) as "FP Rc=1
(sets CR1)" — always verify the instruction class before reasoning about CR field effects.

**⚠️ DR Emulator dispatches per-encoding, not per-instruction.** JMP with addressing mode
`(An)` (opcode 0x4ED0) is a different handler than JMP with `(d8,An,Xn)` (opcode 0x4EF0).
Probing "the JMP handler" means probing the specific encoding's handler. Missing this wasted
a probe run that showed 0 hits for JMP/JSR/RTS/RTR despite the actual wild jump being a JMP.

## 2026-06-08 — New World parcels boot: MMU/page-table wall broken, three harvests

**Sixth harvest — 32-bit address space wrap-around in mem_search (infinite loop).** In `ss_rpc_handle_mem_search`, clamping `end_addr` to `0xFFFFFFFC` to avoid overflow is insufficient if the loop iterator `addr` is a `uint32_t` and the check condition is `addr + 4 <= end_addr`. When `addr` reaches `0xFFFFFFFC`, `addr + 4` wraps around to `0` (which is `<= end_addr`), resetting the search back to the start and causing an infinite loop. **Fix:** Use `uint64_t` for the loop iterator `addr` to cleanly bypass 32-bit overflow limitations and terminate successfully.

**Third harvest — [KDP-0x20] IRP pointer never set (memory-layout gap).** The nanokernel's free-list
bank scan reads bank entries from an Info Record Page (IRP) base stored at `[KDP-0x20]`. The skipped
cold-init normally sets this to `KDP - 0xA000`. Without it, `[KDP-0x20]=0` → bank scan reads guest low
memory (all zeros) → no pages found → `r22=0xFFFFFFFC` → effectively infinite mapping loop.
**Fix:** seed `[KDP-0x20]` in the trampoline + write bank entries at `IRP+0xDF0/DF4`.

**Fourth harvest — `desc_create` ROM patch kills the free-list store.** SheepShaver patches
`stwu r31, 4(r29)` (the instruction that stores page descriptors into the free list) to NOP — correct
for OldWorld flat-addressing, but kills the NW nanokernel's page management. **Fix:** skip this NOP
under `g_rom_904_lenient` (env-gated `SS_NW_TRAMPOLINE` path only).

**Fifth harvest — page descriptors grow UPWARD from KernelMemoryBase.** The free-list builder writes
descriptors via `stwu r31, 4(r29)` starting at `KernelMemoryBase - 4`, growing upward. For 256MB RAM
= 65536 pages × 4 bytes = 256KB. If KernelMemoryBase is too close to KDP (original gap was 56KB), the
descriptors overwrite the sub-KDP pool, IRP (bank entries), KDP itself, and HTAB. **Fix:** lower
KernelMemoryBase to `sub_kdp_base - pgdesc_size` (256KB below the sub-KDP pool), giving room for all
descriptors.

**⚠️ NewWorld CHRP ROMs are decompressed — the ROM file bytes don't match guest memory.** The `.rom`
file is CHRP-compressed. SheepShaver's `rsrc_patches.cpp` decompresses it into the 5MB ROM area
(`0x50000000-0x50500000`). Disassembling the raw `.rom` file yields COMPLETELY WRONG code — the
offsets don't map and the instructions are different. **Always dump the decompressed ROM from guest
memory** (now productized: `SS_DUMP_ROM=/path ./SheepShaver` writes the post-patch image at startup) and disassemble that.
An Opus 4.6 subagent that disassembled the compressed file produced an entirely fabricated analysis
(the addresses and register fields were plausible but the instructions were wrong). This wasted a
full investigation cycle before the error was caught.

**Result:** nanokernel now reaches its idle loop (568 compiled blocks, 153M blocks/s, `[KDP-0x900]`
poll). The entire Init.s → Reset.s → bank-scan → page-descriptor → mapping flow completes. The next
wall is the PPC→68k handoff (`jump68k`), which is a fundamentally different class of problem. Three
general-correctness fixes from this work: BAT register storage, IRP/page-descriptor layout, and the
`desc_create` patch skip.

## 2026-06-08 (earlier) — New World parcels boot: SPRG bug, sub-KDP memory gap, and workflow footguns

**Strategy (maintainer-confirmed):** the goal is **PPC/JIT correctness**, not 9.2 per se. SheepShaver
models only a thin slice of the PPC supervisor stack; the New World ROM / 9.2 boot is the *forcing
function* that drags those gaps into the light. Treat the **bugs as the product, the boot as the
driver, the environment plumbing as the test harness**. Keep advancing the boot while it surfaces
general fixes. (Full policy + the over-conservative "stop now" it replaced: NEW-WORLD-ROM-SUPPORT-PLAN.)

**First real harvest — SPRG0-3 were dropped (general bug).** `mfspr`/`mtspr` for SPRG0-3 (272-275) were
silently dropped (writes ignored, reads→0). Wrong for any OS; just never exercised until the parcels
nanokernel kept its per-CPU/KDP pointer in SPRG0. Fixed (real `sprg[4]`); `test-jit=100`. Method that
found it: `SS_LOG_FIRST_BLOCKS=N` (first-N block-entry PCs = the boot path) + a one-shot lock-state
dump + `SS_JIT_VERIFY` (clean → not codegen). The deadlock was a spinlock whose lock addr came from a
garbage KDP (`r1=0xfcffffff`).

**Second harvest — sub-KDP pool region unmapped (memory-layout gap).** The nanokernel's heap/pool
allocator initializes a free-list at `KDP - 0x7000` (guest `0x68FF7000`). `KERNEL_AREA_SIZE` is only
`0x2000` (8 KB), so the shmem mapping covers `[KDP-0x2000, KDP+0x2000)` after SHMLBA alignment.
Everything below `KDP-0x2000` is **unmapped** → pool init's `stw` stores silently fault
(`ignoresegv=true` default → `SIGSEGV_RETURN_SKIP_INSTRUCTION`) → pool data structure is never
written → allocator reads garbage metadata → zeroing loop computes a multi-GB size → infinite stall.
**Fix:** `vm_acquire_fixed` 32 KB below the shmem base (`SS_NW_TRAMPOLINE` gate, OldWorld untouched).
**VERIFIED** (probe present during both before/after runs): **10 faults → 0 faults**, pool stores land.
Boot advances 128 → 265 unique PCs; pool-init, zeroing loop, allocator, and HTAB/page-table init all
complete (410 compiled blocks). See `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md`.

**⚠️ Methodology: verify faults empirically before fixing.** The advisor correctly blocked the fix
until I instrumented the SIGSEGV handler — the symptom ("stores don't land") had two possible
mechanisms: (a) unmapped memory → fault → skip, or (b) memory IS mapped but stores go somewhere
unexpected. One-shot `[SEGV-SUB-KDP]` log in `sigsegv_handler` definitively confirmed (a): 10 faults
in `[0x68FF5000..0x68FF7000)`, all at ROM PCs. Without this, the fix would have been built on an
unverified theory (cf. session-5 retraction, the a46cda99 debacle).

**What the fix unblocked — and the next wall.** With pool stores landing, the nanokernel advances
through its complete init: allocator (0x50326440), pool-init (0x50322784), zeroing (0x50322990),
serial debug output (the 0x50326xxx PCs are a char-by-char print routine with serial port at
KDP-0x900=0, I/O skipped), SR loading (all 16 SRs), BAT loading (all 8 BAT pairs + 7400 extended) —
**373 unique PCs**, 410 compiled blocks. At block 14000, the nanokernel reads **SDR1** (SPR 25,
`mfspr r8, 0x19` at 0x50311fd4) to locate the HTAB. Gets our sentinel `0xdead001f` → computes HTAB
base=0xDEAD0000, size=2 MB → zeroing loop at 0x50311ff4 for 524K iterations. Entire range unmapped →
~500K SEGVs at ~4K/s → effectively permanent stall. This is the **SDR1/HTAB wall** — the exact wall
predicted by the handoff doc's §3 rung-ladder. Fix: implement SDR1 read/write (general correctness —
qualifies under forcing-function rationale) + map a backing region for the HTAB.

**Two process footguns (don't relearn):** (1) changing `ppc-registers.hpp` needs ALL PPC TUs
recompiled — the Makefile doesn't track that header dep → stale `.o` = silent struct-offset mismatch =
`test-jit=0` (looks catastrophic, isn't; `rm obj/ppc-*.o obj/sheepshaver_glue.o obj/ppc-jit.o`).
(2) The JIT **hardcodes** byte offsets into `powerpc_registers` (e.g. `PPCR_RESERVE_VALID=1060`), so
new struct fields MUST be appended LAST. See memory [[stale_build_struct_offsets]], [[newworld_parcels_port]].

**Workflow footgun: can't override ROM path from the command line.** SheepShaver has no `--rom`
flag — the `rom` pref is read from `~/.sheepshaver_prefs` (or `--config <file>`). To test a different
ROM, either edit the prefs file or create a separate prefs file and use `--config`. The diagnostic
prefs recipe: `printf 'rom /path/to/rom\nramsize 268435456\nnogui true\n' > /tmp/trace.prefs`. Then
`SheepShaver --config /tmp/trace.prefs`. This is not documented anywhere obvious — agents waste time
trying `--rom` flags that don't exist. See ROADMAP improvement item.

## 2026-06-07 — Early-boot dead-ends were invisible in the log → pre-idle boot-stall watchdog ([ALARM])

**Symptom the user kept hitting:** boot a ROM/OS that fails early (the NewWorld "This startup disk
will not work on this Macintosh model" alert) and the log shows ~0.2s of `[JIT … first compile …]`
then **silence** — so you wait on a GUI screen, then screenshot, then realize it failed. Twice the
user asked "why so long?" / "do we not have diagnostic tooling to see this in the logs?"

**Root cause (confirmed from the actual log, not theorized):** every higher-level boot signal —
`[BOOT]`/`[APP] modal`/`[SYSV]`/`[READY]` — rides the `SynchIdleTime` idle hook, which only fires
once the guest reaches Process-Manager idle. A wedged guest **never reaches idle**, so the idle hook
never runs and those signals never emit. Grepping the failed run proved it: **zero** `[BOOT]`/`[APP]`
lines, but **16,350 `[HB]`** heartbeats at ~150M/s, 99.99% in `jDR` (68K emulator), `comp` frozen.

**Why the obvious fix (read WindowList and name the dialog) does NOT work here:** the model-rejection
screen is a **ROM-level DSAlert drawn BEFORE the System boots**. At that point `WindowList` (0x9D6)
reads `0xffffffff` and `CurApName` is junk — the WindowManager doesn't exist yet, so the Toolbox can
**never** name this screen. Verified: probe emitted `win=0xffffffff kind=-8112` while the dialog was
visibly on screen. (Contrast: *later* modal prompts — disk-repair, rebuild-desktop — DO populate
WindowList and are nameable; the watchdog names those.)

**The fix is a WATCHDOG keyed on the failure SHAPE, not the screen content** (the user's insight:
"a few hundred ms of jit activity then nothing should be enough to trigger an alarm"). The shape is
signal-independent: blocks spinning fast + no new compiles + idle never reached = dead-end.
`ss_boot_stall_check` (`emul_op.cpp`), driven by the **host-side JIT heartbeat** (which keeps ticking
through the wedge — the one carrier that survives it), raises `[ALARM]` at ~15s and re-states
`[STALL]` every 30s. Scoped strictly **pre-idle** + self-disarms at `[BOOT] idle` + re-arms on compile
progress → does **not** repeat the forbidden post-boot "same-PC = hang" mistake (session-5 retraction).
`SS_BOOT_STALL_SECS` tunes it (default 15; 0 off). Full reference: `SheepShaver/docs/DIAGNOSTICS.md`.

**Methodology that worked:** the advisor blocked the build until I grepped the EXISTING failed-run log
— which decided whether *any* code was needed (it confirmed the gap AND that the heartbeat carrier was
alive). Then it required emitting RAW readings (`win=`/`kind=`) so one boot proved the WindowList read
was junk rather than assuming it. Don't build diagnostic tooling on an unchecked theory when the
disproving log is already on disk. (See memory [[gui-outcomes-not-in-log]], now updated.)

**Path B (SS_NW_MODEL) result:** injecting a NewWorld device-tree identity (`model="PowerMac3,1"` +
`compatible`) on the 1.1 ROM is **insufficient** to boot 9.2 — the watchdog confirmed it still wedges
at the model-rejection DSAlert. 9.2 needs the real NewWorld ROM environment (Path A: re-RE
`patch_nanokernel_boot` for the parcels layout). The `compatible` injection is kept default-off as
groundwork (needed alongside Path A, not sufficient alone).

## 2026-06-07 — AltiVec FORCE WORKS: 'ppcf' is UNREGISTERED (8.6 AND 9.0); we register it ourselves

Big progress on the AltiVec de-risk (task #26), user-authorized live boots (isolated config, VNC, clean
shutdown via SIGUSR1, always a fresh disk **copy**). Method: an env-gated `SS_FORCE_ALTIVEC=1` one-shot
in `OP_IDLE_TIME` (`emul_op.cpp` `force_altivec_idle_service`) that reads/forces the gestalt from host
code via `Execute68kTrap`.

**Decisive finding — System version is NOT the gate.** Under the working OldWorld 1.1 ROM,
`Gestalt('ppcf')` (gestaltPowerPCProcessorFeatures) returns **gestaltUndefSelectorErr (0xEA51) — the
selector is NOT REGISTERED — under BOTH Mac OS 8.6 (`sysv`=0x0860) AND Mac OS 9.0 (`sysv`=0x0900)**
(the `sysv` control reads correctly, so the probe is sound). So the long-standing "vector bit is clear"
framing was wrong: there is no bit — the selector doesn't exist. (Caveat: the 9.0 disk is the e2e
*mini-boot*, a stripped install; a full 9.x install MIGHT ship the component that registers it. But the
likely root cause is that SheepShaver's OldWorld environment never advertises a vector unit.)
**Method note:** the `macos921.dsk` asset is mislabeled — it's actually Mac OS 8.5/8.6 (raw-scan: 327×
"8.5", 0× "9.x"). The real Mac OS 9.0 disk is the **e2e harness's** `e2e-macos9-mini-boot.dsk` (boots
under the 1.1 ROM). *Follow the e2e config for assets.*

**WORKING FORCE.** Since there's nothing to flip, we REGISTER 'ppcf' ourselves:
- `NewPtrSysClear` ($A71E) a 32-byte block; write a tiny **68k Gestalt SelectorFunction** (Pascal ABI:
  `pascal OSErr fn(OSType, long*)` — on entry `4(sp)`=response, `8(sp)`=selector, `12(sp)`=result word;
  write `*response=0x40`, result=noErr, then Pascal-return dropping 8 bytes of params).
- `NewGestalt` = trap **`$A3AD`** (NOT `$A0AD` — the Gestalt family uses trap-word bits 9-10 to pick the
  op: `$A1AD`=Gestalt, `$A3AD`=NewGestalt, `$A5AD`=ReplaceGestalt, `$A7AD`=GetGestaltProcPtr. `$A0AD` is
  just `_Gestalt` with bit-8/auto-pop clear → still a *query*, which is why the first attempt returned
  undefSelector). Pass the raw 68k proc as the UPP; Mixed Mode calls it as 68k via the ProcInfo.
- **Verified:** `NewGestalt err=0`, and the **read-back** (`Gestalt('ppcf')`, which CALLS our selector
  function) returns `features=0x40 vectorBit SET`, emulator alive — so the selectorProc ABI is correct.

So a guest app now SEES AltiVec.

**FC EXECUTION TEST (2026-06-07) — force works, but FC's hot path is FP, not AltiVec (question still OPEN).**
Booted Mac OS 9.0.4 (`e2e-macos9-workload-boot.dsk`, which has CarbonLib 1.6 — `macos9_fresh.dsk` lacks
`GetPortBitMapForCopyBits` so FC won't launch there) + `e2e-apps.dsk`, with the force + `SS_JIT_PROFILE`.
Added a definitive profiler metric `[JIT-COMPILED-MIX]` (global per-class op-compile histogram across ALL
blocks, not just top-40 hot). A/B:
- **Forced** (`ppcf`=0x40): AltiVec=**0** compiled, FP=4335, 38.8B guest-insns, 651 MIPS; hot path =
  FC's fractal loop `1e5d55d0` (FP, 19 insns) + `1e5d5658` (int) + `1e5d561c` (branch), ~58% of execution.
- **Baseline** (no force): AltiVec=**0**, FP=2996, 8.4B guest-insns, 147 MIPS; hot path = ROM/idle (no FC loop).
So the force DID change FC's behavior (4.4× more work; its compute loop only runs when AltiVec is advertised)
— but **zero PPC AltiVec (op==4) instructions are compiled/executed either way**; the fractal kernel is
scalar double-precision FP.

**Honest interpretation (do NOT overclaim a "detection-insufficiency"):** I did NOT drive FC to an explicit
AltiVec/Calculate mode — I watched its *passive default* render for 30s. "Zero AltiVec" is equally
consistent with "the AltiVec workload was never triggered" or "this Carbon binary emits no PPC AltiVec".
The classic-Mac UI introspection (`SS_UI_DUMP_DIR`) returns an EMPTY menu bar for FC (a Carbon app — the
toolbox-menu structures it reads don't apply), so I can't enumerate its actions host-side. Caveats: (1)
`[JIT-COMPILED-MIX]` counts JIT-compiled ops only, so the precise claim is "no AltiVec in FC's JIT-compiled
hot path," not "FC executed zero AltiVec instructions ever"; (2) the hot-loop-is-visibly-FP evidence is what
carries the claim.

**Bankable regardless:** the gestalt force works (readback 0x40 SET); `'ppcf'` unregistered under BOTH 8.6
and 9.0/9.0.4 (System version is not the gate); the `_NewGestalt $A3AD` register-it-ourselves mechanism;
the `[JIT-COMPILED-MIX]` profiler. **The AltiVec codegen remains validated by `make test-jit` (349/349) —
that is the validation of record; "real app issues AltiVec" was always bonus confirmation, not validation,
so a null FC result does not dent codegen confidence.** OPEN: does a real app emit AltiVec here — needs FC
driven to its compute action (user knows the app), or a different AltiVec vehicle. [[altivec-gestalt-gate]]

**❌ WRONG TURN then ✅ RESOLVED (2026-06-07) — FC DOES use gestalt; my force set the WRONG BIT.**
First (confounded) conclusion was "FC ignores gestalt": with the force active, FC's File menu read
"Turn AltiVec Code On/Off — (not detected)" and a 235s session profiled `AltiVec=0`. **That experiment
was invalid** — a research subagent caught (and I verified against Apple's CarbonCore `Gestalt.h`) that
`gestaltPowerPCHasVectorInstructions` is **bit NUMBER 4 → mask `1<<4 = 0x10`**, but my force wrote
**`0x40`** (= bit 6 = `gestaltPowerPCHas64BitSupport`). So I set the 64-bit-support bit, not vector; FC
tested bit 4, saw it clear, and *correctly* said "not detected." The readback "confirmed" only because it
tested the same wrong bit.
**Fixed `0x40`→`0x10` and re-ran — DECISIVE SUCCESS:** `[FORCE_AV] features=0x10 vectorBit SET`, and
`[JIT-COMPILED-MIX] AltiVec=160` (vs 0 every confounded run), with AltiVec blocks in FC's hot path
(`1e5bd98c` AltiVec/7, `1e5bd9a8` AltiVec/15). **Fractal Carbon now detects AltiVec via gestalt, takes
its vector path, and our AArch64 JIT compiles+runs the PPC AltiVec instructions — real-app end-to-end
AltiVec, achieved.** So: (1) FC DOES use `Gestalt('ppcf')` bit 4; (2) the register-it-ourselves
`_NewGestalt $A3AD` mechanism is the working unlock under the OldWorld 1.1 ROM; (3) no NewWorld ROM port
needed for AltiVec after all. **LESSON: a bit-NUMBER constant is not a mask — `1<<N`. One wrong nibble
produced a fully self-consistent wrong conclusion (set+readback both tested 0x40) that survived until an
independent agent checked the primary-source header. Differential/oracle checks don't catch "both sides
share my constant error" — verify magic numbers against the authoritative source.**
Also corrected: `mfmsr` returns `0xf072` → MSR[VEC] (0x02000000) is **CLEAR**, not set (the prior-session
"reads SET / 0x0200f072" note was wrong; both interp `ppc-execute.cpp:1180` and JIT `ppc-jit.cpp:2719`
hard-code `0xf072`). Irrelevant to FC (it uses gestalt, not MSR), but a latent correctness gap (a real
7400 sets VEC). [[altivec-gestalt-gate]]

## 2026-06-07 — NewWorld 9.0.4 boot attempt (blocks at XPRAM HLE) + AltiVec-force injection hunt (banked)

User authorized booting a NewWorld ROM to test AltiVec. Two tracks, both run to their honest stopping point:

**Track 1 — boot the 9.0.4 G4 ROM (`MacOS-ROM-9.0.4-G4-extracted.rom`).** Ran the real emulator
(`./src/Unix/SheepShaver --config <isolated prefs>`, lenient mode auto-on for cksum `b8d0b672`):
- Type-detects as NewWorld; the `g_rom_904_lenient` whole-image fallback resolves all RELOCATED patches;
  the SKIP guards (mdec/suspend/run_diags) let PatchROM progress.
- **Blocks at the XPRAM/NVRAM HLE** (`nvram2_dat` `48e71ce0`, range `[0xa000,0xd000)`) — absent even
  whole-image (the routine was rewritten in 9.0.4), unguarded → `return false` → "Unsupported ROM type".
- **Why brute-guarding the rest won't yield a boot (inference, not slogged-to-proof):** the absent patches
  are EMUL_OP **HLE shims** (XPRAM/NVRAM/page-size/cpu-speed/time-via/OpenFirmware) that replace ROM
  routines which poke VIA/Cuda/PMU hardware SheepShaver doesn't emulate. SKIP-ing them moves the failure
  from PatchROM-time to a runtime hang in the first un-shimmed hardware access. The static sizing (12
  applicable-absent, NEW-WORLD-ROM-SUPPORT-PLAN.md) already IS the per-patch port worklist; each needs the
  rewritten-routine located + the EMUL_OP re-applied, **boot-verified**. That's the genuine multi-session
  D3 Phase 2 (now unblocked: user is available to boot-verify).

**Track 2 — force the `'ppcf'` vector bit under the WORKING 1.1 ROM (the de-risk for the whole NewWorld
investment).** Forcing bit 6 and seeing `MIX_ALTIVEC>0` would prove *"bit 6 is sufficient AND the codegen
runs real-app AltiVec end-to-end"* — BEFORE committing to the ROM port. (It would NOT prove "NewWorld sets
bit 6"; it only rules out the worst failure mode.) Injection-point hunt (static):
- `'ppcf'` literal `0x70706366` = **0× in the 1.1 ROM** (confirms #23: the System registers it from disk).
- ROM `InitGestalt` patch (`rom_patches.cpp:1757`) sets gestalt **CPU-type (0x1d(a2)) + page-size (0x1e)**
  but **NOT `'ppcf'`** — so it's not the lever.
- The base Gestalt selectors `sysv`/`proc`/`mmu`/`fpu`/`qd`/`kbd` ARE ROM-resident, but the ROM region at
  `~0x12c00` is a **consumer** (`move.l #'sel',d0; _Gestalt(a1ad); tst.w d0; …` building the HW inventory),
  not the `a1ad` dispatcher. The dispatcher that services `_Gestalt` (and where a `'ppcf'`→OR-bit-6 wrapper
  would go) isn't statically locatable in a few steps; it needs a boot-trace of the `a1ad` trap + a new
  EMUL_OP wrapper. **Per the timebox rule, BANKED** (task: "SS_FORCE_ALTIVEC injection point").
- **Cheapest next step for Track 2 (needs a boot):** boot the 1.1 ROM under the e2e harness with a hook that
  logs/Wraps the `a1ad` (`_Gestalt`) trap for selector `'ppcf'`, OR the result bit 6, run Fractal Carbon,
  check the profiler. The dispatcher is ROM-resident (good) — locating it is the work.

**Tradeoff for the user (surfaced):** (a) 9.0.4 NewWorld boot = multi-session HLE port (authentic run);
(b) the force experiment tests the AltiVec hypothesis cheaply and *may deliver real-app AltiVec now* (the
story), demoting the ROM port to nice-to-have. The JIT AltiVec codegen is hardened (this session) so it's
correct the moment either lands. [[altivec-detection-not-pure-pvr]] [[altivec-gestalt-gate]]

## 2026-06-07 — Autonomous session: NewWorld ROM frontier blocker pinpointed + CopyBits probe limits

User staged the frontier assets (`Downloads/New_World_Mac_Roms/New World ROM/` = 19 Mac OS ROM versions,
`macos_921_ppc.iso`, `macos-922-uni.zip`, `3D_pinball_demo.hqx`) and said proceed autonomously. Diagnostics:

- **All 9.x NewWorld ROMs are equivalent** (rom-inspect: CHRP-parcels NewWorld; patch-sizing identical:
  27 in-range / 31 relocated / 17 applicable-absent). No "closer" candidate among them.
- **The frontier blocker is STRUCTURAL, not just absent patterns** (key finding). Added `SS_ROM_LENIENT=1`
  (force lenient mode for any NewWorld ROM → the 31 relocated patterns auto-resolve). Booting 9.1.1 then
  aborts at the FIRST nanokernel-boot patch: `rom_patches.cpp:715` `if (ntohl(lp[6]) != 0x2c0c0001) return
  false;` — the patch logic assumes `cmpwi r12,1` sits at `pvr_read+24`, but the parcels ROM rewrote the
  CPU-detect routine (that word is a `mtspr`; the real compare is ~0x520 bytes away). **So `patch_nanokernel_boot`
  needs RE-reverse-engineering for the parcels layout, not incremental pattern-filling. Patch-sizing
  UNDERCOUNTS the work** — it checks byte-pattern presence, blind to `lp[N]==const` structural assumptions.
  Genuine multi-session RE; not autonomous-away work. (Tooling shipped: `SS_ROM_LENIENT`, `SS_ROM_PATCH_TRACE`.)
- **CopyBits frequency probe hit a PPC/Mixed-Mode wall.** Added `SS_JIT_PROFILE_PC=<pc>` (print any block's
  exec count) + the `SS_COPYBITS_TRACE` resolver. But `_CopyBits` trap addr `0x102daa5c` reads back as
  **"(not compiled) 0"** — on a PPC Mac CopyBits is PowerPC code reached via Mixed Mode, so the 68k trap
  address ≠ the JIT block PC. A RoutineDescriptor-follow (magic 0xAAFE) also said it's **not a standard RD**.
  Identifying CopyBits's real PPC block needs a live memory dump at the trap addr + a **graphics workload**
  (Finder activity blits ~0 — confirmed). The pinball app is the workload, but installing the `.hqx`
  (BinHex → guest install) is fiddly host-side. Tooling in place; identification + workload are the remaining steps.
- **⭐ Graphics workload run + the CopyBits go/no-go ANSWERED with data.** Decoded the user's pinball
  `.hqx` with `unar` (→ PPC PEF app, forks intact), mounted it host-side via `extfs` (guest "Unix"
  volume), launched it via VNC — **3D Ultra Pinball renders its table** (screenshot). Then the JIT
  **heartbeat** region breakdown during gameplay settled the CopyBits-HLE question without even needing
  the per-block profile: **~97% of executed-block growth during gameplay is in the DR region** (the ROM's
  68K emulator) — jDR +2055M vs jRAM +51M over 20s — whereas boot is RAM/PPC-dominated. The PPC game's
  *rendering* (QuickDraw/CopyBits) is 68K code via Mixed Mode → DR emulator → PPC JIT, the exact "worst
  layer" the compatibility memo flagged. **So CopyBits/DR-HLE is GO, data-backed.** (The finer CopyBits-
  vs-other-QuickDraw split needs a per-block profile, blocked by: the profiler only dumps on clean
  shutdown and a fullscreen game resists automated quit → next step is a dump-profile-on-host-signal hook.)
- **Method that worked all session:** env-gated, default-off probes (zero risk to normal boots), boot from a
  disk COPY, clean shutdown via SIGUSR1. A crashing approach (the earlier CopyBits come-from stub) only ever
  affected its own enabled run. **And: `unar` decodes classic-Mac BinHex `.hqx` preserving resource forks +
  type/creator as native xattrs; `extfs <hostdir>` mounts a host folder into the guest — the no-disk-install
  way to get an app/workload in front of the JIT.**

## 2026-06-07 — Extended the XO audit to SCALAR FP (live, not dormant) — found mtfsf/mtfsfi body swap

Ran the same scramble logic against the scalar-FP switches (primary 63/59). Unlike AltiVec (dormant
behind the gestalt gate), **scalar FP runs in every boot**, so a scramble here is a *live* correctness
bug. After filtering parser false-positives (the FP table interleaves X/XFL/A forms; a naive cross-entry
regex mis-pulled primary-31 ops like `mfmsr`/`mcrxr`/`icbi` into the 63 table — verify each hit by hand):
- **`mtfsf`/`mtfsfi` code bodies were SWAPPED vs their XOs.** `case 711` (real `mtfsf`, XFL XO 711) ran
  the *mtfsfi* decode (`crfD`/`imm`); `case 134` (real `mtfsfi`, X XO 134) ran the *mtfsf* decode
  (`fm`/`frB`). Both write `PPCR_FPSCR` + sync rounding, so a guest `mtfsf`/`mtfsfi` would have set the
  wrong FPSCR fields. **Fixed by swapping the two case labels** (correct by inspection: each body decodes
  the OTHER instruction's operand fields). 349/349.
  **Severity — latent, no observed boot impact (don't overstate):** SheepShaver boots Mac OS to Finder
  cleanly, which it could not if a *frequently-executed* instruction were corrupting FPSCR every boot.
  So in practice `mtfsf`/`mtfsfi` are either off the hot path during boot or their mis-set fields were
  benign for the code that ran. Correctness fix worth landing; not evidence that prior boots were wrong.
- **Benign (not bugs):** `fsel`/`fsqrt`/`frsqrte` are A-form ops sitting in the X-form (`xo10`) switch,
  but it works — `fsqrt`/`frsqrte` always have frC=0 (so `xo10` == their 5-bit XO), and `fsel` with
  frC≠0 simply misses its `xo10` case and falls through to the correct interp. No incorrectness.

**Second finding the test surfaced (separate limitation, quarantined not fixed):** the obvious differential
for the swap — `mtfsfi 7,2` (RN=+inf) then `fctiw 2.25` — still diverges (interp 3, JIT 2) because **JIT
`fctiw` uses a FIXED rounding (FRINTA) and ignores the dynamic FPSCR RN**. That's orthogonal to the swap
(which IS fixed); `fctiw` with a non-default rounding mode is rare. Recorded as `run.sh` QUARANTINE
`fp_fctiw_dynround` (xfail; flips to xpass when fctiw honors dynamic RN via FRINTI + host-FPCR sync).
Note: FPSCR is NOT in the REGDUMP, so FPSCR effects are only testable indirectly (via a rounding-sensitive
op) — and that indirection is exactly what entangled the swap with the fctiw limitation. [[altivec-xo-audit-fp-round-compare]]

## 2026-06-07 — Systematic AltiVec XO audit (now a tool) — fixed FP round/compare; vrfin is ties-AWAY not -even

After two scrambled families by hand, built the audit as a **tool**: `SheepShaver/tools/altivec-xo-audit.py`
cross-checks every JIT `case N:` (label from its `/* mnem */` comment) against the authoritative
`{mnemonic->XO}` in `ppc-decode.cpp`. One run surfaced **13 more mismatches**. Triaged by the only axis
that matters while AltiVec is dormant — *does it emit WRONG code for a real op, or already fall back to
correct interp?*:
- **Emitted wrong (fixed):** `vrfin`@522 ran FRINTP, `vrfiz`@586 ran FRINTM, `vcmpgefp`@454 ran FCMGT.
  Remapped all four rounds to authoritative XOs (vrfin=522 vrfiz=586 vrfip=650 vrfim=714) and both
  compares (vcmpgefp=454 FCMGE, vcmpgtfp=710 FCMGT). Also routed the `vmsum*`/`vmhaddshs`/`vmhraddshs`/
  `vmladduhm` VA-form block (XOs 32/33/34/36/37/38/40/41) to `return false` — they emitted wrong native
  code (horizontal multiply-sums NEON can't express as coded; vmladduhm had swapped operands). Kept the
  correct `vmaddfp`/`vnmsubfp` (FMLA/FMLS) and `vsel`/`vperm`.
- **Already correct via interp (DEFERRED, logged not fixed):** 6 dead-XO cases — `vupkhsb/hsh/lsb/lsh`
  (reals 526/590/654/718) and `vexptefp/vlogefp` (394/458) sit at XOs no op decodes to; the real ops
  fall back to interp. Moving them to the right XO only *accelerates dormant code* — near-zero value now.

**The catch that proves the lesson: `vrfin` is round-half-AWAY-from-zero, not ties-to-even.** I mapped it
to `FRINTN` (ties-even) as "obviously correct"; the differential test FAILED (interp `frsin`: 2.5->3.0,
not 2.0). Fixed to `FRINTA` (ties away). *The "obvious" NEON op was wrong; only the boundary/tie test
caught it.* — exactly [[altivec-sum-across-scramble]]'s "untested = unverified."

**Audit blind spot (stated loudly so "0 mismatches" is never mistaken for "verified"):** the tool catches
XO/label mismatches ONLY. **Right-XO-wrong-codegen is invisible to it** — the sum-across *saturation* bug
(correct label, missing clamp) and `vrfin`'s wrong rounding mode would BOTH pass the audit clean. Only
`make test-jit` with boundary operands proves codegen. All fixes gated: 349/349, score=100.

## 2026-06-07 — AltiVec coverage audit found a 2nd scrambled family: whole-vector shifts (vsl/vslo/vsro)

Ran the advisor's suggested **coverage audit** (grep JIT `case` labels vs `TEST_ORDER` vector names)
after the sum-across fix. Trap: a naive substring match (`"vsl" in "vslw"`) hides ops — `vsl/vsr/vsldoi`
looked "covered" because of `vslb/vslw/vslh`; only `vslo/vsro` (the 'o' breaks the substring) surfaced.
**Match on whole mnemonics, not substrings.** Audit (after correction) found the **whole-vector shift
family was scrambled like sum-across**:
- Authoritative XOs (ppc-decode.cpp): `vsl=452 vsr=708 vslo=1036 vsro=1100` (+ `vsldoi` VA-form XO 44).
- JIT had: `case 452` (really `vsl`) running **vsldoi EXT-by-constant** code; `case 1036` (really `vslo`)
  running **per-lane SSHL.16B**; `case 1100` (really `vsro`) running **per-lane NEG+USHL** — all WRONG,
  because these shift the **full 128-bit register**, not each lane. `case 1356/1420` were dead (no real
  op). `vsr` (708) and `vsldoi` (varying vxo) already fell through to the **correct interp** fallback.
- Confirmed by 4 new vectors (`av_vsl/av_vslo/av_vsro` diverged; `av_vsr` passed) — empirical, not assumed.

**Fix:** routed `vsl/vslo/vsro` to `return false` (correct interp fallback), removed dead cases. Chose
fallback over native because the dynamic whole-vector shift is awkward in NEON and these ops are rare
(project rule: *simple by default, performance by earned sophistication*). Documented the native plan in
the code (vslo/vsro = `TBL.16B` with a runtime `[0..15]±sh` index vector; vsl = per-byte shift + cross-byte
carry). `make test-jit` 343/343, score=100. Two scrambled families in one sitting → **the AltiVec XO tables
were never differentially audited**; the remaining audit hits (fctid/fctidz/fcfid/fcmpo/fsqrt etc.) deserve
the same whole-mnemonic check before trusting their "coverage." [[altivec-detection-not-pure-pvr]]

## 2026-06-07 — AltiVec sum-across JIT bug: XO map was SCRAMBLED + no saturation (5 ops, caught by ground-truth diff)

Hardening the dormant AltiVec codegen (task #22) immediately paid off: the **sum-across family was
badly broken** in `ppc-jit.cpp`, and it had **zero test coverage** so nothing caught it. Two stacked bugs:
1. **XO→operation map was SCRAMBLED.** Authoritative XOs (from `ppc-decode.cpp`): `vsum4ubs=1544
   vsum4sbs=1800 vsum4shs=1608 vsum2sws=1672 vsumsws=1928`. The JIT had `1928` labeled `vsum4ubs`
   (1928 is really `vsumsws`), `1672`/`1800` swapping `vsum4sbs`/`vsum2sws`, **`vsum4ubs`/1544 had no
   case at all**, and a dead `case 1932` (not a real XO). So real guest `vsumsws`/`vsum2sws`/`vsum4sbs`
   each got a *different op's* codegen; only `vsum4shs` had the right op (and `vsum4ubs/1544` happened
   to fall back to the interp, so it was accidentally correct).
2. **No saturation.** Every variant did the `+vB` / final reduce with a plain `ADD.4S`/`ADDV` that
   **wraps** at the int32/uint32 boundary; PPC saturates. The interpreter accumulates in **int64**
   (`v4si_sat_operand`, `sat_type=int64`) and clamps — so it's **true ground truth**, independent of
   the JIT, which is why a differential test is valid here (not the "both sides share a bug" trap).

**How it was caught (the method that worked):** wrote 5 boundary vectors in `gen-altivec-vectors.py`
whose operands drive each lane's sum *past* the saturation boundary, ran `make test-jit` → all 5 diverged.
The interp's `vsum4ubs`-vector result `[0,0,0,0xFFFFFFFC]` (= signed sum of four `-1` words = `-4`) was
the tell that XO 1928 is `vsumsws`, not `vsum4ubs` — i.e. *read the oracle's output, don't assume the label.*

**Fix:** correct XOs + proper saturating codegen. Simple per-word ops use `UQADD`/`SQADD.4S` (one-instr
swap from `ADD.4S`). The wide ops need 64-bit accumulation: `vsum2sws` → `SADDLP.2D` + build vB-odd-words
`.2D` + `ADD.2D` + `SQXTN.2S` + `ZIP1`-with-zero (odd-word placement); `vsumsws` → `SADDLV` (64-bit) +
scalar `ADD d` + `SQXTN.2S` + `INS .S[3]`. All NEON encodings verified offline with `as -arch arm64` +
`otool -tvj` before coding (cheap and decisive — do this for any hand-encoded NEON). Result: `make
test-jit` 337/337 score=100. **Lesson:** the "validated by test-jit" claim only holds for ops that HAVE
a vector — a whole family had none. When pivoting to "harden codegen," first audit *coverage*, then trust.
Also: an unmatched `vxo` falls through into the `switch(vao)` (low-6-bit VA-form) — every VX case must
explicitly `return` (correct codegen or `return false` for clean per-op interp fallback), never fall through.

**Caveat (correct *modulo* VSCR[SAT]):** the interp sets `vscr().set_sat(1)` on every saturation; the JIT
does not model VSCR at all (`mfvscr`→0, `mtvscr` NOP), and the REGDUMP has no VSCR field, so the
differential test is blind to this divergence. Pre-existing, family-wide JIT limitation (not introduced
here) — the sum-across result words are correct; only the sticky SAT flag is unmodeled, consistent with
`mfvscr`=0. Coverage scope: both +INT_MAX and −INT_MIN clamps tested for all five (the two wide ops use
`SQXTN.2S`, a different clamp path than the per-word `SQADD`, so both directions are exercised separately).

## 2026-06-07 — AltiVec gestalt gate: the three obvious gates are ALL open; pivoted to codegen-correctness hardening

Re-examined the AltiVec-detection gap (task #21) before the next deep push, and **falsified the three
cheapest theories at once** — all three obvious gates are already open, yet `'ppcf'` still clears the
vector bit (`gestaltPowerPCHasVectorInstructions`, bit 6):
- **PVR** is `0x000c0000` (7400, *with AltiVec*) — `main_unix.cpp:453` (EMULATED_PPC default), and the
  remap switch at :630 forces any newer G4/G5 back to 7400. So the guest sees a G4. Not the gate.
- **MSR[VEC]** *reads SET* (the mfmsr probe returned `0x0200f072`; `&0x02000000` = set). And the
  mtmsr-WRITE-enable theory was already falsified (boot probe). Not the gate.
- **The interpreter fully implements AltiVec** — `vadduwm`/`vperm` etc. are wired in `ppc-decode.cpp`
  and `execute_vector_*` in `ppc-execute.cpp`. So a boot-time *execution* probe wouldn't fault. Not the gate.

So the bit is cleared by the System's own `'ppcf'` selector computation, which task #23 already showed is
**not pure-PVR**. **The ROM-vs-System question is ALREADY ANSWERED by #23 (see the entry below): the
selector literal `0x70706366` appears 0× in the 1.1 ROM and 5× on the Mac OS 9 disk — the handler lives
in the System file (disk→RAM), table-dispatched and relocated at load, so static is exhausted and a
`find_rom_data` ROM patch is NOT the lever.** Therefore this is **decision-ready, not open** — every
remaining unlock path needs a user-in-loop boot:
  1. **NewWorld G4 ROM boot** (the #23 decisive test): the co-requirement the handler reads is plausibly a
     nanokernel vector flag that only G4-era (NewWorld) ROMs set → boot `Mac OS ROM 9.0.4` and watch the
     profiler `MIX_ALTIVEC`. Blocked on **D3 Phase 2** (9.0.4 ROM port; infra landed this session).
  2. **Boot-time RAM patch / `_Gestalt`-trap intercept** to force the vector bit — needs new infra
     (SheepShaver has EMUL_OP routine replacement for XPRAM/NVRAM/SONY/etc. but **no `_Gestalt` intercept**
     today) + a boot to verify.
  3. **PVR-read / handler-read hook** (à la `SS_LOG_ILLEGAL`) to observe what the handler reads — diagnostic, boot-level.
All three need the user. The codegen is now hardened (this session) so it's correct *the moment* any path lands.

**Why this is the consolidate signal, and the pivot:** every injection point needs a boot to verify, i.e.
the user in the loop — AND the marginal value of an e2e gestalt-force over the existing differential
harness is low, because the AltiVec codegen is *already validated by `make test-jit`* (interp-vs-JIT diff
against the mature upstream interpreter). So I pivoted to **hardening that codegen** (task #22, sum-across
+ fctid) instead — fully autonomous, no boot needed, directly de-risks the codegen for the moment
detection unlocks. See the sum-across saturation entry below.

## 2026-06-07 — D3 Phase 2 started: 9.0.4 G4 ROM multi-version patching infra (1.1 boot verified safe)

Began the actual 9.0.4-ROM port (the AltiVec unlock). Built the **multi-ROM-version patching
infrastructure** in `rom_patches.cpp`, all guarded so the working 1.1 boot stays byte-identical
(**verified: `make e2e` still PASS after the changes**):
- **Checksum-based version flag** `g_rom_904_lenient` (auto-on only for 9.0.4 cksum `0xb8d0b672`;
  1.1 is `0xfd86d120`; opt-out `SS_ROM_NO_904`). 1.1 and 9.0.4 share ROMTYPE_NEWWORLD AND version
  `0x77d`, so checksum (or sub-version `@0x18`) is the only discriminator.
- **`find_rom_data` whole-image fallback** in lenient mode → auto-resolves every RELOCATED pattern
  (no per-site edits). This alone handles ~14 of the 9.0.4 drifted patterns.
- Clean 9.0.4 ROM extracted to `/Users/Shared/macemu/MacOS-ROM-9.0.4-G4-extracted.rom` (2.32 MB,
  under the 4 MB `load_mac_rom` cap — so no read-cap fix needed for 9.0.4; that caveat was 9.2.x-only).
- SKIP-placeholder guards for the first few ABSENT patches (mdec/suspend/run_diags + page_size/
  cpu_speed/time_via/open_firmware) so PatchROM progresses.

**Honest state:** the ABSENT patches (~6-8 in the NewWorld path) are **load-bearing** (decrementer
neutralize, 68k-stack setup, nvram, page-size) — SKIP-ing them gets PatchROM further but won't yield
a boot; each needs proper RE of the rewritten 9.0.4 equivalent, **boot-verified** (needs the user /
a 9.0.4-bootable disk). So I stopped the skip-chain rather than ship a known-broken boot. Worklist +
roles are in NEW-WORLD-ROM-SUPPORT-PLAN.md Phase 2. **Key de-risk:** SheepShaver + the 1.1 ROM
*already boots the 9.0.4 OS*, so the 9.0.4-era supervisor environment is proven — this is ROM-patch
parity work, not the 9.1/9.2 "second wall". Method per absent patch: `rom-inspect --dump` both ROMs,
capstone-diff the region, add a `g_rom_904_lenient` variant or confirm unnecessary.

## 2026-06-07 — NewWorld ROM boot trial: blocked on D3 (parcels-ROM support); but the AltiVec evidence firmed up

Tried the decisive AltiVec test — boot the 2001 `Mac OS ROM 9.0.1.rom` and watch for AltiVec blocks.
**It doesn't boot: "Unsupported ROM type."** Ran it down with proper decoded-image analysis (a
standalone harness over the shared `src/include/rom_decode.hpp`) + an env-gated `find_rom_data` tracer
(`SS_ROM_PATCH_TRACE=1`, landed in `rom_patches.cpp`). Findings:

- **This is the existing ROADMAP D3 project** (`docs/planning/NEW-WORLD-ROM-SUPPORT-PLAN.md`,
  "New World parcels-ROM support — break the 9.0.4 ceiling"). I effectively completed its **Phase 0**:
  the 9.0.1 ROM (CHRP-**parcels**) decodes + type-detects as NewWorld, but `PatchROM()` fails at the
  **very first** `find_rom_data` (`patch_nanokernel_boot` `sr_init_dat`, range `[0x3101b0,0x3105b0)`;
  the pattern exists at `0x3106f8`). The plan's "early failure → Phase 1" gate guess was then **refuted
  by deeper analysis** — it's **Phase 2 (deep RE), and Phase 1 is RULED OUT**: (a) parcel enumeration
  shows the `'rom '` parcel decodes to a *complete, valid* image (nanokernel present; the other 27
  parcels are OpenFirmware device-tree nodes, not Mac OS code) → `decode_parcels` isn't dropping the
  image; (b) full pattern sizing (`tools/rom-patch-sizing.py` via `rom-inspect --dump`, control=1.1):
  of 83 patches, **27 in-range / 31 relocated / 25 absent (17 applicable-absent)** — absent = byte
  sequence *rewritten* between the 1998 and 2001 ROMs, needs RE. So #1 (for *this* ROM) is the deep,
  multi-session "days-to-weeks, no guarantee" branch. NOT done tonight; report-back. Lesson: **don't
  trust the plan's heuristic decision gate — measure** (the gate said Phase 1; the data says Phase 2).
- **CLOSER-ROM hunt (paid off): the 9.0.4 "MacROM for NewWorld" is the best AltiVec-unlock target.**
  Extracted Mac OS ROM files from install media by `<CHRP-BOOT>` signature scan (modern macOS can't
  mount HFS) + `rom-inspect --dump` + sizing. 9.0.4 (from `macos904.toast`, `<COMPATIBLE>` lists
  PowerMac3,1/3,2 = **G4**, `'ppcf'`=2): **49 in-range / 14 relocated / 12 applicable-absent** — much
  closer than 9.0.1/9.2.1 (27/31/17), and its **nanokernel-boot patches all match** (the absent ones
  are peripheral hardware init). So the shortest path to a booting `'ppcf'` ROM + AltiVec is the
  **9.0.4 parcels ROM**, not 9.0.1/9.2.x. Setup caveats for the port: clean file extraction + the
  `load_mac_rom` 4 MB read cap. Comparative table in NEW-WORLD-ROM-SUPPORT-PLAN.md "ROM target strategy".
- **TERMINOLOGY FIX (important):** the current working ROM `1998-07-21 - Mac OS ROM 1.1.rom` is **NOT
  "OldWorld"** (CLAUDE.md's label is loose). `rom_detect_type` classes it **`ROMTYPE_NEWWORLD`**
  (nanokernel ID "NewWorld v1.0.p."); it's a 1998 CHRP-**LZSS** NewWorld ROM. The real axis for AltiVec
  is **ROM vintage / format**, not OldWorld-vs-NewWorld. Both ROMs are NEWWORLD; they differ by payload
  format (1.1=LZSS works; 9.0.1=parcels fails PatchROM) and vintage (1998 vs 2001).
- **AltiVec evidence, now on DECODED images (firmer than the earlier raw-file grep):**
  `1.1` (1998 LZSS): `'ppcf'` = **0** in the decoded 4 MB image. `9.0.1` (2001 parcels): `'ppcf'` = **2**.
  So the 1998 ROM genuinely lacks the PowerPC-processor-features (`'ppcf'`) gestalt the vector bit lives
  in; the 2001 ROM carries it. Strongly consistent with "AltiVec detection can't succeed under the
  current ROM" → a newer ROM is a **plausible unlock** (still gated on D3 to boot it).
- **Caveat carried forward:** even a booting 9.x ROM only tests *detection*; a usable AltiVec also needs
  VR save/restore on context switch, and 9.1+ may hit the "second wall" of supervisor-fidelity stubs
  (MMU / nanokernel / MP — see `MMU-NANOKERNEL-MP-PLAN.md`). The MIX_ALTIVEC>0 probe stays valid regardless.

Tooling kept: `SS_ROM_PATCH_TRACE` (env-gated `find_rom_data` tracer) — realizes D3 Phase 0's tracer,
reusable for the next ROM-version triage. The standalone decode/`'ppcf'`-count harness is throwaway (`/tmp`).

## 2026-06-07 — AltiVec detection is NOT pure-PVR — there's a co-requirement, likely ROM/nanokernel-side → **NewWorld ROM may be the unlock after all**

The session-earlier lean "NewWorld ROM is unlikely to help AltiVec" was **too confident — corrected here.**
The flip comes from our own evidence:

- **MEASURED (static, no boot):** the `'ppcf'` selector *literal* (`0x70706366`) = **0× in the OldWorld
  ROM, 5× in the Mac OS 9 disk**. So the selector — and the gestalt entry/handler that owns it — live
  **on disk**, identical under either ROM. (Static raw-disk RE then hit a wall: gestalt is *table-
  dispatched* — the selector literal sits in a (selector, handlerProc) data table, the handler proc is
  relocated at load, so you can't follow the pointer to the handler code from raw disk offsets. A
  `lis 0x7070`/`ori 0x6366` constant-build scan of the ROM found nothing either. Static is exhausted;
  the decisive test is now boot-level.)
- **KEY DEDUCTION (solid, from our own setup):** under EMULATED_PPC the **PVR is already `0x000c0000`
  (7400 = G4 *with* AltiVec)** (`main_unix.cpp:453`) AND the gestalt CPU-type byte is patched to match
  (`rom_patches.cpp:1708`, `move.b #PVR,$1d(a2)` in 68K InitGestalt) — **yet the `'ppcf'` vector bit
  stays clear.** Therefore the `'ppcf'` handler is **NOT pure-PVR**: if it merely read PVR, AltiVec
  would already be on. There is a **co-requirement beyond "PVR says G4."**
- **HYPOTHESIS (deduction solid; the specific mechanism is reconstruction, verify before asserting):**
  AltiVec is a **G4-only** feature; OldWorld ROMs only ever shipped on pre-G4 (non-AltiVec: beige G3
  and earlier) machines, so the OldWorld **nanokernel has no reason to probe/enable a vector unit or
  set the flag the `'ppcf'` handler reads**. That enable/flag is plausibly **present in NewWorld
  (G4-era) ROMs and absent in OldWorld** → swapping to NewWorld could be exactly what flips detection.
  This makes NewWorld a **plausible AltiVec unlock**, not the non-factor claimed earlier.
- **DECISIVE TEST (boot-level — out of #23's no-boot scope, hence a report-back):** boot the staged
  **NewWorld ROM (`Mac OS ROM 9.0.1`)** and see if AltiVec lights up (the profiler's MIX_ALTIVEC blocks
  go nonzero). Most direct — settles it regardless of whether the nanokernel mechanism guess is right.
  Alternative: a PVR-read (`mfspr` SPR 287) hook à la `SS_LOG_ILLEGAL` to watch what the handler reads.
  Caveat: CLAUDE.md notes "New World CHRP ROMs don't scan cleanly" in rom-harness, so NewWorld boot may
  need work — but `ROMTYPE_NEWWORLD` code paths already exist, so it may be less effort than feared.

The mtmsr/MSR[VEC] finding below still stands (enable is downstream of detection); this entry refines
*where* detection's missing piece lives. Net: **AltiVec-enable is foundational Phase-3 work, and the
cheapest next experiment is a NewWorld-ROM boot trial — which happens to match the user's own instinct.**

## 2026-06-07 — AltiVec detection is NOT an `mtmsr`/MSR[VEC] path — the gate is upstream (gestalt `'ppcf'`)

Probed the hypothesis "the OS enables AltiVec by writing MSR[VEC] via `mtmsr`, which we silently drop."
**Result: falsified.** Added an env-gated diagnostic (`SS_LOG_ILLEGAL=1`) at the top of
`execute_illegal` (ppc-execute.cpp) that logs every undecoded opcode, decoding `mtmsr` (op31/XO146) and
testing the MSR[VEC] bit `0x02000000` of rS. Confirmed the path is real first: `mtmsr` is **not**
NOP-stubbed in the JIT — `compile_one` returns false → `emit_inline_interp_call` → `ppc_jit_interp_one`
→ `jit_interp_one` → `decode()` → `execute_illegal` (validated with `SS_TEST_HEX=7C600124`, fires in
both JIT and interp). Then booted **Mac OS 9 + Fractal Carbon (the AltiVec app)** via the sanctioned E2E
workload (`SS_LOG_ILLEGAL=1 make e2e-workload`; make exports env → recipe child inherits, verified):
**ZERO `mtmsr` and zero illegal opcodes across the entire boot+launch+render+shutdown.**

Interpretation (consistent with the [AltiVec-dormant finding](#2026-06-07--altivec-may-be-unreachable-by-real-guest-software-despite-pvrg4--our-vector-codegen-unexercised)):
`mtmsr`-enable is *downstream* of detection. Because the gestalt `'ppcf'`
(gestaltPowerPCProcessorFeatures) **vector bit is not set**, no app issues AltiVec, so nothing ever
needs to enable MSR[VEC] → no `mtmsr`. **The lever is the gestalt `'ppcf'` vector bit** — but note
the *measured* fact vs the *open* one. MEASURED (2026-06-07): the `'ppcf'` selector byte-string =
**0× in the (1998 LZSS `1.1`) ROM, 5× in the Mac OS 9 boot disk** (so the selector is *referenced* on-disk).
[Superseded/refined by the newer entry above: on DECODED images the `1.1` ROM has `'ppcf'`=0 and the 2001
`9.0.1` ROM has `'ppcf'`=2; and "OldWorld" is the wrong label — `1.1` is itself `ROMTYPE_NEWWORLD`.]
OPEN (#23): WHERE the bit is *computed* — a ROM/NanoKernel handler the System merely invokes is fully
consistent with the selector living on-disk, so "computed in ROM" is **unverified, not disproven**;
do not over-read the string count as locating the computation.
SCOPE REFRAME (per advisor): this is NOT a contained CPU-model fix and NOT (as far as we know) a quick
ROM-patch. Two cheap ROM-agnostic follow-on probes remain: (1) static-disassemble the `'ppcf'`
handler to see what it reads — PVR-direct → ROM-orthogonal (NewWorld ROM won't help); ROM-table → NewWorld *might* matter;
(2) experimentally force the `'ppcf'` vector bit and observe whether the guest then emits AltiVec
blocks — but a gestalt-only hack is **unsafe** in production without VR context-switch save/restore.
NewWorld ROM is worth doing for **Mac OS 9.1/9.2 support**, but is unlikely to be the AltiVec unlock —
don't bundle the two. The `SS_LOG_ILLEGAL` diagnostic was kept (cheap, env-gated, reusable for any
undecoded-opcode triage).

## 2026-06-07 — FP register allocator (P5b) landed in one session via a staged "RA-aware bridge"

The FP RA looked like a High-effort multi-session job, but two design choices made it a safe single
session — both worth reusing for future RAs:
1. **Caller-saved V16–V23, not callee-saved d8–d15.** The textbook choice is callee-saved (survives
   calls) but that needs prologue surgery (save/restore) → touches every block → regression risk.
   Instead: verify that *every* `ra_flush_all` site is **block-terminating** (the FP cache never needs
   to survive a BLR). It is → caller-saved V16–V23 works with **zero prologue change**, so integer
   blocks stay byte-identical (provable: alu/shift/carry/rc1 microbench unchanged). The "do all flushes
   terminate the block?" check is the hinge that turns a hard RA into an easy one.
2. **Make the existing scratch helpers RA-aware (a "bridge"), then convert hot cases incrementally.**
   `emit_load_fpr`/`emit_store_fpr` now MOV from/to the cache when present (else raw struct), exactly
   like `emit_load_gpr`/`emit_store_gpr`. So *unconverted* FP ops stay coherent automatically, and you
   can convert just the hot arithmetic to zero-copy `ra_fp_load`/`ra_fp_store` first, validating in
   stages. Stage 1 (machinery + bridge, no conversions) is **behaviour-identical** (cache stays empty)
   → a free checkpoint. NOTE: the bridge alone keeps the instruction COUNT the same (FMOV instead of
   LDR/STR — a timing win, not an a64/op win); only zero-copy conversion drops a64/op (4→1).
Result: fadd/fmul/fmadd zero-copy → fp-add/fp-fma a64/op 4/5→1.0 (ns ~14×/~9×), **Speedometer Math
+16% (13,000→15,075, ~1.89× interp, up from 1.29×)**, test-jit 302/302. Couple `ra_fp_flush_all` INTO
`ra_flush_all` so the two RAs flush together at every barrier (and future barriers) without per-site
edits. The integer RTMP/NZCV-across-ra_store landmine does NOT recur for FP (evict emits only STR Dn;
no lazy-FP state).

## 2026-06-07 — AltiVec may be UNREACHABLE by real guest software (despite PVR=G4) — our vector codegen unexercised

Profiling "AltiVec Fractal Carbon" (the canonical AltiVec workload) showed its hot loop is the
**scalar-FP** path, not AltiVec (hot block 0x1e5bc0d0 = 19-insn **FP**, tagged by primary opcode 59/63;
an AltiVec loop would be op4 → MIX_ALTIVEC). Yet SheepShaver **does** advertise a G4: under EMULATED_PPC
`main_unix.cpp:440` sets `PVR = 0x000c0000` (7400 *with AltiVec*), and name_registry maps `PVR>>16==12`
to "PowerPC,G4". So PVR claims AltiVec but the guest still picks the scalar path → **AltiVec detection
has a gap somewhere between PVR and the gestalt/`cpu_supports_altivec` the OS/app checks**. IMPLICATION:
all the AltiVec JIT codegen we hardened (ev_mixed byte-order, pack/saturate/shift-rotate, vsel, etc.) is
currently **validated only by the test harness — not exercised by any real guest app**, because nothing on
the guest issues AltiVec instructions. This is high-value to resolve: if the detection gap is closed (e.g.
the right gestalt `gestaltPowerPCHasVectorInstructions` bit, or a ROM/nanokernel AltiVec-enable), real
AltiVec software (Fractal Carbon, Photoshop AltiVecCore, SoundJam) would light up the vector path and give
us genuine AltiVec workload coverage + a real reason the vector-codegen work matters. NOT yet root-caused —
needs a boot + a gestalt check (gestaltPowerPCProcessorFeatures) and a look at how Mac OS 9 gates AltiVec.
Tracked as a task. (Caveat: it's also possible Mac OS 9.x simply needs the AltiVec-aware libs, or that the
kpx_cpu CPU doesn't set the MSR/feature the OS probes — confirm before claiming a one-line fix.)

## 2026-06-07 — Interpreter Speedometer: harness `clean_signatures` is JIT-biased → skips score extraction

Capturing the JIT-vs-interpreter Speedometer headline on the M5, the **interpreter** run
(`SS_USE_JIT=0 make e2e-bench`) **ran the full suite** (reached Color Benchmarks) and **shut down
cleanly** ("Shutdown complete", exit 0) — but the harness reported `FAIL: benchmark ran but the
shutdown was not clean (exit=0, clean_signatures=False)`, and because `run_benchmark.py` does
`if not res.ok: return` *before* the best-effort score export, **no interpreter score was recorded**.
Root: the clean-shutdown signature check (scenario.py) keys on lines/timing that interpreter mode
doesn't reproduce (likely JIT `[HB]`/region markers) — a false-negative, not a real shutdown problem.
Two independent fixes (harness owner): (a) make the clean-shutdown signature **mode-agnostic**, or
(b) move the score extraction *above* the `res.ok` gate — the code itself says "nothing here can flip a
PASS to FAIL", so capturing a completed benchmark's score regardless of shutdown-cleanliness is strictly
better. Lesson: **score capture should be decoupled from shutdown-cleanliness** — a benchmark that
completed but shut down messily still has a valid score. (Also: interpreter Speedometer runs in ~the same
wall-clock as JIT — Speedometer is fixed-*time* per subtest, so the mode difference shows in the *score*,
not the run duration; ~40s benchmark gate both modes.)

## 2026-06-07 — Re-enabling lazy CR0 would break divw/lwzx/mulhw/lwarx together (RTMP/NZCV across ra_store)

Adversarial review of the perf commits surfaced a non-obvious coupling: several hot opcodes hold a live
value in **RTMP0–2 and/or NZCV across an `ra_store(rd)` call** (divw: NZCV between CMP and the final CSEL;
lwzx/lwarx: REV'd value in RTMP1 before `mov hD,RTMP1`; mulhw: SMULL product in RTMP0 before the LSR).
This is safe **only because lazy CR0 is eager-in-practice** (`lazy_cr0_valid` is never set true), so
`ra_store → ra_evict → emit_materialize_cr0` (which emits CMP+CSET, clobbering RTMP0–2 + NZCV) never fires.
If §0g (lazy CR0) is ever truly enabled, all of these break **simultaneously**. So lazy-CR0 has a
**prerequisite**: make `emit_materialize_cr0` RTMP/NZCV-safe, or hoist every such `ra_store` above its live
values. divw is now hoisted (commit 0cce0942) as the first step; the rest are tracked in OPTIMIZATION-PLAN
§0g / task #18. Lesson: "eager-in-practice" invariants that nothing *enforces* are landmines — a future
optimization that flips the invariant detonates them all at once.

## 2026-06-06 — A launch-failure modal alert emits NO signal; detect it via the menu bar

Building `scenario.run_workload`, the launch-error path (boot the app on a disk with the *wrong*
CarbonLib → the "application could not be opened because CarbonLib--GetPortBitMapForCopyBits could not be
found" alert) was a false PASS at first. Why it's hard: that alert is thrown in a context that **services
no idle hook** — the run logged **zero `[APP] modal=1`** signals and `uidump.snapshot` of it **times out**
(introspection can't read it). So neither the log nor Backend-A can see a launch failure. The screenshot
is the *only* evidence — and pHash can't read its text. The robust discriminator turned out to be the
**menu bar**: classic Mac OS gives the menu bar to the frontmost app, so a region-pHash of the menu-bar
strip vs the (volume-open) Finder baseline cleanly separates the cases — **app launched** (Fractal Carbon's
menus, Δ≈28-30) vs **a Finder dialog with the menu bar intact** (Δ≈4-8). Threshold 14 with margin on both
sides; boot-validated both directions. Lesson: for "did the app actually launch (vs error behind an
alert)", don't trust `[APP]`/introspection — **the menu bar is the frontmost-app tell, and it's only
visible in the screenshot** (`imagecmp.region_phash` / `workload.classify_launch`). Also: capture the
launch baseline AFTER opening the volume window, so launch-divergence measures the app, not the window.

## 2026-06-06 — A running Carbon app's fullscreen canvas is invisible to Backend-A introspection

Validating Fractal Carbon (it **launches + renders** with CarbonLib 1.6 — the old
`CarbonLib--GetPortBitMapForCopyBits could not be found` error is gone), the guest-UI introspection
returned **degenerate `(0,0,0,0)` + `suspect` windows** and the menu walk still showed *Finder* at the
dump moment — yet the **screenshot showed the fractal rendering fullscreen with the app's own menus**. So
a Carbon app drawing to a fullscreen canvas does **not** present a standard `WindowRecord`/`MenuList` that
the Backend-A memory walk can read. Consequence for `scenario.run_workload`: for Carbon/fullscreen apps,
**gate on the screenshot + masked-pHash + the app-change (`[APP]`) signal, not the window list**. (This is
the LEARNINGS "look at the actual pixels, don't infer from state" rule paying off — the screenshot, not
the introspection dump, was ground truth.) Plan-3 Backend B (trap oracle) or a CGrafPort/QD-global read
may be needed to introspect such apps later.

## 2026-06-06 — Installing classic Mac apps onto HFS images host-side, with resource forks intact

Populating an E2E "apps" disk host-side (no boot) the obvious way silently produces a **dead app** —
the resource fork and `APPL`/creator signature get dropped. Full procedure + code:
**`docs/HOST-SIDE-MAC-SOFTWARE-INSTALL.md`**. The load-bearing lessons:

- **`macutils`/`hexbin` is NOT in Homebrew.** Use **`unar`/`lsar`** (The Unarchiver, `brew install unar`)
  to decode `.hqx`/`.sit` (often "StuffIt in BinHex") — it preserves forks as macOS xattrs
  (`com.apple.ResourceFork` → readable at `"<file>/..namedfork/rsrc"`; `com.apple.FinderInfo` = type/creator).
- **`hcopy` (hfsutils) without `-m` is DATA-FORK-ONLY** — it loses the resource fork and stamps the file
  `????/UNIX`, so the app won't run. Repackage as **MacBinary II** (data + rsrc + FinderInfo, CRC-16-CCITT
  at offset 124) and use **`hcopy -m`**. Verify with `hdir`: want `APPL/<creator>  <nonzero-rsrc>  <data>`.
- **`hformat` makes a raw HFS volume and SheepShaver mounts it directly** — no Apple Partition Map needed,
  no in-emulator "Initialize?" prompt (verified: `modal=False` on boot with it attached).
- `os.getxattr` is **Linux-only**; on macOS Python read FinderInfo via the `xattr` *command*.
- **Launch an app on an attached data volume with keyboard type-select** (Finder: type the volume name →
  ⌘O → type the app name → ⌘O), gated on introspection — no fragile alias or coordinate-clicking.

Proof: AltiVec Fractal Carbon (download → `unar` → MacBinary → `hformat`/`hcopy -m`) installed onto
`e2e-apps.dsk`, `hdir` = `APPL/ddPF 8126 169060`, boot-verified to mount cleanly.

**Installing a system lib (CarbonLib) that ships only as an SMI installer + the dogfooding gap it
exposed.** Fractal Carbon (Carbon) needs CarbonLib ≥1.3; OS 9.0.4 ships ~1.0.x. CarbonLib comes only as
a `.smi` (compressed NDIF) — `unar`/`hmount` can't crack it. So we **drove the Apple Installer over VNC
with introspection** (`SheepShaver/e2e/run_carbonlib_install.py`) and it worked (CarbonLib → 1.6,
`hdir` `INIT/cbon 602351 3521150 Jun 17 2002`). Lessons:
- **The SMI's license alert is a `dialogKind` dialog** (introspection finds its "Agree" button by name),
  but the **Apple Installer's "Continue"/"Install" panels are movable-modal/document windows whose
  controls the dialog-only introspection couldn't see** → drove them with **Return** (default button).
  This gap is exactly what motivated emitting **window control-list items** (see `UI-INTROSPECTION.md` /
  `ui_introspect.cpp` `serialize_window_controls`) — a real dogfooding loop: real use exposed the hole.
- **Poll for each button** (the self-mount + panels appear seconds apart; one-shot clicks miss them).
- **Boot a dedicated *writable* master for the install** (the install must persist — not a per-run
  clonefile copy); **re-create it from clean between attempts** (a force-killed partial install leaves
  it dirty → next boot stalls in Disk First Aid → boot timeout).
- **Extract once, reuse forever:** pull the installed `CarbonLib` extension out as a MacBinary
  (`hcopy -m ":System Folder:Extensions:CarbonLib" CarbonLib_1.6_extension.bin`) → future installs are a
  one-line `hcopy -m` into Extensions, no SMI/boot. CarbonLib 1.6 source: **archive.org**
  (`download/tucows_207427_CarbonLib/carbonlib.sit` — token-free; macintoshgarden links 410 to `curl`).
  **Validated** (2026-06-06): baked the artifact into a clean clonefile of `macos9_fresh.dsk`, upgrading
  its bundled 1.0.x → 1.6 (`hdir` confirmed). **Caveat:** `hfsutils` writes **HFS only** (`BD` sig @ byte
  1024) — `macos9_fresh` is HFS so `hcopy` works; an **HFS+** (`H+`) boot disk would need a boot, not `hcopy`.

## 2026-06-05 — E2E benchmark auto-shutdown (keyboard quit-to-Finder); VNC clicks were never broken (misdiagnosis)

Automating the Speedometer benchmark to shut down **unattended** hit two non-obvious walls. The
fix that works is **keyboard-only**; the click rabbit hole below cost hours — read it before
trying to "just click something" over VNC.

**1. The Power-key shutdown hook only raises the Shut Down dialog at the FINDER, not over a
frontmost app.** The smoke test shuts down cleanly because it's at the Finder; the benchmark hung
because Speedometer was frontmost (no shutdown dialog ever appeared). Fix: get back to the Finder
first. Keyboard path that works: after saving the report, **Cmd-Q**, then answer Speedometer's
"Save before quitting?" (Yes/No/Cancel) and the record-save dialog with **Return** (= Yes = save
the Machine Record) until it quits to the Finder, where the hook shuts down. `scenario.py`.

**2. VNC mouse CLICKS were never broken — the whole investigation was a MISDIAGNOSIS** (it cost ~a
day across three wrong theories; this entry exists so nobody repeats it). The truth, finally pinned
down by building **both SDL2 and SDL3** and driving a clean test: **instant VNC `mousePress` clicks
work on both backends.** An instant click opens the Apple menu and drives a menu selection that
opens the *named* "About This Computer" window — a title noise can't fabricate, reproduced on both.
A second instant click also deactivates a front window. So the committed `SDL_PushEvent` injection
path is correct, and the "hold the button" change I'd committed was **reverted** (unnecessary;
`vnc.click` is back to a plain `mousePress`).

What actually happened: the harness's *desktop-click shutdown* didn't activate the Finder over
Speedometer, and I read that as "VNC clicks don't register." From there I chased three deep theories
in turn — a `RawMouse`/`MoveTo` Toolbox bug, a direct-ADB-from-the-libvncserver-thread rewrite, and
finally down/up "coalescing" (hold the button). **None were real.** The actual cause of that one
desktop-click failure was never confirmed (the click landing on a non-activating spot is plausible
but untested) and is **moot** — the shutdown uses the keyboard quit-to-Finder (§1), which is
reliable.

Not established — do NOT cite as fact: there is **no proven behavioral click difference between
SDL2 and SDL3**. `git 74886987` did change VNC injection from direct-ADB to `SDL_PushEvent` during
the SDL3 port (a code-history fact), but my earlier "direct-ADB-fails-on-SDL3" test was judged by
the *same* coordinate/empty-desktop confound, so it proves nothing. Both backends click fine with
the current code.

**Lesson (the expensive one): rule out the boring causes FIRST.** Wrong coordinates, clicking empty
space (no visible effect), and reading a *noisy* signal (the `frontApp` frames, §3) — confirm or
exclude these with one clean, unambiguous functional test (open a *named* window / read a `[APP]`
title) BEFORE theorizing about emulator internals. Three successive "deep bug" theories all
dissolved under one proper A/B.

**3. The idle-hook `frontApp` signal LIES — spurious `'Finder' Desktop` frames appear ~every 300
ticks even when Speedometer is really frontmost.** Gating on `"Finder" in e.app` gives false
positives (a `click-to-finder`/`back-to-finder` gate "passes" on noise). Don't trust a single
frontApp frame as proof of an app switch.

**4. Speedometer specifics:** "Save Text Report" (Cmd-T) saves a flat `Key:Value` text file under
the **default name** "Power Macintosh Report" (typing a custom name is unreliable — match the
default host-side). The text report has CPU/Graphics/Disk/Math + many sub-scores but **NOT `PR`**
(PowerRating is panel-only). "Save Machine Record" (Cmd-S) is the opaque binary records DB.

## 2026-06-05 — SDL3 shutdown crash = double-`SDL_DestroyMutex` in `VideoExit()` (+ two process lessons)

Switching to the SDL3 backend surfaced a clean-shutdown crash (E2E `code=-9`). Root cause: `Quit()`
calls `VideoExit()` **twice** — directly (`main_unix.cpp:1301`) and via `ExitAll()`
(`:1344`→`main.cpp:312`). `VideoExit()` (`video_sdl3.cpp`) destroyed `frame_buffer_lock`/
`sdl_palette_lock`/`sdl_events_lock` but never NULLed them, so the 2nd pass double-destroyed freed
mutexes → `os_unfair_lock is corrupt` abort in `pthread_mutex_destroy`. SDL2 tolerated it; SDL3's
mutexes are os_unfair_lock-backed. Fix: NULL each pointer after `SDL_DestroyMutex` (commit 3daa9c98).
The `if (lock)` guards already intended idempotency — they just never completed it.

**Process lesson 1 — don't trust shutdown/boot diagnostics on a VBL-degraded host.** A ~13 h, dozens-
of-launches session degrades the macOS VBL timer until boots hang in SCSI init (the "?" no-boot
icon). I burned a long time mis-diagnosing this crash as a Metal/`SDL_Quit` teardown deadlock because
the degraded env masked the real failure. A **host restart** restored reliable boots and the actual
crash backtrace appeared instantly. When boots are flaky, stop and restart before theorizing.

**Process lesson 2 — `printf` absence proves nothing.** "Shutdown complete." is `printf`→stdout,
block-buffered when piped, so it's LOST on a `kill -9`/crash. Its absence is not evidence the code
didn't run. Use `fprintf(stderr, …); fflush` for teardown markers, or read the crash report.

## 2026-06-05 — The `make build-ss` binary links SDL2, not SDL3 (stale generated `configure`)

Despite `configure.ac` and the docs defaulting to **SDL3**, the built SheepShaver links
`libSDL2-2.0.0.dylib` (`otool -L SheepShaver/src/Unix/SheepShaver | grep -i sdl`). Root cause: the
**generated `configure` is stale** — it was generated *before* `configure.ac` flipped the default to
SDL3 (mtimes: `configure` Jun 4 09:28 < `configure.ac` Jun 4 11:34), so the on-disk `configure` still
defaults to SDL2 and never runs the sdl3 pkg-config check. sdl3 *is* installed (pkg-config 3.4.10).
Fix: re-bootstrap — `cd SheepShaver/src/Unix && NO_CONFIGURE=1 ./autogen.sh && ./configure …` (no
`--with-sdl2`), then `make build-ss`.

**Debugging lesson (cost me a wrong conclusion):** a revert/edit of `video_sdl3.cpp` is a **no-op**
for this build — only `video_sdl2.cpp` is compiled in. I reverted the SDL3 file to test the
title-bar boot-hang hypothesis, it "still hung," and I wrongly inferred *environmental*. The crash
backtrace naming `libSDL2-2.0.0.dylib` was the authoritative backend signal all along. **Always
`otool -L` to confirm the linked backend before debugging/reverting SDL/video code.** The E2E
harness has therefore been validating SDL2, not SDL3.

## 2026-06-05 — SDL_SetWindowTitle is main-thread-only on macOS

`SDL_SetWindowTitle` calls into Cocoa (`NSWindow setTitle:`), which asserts "should only be
modified on the main thread" and aborts. The redraw thread (`redraw_func`, `do_video_refresh`)
is NOT the main thread. Calling `set_window_name()` from `do_video_refresh()` caused either an
abort or a VBL stall (same mechanism as the lldb VBL disruption — see below). The live JIT
stats title update was removed; the `ppc_jit_aarch64_get_stats()` API and `status_suffix` param
are kept for a future main-thread reimplementation. Correct approaches: SDL3's
`SDL_RunOnMainThread()`, or `dispatch_async(dispatch_get_main_queue(), ...)` on macOS, or
a custom SDL user event dispatched to the main thread.

## 2026-06-04 — Verified non-issues from the 2026-06-03 diagnostics review (do NOT re-investigate)

An adversarial review of the heartbeat/diag + `build-ss` guard work attacked these and proved
them safe — recorded so nobody re-burns the time. (Its still-open items were folded into
`docs/planning/sheepshaver-research/research/IMPLEMENTATION-BACKLOG.md` Tier D + A6/A7; the
standalone REVIEW-RECOMMENDATIONS doc was then retired.)

- **snprintf findings/line accumulation** — guarded with `(size_t)len < sizeof line` at every
  accumulating call; truncation short-circuits safely. No overflow.
- **`findings[6]` overflow** — the six rule groups are mutually exclusive `if/else-if`, so at
  most 6 fire simultaneously; the cap is exact.
- **Hot-path cost of `hb_tick`/`getrusage`/`task_info`** — out-of-line (confirmed via `nm` on
  `ppc-cpu.o`), behind the pre-existing 4096-block + 5s gate; getrusage/task_info run at
  hb_tick's own 10s/60s cadence, never per-block. No new per-block clock read.
- **Wrongful interpreter fallthrough from the diag changes** — the diag/heartbeat diffs touch
  only diagnostic blocks; dispatch/compile/chaining untouched. (The lwarx/stwcx/mftb fallthroughs
  are a separate, intentional correctness fix.)
- **Same-second double emulator start** — diag filename includes pid; logs stay distinct.
- **`build-ss` Darwin change** — dropping `| tail | || true` is a correctness improvement
  (failed builds now halt instead of silently relinking stale objects).

## 2026-06-03 (session 7 FINAL) — Mac OS 8.6 boots to Finder desktop with full native JIT

### RESOLVED: all investigation items from session 7 are closed

Mac OS 8.6 boots to the Finder desktop with full native JIT — no skip list, no workarounds.
ROM=0x500000 (full range including DR emulator), block chaining enabled. Harness: 235/235,
score=100.

### 8 bugs found and fixed

| # | Bug | Root cause | Fix |
|---|-----|-----------|-----|
| 1 | subfe/adde carry-out | ADDS reads partial carry from two-operand sum, not full three-operand CA | 64-bit arithmetic, extract bit 32 |
| 2 | mftb TBU/TBL | CNTVCT_EL0 stored as-is for both halves (TBU and TBL returned identical values) | LSR #32 for TBU (later superseded by bug 8's interpreter fallback) |
| 3 | DR emulator entry-poll suppression | Block-entry spcflags poll fires mid-dispatch-cycle, injecting CR2.LT too early | Skip poll for DR emulator blocks |
| 4 | icbi NOP | JIT compiled icbi as NOP; stale translations survived code rewrites | Fall through to interpreter's execute_icbi |
| 5 | isync NOP | JIT compiled isync as NOP; deferred icbi invalidation never flushed | Fall through to interpreter |
| 6 | UXTW addressing mode | Register-offset mem access used LSL (option=011), risking upper-32-bit garbage | Changed to UXTW (option=010) for defensive 32-bit extension |
| 7 | fmsub/fnmsub encoding swap | PPC fmsub (a*c-b) was mapped to ARM64 FMSUB, but ARM64 FMSUB computes a-n*m (the opposite sign). PPC fnmsub (-(a*c-b)) had the same problem in reverse | Swapped: PPC fmsub -> ARM64 FNMSUB (n*m-a), PPC fnmsub -> ARM64 FMSUB (a-n*m) |
| 8 | lwarx/stwcx./mftb interpreter fallback | lwarx/stwcx. need CPU-object reservation state (not in regs struct); mftb needs the CPU's timebase model (not raw ARM64 counter) | All three return false from compile_one(), falling through to interpreter |

### Opcode bisection strategy (key for bugs 7-8)

The extension-loading hang could not be isolated by address (Mac OS loads extensions at
different RAM addresses each boot). The opcode-based bisection narrowed it:

1. `SS_JIT_SKIP_OPC=<all 64>` — boot passes (all instructions via interpreter)
2. Removed opcodes from skip list in groups — narrowed to primary opcodes 31 and 63
3. `SS_JIT_SKIP_XO63=28,30` — isolated XO63 sub-opcode 30 (fnmsub) as one cause
4. `SS_JIT_SKIP_XO=20,150,371` — isolated XO31 sub-opcodes 20 (lwarx), 150 (stwcx.), 371 (mftb)

This method works even when code addresses shift between boots (extension ASLR).

### Previous "OPEN" items — all RESOLVED

- **Extension-loading hang**: RESOLVED — caused by bugs 7 (fmsub/fnmsub) and 8 (lwarx/stwcx./mftb)
- **Cross-instruction state leak hypothesis**: RESOLVED — was a red herring; the actual causes were
  incorrect FP multiply-add encodings and missing CPU-object state access
- **Skip-list workaround**: NO LONGER NEEDED — full native JIT boots cleanly
- **"CORRECTION: desktop was Disk First Aid"** (earlier session 7 entry): RESOLVED — the final
  boot after all 8 fixes reaches the real Finder desktop, not Disk First Aid

---

## 2026-06-03 — `make build-ss`/`make test-opcodes` were destructive on macOS (now fixed)

### The trap

`SheepShaver/Makefile` is upstream's (rcarmo) **Linux** dev harness. Its `build-ss` target
hand-compiled `ppc-jit.o` with minimal flags and hand-linked with GTK/X11 libraries. On
macOS that link fails — and **the failed linker deletes the existing `SheepShaver` binary**
(ld removes its partial output). So running the documented test command `make test-opcodes`
silently destroyed the working binary. A running emulator process survives (its image is
memory-mapped), but every subsequent launch fails until someone rebuilds.

This bit us on 2026-06-03: an agent ran `make test-opcodes`, the binary vanished mid-session
while another agent's boot tests were cycling. Recovery: `rm obj/ppc-jit.o` (it had been
overwritten with wrong-flag output), then `cd src/Unix && make` (the autoconf build owns
the JIT on this branch and links correctly).

### The fix (Option A — minimal remediation, committed)

`build-ss` is now platform-guarded with `uname -s`: Darwin delegates entirely to
`cd src/Unix && make -j$(sysctl -n hw.ncpu)`; the Linux recipe is preserved byte-for-byte.
`make test-opcodes` now works end-to-end on macOS.

### The proper fix (Option B — for an eventual upstream PR, not yet done)

Upstream's hand-rolled JIT compile/link exists because **their configure doesn't know about
the JIT**. This branch's `configure.ac` already puts `ppc-jit.cpp` in `CPUSRCS`. Porting that
to Linux ARM64 makes `build-ss` collapse to `cd src/Unix && make -j<n>` on both platforms.
Needs coordination with upstream (only they can test Linux ARM64). See
`docs/superpowers/specs/2026-06-03-build-ss-macos-fix-design.md`.

### Related: per-instance JIT diag logs (same day)

The diag log default changed from shared `/tmp/jit_diag.log` to per-instance
`/tmp/jit_diag.<timestamp>.<pid>.log` with `/tmp/jit_diag.log` as a symlink to the latest
run (commit 8aab88de). Concurrent runs no longer truncate each other's logs.
`SS_JIT_DIAG_LOG` still overrides (no symlink touched). Verified: 235/235 harness,
~35 unique log files from one morning of boot runs, zero clobbering.

## 2026-06-03 (session 7 continued) — three JIT bugs fixed, boot reaches extension loading — RESOLVED (see session 7 FINAL above)

### CORRECTION: "desktop" was Disk First Aid dialog, not Finder — SUPERSEDED: final boot reaches real Finder desktop

The VNC screenshot showing the Mac OS desktop pattern was actually the Disk First Aid
dialog (triggered by kill -9 leaving the disk dirty). After dismissing it, boot continues
to "Starting Up..." and hangs at ~10% progress. This hang is a **pre-existing JIT bug**
confirmed at commit 77d47baa (before all session 7 changes). The interpreter boots the
same 8.6 ISO to Finder desktop in ~2 minutes.

### Three JIT bugs fixed this session

1. **subfe/adde carry-out** (the DR emulator unlocker): three-operand CA computation
   was reading carry from partial ADDS, not the full sum. Fix: 64-bit arithmetic.
2. **mftb TBU/TBL**: CNTVCT_EL0 was stored as-is for both TBL and TBU, giving
   identical values. Fix: LSR #32 for TBU.
3. **DR emulator entry-poll suppression**: block-entry spcflags poll removed for
   DR emulator blocks to prevent premature CR2.LT injection.

### Pre-existing extension-loading hang (NOT caused by our changes)

Traced to a tight loop: ROM epilogue at 0x50132ec8 (function return stub with
`addic r1,r1,64` + `mtspr LR` + `blr`) returns to RAM at 0x10662304 repeatedly.
The epilogue itself is correct — the bug is in the RAM-resident Mac OS code that
calls it. Affects both ROM=0x460000 and ROM=0x500000, both HD and ISO boot.
Same binary-search methodology applies but needs RAM-region exclusion support.

See `docs/PPC-ARM64-JIT-LESSONS.md` for the architectural narrative — why these
bugs aren't Apple-specific and what they teach about PPC→ARM64 JIT in general.

### Extension-loading hang investigation (continued) — RESOLVED (see session 7 FINAL: bugs 7-8 were the root cause)

**icbi (instruction cache block invalidate) was NOP'd** — fixed by falling through
to the interpreter's execute_icbi, and wiring invalidate_cache_range to call the JIT's
ppc_jit_aarch64_invalidate_range. This is a correctness fix but did NOT resolve the
hang: comp=32079 stays frozen (no new blocks compiled), proving stale-code execution
is not the active cause.

**RAM addresses shift between boots** — Mac OS loads extensions at different base
addresses each run. Hot PCs: 10695358 in run 1 vs 106953b8 in run 2. This is why
address-based binary search (SS_JIT_INTERP_RANGE) can't isolate the bug with sub-
ranges — the code moves. Only interpreting ALL RAM (10000000-20000000) works.

**Opcode-based search needed** — added SS_JIT_SKIP_OPC and SS_JIT_SKIP_XO env vars
to force specific instruction types to the interpreter. This enables testing "which
PPC instruction type, when JIT-compiled, causes the hang" regardless of address.

**Pitfall: "skip ALL opcodes" is not a framework discriminator.** In this JIT, a
forced fallback (`compile_one()` returns false) emits an inline interpreter call and
then **ends the block immediately** (conservative: the handler may branch / change PC).
If you skip everything, you effectively interpret **one PPC instruction per JIT
dispatch**, which is orders of magnitude slower and will typically hit watchdog
timeouts even when semantics are correct. Prefer ring-driven targeted skips: dump the
hang loop's last N blocks, histogram the primary opcodes / XO values actually present,
then skip those small sets.

**What we know about the hang**:
- 32K unique blocks compiled during boot, then the same set loops forever
- jNK frozen (nanokernel never re-entered after initial boot)
- Block chaining is NOT the cause (SS_JIT_NO_CHAIN=1 still hangs)
- ROM JIT is NOT the cause (SS_JIT_NO_ROM=1 + RAM JIT still hangs)
- icbi/SMC is NOT the cause (no new blocks compiled during hang)
- The interpreter boots the same 8.6 ISO to Finder desktop in ~2 min

### Opcode-based binary search: cross-instruction state leak confirmed — RESOLVED (see session 7 FINAL: root cause was bugs 7-8, not state leak)

**Workaround found** (no longer needed — all 8 bugs fixed): boot reaches Finder desktop with this skip list:
```
SS_JIT_SKIP_OPC=21,28,29,30,31,32,33,43,44,45,46,47,48,49,50,51,57,61,62,63
```
These opcodes are forced to the inline interpreter call; all others compile natively.

**Key test results** (discriminating the bug class):

| Test | Result | What it proves |
|------|--------|----------------|
| `SS_JIT_SKIP_OPC=<all 64>` | PASS (desktop) | Interpreting every instruction works |
| `SS_JIT_MAX_INSNS=1` (all native, 1/block) | FAIL (stuck) | Single native instructions fail |
| Remove any ONE opcode from skip list | PASS | No single opcode is solely responsible |
| Allow {30,31} native, rest interpreted | FAIL | Multiple native opcodes needed to trigger |
| Opcode 30 (rld*) never appears during boot | — | It's a no-op in the skip list |

**Initial diagnosis**: the bug was hypothesized as a **cross-instruction state leak** —
native instructions sharing dirty ARM64 register/flag state within a block. The inline
interpreter call flushes all state (lazy_flush_cr0 + ra_flush_all + block termination)
before each instruction, which is why SKIP_ALL passes.

**However, targeted experiments refuted all three state-leak candidates** (see
"2026-06-03 — deeper investigation" below):
- Lazy CR0 flush before every instruction: still hangs
- RTMP register zeroing before every instruction: still hangs
- PC store before every instruction: still hangs
- Per-block register verification (SS_JIT_VERIFY): zero RAM divergences detected

The actual mechanism is subtler — the inline interpreter call does not just flush state,
it also **terminates the block**, returning to the C dispatcher where spcflags are checked.
The difference between SKIP_ALL (pass) and MAX_INSNS=1 (fail) may be about dispatcher
re-entry frequency or some non-register side effect, not dirty ARM64 state per se.

**Why the inline interpreter call fixes it**: `emit_inline_interp_call()` calls
`lazy_flush_cr0()` + `ra_flush_all()` before the BLR, then the block ends with a bare
epilogue. This flushes all dirty state to memory AND returns to the C dispatcher. The
next block starts with fresh register state loaded from the struct AND spcflags are
checked. Native instructions that succeed DON'T flush between themselves — they share
the dirty state AND the block continues without a dispatcher round-trip.

## 2026-06-03 (session 7 continued, later) — deeper investigation, state-leak hypothesis weakened — RESOLVED (see session 7 FINAL above)

### Additional bugs fixed

4. **icbi NOP → interpreter fallback**: icbi (instruction cache block invalidate) was
   compiled as a NOP. Fixed by falling through to the interpreter's `execute_icbi` and
   wiring `invalidate_cache_range` to call `ppc_jit_aarch64_invalidate_range`. Correctness
   fix but did NOT resolve the extension-loading hang.
5. **isync NOP → interpreter fallback**: isync (instruction synchronize) was compiled as
   a NOP. Fixed by falling through to the interpreter to flush any deferred icbi
   invalidation. Same correctness fix; did NOT resolve the hang.
6. **UXTW for register-offset memory access**: defensive fix to use option=010 (UXTW,
   zero-extend W to X) instead of option=011 (LSL) for register-offset loads/stores.
   Ensures 32-bit guest addresses in 64-bit registers don't carry upper garbage bits.
   Did NOT resolve the hang (W-ops already zero upper 32 bits on ARM64).
7. **mftb TBU/TBL distinction**: already fixed earlier in session 7 (listed above for
   completeness).

### Ruled-out causes of the extension-loading hang

Every experiment below was tested; all still hang at "Starting Up...":

| Experiment | Result | What it rules out |
|-----------|--------|-------------------|
| Lazy CR0 flush before every instruction | HANG | CR0 state leak |
| RTMP register zeroing before every instruction | HANG | Temp register collision |
| PC store before every instruction | HANG | Stale PC in regs struct |
| UXTW memory access encoding | HANG | 32-bit address extension |
| Block chaining disabled (SS_JIT_NO_CHAIN=1) | HANG | Chain-related interrupt miss |
| icbi/isync coherence fixes | HANG | Stale JIT code |
| Block length MAX_INSNS=1 | HANG | Multi-instruction interaction |
| Block length MAX_INSNS=2,4,64,512 | ALL HANG | Block size effects |
| Any single opcode skipped from native | PASS | Misleading — frequent interpreter calls mask bug |
| Per-block register verification (SS_JIT_VERIFY) | 0 divergences | Register/memory corruption per-block |

### Key discovery: SKIP_ALL vs MAX_INSNS=1

- **SKIP_ALL (every instruction via interpreter call) PASSES** — boots to Finder desktop
- **MAX_INSNS=1 (every instruction native, 1 per block) FAILS** — hangs

Both execute one PPC instruction per dispatch. The difference: SKIP_ALL uses the C
interpreter handler (which executes the full semantic including all side effects through
the C++ code path); MAX_INSNS=1 uses native ARM64 codegen. This proves at least one
native instruction codegen produces different behavior than the interpreter, but
SS_JIT_VERIFY detected zero divergences — the difference is either too subtle for the
register comparison or manifests only over thousands of instructions.

### Memory dump comparison

At t=15s into boot, JIT and interpreter memory states differ by **1.7M bytes**. Key
findings:
- Low-memory globals (0x10, 0x20) are NULL under JIT but valid under interpreter
- **These NULLs are downstream symptoms** — boot hangs BEFORE the code that writes them
- The actual stall is in RAM code at 10653b60/10695xxx (spin-wait), NOT in ROM

### Trace comparison

Execution chains (block dispatch sequences) match between JIT and interpreter for the
first ~2000 blocks. A transient 20-byte SP difference appears and self-corrects. No
persistent divergence in the trace itself — the bug accumulates over many more blocks.

### RAM address instability

Mac OS loads extensions at different base addresses each run. Hot PCs shift between
boots (e.g., 10695358 in run 1 vs 106953b8 in run 2). This is why address-based binary
search (SS_JIT_INTERP_RANGE) cannot isolate the bug with sub-ranges — only interpreting
ALL RAM (10000000-20000000) works.

### Previous entry (partially superseded)

Final state after session 7:
- **subfe/adde carry bug** found and fixed (64-bit carry computation)
- **ROM=0x500000** (full range, DR emulator JIT-compiled) — default config
- **Block chaining=1** — enabled by default
- **VNC screenshot confirms** visual boot to Finder desktop
- **Harness**: 235/235, score=100
- **j2i transitions**: reduced from 2.4M/s to 6K/s (DR emulator no longer interpreted)
- Default ROM range updated from 0x460000 to 0x500000 in Makefile/code
- The "spcflags timing" theory (sessions 5-7a) was a red herring — actual root cause was
  carry codegen, not interrupt delivery timing

### ROOT CAUSE FOUND: subfe (XO=136) carry-out computation was wrong

The `subfe rD,rA,rB` instruction computes `rD = ~rA + rB + CA` and sets CA to the carry-out
of the full 33-bit unsigned sum. The JIT used a two-step approach: ADDS (~rA + rB) then ADD
(+CA). The `emit_write_xer_ca_from_carry()` read the ARM64 carry flag from the ADDS — but
that only reflects the carry from (~rA + rB), not the full sum including +CA.

For the idiom `subfe r4,r4,r4` (carry-to-mask), ~r4 + r4 = 0xFFFFFFFF which NEVER carries
on ARM64. So the JIT always wrote CA=0 regardless of input CA. The correct behavior:
CA_out = CA_in (since 0xFFFFFFFF + 1 wraps to carry, 0xFFFFFFFF + 0 doesn't).

**Impact**: corrupted CA propagated through the DR emulator's address calculation chains
(subfe/adde sequences used for 68k multi-precision arithmetic), causing wrong dispatch
addresses and infinite SCSI retry loops.

**Fix**: compute the full sum in 64 bits (UXTW + two 64-bit ADDs), extract bit 32 as carry.

**This is NOT Apple Silicon specific** — the bug would manifest on ANY ARM64 platform
(Linux, Raspberry Pi, Orange Pi). The ARM64 ADDS instruction legitimately doesn't carry
for this specific input pattern. The issue is architectural: PPC's `subfe` carry semantics
require the carry from the FULL three-operand sum, not a partial two-operand ADDS.

### Boot verified: ROM=0x500000, full DR emulator JIT-compiled

| Metric | Before fix (ROM=0x460000) | After fix (ROM=0x500000) |
|--------|---------------------------|--------------------------|
| jDR blocks/s | 0 (interpreted) | 37M (JIT-compiled) |
| j2i transitions/s | 2.4M | 6K (virtually eliminated) |
| jRAM at t=15s | 919M | 903M (comparable) |
| Boot progress | Working | Working |
| SCSI scan | Completes normally | Completes normally |

---

## 2026-06-02 (session 7) — Boot time measurements and the core JIT imbalance

Summary: Measured HD and CD boot times in both modes. The JIT accelerates the PPC nanokernel
82x but the DR (68k) emulator stays interpreted and actually runs 1.8x *slower* under JIT
due to transition overhead. Net effect: JIT makes the CPU do 7x more total blocks/s but
boot progress (driven by 68k work) is slower. Fix: JIT-compile the DR region.

### Boot time measurements (HD boot, OldWorld ROM + macos921.dsk) — RESOLVED

**NOTE**: These measurements were taken BEFORE the DR emulator was JIT-compiled (ROM=0x460000).
With all 12 bugs fixed (ROM=0x500000, chaining=1), JIT HD boot takes ~10s — same as interpreter.
Both HD (macos86_fresh.dsk) and ISO boot work reliably with no workarounds.

- **Interpreter HD boot to desktop: ~12 seconds.** 535M blocks at t=5s, rate crashes to
  3.8M/s by t=15s (desktop idle signature).
- ~~**JIT HD boot: 100+ seconds at steady 96M blocks/s.**~~ RESOLVED: was caused by
  DR emulator running interpreted (ROM=0x460000). With ROM=0x500000, JIT HD boot is ~10s.
- ~~**Open question**: stuck or detection failure?~~ RESOLVED: was genuinely slow due to
  interpreted DR emulator, not stuck.

### Boot time measurements (CD boot, Mac OS 8.6 ISO)

- **Interpreter CD boot to desktop: ~318 seconds (5 min 18s), 6.7B total blocks.**
- **JIT CD boot: partial measurement only, steady 96M blocks/s.**

### The core imbalance (the key insight) — RESOLVED

**NOTE**: This imbalance was RESOLVED by JIT-compiling the DR emulator (ROM=0x500000) after
fixing all 12 bugs. j2i transitions dropped from 2.4M/s to ~70/s. Both HD and ISO boot
now take ~10s with JIT.

The PPC JIT accelerates the nanokernel 82x (0.4M → 33M blocks/s) but the DR (68k)
emulator ~~stays interpreted~~ was interpreted at the time of this measurement. ~~Worse,~~
the DR emulator ran 1.8x SLOWER under JIT mode (46M → 26M blocks/s) due to transition overhead:

- **2.4M JIT↔interpreter transitions per second** during JIT-mode boot (now ~70/s with ROM=0x500000).
- **NK:DR ratio flips from 1:21 (interpreter) to 1:1 (JIT)** — the JIT-speed PPC hits
  exception dispatch ~20x more frequently relative to the 68k work being done.
- ~~**Net effect**: boot progress slower because DR unaccelerated~~ RESOLVED: DR is now JIT-compiled.
- ~~**Fix direction**: JIT-compile the DR region~~ DONE: ROM=0x500000 with entry-poll suppression
  and all 12 bug fixes.

### Reconciliation: the "10s interpreter boot" discrepancy

Session 5 part 2 declared the "interpreter boots in ~10s" claim FALSE (lines 102-105
below). **That verdict was wrong — it tested only CD boot (~318s) and concluded the
claim was unreliable.** The ~10s figure was always HD boot (pre-installed disk image
with warm NVRAM). Both numbers are correct for their respective configurations:

- HD boot (macos921.dsk): interpreter ~12s, consistent with prior claims.
- CD boot (Mac OS 8.6 ISO): interpreter ~318s, consistent with session 5 part 2 data.

The speedup/slowdown ratios derived from comparing HD interpreter (12s) to CD JIT
measurements remain unreliable — they were comparing different boot media.

### Opcode encoding error corrected: bclr 5,8 = 0x4CA80020, not 0x4C420020

All prior sessions (4, 5, 6) carried the wrong hex constant for the DR emulator interrupt
gate instruction. `bclr BO=5, BI=8` encodes as `0x4CA80020` (primary=19, BO=5, BI=8, XO=16).
The previous constant `0x4C420020` decodes as `bclr 2,2` (BO=2, BI=2 — decrement CTR, branch
if CTR==0 AND CR0.EQ==0), which is a completely different instruction. Caught by adversarial
validator review during the Option A implementation. Fixed in the code, LEARNINGS, and
CHAINING-VERIFICATION-PLAN.

### ROM=0x500000 investigation: three approaches tried, root cause narrowed

**Experiment 1: Option A alone (inline spcflags check before bclr 5,8)**
Result: SCSI loop hangs. DR emulator fully JIT-compiled (jDR=584M at t=5s, j2i=33).
The bclr 5,8 opcode (`0x4CA80020`) was confirmed correct (10 DR gate sites compiled).
Interrupts delivered (~55/s), but jRAM stuck at 33 — boot never progresses past SCSI.

**Experiment 2: Entry-poll suppression for DR blocks**
Result: Same SCSI loop hang. Suppressing `emit_entry_spcflags_poll` for blocks in
ROM+0x460000..0x500000 didn't fix it. The C dispatcher's between-block spcflags check
(ppc-cpu.cpp line 1199) still fires between the DR dispatch block and the toolbox handler,
setting CR2.LT mid-dispatch-cycle.

**Experiment 3: Full spcflags deferral (skip check when inside DR region)**
Result: Interrupts completely starved. jNK went flat at 810K (VBL never fires), jDR
grew at 200M/s. Deferring ALL spcflags checks while in/near the DR region prevents
interrupt delivery entirely — the bclr 5,8 never sees CR2.LT=1.

**Root cause narrowed:** The fundamental problem is that the JIT separates the DR
dispatch cycle into multiple blocks (dispatch head + handler) with spcflags checks
between them. The interpreter's decode cache runs the entire cycle as one block.
The extra spcflags check point in the JIT sets CR2.LT at the wrong time, causing
the bclr 5,8 interrupt gate to fire prematurely. Fully deferring starves interrupts.

**Additional finding:** DR emulator region has 57 fallback instructions (23 `lswx`,
11 `stswx`, 17 EMUL_OP, 1 `stbcx.`). Each fallback splits the enclosing block,
creating additional spcflags check opportunities.

**Proposed fix direction:** Instead of suppressing/deferring spcflags checks, make the
JIT deliver interrupts at the CORRECT point — after bclr 5,8 falls through to the
interrupt path — by NOT calling HandleInterrupt (which sets CR2.LT) during the mid-
dispatch-cycle check_spcflags calls. Only call HandleInterrupt when re-entering the
DR dispatch HEAD (where bclr 5,8 can evaluate it). This requires tracking "inside DR
dispatch cycle" state in the C dispatcher.

### Additional finding: bdnz loop at 5046db24 is NOT the infinite loop

Traced block 5046db24 which appeared to loop to itself indefinitely. Decoded as a
counted bdnz copy loop (lhau/or/stwu/bdnz with CTR=150 initial). CTR correctly
decrements each iteration. The loop exits normally. The SCSI infinite loop is at
a HIGHER level — the 68k SCSI scanning code repeatedly invoking SCSIGet/SCSISelect.
This narrows the bug: it's NOT a tight PPC loop stuck on a wrong branch condition.

### Binary search result: failing region is ROM+0x467E00..0x467F00 (256 bytes)

Using `SS_JIT_ROM_SIZE` env var, binary-searched to isolate the exact failing code:
- 0x466000 (+24KB): PASS — includes all 6 main DR dispatch variants (0x466080-0x466120)
- 0x467E00: PASS — last safe boundary
- 0x467F00: FAIL — first 256 bytes that break boot
- 0x468000: FAIL

The failing code at 0x50467E00-0x50467F00 is NOT the main dispatch variants — those work.
It's auxiliary DR emulator code containing: `bcctr 12,5` (0x4D850420), `bclr 5,8`
(0x4CA80020), various `rlwimi`/`lhau` patterns, and 68k instruction handler dispatch.

Key opcodes in the failing blocks:
- `4D850420` = bcctr BO=12 BI=5 (branch to CTR if CR[5]=1 i.e. CR1.SO)
- `4CA80020` = bclr 5,8 (interrupt gate)
- `509d1b78` / `537d1b78` = rlwimi with various insert masks
- `af780002` = lhau (halfword load-update)
- `7f64c2ee` = unknown XO31 (needs decode)
- Multiple `48000004` sequences = series of `b +4` (NOP-equivalent padding)

**Interrupt timing ruled out as root cause**: with correct interrupt prediction
(jNK growing at 12M/s, ~110 fallbacks/s) the SCSI loop still hangs. The bug is a
codegen correctness issue in one of these blocks, not spcflags timing.

Next step: decode all instructions at 0x50467E00-0x50468000, identify which specific
opcode's JIT codegen produces wrong results, and add a targeted harness test.
It's the Mac OS ROM's 68k SCSI scan logic deciding to rescan after each attempt.

### Missing terminator: `bc` (opc=16) not in is_terminator list

The block at 5046db24 compiled 10 instructions (n=10) despite having a bdnz backward
branch at offset +0xC. The `is_terminator` check (ppc-jit.cpp:4291-4295) only recognizes
`b` (opc=18), `bclr` (opc=19 xo=16), and `bcctr` (opc=19 xo=528). It does NOT recognize
`bc` (opc=16). This causes blocks containing conditional branches to include dead-code
fall-through paths. Not a correctness bug (the bdnz codegen correctly emits both paths)
but a code-size/cache efficiency issue. Adding `opc==16` to the terminator check would
be a minor optimization but is NOT the cause of the SCSI hang.

### Session 7a crashed due to API error loop

The advisor tool triggered repeated 400 errors in the first half of the session.
Session was restarted.

---

## 2026-06-02 (session 5, part 2) — RETRACTION: the "deadlock" theory was wrong

### What we got wrong (read this before the section below)

The session 5 "spin-wait deadlock" theory (next section) was based on register-state inference
without ever disassembling the actual instructions at 0x50313d34. When we finally dumped the
live ROM memory (single lldb attach) and ran it through capstone, every pillar of the theory
collapsed:

1. **0x50313d34 is NOT a spin-wait.** It is the nanokernel's exception/interrupt dispatcher —
   a comparison chain that matches a masked PC (r3) against a table of handler addresses stored
   at KernelData+0x340/0x344/0x348/0x350/0x358, then dispatches. It is the hottest code path
   in the entire system, which is exactly why the 5-second heartbeat keeps sampling it.

2. **"STUCK at pc=50313d34" is a sampling artifact**, not a hang. The STUCK detector fires when
   two consecutive 5-second heartbeats land on the same PC. For the hottest dispatch PC, that
   happens by chance. The system was never stuck there.

3. **r11 = 0x0002f072 is the MSR value, not a pointer.** The ROM patch at rom_patches.cpp:1155
   even says so: `ori r11,r11,0xf072 (MSR)`. Our entire "[r11+4] task field at 0x2f076" theory
   was built on misreading an MSR value as a pointer.

4. **r9 = 0x68fff400 is loaded from KernelData+0x344** (`lwz r9, 0x344(r1)`), a legitimate
   kernel handler-table entry — not from guest address 0x2f076.

5. **The a46cda99 "fix" was wrong and potentially harmful.** It wrote 0 to guest 0x2f076
   whenever that address contained 0x68fff400 — corrupting whatever Mac OS data structure
   happens to live there. It was reverted (commit 9f9e617e). The apparent improvement
   (0 STUCK events) was the sampling artifact described in #2, not a real fix.

**Disassembly of the actual code at 0x50313d20-0x50313d58** (from live memory dump):
```
50313d20:  and. r8, r4, r13          ; CR0 test
50313d24:  lwz r9, 0x340(r1)         ; r9 = [KernelData+0x340]
50313d28:  rlwinm r8, r3, 0, 0, 0x19 ; r8 = r3 & 0xFFFFFFC0
50313d2c:  cmpw cr1, r8, r9
50313d30:  bne 0x50312cf0            ; back to dispatcher
50313d34:  lwz r9, 0x344(r1)         ; <<< the "STUCK" PC — just a table load
50313d38:  bne cr1, 0x50313d5c
50313d3c:  li r8, 1
50313d40:  stw r8, 0x2810(0)         ; XLM global write
50313d44:  lwz r8, 0x648(r1)
50313d48:  mtcrf 0x3f, r7
50313d4c:  clrlwi r7, r7, 8
50313d50:  stw r8, 0x5c(r9)
50313d54:  stw r9, 0x65c(r1)
50313d58:  b 0x50312b54              ; back to dispatcher
```

### Methodology lessons

- **Never infer code behavior from register state alone.** Disassemble the actual instructions
  first. A single lldb attach (dump + immediate detach) is safe and takes 30 seconds.
- **The STUCK detector needs to be smarter**: it should verify the PC is in a tight loop
  (e.g., check that block-count delta between heartbeats is small, or track PC variety),
  not just "same PC sampled twice."
- **Verify-before-commit**: a46cda99 was committed without meeting the project bar
  (boot-to-desktop verification). The follow-up data that contradicted it arrived within
  the hour. Fixes based on unverified theories should stay uncommitted experiments.

### The REAL question (still open): where does JIT boot time actually go?

Interpreter boots in ~10s. JIT takes 10-22+ minutes. The JIT executes ~25M blocks/sec
(measured), which at ~5 instructions/block is ~125 MIPS — faster per-instruction than the
interpreter. Yet boot takes 60-130x longer. **This means the guest executes vastly more
instructions in JIT mode, OR the per-block overhead dominates.**

Leading hypothesis (UNVERIFIED — needs the region-counter diagnostic below): the DR emulator
(68k emulation, ROM 0x460000-0x500000) is NOT in the JIT range, so it runs in SheepShaver's
PPC interpreter. During boot, 94-97% of dispatches are 68k emulation. In JIT mode, execution
ping-pongs between JIT-compiled nanokernel code and interpreted DR-emulator code at high
frequency, paying dispatch/transition overhead every time. In pure interpreter mode there
are no transitions — everything stays in the interpreter's optimized block-link loop.

**Next diagnostic**: per-region block counters in the heartbeat (nanokernel/DR/RAM split,
plus JIT↔interpreter transition count). Run both modes, compare. This tells us definitively
whether the time goes to (a) transition overhead, (b) more guest work, or (c) slow
interpretation of DR code under JIT mode.

### Region-counter data (session 5 part 2) — first results

Region counters implemented (commit d0307ad6) and run in both modes:

**JIT mode at t=65s** (still booting):
- 4.3B JIT blocks: jNK=2.14B (32%), jRAM=2.18B (33%), jDR=0
- 2.4B interpreted blocks: all in DR region (35% of total)
- j2i transitions: 158M = **2.4M transitions/sec**
- Combined throughput: ~103M blocks/sec

**Interpreter mode at t=105s** (still booting):
- 615M blocks total: iNK=11.2M (2%), iDR=237M (39%), iRAM=366M (59%)
- Throughput: 8.6M blocks/sec for first 70s, then drops to ~390K blocks/sec
  (rate drop suggests entering an I/O-bound or idle-wait boot phase)

**Three major findings:**

1. **SUPERSEDED by session 7**: ~~The "interpreter boots in ~10s" claim is FALSE~~.
   The ~10s figure was HD boot (macos921.dsk); session 5 only tested CD boot (~318s).
   Both numbers are valid for their respective boot media. See session 7 reconciliation.

2. **NK:DR block ratio anomaly**: interpreter mode executes 1 nanokernel block per ~21 DR
   blocks; JIT mode executes ~1 nanokernel block per DR block (1:1). The JIT-mode guest
   enters the nanokernel exception dispatcher at a proportionally ~20x higher rate. Cause
   unknown — candidates: EMUL_OP frequency, Mixed Mode switch frequency, sc/trap frequency,
   or a feedback loop where fast PPC code polls/retries something completed by slow 68k code.

3. **Block counts are not directly comparable across modes** (DR dispatch blocks are 2-4
   instructions; RAM/toolbox blocks are larger). Wall-clock boot time to desktop is the only
   honest comparison metric. **SUPERSEDED by session 7**: both modes' boot times are now
   measured — interpreter HD ~12s, interpreter CD ~318s, JIT HD 100+s (detection issue).

**Hardware note**: the i2j (interp→JIT) counter always reads 0 — the increment was never
added at the interpreter's JIT-handoff break (ppc-cpu.cpp ~line 1295). j2i is a good proxy
(every j2i is eventually followed by an i2j). Fix when convenient.

### Ground truth data point #1: interpreter boots in ≤16 minutes (CD config)

The SS_USE_JIT=0 run (started 20:03) reached the Finder desktop by 20:19 (Mac clock 8:19 PM
visible in screenshot) — **interpreter boot ≤ 16 minutes** with the CD-ROM boot config.
CPU time was only ~3.6 min over 18 min wall → the interpreter spends most of boot
idle/waiting, not computing. Exact desktop-reached time unknown due to the log clobbering
below; Agent A should re-run with SS_JIT_DIAG_LOG set for a precise number.

Idle-desktop log signature (useful for boot-completion detection): dense alternating
interrupt deliveries at two DR-emulator PCs (0x50466084 / 0x50466094) at ~60Hz, with the
block rate dropping to near zero.

### GOTCHA: diag log was hardcoded — concurrent SheepShavers clobber each other

`/tmp/jit_diag.log` was opened with fopen(...,"w") by every SheepShaver process that runs
5+ seconds. While the interpreter ground-truth run was in progress, the jit-test harness ran
(233 vectors × 2 modes = 466 short-lived SheepShaver processes); at least one truncated the
interpreter's log mid-measurement, interleaving JIT-mode lines into it and destroying the
boot-progress record.

**Fix (committed)**: `SS_JIT_DIAG_LOG=<path>` env var; default remains /tmp/jit_diag.log.
Every parallel agent MUST set a distinct path. The harness should also set it (to /dev/null
or a temp path) to avoid clobbering concurrent measurements.

### Harness verification at d0307ad6

233/233, score=100 — the region counters + heartbeat throttle in the hot dispatch path do
not regress opcode correctness.

## 2026-06-02 (session 5, part 1 — SUPERSEDED, see retraction above) — Root cause of 7-minute JIT boot found

### Root cause: initialization-order deadlock at nanokernel spin-wait 0x50313d34 [WRONG — see retraction]

**Symptom**: JIT boot takes 7-22+ minutes; interpreter boots in ~10 seconds.

**The spin-wait**: ROM address 0x50313d34 is a nanokernel condition check — part of the normal
dispatch loop, not a simple tight spin. Each "iteration" traverses the full nanokernel dispatch
path (~15 JIT blocks) and checks the condition only when re-entering via 0x50313d20.

**Condition checked**: The block at 0x50313d34 checks r9 for zero. r9 is loaded by preceding
code at 0x50313d20 from the task structure at `[r11+4]`. With r11=0x2f072, it reads from
guest address 0x2f076. The nanokernel exits the spin-wait when [0x2f076] = 0.

**In interpreter mode**: [0x2f076] = 0 on first check (task structure not yet initialized).
Exits immediately. Boot continues fast.

**In JIT mode**: By the time the JIT first executes 0x50313d34, some initialization code has
already written 0x68fff400 (= KernelDataAddr+0x1400, a pointer into EmulatorData) to 0x2f076.
The exit condition is never immediately true.

**The deadlock**: The nanokernel spins waiting for [0x2f076] to clear. [0x2f076] is cleared
when the task finishes its work. But the task's completion depends on a VBL interrupt, and VBL
is suppressed during this phase because XLM_IRQ_NEST > 0 (interrupt nesting counter is raised).
Resolution takes ~20 real seconds per occurrence, when XLM_IRQ_NEST eventually drops to 0.

**Ticks frozen during stuck phases**: Ticks at 0x16a does not advance during stuck phases.
This is correct: the tick_func (60Hz) gates TriggerInterrupt on `XLM_IRQ_NEST == 0`. With
IRQ_NEST > 0, VBL is suppressed and HandleInterrupt never fires. The large Ticks value seen
in dumps (0xb5a01066 ≈ 3 billion) is set by the ROM initialization from the emulated RTC —
it's NOT from our "always tick Ticks" code overflowing.

**How it resolves**: After ~20 seconds, XLM_IRQ_NEST eventually drops to 0, VBL fires, the
task runs its cleanup, [0x2f076] becomes 0, and the spin-wait exits. The nanokernel then
processes the next initialization step.

**Occurrence pattern**: The deadlock fires at every initialization transition involving the
task at 0x2f072. With chaining=0: ~2 occurrences in a 22-minute run (at ~t=150s and ~1315s).
These are the most expensive initialization milestones.

**Chaining=1 makes it dramatically worse**: With chaining=1, the JIT runs faster, triggering
MORE initialization transitions per second. Each transition hits the deadlock. Result: 10+
stuck episodes in ~10 minutes vs 2 in 22 minutes with chaining=0. Total stuck time is
much higher with chaining=1 because the deadlock is hit more frequently, not less.

**Fix direction**: ROM patch or emulator shim that ensures [0x2f076] = 0 before the nanokernel
first checks 0x50313d34 for each task activation. The correct approach:
1. Find what writes 0x68fff400 to 0x2f076 (task activation code)
2. Either delay that write until after the spin-wait, OR
3. Add a ROM patch at 0x50313d34 that clears [r11+4] before the check when r1=KernelDataAddr
   (this matches the emulator's init state)

### Partial fix implemented (session 5, commit a46cda99) — and why it's still slow

A HandleInterrupt shim was added in `sheepshaver_glue.cpp` `case MODE_68K`: when
`r->gpr[1] == KernelDataAddr` and guest 0x2f076 == KernelDataAddr+0x1400, clear it to 0.

**Result**: Eliminates the 20-second hard deadlocks (0 STUCK events at 695s vs 2 STUCKs
at 150s/1315s without the fix). But boot is still ~10+ minutes instead of 10 seconds.

**Why still slow**: The nanokernel dispatch loop RE-WRITES 0x68fff400 to 0x2f076 on every
dispatch cycle (millions of times/sec). HandleInterrupt only fires at 60Hz. Pattern:
- VBL fires (every 16ms): fix clears 0x2f076 → spin-wait at 0x50313d34 passes ONCE
- Next dispatch cycle (microseconds later): nanokernel re-writes 0x68fff400
- Wait 16ms for next VBL → repeat

This gives 60 spin-wait passes/second instead of millions. With ~32,000 initialization
steps each taking 16ms: 32K × 16ms ≈ 512 seconds. Matches observed 535-695s boot time.

**What's needed**: Identify and patch the specific nanokernel instruction that writes
0x68fff400 to 0x2f076. A JIT write-probe was added that logs any change to 0x2f076 in
the first 500K block executions (~20ms of boot) along with the responsible block's PC.

### Diagnostics added (session 5)

- **STUCK detector register dump**: When STUCK fires (same heartbeat PC for 10+ seconds),
  stderr now prints r1, r9, r10, r11, r12, Ticks, and CR. Dump happens at first and subsequent
  STUCK events for the same PC.
- **STUCK → ring dump**: First STUCK event calls `ppc_jit_dump_trace_ring()` automatically
  (requires `SS_JIT_TRACE_RING=1`), dumping the ring to /tmp/ss_jit_ring.txt without needing
  lldb or SIGSEGV.
- **`SheepShaver/tools/jit-analyze.py`**: Log analysis tool. Subcommands:
  - `diag [log]` — heartbeat progression, hot PC frequency, 10s-window PC activity
  - `ring [log] [pc]` — show ring around last visit to a PC (context before stuck entry)
  - `hot [log] [N]` — top-N interrupt-delivery PCs

### `jit false` prefs vs aarch64 JIT (session 5 clarification)

The `jit` pref in `~/.sheepshaver_prefs` controls the LEGACY kpx_cpu codegen JIT only
(compiled out via ENABLE_DYNGEN=0 in this build). The aarch64 JIT (ppc-jit.cpp) runs
independently of the pref, gated by `SS_USE_JIT=0` env var only. Running bare `./SheepShaver`
always uses the aarch64 JIT. The prefs default was changed to `true` (session 5) for
documentation clarity; it has no functional effect.

### SS_JIT_NO_ROM=1 makes boot slower, not faster

With `SS_JIT_NO_ROM=1`, ROM code runs in the interpreter. The interpreter processes ROM code
~30x slower per iteration than the JIT. ROM spin-waits that JIT resolves in seconds take
minutes in the interpreter. The flag is useful for ISOLATION (proving a bug is ROM-JIT-specific)
but makes overall boot performance worse. It is NOT a workaround for slow boot.

---

## 2026-06-02 (session 4) — JIT boot hang root cause found and fixed; mouse lag diagnosed

### Mouse tracking lag — root cause and fix options (session 4)

Mouse input flows through a **two-stage 60 Hz pipeline**:
1. `HandleInterrupt()` calls `SDL_PumpEvents()` at ~60Hz (VBL-tied) → moves OS events into SDL queue (~16.7ms)
2. Redraw thread calls `SDL_PeepEvents()` at its own ~60Hz timer → ADB mouse moved (~16.7ms more)

Worst-case latency: **~33ms**. Typical: **~16ms**. Both are perceptible as sluggish.

**Additional issues**:
- `SDL_PumpEventsFromMainThread()` is implemented but **never called** — the intended 2-stage design is broken; the fallback path always runs
- `SDL_WarpMouseInWindow` in `video_set_cursor()` adds ~16ms per cursor-image change in grabbed mode (goes through Quartz)
- `frame_skip` defaults to **8** → display refreshes at only ~7.5 Hz, making the cursor appear to stutter even if input latency were improved. Should be 1 or 2.

**Fix options (video_sdl2.cpp)**:
1. **Option 1 (best)**: Move mouse motion handling into the `on_sdl_event_generated()` event watch callback (already registered via `SDL_AddEventWatch`). Fires synchronously inside `SDL_PumpEvents()`, eliminating Stage 2. Cuts max latency from ~33ms to ~16.7ms. ~30 min.
2. **Option 2**: Wire up `SDL_PumpEventsFromMainThread()` in `HandleInterrupt()` — correctness fix for the broken design.
3. **Option 3**: Replace `SDL_WarpMouseInWindow` in `video_set_cursor()` with `SDL_HINT_MOUSE_RELATIVE_MODE_CENTER=0` — fixes per-cursor-change stutter in grabbed mode.
4. Lower `frame_skip` default from 8 to 1 in `prefs_items.cpp` — independent of latency but makes cursor rendering smooth.

Options 1+2+3+frame_skip would bring typical latency from ~33ms to ~8-10ms.

### Backward branch spcflags bypass — root cause of JIT boot hang (FIXED, commit 647a58d1)

The JIT's `bc` handler (case 16, ppc-jit.cpp) used `find_code_for_pc(target_pc)` to emit direct native ARM64 branches to `insn_code_offset[i]` for BOTH forward and backward branch targets. `insn_code_offset[i]` is located AFTER the spcflags poll (`emit_entry_spcflags_poll` at `chain_entry_start`). For backward branches (spin-loops), this created closed ARM64 inner loops that bypassed the spcflags poll entirely — the VBL interrupt flag was set but never checked, so the emulator hung permanently at the ROM's early-boot VBL spin-wait (0x5031040c).

**Fix**: Guard all three `find_code_for_pc` call sites with `target_pc > pc`. Backward branches fall through to `emit_epilogue_with_pc(target_pc)`, which returns to the C dispatcher → spcflags poll on every loop iteration.

Three sites changed in case 16: `bdnz` path, `bdz` path, pure conditional (`beq`/`bne`/etc.) path.

**Correctness confirmed**: CTR is NOT double-decremented (one decrement per `bdnz` execution regardless of re-entry). LR is written unconditionally before branch logic in `bcl` variants. State (lazy CR0, register allocator) is fully flushed before every epilogue path.

### Forward intra-block fast-path is dead code (pre-existing)

`insn_code_offset[i]` / `insn_ppc_pc[i]` are recorded BEFORE `compile_one` is called for each instruction. This means forward branch targets are never in the table when the branch is being compiled. The `(target_pc > pc) ? find_code_for_pc(...) : NULL` guard excludes backward branches but the remaining forward-branch path ALSO always returns NULL — the optimization never fires in either direction. Two-pass compilation would be required to enable forward intra-block branches: first pass builds the PC→offset map, second pass emits branch code.

### VBL spin-wait still hangs after backward-branch fix — VBL handler miscompilation suspected

After the backward-branch fix, interrupts ARE delivered at ~60Hz to 0x5031040c (verified via diagnostic log). But the spin-wait never exits — its exit condition never becomes true. `SS_JIT_NO_ROM=1` (keep ROM interpreter-only, JIT-compile only RAM) bypasses the hang entirely: boot progresses past 0x5031040c and crashes later at a different address (pc=04000000, post-VBL). This proves: the VBL interrupt *handler* (itself ROM code, also JIT-compiled) is the problem — it runs but doesn't correctly update the condition the spin-wait polls. Under investigation.

### SS_JIT_NO_ROM=1 — diagnostic flag for ROM vs RAM isolation

Keeps the ROM range interpreter-only while still JIT-compiling RAM code. Useful for isolating whether a bug is in ROM JIT codegen vs RAM JIT codegen. With this flag the VBL hang is bypassed; without it the VBL handler ROM block is JIT-compiled and produces incorrect results.

### Diagnostic logging added to ppc-cpu.cpp (session 4)

JIT now emits structured boot-time diagnostics:
- **stderr**: JIT init message (one-time), `STUCK` alert if same PC for 10+ seconds
- **`/tmp/jit_diag.log`**: 5-second heartbeat (`blocks=N pc=XXXX`) + every interrupt delivery
- Absent heartbeats (despite process alive) = JIT dispatch froze or tight native loop (new backward-branch regression if chaining=0)
- Dense interrupts at one PC = spin-wait (normal early-boot behavior)

### Spin-wait at 0x5031040c is a memory scan, not a Ticks check

The loop at ROM guest address 0x5031040c is NOT a VBL/Ticks counter wait. Decoded PPC:
```
5031040c: or. r9, r9, r9       ; test r9 for zero
50310410: beq 50310424          ; EXIT if r9 == 0 (sentinel found)
50310414: lwzu r4, 4(r11)       ; load from [r11+4], r11 += 4
50310418: [operation on r4]
5031041c: lwzu r9, 4(r11)       ; load r9 from [r11+4], r11 += 4
50310420: b 5031040c            ; loop back
```
Scans memory advancing r11 by 8 per iteration until r9 (loaded from guest memory) is zero. In interpreter mode, r9 is already 0 on first entry → beq taken → exits immediately (never appears in interrupt logs). In JIT mode, r9 may not be 0 on entry — either a JIT miscompilation of preceding code or a timing race where the JIT reaches this point before initialization completes.

### JIT DOES reach MODE_NATIVE and execute RAM code

With the HandleInterrupt early-boot guard (see below), the JIT progresses past 0x5031040c, through a second spin-wait at 0x50313d34 (~20s, self-resolves), and into MODE_NATIVE executing RAM code at 0x10xxxxxx addresses. The boot continues further than previously observed.

### HandleInterrupt guard for uninitialized kernel data (working fix candidate)

In `sheepshaver_glue.cpp` `HandleInterrupt`, during `MODE_68K`: `cr_mask = ReadMacInt32(KERNEL_DATA_BASE + 0x674)`. In early boot (before the nanokernel initializes this address), cr_mask is 0. The original code applied the zero mask to CR unchanged. The proposed fix:
```cpp
if (cr_mask != 0) {
    r->cr.set(r->cr.get() | cr_mask);
} else {
    // KernelData not yet initialized — directly tick Ticks so early-boot
    // spin-waits can exit.
    WriteMacInt32(0x16a, ReadMacInt32(0x16a) + 1);
}
```
This causes the Ticks counter at 0x16a to increment on each interrupt while the kernel isn't set up yet, allowing early-boot spin-waits to exit. Confirmed to let the JIT reach MODE_NATIVE. Under review by vbl-investigator before committing.

**REJECTED — causes "Starting Up..." hang (session 5)**: The else-only approach was tried as an uncommitted change (introduced by a subagent). It boots past VBL spin-waits but freezes at the Mac OS 8.6 extension-loading screen (~3–4 minutes in, never completes). Root cause: once `cr_mask` becomes nonzero (kernel initialized), the Ticks fallback stops, and the 68k interrupt handler does not reliably increment Ticks in JIT mode — so any extension-load timeout loop spins forever. The emulator appears alive (scattered PCs, ~60Hz interrupts in jit_diag.log) but OS-level time is frozen.

**The working approach (commit 3da25938)**: unconditionally tick Ticks on every VBL interrupt, outside the if/else entirely. Double-counting by the 68k interrupt handler is not a problem in practice — early-boot spin-waits exit quickly and the post-handoff 68k handler runs so infrequently that a small drift is harmless. This version boots to Finder.

### Second spin-wait at 0x50313d34

A second spin-wait at ROM address 0x50313d34 appears ~20–25 seconds into JIT boot and self-resolves after ~20 more seconds. No action needed — it exits on its own. Probably a similar sentinel scan that eventually finds its zero value.

### XLM_RUN_MODE = 0 (MODE_68K) during early boot spin-waits

During the 0x5031040c hang, `XLM_RUN_MODE` is 0 (`MODE_68K`). In this mode, SheepShaver's HandleInterrupt only sets a CR bit in the kernel data — it does NOT run the nanokernel or execute the VBL handler. The fix above adds a Ticks increment as a fallback for the uninitialized-kernel window.

---

## 2026-06-02 (session 4) — Harness audit, ROM range revert, boot test

### Harness vector audit: 3 of 4 "uncovered" vectors already existed

Audited four opcodes flagged as potentially uncovered: lhbrx, lwbrx, mcrxr, mtcrf. Found that `lhbrx_basic`, `lwbrx_basic`, and `mcrxr_basic` were already present (added in prior sessions). Only `mtcrf_partial` was genuinely new. Encoding: `38600123 7C680120` = `li r3,0x123; mtcrf 0x80,r3` (writes CR field 0 only, FXM=0x80). Note: `7C620120` (FXM=0x20) was initially proposed but incorrect — `7C680120` is the right encoding for CRM=0x80 targeting CR field 0. Harness: **230 vectors** after mtcrf_partial (was 229); stwbrx/sthbrx/cntlzw vectors being added — target 233. Score=100 in both interpreter and JIT modes.

### ROM range reverted to 0x460000 (toolbox-only)

Changed `ppc-cpu.cpp` line 1042 back from 0x500000 to 0x460000. Rationale: the range 0x460000–0x500000 covers the 68k DR emulator, which is unsafe to JIT due to spcflags/CR-bit-injection interleaving during interrupt delivery (documented in session 3). The 95-second clean run at 0x500000 with chaining=1 was not evidence of safety — the bug is timing-dependent. Build: clean, only pre-existing K&R deprecation warnings in slirp. Harness: 230/230, score=100.

### JIT opcode coverage audit (session 4)

All previously-flagged "uncovered" opcodes are in fact native in the JIT — no fallback:

- **mcrxr** (XO=512): NATIVE — full XER→CR move, clears XER SO/OV/CA flags
- **lhbrx** (XO=790): NATIVE — LDRH in native LE order (byte-reversed for BE guest)
- **lwbrx** (XO=534): NATIVE — LDR word in native LE order
- **stwbrx/sthbrx**: NATIVE
- **mtcrf** (XO=144): NATIVE — handles FXM=0xFF and all partial masks via computed mask loop
- **sthu/lhau/lhzu** (primary opcodes 45/43/41): NATIVE
- **cntlzw** (XO=26): NATIVE

**Fallback mechanism is sound**: when `compile_one()` returns false, `emit_inline_interp_call()` runs one instruction through `ppc_jit_interp_one` and the block exits cleanly. No state corruption is possible. This is better than the old behavior where a false return left the block incomplete and caused mixed JIT/interp at the same PC.

**Actual JIT gaps** (interpreter fallback, all intentional or benign):
- `mfspr`/`mtspr` for unknown SPRs (only LR/CTR/XER are native) — SPRG0-3, IBAT/DBAT, HID0/HID1, DEC fall back but work correctly, just at interpreter speed
- `sc` (syscall): intentional — interpreter must raise Mac OS traps
- `tdi`/`twi`/`tw` (trap instructions): intentional, rare
- `lswx`/`stswx`: intentional — runtime NB not knowable at compile time
- `bcl` with CTR+cond combined (lk=1, no_ctr_test=0, no_cond_test=0): known incomplete, noted in source
- Unknown AltiVec vxo: fallback for uncommon VMX ops

**Implication**: the new test vectors this session (mcrxr, mtcrf_partial, stwbrx, sthbrx, cntlzw) all exercise native JIT code paths. Coverage additions are meaningful.

### JIT block chaining safety analysis (session 4)

**Verdict: JIT_BLOCK_CHAINING=1 is SAFE with ROM range=0x460000 (high confidence)**

Every chained branch lands at `chain_entry_start` = first instruction of `emit_entry_spcflags_poll()`. The poll checks `spcflags.mask & 0x0F` (bits 0–3: exec-return, trigger-interrupt, handle-interrupt, enter-mon). If any flag is set, the PC is saved and control returns to the C dispatcher before the block body executes. Interrupt latency is at most one block body (≤512 PPC instructions). `TriggerInterrupt()` sets bit 1 of `regs->spcflags.mask`, which is caught at the next chain entry — no interrupts are lost.

**Chain invalidation correctness:**
- `jit_bc_invalidate_range()`: correctly reverts B instructions to LDP before nullifying pool entries. SAFE.
- Full cache flush / pool exhaustion: stale chains are unreachable. SAFE.
- `jit_bc_invalidate_pc()` (single PC): does NOT revert B instructions — stale chains possible for RAM code with self-modifying code. **Not relevant for ROM execution** (ROM is `vm_protect(READ|EXECUTE)`, immutable).

**Separation of concerns:** The prior LEARNINGS concern "chaining unsafe with DR emulator" was about a missing spcflags poll (since fixed). The remaining "DR emulator unsafe to JIT" concern is about opcode-correctness of the 68k emulator's PPC code — a distinct issue, not a chaining issue.

**ROM range=0x460000 + chaining=1: both safety conditions met** (spcflags poll present, no SMC in ROM).

Test tiers recommended before shipping chaining=1:
1. Boot to Finder with chaining=1 + ROM=0x460000 (basic correctness)
2. 10+ min timer-intensive workload; verify Mac OS tick counter at guest 0x16a advances at ~60 Hz
3. *(Optional)* Debug counter on consecutive zero-spcflags chain entries to empirically verify ≤1-block interrupt latency

**Next step**: enable chaining=1 after Path 1 boot succeeds (currently in progress with chaining=0).

### SPR fallback frequency audit (session 4)

ROM contains only 75 `mfspr` and 150 `mtspr` instructions total. Of these, 68/75 `mfspr` and 126/150 `mtspr` are already NATIVE (LR/CTR/XER). Only 7 `mfspr` and 24 `mtspr` fall back to interpreter, with at most 3 instances of any single SPR number. All fallback SPRs are supervisor-mode one-time boot operations: BAT registers, SRR0/SRR1, SDR1, SPRG0-2, HID0, PVR.

Previously-suspected high-frequency targets TB/TBU (SPR 268/269) appear **zero** times as `mfspr`. DEC (SPR 22) appears once. **No new native SPR handlers are warranted** — the inline-interp-call path handles these correctly at negligible cost given their frequency.

Known remaining JIT gap worth a future test vector: `bcl` with CTR+cond+link=1 combined (comment in `ppc-jit.cpp` says "not yet implemented"). Likely rare in practice but unverified.

### New test vectors added (session 4)

- **stwbrx_basic** (XO=662): `3C60DEAD 6063BEEF 38800600 7C61252C 80A10600` — store word byte-reversed, verified with readback via `lwz`
- **sthbrx_basic** (XO=918): `38601234 38800700 7C61272C A0A10700` — store halfword byte-reversed, verified with readback via `lhz`
- **cntlzw_mid** (XO=26): `3C6000FF 7C650034` — count leading zeros mid-range case (0x00FF0000 → r5=8); cntlzw_zero/allones already existed
- **Harness total: 233 vectors** (up from 229 at session start)
- All 4 additions (mtcrf_partial + these 3) exercise NATIVE JIT code paths (confirmed by jit-auditor)

### DR emulator JIT unsafe: exact mechanism confirmed (session 4)

**Verdict: Definitely unsafe — not a conservative precaution.**

**Exact mechanism — one-step-early interrupt delivery:**

Mac OS sets CR2.LT (bit 8) to signal a pending interrupt. The DR emulator checks it via `bclr BO=5,BI=8` (opcode `0x4CA80020`) at the end of every dispatch cycle. JIT polls spcflags at **block entry**: when TRIGGER is detected it saves PC, returns to C, which runs `check_spcflags()` → sets CR2.LT, then re-dispatches the same block. The interpreter polls at **block exit** — after `bclr 5,8` has already run the current handler. Net effect: JIT fires the interrupt one dispatch step early, before the current 68k instruction handler completes.

**The double-lhau block (504613e0) makes it critical:**

Block 504613e0 does two `lhau r27,2(r24)` fetches per dispatch cycle — speculatively pre-fetching the extension word. When JIT re-runs this block after injecting CR2.LT, r24 has already advanced 4 bytes, and r27 holds the extension word, not an opcode. The interrupt handler fires with r24 pointing 2 bytes past the correct position → extension word is dispatched as a 68k opcode → register corruption.

**This explains all prior Bug #2 symptoms:** extension word dispatched as opcode, OE arithmetic step interrupted mid-sequence, A3 = 0x103ffffe corruption (pre-decrement step never ran).

All 6 DR emulator dispatch variants (ROM+0x466080/84/c0/e0/100/120) use `bclr 5,8` as the structural interrupt gate.

**Fix (Option A, ~100–150 lines in ppc-jit.cpp):** In `compile_one()`, detect opcode `0x4CA80020` when PC is in ROM+0x460000–0x500000. Emit an inline spcflags-check-and-inject prologue BEFORE the branch, forcing CR2.LT to be current at the exact moment `bclr` evaluates it — matching the interpreter's one-cycle-later delivery.

**Current safe config**: ROM range = 0x460000 (toolbox only). The JIT↔interpreter boundary costs performance but not correctness during 68k-heavy workloads. **Do NOT attempt ROM range = 0x500000 until Option A is implemented and verified.**

### Boot test: CD ISO hangs in SCSI loop (session 4)

Config: ROM=0x460000, chaining=0, SS_USE_JIT=1, boot media = Mac OS 8.6 CD ISO. Result: 100% CPU for the full 300s timeout, never reached Finder. JIT cache allocated successfully (MAP_JIT confirmed). Diagnosis: SCSI loop on CD boot is known behavior when booting off an ISO rather than a pre-installed disk image — this is NOT a JIT regression. Fix: pivot to a Mac OS 9.2.1 pre-installed disk image; disk image boot bypasses the SCSI loop entirely.

### Boot preflight findings

- `~/.sheepshaver_prefs` sets `jit false` — JIT is controlled exclusively via the `SS_USE_JIT=1` env var at launch, not the prefs flag. The prefs flag appears to be a legacy or alternate gate; the env var takes precedence.
- Display mode is `screen win/800/600` (SDL window on local Mac display). VNC is NOT configured by default. Boot tests run against the local display.
- No stray SheepShaver processes were found before boot test.
- ROM: `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom` (1.8 MB, confirmed present)
- Disk: `/Users/Shared/macemu/Mac OS 8.6 Internal Edition.iso` (647 MB, confirmed present, not locked)

### Boot test in progress (config: ROM range=0x460000, JIT_BLOCK_CHAINING=0, SS_USE_JIT=1)

Running the conservative known-safe baseline. Result pending — will be updated when builder reports.

## 2026-06-02 (session 3) — Bug #2 investigation: systematic debugging phase 1-2

### Opcode coverage audit and new test vectors (lhau/lhzu)

The Haiku agent opcode-histogram found sthu/lhau/lhzu with 4,900–5,600 ROM instances each, marked as uncovered. However, sthu_basic existed in the harness. **lhau and lhzu were NOT explicitly tested** — added as `lhau_basic` (opcode 0xA5640002) and `lhzu_basic` (opcode 0xA1640002), both verifying halfword load-with-update with register offset. Harness: **229/229 (was 227), score=100**. Verified correct in both interpreter and JIT (chaining=1).

### Chaining enabled + full ROM range (0x500000): No immediate crash, no watch events

Rebuilt with `JIT_BLOCK_CHAINING=1` and ran with `SS_JIT_WATCH_ADDR=103fff0c,100a1cc0 SS_JIT_WATCH_DUMPS=0`. Expected: CDROM eject (watch event at 0x103fff0c) within minutes. Observed: 95+ seconds of sustained SCSI loop (100% CPU, clean log), zero watch events, no crash. Possible explanations:
1. Crorc fix eliminated the bug entirely (Bug #2 required both crorc issue + chaining interaction)
2. Bug manifests under different conditions (specific code sequence, register state)
3. Watch mechanism positioning needs adjustment
4. Bug is rare/timing-dependent (occurred in previous session, not always reproducible)

### A3 register location confirmed: r19 (gpr[19]), offset 0x4c

68k A3 (stack-frame param block pointer that got corrupted to 0x103ffffe) is stored in PPC r19. Mapping: 68k A0–A7 → PPC r16–r23. Offset from state struct start: 0x4c. Verified in ppc-registers.hpp, ppc-cpu.cpp comments, and PPCR_GPR(19) macro.

### Interrupt-delivery interleaving remains the 68k-emulator-in-JIT blocker

From prior session: JIT compilation of the 68k DR emulator (ROM+0x460000–0x500000) was unsafe because spcflags/CR-bit-injection interleaving differs between interpreter and JIT at multi-step instruction boundaries. Resolution: keep 68k emulator interpreter-only (ROM range = 0x460000). Current config (0x500000, chaining=1) violates this — the lack of visible corruption in a single 95s run is not evidence of safety.

**See session 4 entry "DR emulator JIT unsafe: exact mechanism confirmed" for the full root-cause analysis**, including the one-step-early interrupt delivery mechanism, why the double-lhau block makes it critical, and the proposed Option A fix.

## 2026-06-02 (session 2) — Bug #2 investigation: block 504613e0 decoded

### lldb SIGSTOP disrupts 60Hz VBL timer — early-boot spin

Multiple `lldb -p PID -o ... -o detach -o quit` invocations in sequence cause the macOS
kernel to defer pending signals/timers during SIGSTOP.  After 3-4 lldb sessions, the
emulator's 60Hz VBL interrupt (delivered via `timer_unix.cpp`) stops firing.  The
ROM's early-boot spin-wait at 0x5031040c/0x50310414 waits for a VBL tick to exit —
without ticks it loops forever.  **Mitigation**: attach lldb AT MOST ONCE per run, do
the minimal dump, and immediately detach.  If the emulator gets stuck in a 2-block loop
with all-zero a0-a7 registers in the ring, the VBL timer was disrupted — just restart.

### Block 504613e0 PPC instructions (DR emulator dispatch variant)

Decoded from ROM bytes (NATMEM_OFFSET + guest_addr, then BSWAP32 each 4-byte word):
```
504613e0: lhau  r27, 2(r24)          ; fetch next 68k opcode, advance PC by 2
504613e4: addco. r4, r4, r0          ; OE arithmetic for 68k CC (N/Z/V/C)
504613e8: rlwimi r27,r29, 3, 13, 28  ; merge CC bits into dispatch register
504613ec: mtlr  r29                  ; load handler address into LR
504613f0: lhau  r27, 2(r24)          ; fetch next-NEXT opcode, advance PC by 2 more
504613f4: sthu  r4, -4(r1)           ; push halfword of r4 to [r1-4], r1 -= 4
504613f8: bclr  5, 8                 ; branch to handler (LR) if CR2.LT=0 (no interrupt)
504613fc: b     0x5046D0D4           ; fall-through: interrupt handler path
```
Two `lhau r27, 2(r24)` instructions in a single block = this dispatch variant processes
TWO 68k instructions per cycle (common for short-pair sequences like back-to-back pushes).

### 68k instruction at 0x5007aed4 = MOVE.L A3, -(A7)

Bytes 0x2F0B at that ROM address decode as:
- opcode bits 15:12 = 0010 = MOVE, bits 13:12 = 10 = long
- dest: mode=100 (predecrement), reg=111 (A7) → -(A7)
- source: mode=001 (addr reg direct), reg=011 (A3)
= `MOVE.L A3, -(A7)` (push A3 onto system stack)

Block 504613e0 is the dispatch handler for this 68k instruction.  The sthu at 504613f4
pushes r4 (= A3 in the DR emulator register mapping) as a HALFWORD.  A3 = 0x103ffffe
at this point → the garbage ends up on the stack → Device Manager reads it as a
param-block pointer and triggers a spurious CD-ROM eject.

### sthu and lhau JIT handlers are correct — harness confirms

Verified via: (1) code review showing correct EA computation, byte-swap, and write-back
sequencing; (2) harness 227/227 both modes after this session.  The bug is NOT in the
sthu/lhau codegen.  A3 = 0x103ffffe was the wrong value BEFORE the push — the block
correctly assembles that garbage value and pushes it.

### NATMEM_OFFSET address arithmetic for lldb dumps

NATMEM_OFFSET = 0x400000000000. Guest addr → host = 0x400000000000 + guest_addr.
Examples: guest 0x5007aed4 → host 0x40005007aed4. Guest 0x504613e0 → host 0x4000504613e0.
lldb `--size 4 --format x` shows little-endian 4-byte integers; BSWAP32 each value to
get the big-endian PPC/68k instruction bytes.

## 2026-06-01 — Project setup & landscape research

### Fork landscape
- **cebix/macemu** (original) is dormant; 916 commits behind kanjitalk755. Its only unique
  value: a few post-2020 commits. The SLiRP buffer-overflow fix (`d26ae37e`) was NOT in
  rcarmo/kanjitalk755 trees — cherry-picked here as `877782f3`. The AARCH64 Mach exception
  commit (`e5be177f`) was already present (better integrated) in rcarmo's tree.
- **kanjitalk755/macemu** is the community-standard fork but is "arm64 non-JIT" by design;
  it has zero MAP_JIT / `pthread_jit_write_protect_np` usage anywhere.
- **Jagmn's 2021 M1 ARM64 JIT** (emaculation forum) was never published; his GitHub is
  empty. Not recoverable. His approach notes: dyngen ops regenerated with GCC-10, RX↔RW
  cache toggling for W^X, NATMEM_OFFSET instead of zero-page.
- **rcarmo/macemu-jit** (our upstream) is Linux-ARM64-first: developed on Orange Pi 6 Plus /
  RPi, tested over VNC (port 5999), CI builds .deb packages. macOS is not its focus —
  that's our niche.

### Upstream JIT state (from JIT-STATUS.md, 2026-05-17)
- SheepShaver PPC→ARM64 JIT: hand-written emitters (no dyngen),
  `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/` (~4.1K lines; `ppc-jit.cpp` is the core).
- Status: 209/209 opcode vectors, 1800/1825 ROM blocks (98.6%), ~737 MIPS tight-loop.
  JIT boot reaches "Welcome to Mac OS" splash; interpreter boots to desktop.
- Known root cause of boot hang: partial-block truncation epilogue corrupts state
  (upstream commit `98fd798`). Lazy CR0 + register allocation are scaffolded but DISABLED
  as containment. Re-enabling them = the big optimization opportunity after correctness.
- Remaining ROM harness failures: CR field interactions in multi-instruction blocks,
  complex branch BO patterns (CTR+condition combos).
- The 68k (BasiliskII) JIT in `BasiliskII/src/uae_cpu_2026/compiler/` is much larger
  (~60K lines) and has the MAP_JIT / W^X handling — the SheepShaver side may not. Audit
  needed (Phase 2).
- Upstream's ASLR workaround uses Linux-only `personality(ADDR_NO_RANDOMIZE)` + re-exec —
  this does not exist on macOS; we need a different approach.

### Old (cebix/kanjitalk755) JIT for reference
- The legacy SheepShaver JIT is dyngen-based (QEMU-derived), x86/x86_64 only, allocates
  one permanently-RWX cache via `vm_acquire(VM_MAP_32BIT)` — architecturally incompatible
  with modern macOS W^X. rcarmo's reset to hand-written emitters was the right call.

### Environment
- Dev machine: arm64, macOS 26.4.1 (Darwin 25). Ken has Mac OS ROM + OS 9 install media.
- kanjitalk755's x86_64-JIT-under-Rosetta-2 build is the performance bar to beat
  (historically ~271% MacBench vs ~96% for native interpreter; Jagmn's lost JIT hit ~470%).

## 2026-06-01 — Phase 1 baseline

### Build environment
- macOS 26.4.1 arm64, Apple clang 17.0.0 (Command Line Tools, no full Xcode)
- Homebrew: autoconf, automake 1.18.1, libtool, pkgconf, sdl2 2.32.10, gmp, mpfr

### Configure on macOS arm64

**autogen.sh invocation:**
```
cd SheepShaver/src/Unix
NO_CONFIGURE=1 ./autogen.sh
```
Completed with exit 0. Only deprecation warnings (obsolete AC_* macro names) — no missing m4 macros, no errors. `configure` file generated successfully.

**configure.ac bug found and fixed:**
The aarch64 JIT path in configure.ac (line ~1590) tested `$host_cpu = xaarch64`, but on macOS arm64 `config.guess` reports `arm-apple-darwin25.4.0` — so `host_cpu` is `arm`, not `aarch64`. Without the fix, configure falls through to the dyngen path, dyngen reports "no" for `arm:mach`, and JIT is disabled.

Fix: replaced the single `if [[ "x$host_cpu" = "xaarch64" ]]` test with a nested `case` pattern that matches both `aarch64` (Linux arm64) and `arm` on Darwin only (macOS arm64), while leaving bare `arm` on Linux untouched to avoid wrongly enabling the aarch64 JIT on 32-bit ARM Linux hosts:

```sh
case $host_cpu in
  aarch64) aarch64_host=yes ;;
  arm) case $host_os in darwin*) aarch64_host=yes ;; esac ;;
esac
```

The fix is in `configure.ac` only; the generated `configure` is regenerated by autogen.sh.

**Working configure invocation:**
```
./configure --enable-sdl-video --enable-sdl-audio --enable-jit \
  --without-gtk --without-x --without-esd
```
(`--without-PACKAGE` is equivalent to `--with-PACKAGE=no` in autoconf; `--without-x` is standard autoconf.)

**Configure summary (after fix):**
```
SDL support ...................... : video audio
SDL major-version ................ : 2
Using PowerPC emulator ........... : yes
Enable JIT compiler .............. : yes
GTK user interface ............... : no
ESD sound support ................ : no
Addressing mode .................. : real
Bad memory access recovery type .. : mach
```

**JIT decision:**
- `ENABLE_DYNGEN` is `#undef` in `config.h` (the new aarch64 JIT does not use dyngen)
- `-DUSE_AARCH64_JIT` and `-DUSE_JIT` are set in Makefile `CPPFLAGS` (via configure.ac `CPPFLAGS` substitution, not `AC_DEFINE` — these are compiler flags, not config.h entries)
- `CPUSRCS` includes `../kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` as the first entry

**Darwin-specific sources selected (SYSSRCS):**
- Video: `../SDL/video_sdl.cpp`, `../SDL/video_sdl2.cpp`, `../SDL/video_sdl3.cpp`
- Audio: `../SDL/audio_sdl.cpp`, `../SDL/audio_sdl3.cpp`
- macOS-specific: `../MacOSX/sys_darwin.cpp`, `../MacOSX/extfs_macosx.cpp`,
  `../MacOSX/prefs_macosx.mm`, `../MacOSX/Launcher/VMSettingsController.mm`,
  `../MacOSX/Launcher/DiskType.m`, `../MacOSX/clip_macosx64.mm`,
  `../MacOSX/utils_macosx.mm`, `../pict.c`
- Slirp networking enabled (byte bitfields check passed)
- Sigsegv recovery: Mach exceptions (`HAVE_MACH_EXCEPTIONS`)
- PAGEZERO hack: no (darwin arm64, not x86_64)
- `NATMEM_OFFSET` set to `0x400000000000` for `arm-apple-*` host (hardcoded in configure.ac)

**Notes for Task 3 (build):**
- The Makefile references `basic-dyngen-ops.hpp` and `ppc-dyngen-ops.hpp` in a dependency rule even though dyngen is not used. This may generate a warning but should not block the aarch64 JIT path build.
- Frameworks available: Carbon, IOKit, CoreFoundation, CoreAudio, AudioUnit, AudioToolbox, AppKit, Metal.
- `clock_nanosleep` is absent on macOS (as expected); `clock_gettime` is present.
- `EMULATED_PPC=yes` (we're on arm64, not native PPC), so the full kpx_cpu emulator + JIT will be compiled.

### macOS arm64 build fixes
Root theme: autoconf 2.73 (Homebrew) probes the compiler and bakes `-std=gnu23` into `@CC@` (`CC = gcc -std=gnu23`). clang 17 defaults to C17, but autoconf forces the newest standard. C23 broke two things in this tree.
- **slirp K&R definitions:** the vendored BSD-derived slirp `.c` files (`ip_output.c`, `tcp_input.c`, ~13 files) use K&R-style function definitions, which C23 removed — clang errors `unknown type name`, `illegal storage class on file-scoped variable`. Fix: detect this in `configure.ac` with an `AC_COMPILE_IFELSE` K&R test; if the compiler rejects K&R syntax (e.g. clang with autoconf-baked `-std=gnu23`), append `-std=gnu89` to `SLIRP_CFLAGS` only. The `Makefile.in` slirp rule uses `$(SLIRP_CFLAGS)` with no hardcoded flag, keeping Linux builds untouched. A later `-std=` flag overrides the earlier `-std=gnu23` baked into `$(CC)`. Configure check: `checking whether slirp needs -std=gnu89 for K&R definitions`. (original workaround: commit 5d950374; refined to configure detection: see the following commit)
- **empty ppc-execute-impl.cpp → link failure:** the genexec rule preprocesses `ppc-decode.cpp` (C++) via `$(CPP)`, which is GNU Make's built-in default `$(CC) -E` = `gcc -std=gnu23 -E`. gcc treats `.cpp` as C++ and rejects `-std=gnu23 not allowed with C++`, so the pipe to `genexec.pl` got empty input and produced a 0-byte `ppc-execute-impl.cpp`. The link then failed with hundreds of undefined `powerpc_cpu::execute_*` template symbols from `ppc-decode.o`. Fix: use `$(CXX) -E` in the genexec rule so the C++ frontend preprocesses it. `Makefile.in` rule line ~271. (commit e0a59d85)
- **MAP_FIXED_NOREPLACE undeclared:** Linux-only mmap flag (Linux >= 4.17) used by the `SS_TEST_HEX` opcode-test harness in `sheepshaver_glue.cpp` for a low-4GB REAL_ADDRESSING mapping. macOS lacks it. Fix: `#define MAP_FIXED_NOREPLACE 0` when undefined; the existing code already retries with a kernel-chosen address and validates the result is in the low 4GB, so behaviour is unchanged. (commit 705d3dad)
- The slirp K&R / gnu23 errors that appeared interleaved in the first parallel build log (e.g. `error: invalid argument '-std=gnu23' not allowed with 'C++'` with no source prefix) were the genexec preprocessing step failing, not the `.c` compiles — easy to misattribute under `-j8`.
- No JIT logic, no W^X/MAP_JIT allocation behaviour, and no architectural decisions were touched — all three fixes are pure portability/build-config.
- Final binary: `SheepShaver: Mach-O 64-bit executable arm64`, linked against `/opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib` and system frameworks (Carbon, IOKit, CoreFoundation, CoreAudio, AudioUnit, AudioToolbox, AppKit, Metal). `make clean && make -j8` succeeds from clean.

### JIT opcode harness on macOS arm64
- **Result: `METRIC build_ok=1 pass=0 fail=209 total=209 score=0`** (every vector `opcode_*=-1`, i.e. "no REGDUMP from run 1"). Upstream Linux baseline is `pass=209 fail=0 score=100`. The harness runs to completion and is deterministic (both runs of every vector produce identical *empty* output), so this is a clean, reproducible runtime failure, **not** a flaky/timeout artifact. No crash: exit code 0, no EXC_BAD_ACCESS, no entry in `~/Library/Logs/DiagnosticReports/`.
- **Harness script fixes (commit `da830d67`, `fix: make jit-test harness run on macOS`)** — the script never reached the test loop on stock macOS until these were applied; all are pure portability, no behaviour change on Linux:
  - Stock macOS `/bin/bash` is **3.2.57** (no bash 4 anywhere on this box, no Homebrew bash). `declare -A TESTS` errored `declare: -A: invalid option`. Replaced the associative array with plain `T_<name>` vars + indirect expansion (`eval "hex=\"\${T_${name}}\""`).
  - The unconditional `g++` recompile + relink (lines 45–49) used Linux-only link flags (`-lgtk-x11-2.0`, `-lvncserver`, …) and, under `set -e`, the failing compile aborted the whole script before any METRIC was printed. Now skipped entirely when `src/Unix/SheepShaver` already exists (the clang/SDL2 build from Task 3 is reused).
  - `timeout` / `gtimeout` are both absent (no GNU coreutils). Added an `ss_timeout` shim: `timeout` → `gtimeout` → perl `fork`+`alarm` SIGTERM-then-SIGKILL fallback mirroring `timeout -k 5s 15s`.
  - `nproc` → `getconf _NPROCESSORS_ONLN`; `Xvfb` launch guarded by `command -v Xvfb` (macOS has no X server; binary runs `nogui true`).
- **Root cause of the 209 failures — RWX anonymous `mmap` is rejected with EPERM on macOS arm64 (THE Phase 2 blocker).** The test RAM allocation in `ss_run_opcode_test` (`src/kpx_cpu/sheepshaver_glue.cpp:938-948`) requests `PROT_READ|PROT_WRITE|PROT_EXEC` with `MAP_PRIVATE|MAP_ANONYMOUS`. On Apple Silicon, W^X policy refuses RWX anonymous mappings outright. Both the `0x10000000` hint mmap and the `NULL`-hint retry return `MAP_FAILED` with `errno=13 (EPERM)`, so the code hits the `fprintf(stderr, "SS_TEST: cannot allocate RAM in low 4GB\n")` bail at line 951 and returns before executing a single opcode. The interpreter (`cpu->execute`, line 1034) and the JIT path (lines 1015-1031) are never reached — `SS_TEST_JIT=1` fails identically and never even prints `compiled ... insns` or `fallback to interpreter`. Standalone reproducer confirms the per-flag behaviour on this exact machine (16 MB region):
  - `mmap(0x10000000, RWX, MAP_PRIVATE|MAP_ANONYMOUS)` → `MAP_FAILED`, errno 13
  - `mmap(NULL, RWX, MAP_PRIVATE|MAP_ANONYMOUS)` → `MAP_FAILED`, errno 13
  - `mmap(NULL, **RW**, MAP_PRIVATE|MAP_ANONYMOUS)` → **succeeds** at `0x107270000`
  - `mmap(NULL, RWX, MAP_PRIVATE|MAP_ANONYMOUS|**MAP_JIT**)` → **succeeds** at `0x102690000`
  - The `error: invalid argument` is purely the `PROT_EXEC` bit; dropping it or adding `MAP_JIT` both make the mapping succeed.
- **Second, independent blocker for the same code path — the low-4GB / REAL_ADDRESSING assumption is false on macOS arm64.** Even with PROT_EXEC removed, the successful RW mapping lands at `0x107270000`, which fails the `(uintptr_t)test_ram > 0xFFFFFFFFUL` check at line 950 → same "cannot allocate RAM in low 4GB" bail. `RAMBase`/`test_addr` are computed as `(uint32)(uintptr_t)test_ram` (lines 981-984), so the design *requires* the host pointer to fit in 32 bits. macOS arm64 places anonymous mappings high in the address space; the `MAP_FIXED_NOREPLACE`→0 shim (lines 61-63) makes the `0x10000000` hint advisory-only and the kernel ignores it. Getting RAM into the low 4 GB will need an explicit fixed mapping strategy (`MAP_FIXED` at a reserved low address, or a separate Mac↔host address translation rather than identity REAL_ADDRESSING). The binary is adhoc/linker-signed with no entitlements (`codesign -dv`: `flags=0x20002(adhoc,linker-signed)`, no `Internal requirements`, no entitlements), which is why MAP_JIT works without `com.apple.security.cs.allow-jit`.
- **What Phase 2 must address, prioritized:**
  1. **Low-4GB test RAM** (`sheepshaver_glue.cpp:938-954`): get the 16 MB SS_TEST region mapped below 4 GB (e.g. `MAP_FIXED` over a reserved low VA), or replace the identity `uint32(host ptr)` REAL_ADDRESSING with proper Mac↔host translation. This blocks *interpreter* tests too — it must be fixed before any pass count can move off 0.
  2. **W^X / MAP_JIT for the JIT code cache** (`src/kpx_cpu/src/cpu/jit/aarch64/jit-target-cache.hpp:41-46`, `jit_cache_alloc`): same RWX-anon-EPERM problem. Needs `MAP_JIT` plus `pthread_jit_write_protect_np(false/true)` toggling around code emission, and `sys_icache_invalidate`/the existing `ic ivau` flush (lines 30-38) on Apple Silicon. The SS_TEST RAM mmap (item 1) currently also asks for PROT_EXEC but does not actually need it once the JIT executes from the dedicated cache — that RWX request is the immediate EPERM trigger and should drop PROT_EXEC.
  3. Only after 1 & 2 can the harness exercise interpreter-vs-JIT equivalence; expect to re-run `./jit-test/run.sh` and chase real opcode mismatches then.

### ROM harness on macOS arm64 (Task 5)
- **ROM used:** `1998-07-21 - Mac OS ROM 1.1.rom` (oldest in `~/Downloads/New_World_Mac_Roms.zip`), 1,900,274 bytes, md5 `e0fc03faa589ee066c411b4603e0ac89`.
- **Result: build now succeeds; runtime fails immediately with `mmap: Permission denied` (EPERM), exit code 1, zero blocks tested, no score.** Same RWX-anon-EPERM blocker as the opcode harness — see below.
- **Build fix (commit `fix: build rom-harness on macOS arm64`):** `rom-harness.cpp:1141` used the Linux-only `MAP_FIXED_NOREPLACE` flag → `error: use of undeclared identifier`. Added `#ifndef MAP_FIXED_NOREPLACE / #define MAP_FIXED_NOREPLACE 0 / #endif` guard (matches the existing `sheepshaver_glue.cpp` shim). The existing non-fixed fallback mmap handles relocation, so no behaviour change on Linux. Pure portability; no JIT logic touched.
- **Root cause of runtime failure:** `rom-harness.cpp` allocates its RAM+ROM region with `mmap(..., PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS, ...)` — both the `0x10000000` fixed-hint attempt (lines 1138-1142) and the `NULL`-hint fallback (lines 1144-1147) request RWX, so both hit EPERM on Apple Silicon W^X and the harness prints `perror("mmap")` → "Permission denied" and returns 1. Identical mechanism to `jit_cache_alloc` and the opcode-test RAM mmap. To get a comparable score this would need `MAP_JIT` (+ `pthread_jit_write_protect_np` toggling) or separate RW-data / RX-code regions.
- **New World vs OldWorld format:** the user's ROMs are New World "Mac OS ROM" CHRP files — `1.1.rom` begins with the ASCII XML container `<CHRP-BOOT>\r\r<DESCRIPTION>\rMacROM for CHRP 1.1.\r...`, not a raw 68k/PPC ROM image. The harness scans raw PPC instruction words from offset 0, so even once the mmap blocker is fixed it would be scanning the CHRP/XML/icon header as if it were code, not a clean ROM code stream. Upstream's 1800/1825 baseline used `PowerMac-9500-OldWorld.rom` (a raw OldWorld dump, the Makefile's default `ROM=` path). **For comparable numbers an OldWorld ROM dump is required;** the New World CHRP container is not directly equivalent input. (Note: SheepShaver's *runtime* loader handles the CHRP container fine — see Tasks 6-7 below — it's only the standalone harness's raw-scan model that mismatches.)

### Boot attempts on macOS arm64 (Tasks 6-7)
- **Prefs used** (`~/.sheepshaver_prefs`, all item names verified against `src/prefs_items.cpp`): `rom`=Mac OS ROM 1.1, `cdrom`=`~/Downloads/Mac OS 8.6 Internal Edition.iso`, `ramsize 268435456` (256 MB), `screen win/800/600`, `nosound true`, `nocdrom false`, `bootdriver -62`. `jit false`+`SS_USE_JIT=0` for interpreter, `jit true`+`SS_USE_JIT=1` for JIT. (In this fork the AArch64 JIT path is gated by the `SS_USE_JIT` env var, default-on — `ppc-cpu.cpp:712`; the `jit` pref drives the separate legacy `use_jit` flag.)
- **Interpreter mode (Task 6): boots much further than the prior diagnosis predicted — this is the headline finding.** No RAM-allocation failure. The emulator reads the ROM, allocates RAM, brings up video (SDL Metal renderer, `the_buffer = 0x400050590000`, `mac_frame_base = 50590000`), starts "PowerPC CPU emulator by Gwenole Beauchesne", opens the Sony/disk drivers, and begins the Mac OS SCSI bus scan. It then spins **forever** in a `SCSIReset → SCSISelect 6…0 / SCSIGet` loop (1.81M log lines / ~18 MB in ~30 s) because **no bootable volume is available**. Ran to the 30 s manual cutoff without crashing; no `DiagnosticReports` entry.
- **Why no boot volume:** the CD-ROM ISO fails to open — `WARNING: Cannot open /Users/khawkins/Downloads/Mac OS 8.6 Internal Edition.iso (Resource temporarily unavailable)` (EAGAIN). In `sys_unix.cpp:636-650`, the macOS path opens regular-file media (`is_file`) with `O_EXLOCK | O_NONBLOCK`; the non-blocking exclusive-lock request returns EAGAIN, which the code interprets as "already locked by another process" and returns NULL. The ISO is readable by other tools (`head -c`, `md5` work) and not mounted, so the EAGAIN is the `O_EXLOCK|O_NONBLOCK` semantics, not a real lock. With no CD and no installed HD image, Mac OS never finds a boot device. (Emulator behaviour — left for Phase 2, not fixed here.)
- **JIT mode (Task 7): fails earlier and differently — at JIT init, NOT at the SCSI loop.** With `SS_USE_JIT=1` the first thing printed is `PPC-JIT-A64: failed to allocate 4096 KB code cache` and nothing else (51-byte log). `ppc_jit_aarch64_init` (`ppc-jit.cpp:3499-3506`) calls `jit_cache_alloc` (`jit-target-cache.hpp:41-46`), which is the exact RWX-anon mmap (`PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS`) that returns EPERM on Apple Silicon → returns NULL → the message. So the JIT failure point is **strictly earlier** than the interpreter's: the W^X mmap blocks JIT init before the emulator ever reaches video/SCSI. (The process did not exit on its own after the message within the 20 s window — it appears to stall rather than cleanly fall back to interpreter — but the JIT itself is hard-blocked at allocation.)
- **Crash report excerpts / key log lines:** no crash reports generated for either run (no `EXC_BAD_ACCESS`, nothing new in `~/Library/Logs/DiagnosticReports/`). Key lines — interpreter: `the_buffer = 0x400050590000` (RAM/framebuffer mapped **above 4 GB**, yet the runtime still executes — so the main emulator path does **not** enforce the low-4GB REAL_ADDRESSING that the SS_TEST/opcode-harness path requires), `WARNING: Cannot open …iso (Resource temporarily unavailable)`, then endless `SCSISelect/SCSIGet`. JIT: the single `PPC-JIT-A64: failed to allocate 4096 KB code cache` line. Binary has no entitlements (`codesign -d --entitlements -` prints nothing) — adhoc/linker-signed only.
- **Conclusion — what Phase 2 must fix before any boot is possible:**
  1. **W^X / `MAP_JIT` for `jit_cache_alloc`** (`jit-target-cache.hpp:41-46`) is the hard blocker for JIT-mode boot: the RWX-anon mmap must become `MAP_JIT` + `pthread_jit_write_protect_np()` toggling around emission (plus an `allow-jit`/`allow-unsigned-executable-memory` entitlement once codesigned). This is the **same** root cause as the rom-harness (Task 5) and the opcode harness — one fix unblocks all three. Until then JIT boot dies at init.
  2. **CD-ROM / disk-image open on macOS** (`sys_unix.cpp:636-650`): the `O_EXLOCK|O_NONBLOCK` path turns a perfectly openable `.iso` into a spurious EAGAIN "already locked" failure, leaving the VM with no boot medium. Needs an EAGAIN retry without `O_EXLOCK` (or `O_SHLOCK`/no-lock for read-only CD images). Without a boot volume even a fully-working interpreter just spins the SCSI scan forever.
  3. **Interpreter mode is already viable on macOS arm64** — it allocates RAM (even above 4 GB), drives the SDL/Metal video path, and runs PPC code through ROM init into the OS SCSI manager. The previously-assumed "RAM allocation fails before any window opens" does **not** hold for the runtime emulator (only for the SS_TEST harness path). Once item 2 supplies a boot medium, interpreter-mode boot-to-desktop should be the first reachable milestone; JIT boot follows item 1.
  4. (Lower priority) the standalone rom-harness needs an OldWorld ROM dump for comparable scores; New World CHRP ROMs are not equivalent raw input for its scanner, though the runtime loader handles them fine.

### CD-image open fix and first boot (macOS arm64)
- **Root cause (confirmed):** in `sys_unix.cpp` (the real file is `BasiliskII/src/Unix/sys_unix.cpp`; the SheepShaver path is a symlink to it — commit `ca96911c`), the macOS open path applied `O_EXLOCK | O_NONBLOCK` to *every* file-backed image (`is_file`), read-only or not. A non-blocking *exclusive* lock on a file the OS considers contended (Spotlight, a prior handle, or APFS semantics) returns `EAGAIN` immediately, and the code treated that as a fatal "already locked by another process" → returned NULL. For a read-only CD `.iso` the exclusive lock is pointless (read-only media cannot be corrupted), so this was a spurious failure. **This logic is inherited verbatim from upstream `kanjitalk755/master` — not a fork addition** (the fork's only nearby diffs are an unrelated `strdup` NULL-check and a `pread`/`pwrite` conversion). So the fix is upstream-relevant, not fork-specific.
- **Fix (minimal, macOS-guarded):** request an exclusive lock (`O_EXLOCK`) only for read-write file images; use a *shared* lock (`O_SHLOCK`) for read-only ones so multiple readers can coexist. On `EAGAIN`, read-only opens now retry once *without* any lock (safe — read-only media can't be corrupted) instead of failing; read-write opens keep the original fail-with-warning behavior, preserving the concurrent-write protection. The `EOPNOTSUPP` "filesystem doesn't support locking" fallback now clears both lock bits. All still inside `#if defined(__MACOSX__)` (O_EXLOCK/O_SHLOCK are BSD/macOS-only).
- **Boot result (interpreter mode, `jit false`, the user's Mac OS 8.6 ISO):** **the CD now opens — the spurious EAGAIN is gone, and the boot proceeds past the SCSI scan.** Before the fix the emulator printed `WARNING: Cannot open …iso (Resource temporarily unavailable)` and then spun the SCSI bus scan forever (1.81M log lines / ~18 MB in ~30 s, no boot volume). After the fix: **zero "Cannot open" warnings, zero SCSI-spin output** — the log stays at a single harmless line (`PPC-JIT-A64: failed to allocate 4096 KB code cache`, irrelevant in interpreter mode). The process ran continuously and was alive at the 30 s / 60 s / 120 s / 240 s checkpoints, burning **100% CPU with ~4m37s of CPU time over 4m37s elapsed** (RSS ~69 MB) — i.e. actively executing the PPC boot, not idle-waiting on a missing device. No crash, no `DiagnosticReports` entry. This is exactly the behavior expected once a boot volume becomes available: the SCSI manager finds the CD and hands off to the OS boot, which runs quietly (no console logging) inside the SDL/Metal window.
- **Screenshot caveat:** could **not** capture the emulator window — `screencapture -x` fails in this environment with `could not create image from display` (the agent's shell lacks macOS Screen Recording permission). So the *visual* boot stage (Happy Mac → "Welcome to Mac OS" splash → desktop/installer) was not directly observed; the evidence is the absence of the failure signature plus sustained 100%-CPU PPC execution. To see the screen the user (or a process with Screen Recording permission) should re-run `./SheepShaver` and watch the window directly.
- **Net:** item 2 of the Phase-1 conclusion is resolved; interpreter-mode boot is now unblocked at the I/O layer. Remaining for a confirmed boot-to-desktop: visually verify the window (needs Screen Recording permission). JIT-mode boot still blocked on item 1 (W^X / `MAP_JIT`).

## 2026-06-01 — Phase 2 design research (addressing model & W^X)

### __PAGEZERO cannot be shrunk on macOS arm64 (Option "REAL_ADDRESSING with low memory" is impossible)
- Apple DTS states explicitly that custom `pagezero_size` is "not a supportable option in the arm64
  environment" — arm64 code must be ASLR-compatible, and a small pagezero is incompatible with that.
  `-Wl,-pagezero_size,0x1000` produces "Malformed Mach-O" at link time on arm64; the minimum viable
  __PAGEZERO is 0x100000000 (4 GB). Source: https://developer.apple.com/forums/thread/655950
- The classic pagezero hack only ever worked on x86_64 (cebix's `PAGEZERO_HACK` exists only in the
  x86 config headers; `config-macosx-aarch64.h:431` leaves it commented out).
- macOS has no `personality()` equivalent to disable ASLR (upstream's Linux workaround,
  `main_unix.cpp:822-835`). Net: guest addresses can NEVER equal host addresses on macOS arm64.

### DIRECT_ADDRESSING is the correct model — and it's already half-wired
- `configure.ac` already sets `NATMEM_OFFSET 0x400000000000` for `arm-apple-*` hosts; when
  NATMEM_OFFSET is defined (and EMULATED_PPC), `sysdeps.h:90-99` selects DIRECT_ADDRESSING.
- Under DIRECT: host = NATMEM_OFFSET + (guest & 0xFFFFFFFF) (`vm.hpp:188-223`). This is the proven
  approach kanjitalk755 uses for macOS x86_64 (with `gZeroPage`/`gKernelData` redirection for the
  few regions Mac OS needs at fixed low guest addresses — `vm.hpp:207-219`).
- CONFIRMED WORKING at runtime: the Task 6 interpreter boot allocated RAM/framebuffer at
  0x400050590000 (= NATMEM_OFFSET + guest range) and executed PPC code through ROM init into the OS.
  The interpreter path is already DIRECT-correct on macOS arm64.
- The SS_TEST harness path (`sheepshaver_glue.cpp:935-984`) is the ONLY runtime piece still assuming
  REAL addressing (low-4GB identity); it needs to be ported to the same DIRECT model so the opcode
  harness can serve as a regression gate.

### The aarch64 JIT hardcodes REAL addressing — the core Phase 2 code change
- `ppc-jit.cpp` contains zero references to NATMEM_OFFSET / VMBaseDiff / vm_wrap_address. Load/store
  codegen (`emit_load_ea_base`, `ppc-jit.cpp:516-524`, lwz at ~1846-1862, etc.) uses the bare 32-bit
  guest EA as a host pointer (`LDR Wt, [Xn]` with no base offset).
- This works on Linux only because upstream maps guest RAM at low addresses (ASLR disabled via
  personality()). On macOS (RAM at NATMEM_OFFSET) every JIT memory access would fault or read garbage.
- Fix shape (mechanical, well-contained): reserve a callee-saved register (prologue at
  ppc-jit.cpp:~3570 currently saves x20=RSTATE), load NATMEM_OFFSET once, convert guest accesses to
  register-offset form (`LDR Wt, [Xbase, Xea]`). All access sites funnel through the EA-in-RTMP0
  idiom, so a small set of helpers centralizes the change. AArch64 has native register-offset
  load/store encodings — usually zero extra instructions per access.
- Bonus: this also closes a latent interpreter-vs-JIT divergence that exists upstream (interpreter
  nominally DIRECT, JIT effectively REAL — only coincidentally consistent on Linux).

### W^X / MAP_JIT facts (verified on this machine + Apple docs)
- The JIT cache RWX mmap fails with EPERM on macOS arm64. Adding MAP_JIT makes it succeed.
- `pthread_jit_write_protect_np(0/1)` is the per-thread W↔X toggle; `sys_icache_invalidate()` is
  mandatory after emission (separate I/D caches on arm64).
- `com.apple.security.cs.allow-jit` is only enforced when the Hardened Runtime is enabled (a
  code-signing flag, off by default). Ad-hoc/linker-signed dev builds (what we have:
  `flags=0x20002(adhoc,linker-signed)`) can use MAP_JIT freely — expected, stable behavior, not an
  accident. The entitlement becomes mandatory in Phase 4 when we sign with Hardened Runtime for
  notarization.
- Guest RAM/ROM should drop PROT_EXEC entirely: emulated PPC code is only ever read as data; the JIT
  never branches into guest memory (translated code lives in the separate MAP_JIT cache).

### Phase 2 task ordering (informed by all of the above)
1. MAP_JIT + write-protect toggling for `jit_cache_alloc` — single shared blocker for JIT boot,
   rom-harness, and JIT opcode vectors
2. Port SS_TEST harness RAM allocation to DIRECT addressing — makes the opcode harness usable as the
   regression gate for step 3
3. aarch64 JIT DIRECT_ADDRESSING codegen (base register) — the centerpiece; verified family-by-family
   with the harness
4. Visual confirmation of interpreter boot-to-desktop (needs Screen Recording permission or the user
   watching) — already unblocked by the CD-open fix

## 2026-06-01 — SDL window never appears on macOS: root cause was a JIT block-cache cycle, NOT the event pump

**Symptom:** SDL window never appears on macOS; SDL init succeeds, video_open() runs, renderer
("metal") is created, then nothing. 100% CPU. User's log always ends with
`PPC-JIT-A64: failed to allocate 4096 KB code cache`.

**Hypothesis going in (from the bug brief):** Cocoa requires the SDL event pump on the *main*
thread; SheepShaver hands the main thread to PPC emulation (`emul_func` at main_unix.cpp:1256), so
nobody pumps Cocoa events → no window. **This hypothesis was WRONG.** The architecture is fine:
SheepShaver's main thread IS the emul thread, and on the EMULATED_PPC path
`HandleInterrupt()` (sheepshaver_glue.cpp:1168) calls `SDL_PumpEvents()` on that thread once the
60Hz VBL interrupt flows. kanjitalk755 does the same (and it works on x86_64). The event pump was
never the problem — the guest just never booted far enough to reach it.

**Actual root cause (verified with lldb + instrumented builds):**
The arm64 direct-codegen JIT (`SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`, an
rcarmo-only file — kanjitalk755 has no aarch64 JIT at all) keeps a block-address cache as a hash of
linked lists: `jit_bc_heads[bucket]` indexes into `jit_bc_pool[]`, walked by `jit_bc_lookup()`:
`while (idx >= 0) { ...; idx = pool[idx].next; }`. The "empty" sentinel is **-1**, but
`jit_bc_heads[]` and `jit_bc_pool[]` are **static, zero-initialised** arrays, and **0 is a valid
pool index**. The arrays are only set to -1 by `jit_bc_flush()`, which was called *only* from
`ppc_jit_aarch64_init()` **after** a successful code-cache allocation.

When the code cache fails to allocate (`jit_cache_alloc` returns NULL — exactly the user's log
line), `ppc_jit_aarch64_init()` returns early, before `jit_bc_flush()`. The execute loop
(ppc-cpu.cpp:716) **ignores** that return value, sets `jit_init_done=true`, and calls
`ppc_jit_aarch64_compile()` → `jit_bc_lookup()`. With heads/pool all zero:
`idx = heads[bucket] = 0` → `pool[0].next = 0` → idx stays 0 → **infinite self-loop**. The emul
thread wedges on the very first block lookup at guest pc=0x50310000 (a tiny early-boot spin block),
never advances, never services interrupts → no boot → no window.

**Evidence chain:**
- lldb `bt all`: main thread always in `jit_bc_lookup` (ppc-jit.cpp:136 `while (idx >= 0)`), guest
  pc pinned at 0x50310000 across all samples. Redraw/tick/nvram threads all alive and idle.
- Instrumented `TriggerInterrupt`: fires ~390×/8s (tick thread, ppc_cpu non-null, `trigger_interrupt`
  called). Instrumented `HandleInterrupt`: **0 calls**. So interrupts were raised but never serviced.
- Instrumented `check_spcflags`: 0 calls → the execute loop never reaches its spcflags poll.
- Instrumented `ppc_jit_aarch64_compile` ENTER: called **exactly once** (pc=0x50310000) and never
  returns → hang is *inside* it. A cycle-guard in `jit_bc_lookup` fired immediately
  (`CYCLE pc=0x50310000 idx=0`), confirming the zero-init self-loop.

**The fix (ppc-jit.cpp only, ~30 lines):**
- Add a `jit_bc_ready` guard + `jit_bc_ensure_init()` that runs `jit_bc_flush()` lazily on first use,
  called at the top of `jit_bc_lookup/insert/invalidate_pc/record_chain_site/patch_chain_sites`.
  Guarantees the -1 sentinel before any bucket walk, independent of code-cache allocation.
- In the alloc-failure branch of `ppc_jit_aarch64_init()`, also call `jit_bc_flush()` and reword the
  log to say it falls back to the interpreter (it does — `compile()` returns false when the cache is
  NULL, ppc-cpu.cpp:721 then uses the interpreter).
- In `ppc_jit_aarch64_compile()`, bail out early (`return false`) when `jit_cache_base == NULL` to
  avoid spamming "code cache full, flushing" on every block (was ~147k lines in 6s).

**Verified after fix:** guest boots — main thread sample shows `EmulOp → idle_wait()` (timer_unix.cpp:372,
`__psynch_cvwait`), i.e. MacOS reached its idle loop; CPU dropped 100% → ~2%; `jit_bc_lookup` no
longer in any sample. The VBL/`present_sdl_video`/`SDL_PumpEvents` main-thread path now runs, so the
window displays.

**Why Linux/x86 were unaffected:** the file is `#if __aarch64__ && USE_AARCH64_JIT` only. x86_64
(kanjitalk755's macOS target) uses a different execute loop and has no aarch64 block cache. On Linux
arm64 the code cache normally allocates fine (no MAP_JIT/EPERM issue), so `jit_bc_flush()` ran and
the latent bug stayed hidden. The fix is purely additive (a defensive sentinel-init) and changes
nothing when the cache allocates successfully.

**Latent upstream note:** `spcflags::mask` (kpx_cpu/src/cpu/spcflags.hpp) is a plain non-volatile
`uint32` read locklessly by `empty()/test()` in the hot loop while helper threads `set()` it under a
spinlock. Identical to kanjitalk755. Not the cause here (the loop never even reached the poll), and a
trial `volatile` did not change behavior, so it was left untouched — but it's a real
memory-visibility smell worth revisiting if interrupt-latency bugs surface later.

## 2026-06-01 — MILESTONE: First boot to desktop on macOS arm64

**Mac OS 8.6 boots to the Finder desktop, natively on Apple Silicon (macOS 26.4.1, M-series), in
interpreter mode** — confirmed visually by Ken (screenshot: Finder running, "Mac OS Internal
Edition" CD mounted and browsable, Unix extfs volume on desktop, menu bar/Trash functional).

What it took (the full chain, all on branch macos-arm64):
1. configure fix: host_cpu=arm on macOS → JIT/build wiring (214b1a3a)
2. Three build portability fixes: slirp/C23, genexec $(CXX) -E, MAP_FIXED_NOREPLACE (5d950374..705d3dad)
3. CD-image open fix: O_EXLOCK spurious EAGAIN → read-only opens retry without lock (9bc215cd)
4. Boot-hang fix: zero-initialized JIT block cache + failed cache alloc = infinite loop; now falls
   back to interpreter cleanly (3ed724ea)

Boot sequence observed: ROM load → SCSI scan finds CD → Sony/Disk drivers up → video (SDL2/Metal,
800x600) → Mac OS 8.6 splash → Finder desktop. Boot time: a few minutes (interpreter).

Remaining for full Phase 2: MAP_JIT cache fix (Task 2a) → JIT initializes; SS_TEST DIRECT port
(2b) → harness gates work; JIT DIRECT addressing (2c) → JIT actually executes correctly. Then
this same boot should be ~5x faster.

## 2026-06-01 — Phase 2 implementation

### Task 2a: MAP_JIT code cache — results

Implemented MAP_JIT + per-thread write-protect toggling for the aarch64 JIT code cache.
The W^X RWX-mmap blocker is resolved for the code cache and the rom-harness.

**Inventory (cache alloc + every cache-write site):**
- Alloc: `jit-target-cache.hpp:jit_cache_alloc()` — the single `mmap` for the code cache.
- Write site 1 — block compilation: `ppc-jit.cpp ppc_jit_aarch64_compile()`, all `emit32()` between
  `code_start` and the final `jit_cache_flush(code_start, code_bytes)`.
- Write site 2 — runtime chain back-patch: `ppc-jit.cpp patch_chain_sites()`, the single-word
  `*site->patch_loc = B<off>` overwrite (one bracket per patched word).
- rom-harness: its `mem` mmap is the emulated RAM+ROM **data** region (read by the JIT, never
  executed); native code runs only from the separate `jit_cache_alloc` cache.

**Changes:**
- `jit-target-cache.hpp`: added `JIT_CACHE_MAP_FLAGS` (`+MAP_JIT` on Apple arm64),
  `jit_cache_begin_write()` = `pthread_jit_write_protect_np(0)`, `jit_cache_end_write()` =
  `pthread_jit_write_protect_np(1)` + `sys_icache_invalidate()`. `jit_cache_alloc()` uses the new
  flags (kept `PROT_READ|WRITE|EXEC`). `jit_cache_flush()` early-returns on Apple (icache handled by
  `sys_icache_invalidate`); Linux DC CVAU / IC IVAU path is byte-identical, behind `#else`.
- `ppc-jit.cpp`: bracketed the whole-block compile with `begin_write` (after setting `jit_code_ptr`)
  and `end_write` (after the flush); also restore executable state on the early `return false` empty-block
  bail. Bracketed each `patch_chain_sites` word write. Added a one-time `code cache allocated (MAP_JIT)`
  log on Apple.
- `rom-harness.cpp`: emulated `mem` region now requests `PROT_READ|PROT_WRITE` (no EXEC) on Apple
  arm64 via `ROM_HARNESS_MEM_PROT` — it is never executed, so dropping EXEC clears the RWX EPERM.
  Linux keeps RWX.

**Thread-safety:** `pthread_jit_write_protect_np` is per-thread. The JIT is single-threaded:
`ppc-cpu.cpp` execute loop (and rom-harness) compile a block, then immediately call `fn()` into the
cache on the SAME thread, serially. Every write bracket ends with `end_write` (write-protect ON =
executable) BEFORE any call into compiled code, so the executing thread always sees the executable
state. No nested brackets: `jit_bc_insert`→`patch_chain_sites` runs after the compile bracket already
closed.

**Verification:**
- SheepShaver builds clean; rom-harness builds (only pre-existing warnings).
- Plain RWX anon mmap still fails EPERM on this machine; MAP_JIT succeeds (reconfirmed).
- Standalone test using the actual `jit-target-cache.hpp` helpers: alloc → begin_write → write
  (MOVZ/RET) → flush → end_write → execute returned correct value; back-patch path (rewrite one word,
  re-flush, re-exec) also correct. ALL PASS.
- rom-harness on the New World ROM (`--count=100`): no "mmap: Permission denied"; prints
  `code cache allocated (MAP_JIT)`; 27 blocks compiled, 228 JIT hits, **0 SIGSEGV** — the full
  toggle+icache+execute cycle works through the real JIT. Score 0/0 garbage on CHRP ROM (expected).
- jit-test harness: still `pass=0 score=0`, unchanged — every vector fails on the Task 2b
  `SS_TEST: cannot allocate RAM in low 4GB` blocker BEFORE JIT init runs, so the harness cannot
  exercise this code (the standalone + rom-harness checks cover it instead). No regression.

**SS_TEST/boot caveat:** `ppc_jit_aarch64_init` runs only from the CPU execute loop, not the SS_TEST
path. SS_TEST still dies on the RAM blocker (Task 2b). With `SS_USE_JIT=1`, full boot will now
initialize the JIT cache successfully and start executing compiled blocks — but JIT codegen still
hardcodes REAL addressing (Task 2c), so executed blocks will compute wrong addresses; GATE3
(out-of-range PC) / SIGSEGV skip paths hand those back to the interpreter, so boot should still
proceed (correctness via interpreter fallback) rather than the JIT being correct on its own.

### Task 2b: SS_TEST DIRECT addressing — results

Ported the SS_TEST opcode-test RAM allocation from REAL (low-4GB) to DIRECT (NATMEM_OFFSET) so
the jit-test harness runs on macOS arm64.

**What the build defines:** this Unix/configure build is **DIRECT_ADDRESSING**. `config.h` already
has `#define NATMEM_OFFSET 0x400000000000` (configure.ac line ~1320 unconditionally defines it),
and `sysdeps.h:95` selects `DIRECT_ADDRESSING` when `NATMEM_OFFSET` is set. Nothing in configure.ac
needed wiring — it was already correct; only the SS_TEST code path still assumed REAL. (Confirmed by
`grep NATMEM_OFFSET config.h`.)

**Addressing model (vm.hpp ~188-219):** DIRECT translates guest→host as
`host = vm_wrap_address(guest) + VMBaseDiff`, where `VMBaseDiff = NATMEM_OFFSET` and
`vm_wrap_address` truncates the guest address to 32-bit. So a guest RAM address is a *small 32-bit
value* and its backing store lives at the *high* host address `NATMEM_OFFSET + guest`. REAL is the
degenerate case `VMBaseDiff = 0` (guest addr == host addr as uint32), which is impossible on macOS
arm64 because __PAGEZERO owns the low 4GB.

**The fix (`sheepshaver_glue.cpp ss_run_opcode_test`):**
- `#if REAL_ADDRESSING`: original logic unchanged (mmap at 0x10000000, low-4GB check, PROT_EXEC,
  `RAMBase = (uint32)test_ram`).
- `#else` (DIRECT): pick guest base `test_base = 0x10000000` (same as main path's `RAM_BASE`), mmap
  `MAP_FIXED` at `Mac2HostAddr(test_base)` = `NATMEM_OFFSET + test_base` with `PROT_READ|WRITE` only
  (no EXEC — interpreter never executes guest RAM, JIT uses its own MAP_JIT cache), set
  `RAMBase = test_base` (guest) and `RAMBaseHost = test_ram` (host). All guest addresses handed to the
  CPU (`test_addr`, `stack_addr`, LR) are now `RAMBase + offset` (guest values), which
  `vm_do_get_real_address()` maps back to the host buffer. Opcodes are written into the host buffer.
- Added a `POWERPC_EXEC_RETURN` sentinel at RAM offset 0x8000 and pointed LR there, so the appended
  `blr` returns into the emul-op that cleanly stops the interpreter loop (previously LR was a raw host
  pointer; under DIRECT that would be a bogus guest address).

**The second, non-obvious bug — default execute() re-enters the JIT.** After fixing RAM, pure-ALU
vectors passed but every load/store vector *crashed* (EXC_BAD_ACCESS at the bare guest address, e.g.
`0x10ffc100`, in code living in the MAP_JIT cache). Root cause: `powerpc_cpu::execute()` on aarch64
uses the JIT **by default** (gated only by `SS_USE_JIT`, default-on at `ppc-cpu.cpp:712`), and that
JIT still emits REAL-style accesses (treats the 32-bit guest address as a host pointer). So the
"interpreter" fall-through `cpu->execute(test_addr)` wasn't actually the interpreter. Fix: in the test
path, unless the caller explicitly set `SS_TEST_JIT`, `setenv("SS_USE_JIT","0")` before the first
`execute()` so the default test run is genuinely interpreter-only (the gate caches the env value in a
`static`, and each harness vector is a fresh process). The explicit `SS_TEST_JIT` path is untouched.
**This cleanly splits Task 2b (interpreter, done) from Task 2c (JIT codegen).**

**Harness results (`jit-test/run.sh` — runs every vector TWICE in interpreter mode and checks the two
REGDUMPs are identical; it does NOT test JIT mode at all — `SS_TEST_JIT` is never set):**
- BEFORE: `pass=0 fail=0/209 score=0` — every vector died on `SS_TEST: cannot allocate RAM in low 4GB`
  (empty REGDUMP).
- AFTER (RAM fix only, JIT still default-on): `pass=167 fail=42 score=79` — the 42 fails were all
  load/store/FP vectors crashing in the JIT; pure-ALU passed.
- AFTER (RAM fix + interpreter pin): **`pass=209 fail=0 score=100`**. All 209 interpreter vectors pass
  deterministically.

**Boot regression:** no regression. My changes are entirely inside `ss_run_opcode_test()` (only runs
when `SS_TEST_HEX` is set) plus the test-RAM mmap — the boot path never calls it. Verified by building
the pristine (pre-change) binary and running the identical boot check: pristine and modified binaries
produce **byte-identical** boot behaviour. Note: default boot (`./SheepShaver`, no SS_USE_JIT) SIGSEGVs
early in *both* binaries (`ea 0x400004000000`) — that is the pre-existing Task 2c JIT-boot crash, not a
regression. The `video_open`/`the_buffer` grep target in the suggested boot-check is `D(bug(...))`
debug-only output and never appears in this `DEBUG 0` / -O2 build; interpreter-mode boot
(`SS_USE_JIT=0`) runs silently for 15s+ with no crash, matching the documented "interpreter spins in
the SCSI scan" behaviour.

**For Task 2c (JIT codegen, DIRECT addressing in emitted code):**
- Set `SS_TEST_JIT=1` to drive the JIT directly in the harness path.
- JIT **ALU** vectors (e.g. `38600005`): compile fine ("compiled 2 PPC insns", "native execution
  complete") but currently print **no REGDUMP** — the explicit `SS_TEST_JIT` path's `regs_for_jit()`
  register-state plumbing needs checking, separate from addressing.
- JIT **memory** vectors (e.g. `stw`/`lwz`): **crash** (SIGSEGV, no "native execution complete"). The
  emitted load/store uses the 32-bit guest address as a raw host pointer. Task 2c must make the JIT emit
  `host = guest + NATMEM_OFFSET` (add the VMBaseDiff base register, or fold the offset) for every memory
  access — the same translation `vm_do_get_real_address` does. The opcode-*fetch* in
  `ppc_jit_aarch64_compile` (`p = ram + (cur_pc - (uint32)ram)`) happens to work only because guest
  base low-32 == host base low-32 for this test; emitted *runtime* accesses do not.

## 2026-06-02 — Phase 3 JIT boot investigation

### Crash diagnosis: GATE3 PC-reset caused double-execution (commit 7cc741da)

**Symptom:** First JIT boot attempt with DIRECT addressing (after Phase 2 complete) crashed with
SIGSEGV within ~10s of launch: `pc 0xffffffffffffffff ea 0x400000006524 (guest pc 0x6524)`.

**Two diagnostic findings:**

1. **`pc 0xffffffffffffffff` in SIGSEGV dumps = SIGSEGV_INVALID_ADDRESS, not a real crash address.**
   `sigsegv.h` had no `SIGSEGV_FAULT_INSTRUCTION` for the `__aarch64__` + Mach exceptions path — only
   PPC, x86, x86_64 were defined. `sigsegv_get_fault_instruction_address` returned `SIP->pc` which was
   initialized to `SIGSEGV_INVALID_ADDRESS = -1`. The real crash is always a DATA ACCESS fault; the
   `ea` (= ARM64 FAR from exception state) is the actual bad memory address.
   Fix: added `#define SIGSEGV_FAULT_INSTRUCTION SIP->thr_state.MACH_FIELD_NAME(pc)` to `sigsegv.h`
   (`BasiliskII/src/CrossPlatform/sigsegv.h:121`, same file SheepShaver symlinks to).

2. **GATE3 double-execution bug (root cause of crash).**
   GATE3 handler in `ppc-cpu.cpp:739` was:
   ```
   set_register(powerpc_registers::PC, any_register(jblk.ppc_start_pc));  // WRONG
   ppc_jit_aarch64_invalidate_pc(jblk.ppc_start_pc);
   goto skip_jit;
   ```
   When the JIT block at 0x100518e0 (RAM) branched to 0x5058ffc8 (SheepMem thunk area), GATE3 reset
   PC back to 0x100518e0 and fell to the interpreter. The interpreter's `bi` pointed to the block for
   0x100518e0, so the interpreter **re-executed the same block** — all register writes and memory stores
   happened twice. This caused state divergence: after a few such replays, execution reached guest PC
   0x6524 (low-memory area, guest addr 0x6524 → host NATMEM_OFFSET+0x6524 = 0x400000006524,
   unmapped) → SIGSEGV.
   
   The GATE3 comment claimed "safe interpreter start" but the JIT had already computed the correct PC.
   Resetting to block entry was both wrong and unnecessary.

**Fix (commit 7cc741da):**
- Removed the `set_register(PC, ppc_start_pc)` line from GATE3.
- After eviction, use `bi = my_block_cache.find(pc()); if (bi) goto pdi_execute; continue;`
  so the interpreter dispatches from the CORRECT jit_pc (0x5058ffc8), not the stale block entry.
- Extended GATE3 exclusion range from `ROMBase + 0x500000` to `ROMBase + 0x600000` to cover
  SheepMem (0x50510000–0x5058FFFF, mapped as ROM_AREA_SIZE + SIG_STACK_SIZE + SheepMem::size).
  SheepShaver thunk calls (all ~0x5058xxxx) no longer trigger GATE3 and no longer spam the log.

**SheepMem memory layout (macOS arm64, DIRECT addressing):**
| Guest Range | Contents |
|---|---|
| 0x00000000–0x00002FFF | Low Memory (3 pages, explicitly mapped) |
| 0x10000000–0x1FFFFFFF | RAM (256 MB) |
| 0x50000000–0x504FFFFF | ROM (4 MB file, 5 MB area) |
| 0x50500000–0x5050FFFF | SIG_STACK (64 KB) |
| 0x50510000–0x5058FFFF | SheepMem (512 KB) |
| 0x68070000–... | DR Emulator |
| 0x68FFE000–... | Kernel Data |
| 0x69000000–... | DR Cache |

The gap 0x00003000–0x0FFFFFFF is unmapped. Guest PCs in this range cannot be safely executed by
either JIT or interpreter (instruction fetch would fault). Normal Mac OS execution never reaches
these addresses when booting from ROM.

**Result:** After fix, JIT boot runs indefinitely without crashing. Log is clean (only 4 lines:
cache alloc + 2 cache flushes). 100% CPU sustained — actively executing Mac OS boot code.
JIT desktop boot needs visual confirmation from Ken.

### JIT boot timing anomaly (Phase 3 investigation, 2026-06-02)

**Finding: interpreter boots in ~12 seconds (hot NVRAM), JIT takes 20+ minutes without reaching idle.**

**Setup:**
- Interpreter mode (`SS_USE_JIT=0`): reaches `idle_wait()` at `EmulOp:OP_IDLE_TIME_2` in ~12s.
  `sample` shows `powerpc_cpu::execute → sheepshavar_cpu::execute_sheep → EmulOp → idle_wait()`.
  This is the Finder idle loop — Mac OS 8.6 booted to desktop in 12s because NVRAM from a prior
  interpreter session cached the boot state.
- JIT mode (`SS_USE_JIT=1`): 100% CPU for 20+ minutes, never reaches `idle_wait()`. Log stays
  clean (no GATE3 messages). Code cache fills twice at startup (~80,000 blocks compiled in first
  6s), then stable (no more flushes → cached blocks being reused repeatedly).

**Root cause hypothesis**: JIT compiled some block(s) incorrectly and wrote wrong values to
guest memory or registers. The interpreter then ran with corrupted state and entered a loop that
it normally exits (via EmulOp/idle_wait). The interpreter code at `skip_jit:` runs the SAME logic
in both modes — if it's stuck in JIT mode, the JIT execution upstream must have corrupted state.

**Key observations:**
- ALL samples (~1500) from the JIT boot are at the INTERPRETER level (`powerpc_cpu::execute` at
  ppc-cpu.cpp:779, the `skip_jit:` inner loop). Zero samples in the JIT cache (0x119bXXXXX).
  This means execution settled into the interpreter inner loop after the initial JIT compilation.
- Counter reads (via lldb on a running JIT boot, reading `jit_blocks_attempted` / `jit_blocks_complete`):
  - T=15s: attempted=66,823, complete=16,916 (25% complete rate). JIT IS running native blocks.
  - T=30s: attempted=66,836 (+13 in 15s ≈ 1/sec). Counter essentially STOPPED growing.
  - After the 15s burst, the interpreter inner loop dominates. JIT gate is only re-entered
    when the interpreter inner loop hits an uncached block (~1/sec). The 16,916 complete
    JIT-compiled blocks that ran in the first 15 seconds set the guest state that the
    interpreter then runs with indefinitely.
- Harness regression during this session (42 JIT failures) was due to **stale .o files** from
  incremental make — a probe was added to ppc-jit.cpp, then reverted, but `make` saw same-second
  timestamps and didn't recompile. `make clean && make` restored 209/209 immediately. The probe
  code itself did NOT cause any behavioral change. **KEY RULE**: always `make clean && make` when
  changing JIT source files before running harnesses.
- `regs_for_jit()` is fragile in principle (runtime sentinel scan could use `offsetof` instead)
  but it was NOT proven to break from probe code in this session.

**RESOLVED (2026-06-02, Phase 3 complete): JIT boot now reaches Mac OS 8.6 Finder desktop.**
See section below for root cause and fix.

## 2026-06-02 — Phase 3 root cause: W^X overhead + differential trace methodology

### Root cause: W^X overhead for out-of-range blocks (commit 8f2acc9b)

**Symptom:** JIT mode was 37× slower than interpreter-only mode during boot. Interpreter boots
in 12s (warm NVRAM); JIT boot was observed at 100% CPU indefinitely without reaching idle.

**Discovery via differential PC trace:**
Ran both modes with `SS_JIT_TRACE=/tmp/trace.txt` (added env-var trace at `pdi_execute:`). First
mismatch between interpreter and JIT traces was at entry #2042 (block `50463168`), but this was
a FALSE divergence — the interpreter had already booted to Finder (12s) while JIT was still in
ROM init at the same clock time. The traces diverged because the two modes were at different
boot phases, not due to a correctness bug.

The REAL finding from the trace: the JIT trace had only 5,958 entries in 20 seconds (297/sec) vs
interpreter's 520,000+ entries in 47 seconds (11,000/sec). The 37× throughput drop explained the
entire performance problem.

**Root cause (ppc_jit_aarch64_compile, ppc-jit.cpp):**
`ppc_jit_aarch64_compile()` called `jit_cache_begin_write()` (`pthread_jit_write_protect_np(0)`)
and emitted the block prologue (7 STP instructions) **before** the compile loop's first iteration
checks `if (cur_pc < ram || cur_pc >= ram + ramsize) break`. For ROM/SheepMem blocks (~85% of
block entries during early boot), this meant:
1. `pthread_jit_write_protect_np(0)` — W^X write-enable (~μs on Apple Silicon)
2. Emit 7-instruction prologue to JIT cache
3. Compile loop: immediate break (out of range)
4. `pthread_jit_write_protect_np(1)` + `sys_icache_invalidate()` — W^X re-protect (~μs)

At 11,000 blocks/second × ~3μs overhead = ~33ms overhead/second = 97% of CPU spent on W^X
syscalls for ROM blocks, leaving only 3% for actual emulation.

**Fix:** Add an early-out range check BEFORE `jit_cache_begin_write()`:
```cpp
if (pc < (uint32_t)(uintptr_t)ram || pc >= (uint32_t)(uintptr_t)ram + ramsize) {
    out->complete = false; out->code = NULL; out->chain_code = NULL;
    return false;
}
```
ROM/SheepMem blocks now return immediately at zero cost. **Result: JIT boot completes in ~2:40
(first boot, warm NVRAM from interpreter), CPU drops to ~0% at idle_wait, Finder desktop running.**

### Chain-patch tracking (also in 8f2acc9b)

Added `bool patched` field to `jit_chain_site` so that live chain-patches (B instructions
baked into block epilogues) can be reverted when a range is invalidated. Previously, nullifying
a pool entry did NOT revert B-patches in calling blocks, causing stale ARM64 code to run.
`ppc_jit_aarch64_invalidate_range(start, end)` now:
1. Reverts B patches targeting invalidated range → restores `LDP x27,x28,[sp],#16`
2. Nullifies pool entries for PCs in range → next lookup recompiles

This is activated by connecting `icbi`/`isync` to `invalidate_cache_range()`. Currently `icbi`/
`isync` are NOPs in the JIT (reverted because icbi-fix made the boot ~24× slower via excessive
full-cache flushes on every isync call); range-based invalidation is the correct solution once
the chain-patch safety is confirmed working.

### icbi/isync status and path forward

**icbi is still a NOP in the JIT.** The icbi-triggered-invalidation approach was tried:
- Full flush on every isync: slow (each flush requires recompiling ~50+ active blocks)
- Range-based invalidation without chain-patch tracking: caused CTR/LR corruption from stale JIT

The range-based approach WITH chain-patch tracking (now implemented) is the correct fix. To
re-enable: in `invalidate_cache_range()`, call `ppc_jit_aarch64_invalidate_range(start, end)`.
In `ppc-jit.cpp`, change `icbi` case 982 from `return true` to `return false` (fall to interpreter).
The performance impact should be minimal because range invalidation only touches blocks whose PC
falls in the icbi'd range, not the full cache.

### SS_JIT_TRACE debugging tool

`SS_JIT_TRACE=/path` logs every block entry (`I <pc>`) and JIT execution (`J <from> <to> ...regs...`).
Use for differential JIT vs interpreter analysis. Zero overhead when env var is not set.
The J lines include r24/r27/r29/LR/CR/XER — chosen because they are the Mac ROM 68k emulator's
working registers (r24=68k PC, r27=68k opcode, r29=handler address).

## 2026-06-02 (later) — JIT performance work: ROM compilation, OE arithmetic, chaining post-mortem

### Performance changes (all behind this session's commits)

1. **64 MB code cache** (was 4 MB), block pool 65536 (was 16384), buckets 32768 (was 8192).
   Eliminates the recurring full-cache flushes that forced recompilation of everything.
   Also: cache-full margin raised from 256 bytes to 256 KB (a single block can emit up to
   ~100 KB; the old margin was a latent buffer overflow).

2. **ROM block compilation** (`ppc_jit_aarch64_set_rom_range`). The Mac ROM
   (`vm_protect`ed READ|EXECUTE after patching — immutable) is registered as a second
   JIT-compilable range. ROM toolbox code is ~85% of boot-time block dispatches and was
   100% interpreted before. `jit_fetch_ptr()` resolves guest PCs in either RAM or ROM.
   Bisect switch: `SS_JIT_NO_ROM=1` keeps ROM interpreter-only.

3. **OE=1 (overflow) arithmetic**: addco/subfco/addo/subfo/nego (10-bit XO = 512+base).
   Discovered via the failure histogram: XO=522 (addco) + XO=520 (subfco) were 95% of all
   block-compile failures. These are the hot loops of the **68k emulator inside the Mac ROM**
   (it uses PPC overflow arithmetic to derive 68k condition codes).
   Bisect switch: `SS_JIT_NO_OE=1` falls back to the interpreter for OE variants.

### The chaining post-mortem (critical findings)

**Finding 1: block chaining never worked.** The post-compile check required the last emitted
instruction to be RET; blocks ending with a chain branch (B <chain_code>) were marked
incomplete → GATE2 excluded them → they always ran interpreted. Since hot blocks' targets
are compiled first, *the hottest blocks were systematically the ones excluded.* Chaining was
self-defeating from the day it was written.

**Finding 2: chaining is architecturally unsafe as-is.** Once the marking bug was fixed and
chained blocks actually ran, boots hung: chained cycles (guest polling loops) spin in native
code without ever returning to the dispatcher, so spcflags are never polled and interrupts
(60 Hz VBL → all Mac OS I/O completion) are never serviced.

**Resolution: `JIT_BLOCK_CHAINING=0`** — chaining is disabled entirely. Every block returns to
the dispatcher (which polls spcflags) after execution. Re-enabling requires a designed-in
interrupt strategy (options documented at the #define site in ppc-jit.cpp).

**Corollary discovered by enabling/disabling chaining:** blocks ending in `b`/`bl` to
already-compiled targets had *never* executed natively before this session (they were the
chained ones). The bug class "compiles fine, executes wrong, but only visible in whole-system
boot" lives in exactly this population — the opcode harness cannot cover them because branch
targets fall outside single-vector test environments.

### Diagnostic tooling added this session

- `SS_JIT_DEBUG_PC=<hex>`: per-PC compile decision trace (cache hit/miss, fetch failure,
  which instruction failed, complete/incomplete).
- `SS_JIT_NO_OE=1` / `SS_JIT_NO_ROM=1`: bisect switches.
- `tools/screenshot.sh`: captures the guest framebuffer to PNG via lldb memory dump
  (no Screen Recording permission needed). Note: SheepShaver's built-in VNC server is a
  no-op stub unless built with libvncserver.
- Trace fflush: SS_JIT_TRACE lines are flushed per-line so the final entries survive a crash.

### CLAUDE.md correction needed

CLAUDE.md says XER byte offsets are "so=900, ca=902" — stale. The current powerpc_registers
layout (with gpr_hi[32] for 64-bit G5 mode) puts them at so=1028, ov=1029, ca=1030.
The code (PPCR_XER_*) is correct; only the doc note is outdated.

### The 68k-emulator-in-JIT problem (why ROM JIT range stops at +0x460000)

**The hardest bug of the session.** Once OE arithmetic made the Mac ROM's built-in 68k
emulator blocks compile, boots crashed non-deterministically (5s–60s, sometimes not at all)
with the guest jumping to garbage addresses.

**What the ROM's 68k emulator is:** an interpreter written in PPC, living at ROM+0x460000
onward. Dispatch loop at +0x466080/84/c0/e0/100/120 (six variants differing in flag
handling), micro-handler continuations at +0x463xxx/+0x46xxxx, and an opcode-indexed
handler table at +0x48xxxx..+0x4Fxxxx (8 bytes per 68k opcode: one PPC instruction + a
branch back to a dispatch variant). Multi-step 68k instructions chain through multiple
dispatch round-trips, carrying intermediate state in r0/r8 and the prefetch pipeline
(r24=68k PC, r27=prefetched word, r29=handler address).

**The dispatch protocol's interrupt mechanism:** every dispatch variant ends with
`bclr BO=5,BI=8` — branch to the computed handler if CR[8] (CR2.LT) is clear, else fall
through to the interrupt path at +0x46d0d4. `HandleInterrupt()` (MODE_68K case) injects
the interrupt by OR-ing a mask into the guest CR — i.e., **the interrupt is delivered by
asynchronously flipping a CR bit that the dispatch loop polls.**

**Why JIT-compiling this breaks:** every individual instruction (rlwimi, mtlr, lhau,
addco., bclr) and every complete dispatch block tests bit-identical between interpreter
and JIT in the opcode harness — the codegen is *correct*. What differs is the
*interrupt-delivery interleaving*: with the dispatch/handler blocks JIT-compiled, the
spcflags poll (and hence the CR-bit injection) lands at different points in the 68k
instruction emulation pipeline than it does under the interpreter, and the DR emulator's
multi-step sequences (e.g. `bset #imm,d0` = clear-scratch → read-immediate → compute-mask
→ test-and-set, or `cmp.l -(a0),d0` = copy-A0 → pre-decrement-load → subtract) get cut
mid-sequence: an interrupt-path excursion at the wrong step loses the in-flight state
(observed: D0 ends up 0 instead of 0x80000000; the immediate word 0x001f gets dispatched
as a 68k opcode; A0 misses its pre-decrement).

**Resolution:** the JIT ROM range stops at ROMBase+0x460000. The PPC toolbox/nanokernel
code below (the bulk of what Mac OS 8.x executes — it is a native PPC OS) is JIT-compiled;
the 68k emulator runs interpreted, where the interrupt interleaving matches the original
semantics. The 9 OE-arithmetic instructions remain enabled (they are correct and also
appear in toolbox code).

**Diagnostic methodology that found this** (recorded for reuse): trace J-lines at handler-
entry granularity (execution entering 0x5048xxxx-0x504Fxxxx = exactly one entry per 68k
instruction in both modes — block-level and r24-level comparisons produce pipeline-position
artifacts and false divergences). First real divergence: 4th 68k instruction ever
dispatched, `bset #31,d0`.

### isync must compile as a NOP (the 42× slowdown)

An attempt to wire icbi/isync SMC invalidation by making both fall back to the interpreter
(`return false` in compile_one) made JIT boot **42× slower than interpreter boot**
(426s vs 10s, warm NVRAM). Root cause: **isync appears after every mtmsr/mtspr sequence in
OS code** — making isync-containing blocks incomplete marks most of the ROM toolbox
interpreter-only, negating the ROM compilation win entirely.

Resolution: icbi and isync stay native NOPs (upstream behavior). The SMC gap this leaves
(stale JIT translations if guest code is rewritten in place at the same address) is
pre-existing, rarely triggered (extensions load into fresh RAM), and documented at the
icbi case in ppc-jit.cpp. The correct future fix is bucket-based invalidation called from
the interpreter's execute_icbi — never isync-falls-back.

### JIT↔interpreter handoff (required once any region is interpreter-only)

The interpreter inner loop only exits on a block-cache miss. Once execution enters an
interpreter-only region (the 68k emulator), the interpreter captures ALL subsequent
execution — including JIT-compiled toolbox/RAM code — because those blocks are also in
its cache. Fix: the inner loop now also exits when `ppc_jit_aarch64_is_compilable(pc())`
(2-4 compares per interpreted block), handing compilable code back to the dispatcher.
Note: the check must test compilABILITY, not "already compiled" — code first reached from
inside an interpreter session would otherwise never meet the compiler at all.

### Boot time is the JIT's worst case — and the honest performance picture

Measured on this machine (warm NVRAM, Mac OS 8.6, all of today's work):

| Configuration | Boot to desktop |
|---|---|
| Pure interpreter (SS_USE_JIT=0) | ~10 s |
| JIT, any of today's configurations | 160 s – 7 min |

**The JIT has never beaten the interpreter for BOOT TIME in any configuration**, and the
boot is structurally hostile to it:
1. Extension loading is 68k-heavy → runs in the (correctly) interpreter-only 68k emulator
2. Every JIT/interpreter boundary crossing costs a dispatcher round-trip (~3 compile()
   calls + hash lookups).  During 68k phases these crossings happen per 68k instruction —
   profiling shows compile() call overhead + SDL_PumpEvents-per-interrupt dominating.
3. Every block is compiled exactly once and many run only once during boot — pure overhead.

The JIT's value proposition is steady-state execution (desktop, applications, benchmarks)
— NOT boot. That measurement (MacBench / app responsiveness) is the next session's first
task, and the boot-time gap should not be read as "the JIT is pointless."

Structural fix directions for the boundary thrashing (next session):
- Make dispatcher transitions cheaper (slim compile()'s early path: the 4 KB report
  arrays in its stack frame force __chkstk probing on every call)
- Reduce interrupt cost (SDL_PumpEvents per 60 Hz interrupt walks the whole Cocoa event
  machinery — batch or throttle it)
- Ultimately: solve the 68k-emulator-in-JIT problem so there is no boundary at all.

## 2026-06-02 (evening) — Test environment: macOS TCC dialogs were silently disrupting boot tests

### Symptom and root cause

The user reported macOS permission dialogs ("SheepShaver would like to access files in
your Downloads folder") appearing and sitting unanswered for 5-10 minutes. Root cause:
the ROM and the boot ISO lived in `~/Downloads`, which is TCC-protected (same protection
class as Desktop/Documents). Two factors made this recur:

1. **Every rebuild creates a "new" app** in TCC's eyes (ad-hoc signature, new CDHash) —
   consent does not persist across rebuilds.
2. Background boot tests launch SheepShaver dozens of times unattended — the prompt can
   appear when nobody is watching, and a pending TCC prompt **blocks the file open()**,
   which presents as a stalled or oddly-timed boot test.

This may explain earlier timing anomalies (e.g. boot tests that "stalled" with no other
explanation, or crashes whose wall-clock time varied while guest state was identical).

### Fix (applied)

Test assets moved to `/Users/Shared/macemu/` (not TCC-protected; world-readable):
- `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom`
- `/Users/Shared/macemu/Mac OS 8.6 Internal Edition.iso`

`~/.sheepshaver_prefs` updated to point there (old prefs backed up to
`~/.sheepshaver_prefs.bak`). Originals remain in `~/Downloads`.

**Rule for future test assets: never reference files in Desktop/Documents/Downloads from
an emulator under test.** Use `/Users/Shared/` or a path inside the repo working tree.

## 2026-06-02 (evening) — ROOT CAUSE of the deterministic 68k-region boot crash: crorc miscompilation

### The bug (one missing AND #1 in ppc-jit.cpp case 417)

`crorc crbD,crbA,crbB` (CR logical OR-with-complement) was compiled as a bare ARM64 `ORN`:
result = bitA | ~bitB. With bitA/bitB ∈ {0,1}, ~bitB is the FULL 32-bit complement, so the
"result bit" is 0xFFFFFFFE or 0xFFFFFFFF — not 0/1. The merge step then ORs this entire
value (shifted) into the CR word: **CR becomes 0xffffffff**. It is also wrong on truth
value: crorc(0,1) must be 0 but produced a truthy garbage value.

The asymmetry that hid it: crand/cror/crxor are naturally bounded; crnor/creqv/crnand do
MVN then AND #1 (masked); crandc uses BIC which is bounded *by accident* (a & anything ≤ a).
Only crorc (ORN) both needs the mask and lacks it.

### Why it only appeared when the 68k emulator region was JIT-compiled

`crorc` lives in the DR (68k) emulator's interrupt/context-switch code (ROM block
0x5046e100: `crorc 30,30,18`). That region was interpreter-only until today's dyngen-parity
work compiled it. The toolbox/RAM code the JIT had been running for weeks doesn't execute
crorc in any path boot exercises — which is also why the 218-vector harness (no CR-logical
coverage) and every prior boot stayed green.

### The full causal chain (every crash-signature element explained)

1. JIT-compiled block 0x5046e100 executes broken crorc → **CR = 0xffffffff** (trace shows
   cr=fffffffe → ffffffff exactly there)
2. Corrupted CR propagates through the DR emulator's 68k context save/restore
   (mfcr/mtcrf blocks at 0x5046e004/0x5046e084)
3. Later, inside a re-entrant `execute_68k()` (EMUL_OP → Execute68k — which sets r23=0
   BY DESIGN), the 68k dispatch variant `mtctr r23; …; bcctr 12,5; bclr 5,8` runs.
   CR bit 5 (garbage from step 1) is SET → `bcctr` fires → **branch to CTR = r23 = 0**
4. Guest executes low-memory garbage at PC 0/0x60/0x104/…/0x134 → wild store → SIGSEGV
   (the recurring signature: pc≈0x134, lr=504a1fc0 [the handler it SHOULD have gone to],
   ctr=0, ea=0x4000ffffxxxx, CR1=f)

### Diagnostic methodology that found it (reusable)

1. **In-memory trace ring** (SS_JIT_TRACE_RING=1): 256K-record execution history with zero
   I/O overhead, dumped by the SIGSEGV handler → /tmp/ss_jit_ring.txt. fprintf-per-block
   tracing distorts timing and produces multi-GB files; the ring does neither.
2. **Runtime env-var bisect** (SS_JIT_NO_CHAIN=1): deconfounded chaining from 68k-region
   compilation in one run without rebuilds — chaining was exonerated immediately.
3. **lldb attach + guest memory dump**: byte-swap lldb's little-endian display to read
   big-endian PPC opcodes; decode ROM blocks at deterministic addresses.
4. **Isolated block reproduction**: extract a suspect ROM block's exact instructions into
   an SS_TEST_HEX vector with memory read-backs → proves codegen correct/broken in
   isolation, free of all boot noise. (Block 5010bb90 passed → redirected the hunt from
   stores to CR state.)
5. **Software watchpoint in the dispatcher** (SS_JIT_WATCH_STUB=1): ROM-anchored arm +
   per-block memory check. Caveat learned: executing a stub doesn't modify it, so
   "changed" events can't distinguish consumed from unconsumed stubs — correlate with the
   ring's r24 history instead.
6. **Key trap to avoid**: cross-run comparisons anchored on RAM/stack addresses are noise
   (run-specific layout). Anchor on ROM PCs, which are deterministic.

### Status

- Fix: mask crorc's ORN result back to 1 bit (matches the crnor/creqv/crnand pattern).
- Harness gap: CR-logical family (crand/cror/crxor/crnor/crandc/creqv/crorc/crnand) had
  zero coverage — vectors added for all 8, crorc with all four input combinations.
- The harness alone is NOT the gate for this work: block 5010bb90 passes the harness while
  the boot failed. Boot-to-desktop with the ring live is the verification bar.
- Expect more latent bugs in the newly-compiled 68k region to surface after this fix
  (it has only ever executed a few hundred ms before dying). Same methodology applies.

---

## 2026-06-03 (session 9) — Register allocator, performance optimizations, clean shutdown

### Register allocator re-enable (P1)

The RA was disabled with a "boot hang regression" comment. Re-enabling it was
safe because the original regression was one of the 12 bugs fixed in sessions 7-8.

**Key lesson: ra_load before ra_store.** When `rd == ra` (e.g., `addi r3, r3, 100`),
calling `ra_store(rd)` before `ra_load(ra)` allocates the RA slot without loading
the old value. The subsequent `ra_load(ra)` sees it "cached" and returns the
uninitialized register. This caused a black screen on boot — VERIFY showed no
divergence because the first block hung before completing. Fix: always load source
operands before allocating the destination.

**Shim vs direct: the MOV bounce disaster.** First attempt re-enabled the RA via
a shim in `emit_load_gpr`/`emit_store_gpr` that MOVed between RA regs and RTMPs.
This was 5-8% SLOWER than no RA — Apple Silicon's L1 is so fast that the extra
MOV costs more than the LDR it replaces for single-use GPRs. The fix: convert
all 339 callsites to use `ra_load`/`ra_store` directly, operating on RA-assigned
registers in emit32 encodings. Hybrid fallback for unconverted sites checks the
RA cache first.

### adde/subfe carry bug (backlog A1/A2)

The 64-bit-sum approach for adde/subfe dropped the carry when CA wraps:
`ADDS ~rA+rB` then non-flag `ADD` of CA loses the second carry contribution.
Fix: `CMP W(CA),#1` to materialize CA into host C flag, then `ADCS` computes
the full sum with correct carry-out. 10→4 instructions.

### bcctr: harder than expected (P2)

Attempted unconditional `bctr` with bit-0 Mixed Mode guard. Stalls during
extension loading. `SS_JIT_VERIFY=1` showed the interpreter's `execute_bcctr`
does more than `PC = CTR` — it handles Mixed Mode Manager transitions
(CallUniversalProc) that modify GPR0, GPR2, GPR12, LR, CTR, and CR. A simple
TBZ guard cannot replicate this. Needs Mixed Mode dispatch understanding.

### Lazy CR0: NZCV is a shared resource (P0g)

Attempted lazy CR0: emit CMP immediately, defer the ~12-instruction CR0 field
construction. Crashes in early boot — an intervening instruction clobbers NZCV,
and the re-CMP from `lazy_cr0_reg` fails because the RA evicted/reused the
register. The fundamental problem: NZCV is shared by all flag-setting instructions.
Fix requires tracking which instructions between Rc=1 and flush clobber NZCV.

### Atomic spcflags (P0d)

`basic_spcflags` used a spinlock for every set/clear/test. The 60 Hz VBL timer
thread serialized with the JIT dispatch loop's per-block poll. Replaced with
`std::atomic<uint32>` — `fetch_or`/`fetch_and` with relaxed/release ordering.
CPU score 65.2 (new high).

### Clean shutdown

Mac OS 8.6's Special > Shut Down calls the `PowerOff()` trap, which was patched
to `M68K_EMUL_RETURN` — this just returned to the nanokernel idle loop without
telling the host to exit. Fix: new `OP_POWEROFF` emul op sets a flag and breaks
the CPU loop via `SPCFLAG_CPU_EXEC_RETURN`. Video/redraw thread must be stopped
FIRST in `Quit()` — otherwise the redraw thread segfaults accessing freed guest
memory.

### Benchmark variance

Speedometer PR is highly volatile (driven by Disk and Graphics sub-scores).
Benchmark Mix and Dhrystones are the stable integer metrics. Always compare
Mix, not PR, for JIT codegen changes.

### SS_JIT_VERIFY false positives (2026-06-04)

VERIFY produced 20+ divergences per boot — all cascading from blocks that
call through the Mixed Mode Manager via `bl`.  The interpreter replay follows
the call into the callee (different dispatch path), while the JIT treats `bl`
as a block terminator.  Once one block diverges, every subsequent block sees
poisoned register state and reports a false divergence too.

Fix: skip verifying blocks ending with link-setting branches, and suppress
further checks after any divergence until a clean block is found.  Reduces
false positives from 20+ to 1.

## 2026-06-04 — FP/AltiVec test-vector audit: vacuousness, ev_mixed byte order, harness integrity

A "just add FP/AltiVec test vectors" task turned into a multi-round audit (two
adversarial-subagent reviews) that found the entire FP/AltiVec test suite was
fake and uncovered a pervasive AltiVec correctness bug. The non-obvious findings:

### "Passes make test-jit" ≠ "tests something" — the vacuousness trap

The harness REGDUMP captures **GPRs/CR/LR/CTR/XER only — NOT FPRs or VRs**
(sheepshaver_glue.cpp). So any FP/AltiVec vector that leaves its result in an
FPR/VR/memory and never `lwz`s it into a GPR produces an identical REGDUMP whether
the op is right or wrong → the JIT-vs-interpreter diff passes **trivially**. ALL
9 pre-existing `fp_*` vectors and ALL 12 pre-existing `vec_*` vectors were vacuous
(verified: r4=r5=r6=0). FP/AltiVec arithmetic was *effectively untested* despite
the green score. Rule: a vector must `op -> stvx/stfd -> lwz result into a GPR`.

### "Reaches a GPR" still isn't enough — the masking trap

`vmuleub` with byte-uniform operands (0x05×0x03) passed, but even/odd byte
selection gives the same product, so it couldn't test the op's defining property.
Position-dependent ops need **distinct per-lane operands** (lvx a `00 01 … 0F`
pattern), not `vspltisb` uniform splats.

### AltiVec ev_mixed byte order (the real bug)

VRs are stored in the interpreter's `ev_mixed` order (ppc-operands.hpp):
`byte_element(i) = (i&~3)+(3-(i&3))` — **bytes reversed within each 32-bit word,
word order preserved**. `emit_load_vr` is a plain `LDR Q` that loads this raw, so
NEON lane `i` holds PPC element `byte_element(i)`, not `i`. Every op that depends
on sub-word byte position is wrong. **Fixed** vspltb/vsplth (remap the DUP index).

**The merge encodings also LIED (2026-06-04).** Separate from ev_mixed: the
vmrgh*/vmrgl* cases emitted `0x..C400`/`0x..C800` — bit15 set makes those
three-same *arithmetic* ops, NOT the ZIP1/ZIP2 permutes the inline comments
claimed. So merges produced garbage regardless of byte order. Corrected to real
ZIP1/ZIP2 {16B,8H,4S}. With the right encoding, **vmrghw/vmrglw (word-granular)
are now fully correct** — word order is preserved under ev_mixed, so ZIP.4S needs
no byte remap. Verified xpass + boot-clean (zero VR divergence), promoted to the
scored gate (255→257). Lesson: a wrong-but-plausible encoding hidden behind a
correct-sounding comment passes a *vacuous* harness silently — distrust the
comment, decode the hex.

**Byte/halfword merges fixed via per-op normalize (2026-06-04, 257→261).** The
right encoding alone isn't enough for sub-word merges: `ev_mixed` is *exactly*
`REV32.16B` at the byte level vs natural PPC element order. So `emit_vmrg`
normalizes both inputs with `REV32.16B` (→ lane k = PPC byte k), merges with
`ZIP1`/`ZIP2.{16B,8H}` (PPC elem 0 = MSB = NEON's lowest lane after the rev, so
PPC-high = ZIP1, PPC-low = ZIP2), then `REV32.16B` back. **Key insight: a *local*
(per-op) REV32 normalize works, but the *global* version — REV32 in
`emit_load_vr`/`emit_store_vr` — was empirically ruled out** (it re-breaks the
ops that are already correct under the raw convention, e.g. word-granular ones).
Same transform, right granularity. (My earlier `REV64.4S` guess was wrong —
distrust the hypothesis, derive it.) Verified xpass with DISTINCT operands
(vA=00..0F, vB=10..1F) so a ZIP1↔ZIP2 / A↔B swap can't pass coincidentally;
boot-clean under SS_JIT_VERIFY.

**vpkuhum fixed the same way (2026-06-04, 261→262).** Pack keeps each halfword's
LOW byte = PPC byte 2i+1 = the ODD lane in natural order, so on the REV32.16B-
normalized inputs it's `UZP2.16B` (reuses emit_vmrg). It had *also* ignored vA
(loaded only vB) — a second bug the harness caught once operands were distinct.
The non-saturating word pack `vpkuwum` has the identical ignore-vA bug, un-vectored.

**Even/odd byte multiplies fixed (2026-06-04, 262→264) — the ev_mixed class is now
complete.** `vmuloub`/`vmuleub` had TWO bugs: a non-widening `MUL.8B` (must widen
8×8→16) and no ev_mixed even/odd select. `emit_vmul_byte`: REV32.16B normalize ->
UZP1 (even bytes 0,2,..) / UZP2 (odd 1,3,..) select into low 8 lanes -> `UMULL.8H`
widen -> `REV32.8H` (output is halfwords, so the OUTPUT normalize is `.8H`, matching
`half_element`, not `.16B`). Distinct operands caught both: even-lane products like
0x0A×0x1A=260 exceed 255, so a non-widening op truncates visibly. Encoding tip:
`UMULL.8H` = the file's signed widening encoding + the U bit (`0x0E20C000 | 0x20000000`),
more reliable than reciting the bitfield. The whole AltiVec ev_mixed element-order
class (splats, merges, pack, byte multiplies) is fixed + scored; the quarantine lane
is empty.

**Still untested (no test vector, signposted in code):** halfword multiplies `vmul*h`
(need the hw→word analogue), word pack `vpkuwum`, signed byte mults `vmulosb`/`vmulesb`.
Fix path = per-op ev_mixed-aware codegen (same as above); the global load/store REV32
approach is ruled out (above).

### VX-form XO is UNSHIFTED

Unlike X/A-form (XO at bits 21-30, emitted `xo<<1`), VX-form AltiVec ops put an
11-bit XO at bits 21-31 with **no shift**. Shifting it (the natural mistake) gives
an illegal no-op → another vacuous pass. (vsel is VA-form, 6-bit XO — encoded ok.)

### The SheepShaver harness had NO integrity self-validation (BasiliskII does)

The PPC harness lacked the duplicate/format/sentinel checks the 68k harness has.
Adding a preflight immediately caught 3 real pre-existing **duplicate vector
names** (crand_basic/mcrf_basic/orc_basic) where the 2nd `T_<name>` shadowed the
1st in bash var lookup, so one vector of each pair **never ran** — silent lost
coverage. Note: the preflight catches malformed/duplicate, NOT vacuousness (a
vacuous vector still touches scratch GPRs, so a "changed nothing" guard misses it;
the real defense is the gen-*-vectors.py generators).

### Process: adversarial review caught what the gate didn't

Both adversarial-subagent rounds found real defects (vacuous vectors, wrong fmadd
frB/frC field order, a masking vector, the duplicate names) that a green
`make test-jit` hid. Generators (`gen-fp-vectors.py`, `gen-altivec-vectors.py`)
now encode correctly and document the traps — use them, don't hand-encode.

## 2026-06-04 — Evaluated and rejected: per-opcode spcflags gate before `bclr 5,8`

A salvaged experiment (`wip/dr-bclr-spcflags-gate`, tip 51b6bec3, since deleted)
injected a spcflags poll **before one hardcoded instruction** — the DR emulator's
`bclr 5,8` (op `0x4C420020`): `LDR spcflags; AND #0x0F; CBZ fast-path; else store PC +
bare epilogue back to the C dispatcher`. The idea was to make CR2.LT reflect pending
interrupts at that branch's evaluation time.

**Verdict: superseded — do not re-salvage.** Mainline already does a strictly more
general version: `emit_entry_spcflags_poll` (ppc-jit.cpp) polls spcflags at **every
block / chain entry** (the dyngen `gen_start` equivalent), masking the **same**
actionable bits (`PPCR_SPCFLAGS_POLL_MASK = 0x0F`) and re-dispatching via
`check_spcflags()`. It uses the same struct offset the experiment "discovered"
(`PPCR_SPCFLAGS = 1056`, where the 0d atomic-spcflags change relocated the field).
A per-block-entry poll subsumes a single-opcode poll completely; hardcoding
`op == 0x4C420020` is the inferior design (catches that one instruction, misses every
other boundary). The DR emulator is JIT-compiled and boots correctly **without** this
gate. This approach also descends from the spcflags-timing theory that produced the
reverted, guest-memory-corrupting fix (see the session-5 retraction above) — extra
reason not to revive it. If interrupt-poll *frequency* ever becomes a perf concern,
optimize `emit_entry_spcflags_poll`, don't resurrect a per-opcode gate.

## 2026-06-07 — AltiVec is dormant, not missing; `mfmsr[VEC]=1` is NOT the detection gate

Two findings that should stop a future session re-treading this ground.

**1. The aarch64 AltiVec NEON codegen already exists and is mature.** `ppc-jit.cpp`
emits real NEON for the AltiVec families (arith/logical/compare, shifts/rotates,
saturating, averages, merges, splat, byte-multiply; 54 differential vectors in
`jit-test/`; 27 codegen bugs already found+fixed — see
`docs/planning/ALTIVEC-SHIFT-ROTATE-BUGS.md`). It is **dormant** only because the
guest never *issues* AltiVec: Mac OS runs everything scalar. So "0 AltiVec blocks in
the Fractal Carbon profile" (task #16) is a **detection** gap, not a codegen gap. Open
codegen families that must be closed before broad enablement is *safe*: pack / pixel /
sum, and `fctiw`/`fctid` non-default rounding.

**2. `mfmsr` returning MSR[VEC]=1 does NOT enable AltiVec — falsified empirically.**
Hypothesis (task #21): the OS might *read* `mfmsr[VEC]` (no `mtmsr` write needed, so the
undecoded-`mtmsr` crash is irrelevant) to gate AltiVec. Tested by making both interp
(`ppc-execute.cpp` execute_mfmsr) and JIT (`ppc-jit.cpp` case 83) return
`0xf072 | 0x02000000 = 0x0200f072`, rebuilding, booting Fractal Carbon via the e2e
workload harness with `SS_JIT_PROFILE`. Result: **still 0 AltiVec blocks** (hot mix
unchanged: 24 integer / 11 load-store / 4 branch / 1 FP, 1104 guest-MIPS). The app
launched and rendered fine (no illegal-instruction trap — confirms the interpreter
also handles whatever it issues), but issued **zero** vector instructions. VEC math was
correct (VEC = MSR bit 6 = `0x02000000`); the research's earlier `0x0002f072` was a
*different* bit and is moot. Change reverted.

**The real gate is gestalt `'ppcf'` (`0x70706366`) bit 4
(`gestaltPowerPCHasVectorInstructions`), computed inside the ROM/NanoKernel.** There is
**no** SheepShaver hook for it: a grep for `0x70706366`/`ppcf`/`gestaltPowerPC*` across
`SheepShaver/src` is empty. The InitGestalt patch (`rom_patches.cpp:1699`) is a fragile
ROM-version-specific binary patch that writes only CPU-type byte `$1d(a2)`, page size
`$1e(a2)`, and RAM size — it does **not** touch the processor-features word. So enabling
detection means finding and patching the ROM's vector-feature computation (a ROM-spelunk),
not a one-line config change.

**Kept from this excursion (committed independently):** the JIT `mfmsr` now returns
`0xf072` to match the interpreter (it previously returned 0 — a latent JIT/interp
divergence for any guest reading MSR). test-jit 303/303.

**Ordering decision (codegen-first):** close pack/pixel/sum + `fctiw` rounding via the
`SS_TEST_HEX` differential harness (needs no guest detection at all) *before* attempting
gestalt enablement, so that whenever the `'ppcf'` site is found, flipping it is a pure
win and not silent corruption for apps that hit the open families. See ROADMAP B5.

## 2026-06-07 — FP-RA op conversion clean; lazy-CR0 RTMP1-clobber landmine documented

Completed the P5b FP register-allocator op conversion: all FP **indexed/update memory**
(`lf{s,d}x`/`lf{s,d}ux`/`stf{s,d}x`/`stf{s,d}ux`/`lf{s,d}u`/`stf{s,d}u`) and the single-op
`frsp`/`fsel`/`frsqrte`/`fsqrt` moved from the FMOV bridge to zero-copy `ra_fp_load`/`ra_fp_store`.
test-jit 303→317 (added 14 vectors: 12 FP-mem + 2 fsel; `frsqrte`/`fsqrt` are prospective —
not differentially testable). FC perf-neutral (its Mandelbrot loop is register FP arith, doesn't
use these ops). **No hot FP op uses the `emit_*_fpr` bridge anymore** (it survives only as the
coherence shim for struct-resident mffs/mtfsf/fcmp).

**Adversarial review (subagent) found no new bug**, but surfaced a *pre-existing* landmine that
matters for [[the §0g lazy-CR0 re-enable]]: when lazy-CR0 is armed, `ra_evict` →
`emit_materialize_cr0` emits **`CSET RTMP1, GT`**, clobbering RTMP1. The FP **store** forms
(stf*x/stf*ux/stf*u) and `lmw` hold a live value in **RTMP1 across a trailing integer
`ra_load`** → silent store corruption *the moment §0g is wired up*. It is **dead today**
(`lazy_cr0_valid` is never set `true`). Fix when re-enabling lazy-CR0: compute the EA via
`ra_load` *before* materializing the store value into RTMP1 (mirror the load forms). Recorded in
OPTIMIZATION-PLAN §0g. (The load/update forms are already safe — `ra_fp_*` eviction is GP-clean.)

Minor pre-existing nit (not fixed — untestable, invisible): `fsel` emits signaling `FCMPE` vs
`#0.0`; PPC `fsel` is exception-free, so `FCMP` would be more correct, but FPSCR exceptions aren't
surfaced so it's unobservable and the differential harness can't validate a change.

## 2026-06-07 — AltiVec ev_mixed normalize rules (generalizable; cracked the saturating packs)

Fixed all 6 saturating packs (vpk{sh,uh,sw,uw}{ss,us}) by deriving — empirically against the
interpreter REGDUMP, never from input-byte reasoning — these **reusable ev_mixed normalize rules**
for `emit_load_vr` (raw `LDR Q`) / `emit_store_vr` (raw `STR Q`). They should accelerate the
remaining pixel/sum-across families and any future AltiVec codegen:

- **REV16.16B and REV32.16B COMMUTE** (both are byte permutations; on a word `[a,b,c,d]` either
  order → `[c,d,a,b]`). So "REV32 then REV16" == "REV16 then REV32" = *swap adjacent halfwords
  within each word, halfword values intact*.
- **Byte elements** (`byte_element` = reverse within word): raw↔natural = **REV32.16B** (self-inverse).
  So byte-granular ops (merges, vpkuhum, byte output) normalize with a single REV32.16B each side.
- **Halfword elements** (`half_element` = swap within pairs, values intact): raw↔natural =
  **REV32.16B + REV16.16B** (the commuting pair). Use on halfword INPUTS (to get correct `.8H`
  values in order) and halfword OUTPUTS.
- **Word elements** (`word_element` = identity): the raw `.4S` lane **already holds the correct PPC
  word value in the correct order** — NEON's little-endian `.4S` read cancels the ev_mixed in-word
  byte reverse. So word inputs need **NO** normalize; a REV32 there byte-swaps the words (a bug I hit).
- **Saturating narrow signedness encodings** (these are easy to mislabel — the pre-existing code had
  `0x2E212800` commented "UQXTN" but it is **SQXTUN**): SQXTN `.8B`=0x0E214800/`.4H`=0x0E614800
  (opcode 10100,U=0); SQXTUN `.8B`=0x2E212800/`.4H`=0x2E612800 (10010,U=1); UQXTN
  `.8B`=0x2E214800/`.4H`=0x2E614800 (10100,U=1). `2` (high-half) variant sets bit30.

**Test-operand rule (the false-PASS trap):** saturating ops MUST be tested with operands that
**cross the saturation boundary** (negatives + over-range), else SQXTUN and UQXTN are
indistinguishable and a wrong impl passes. The earlier reverted attempt used non-saturating
positives and got a false PASS. Recipe in `gen-altivec-vectors.py` (`packop`/`packwop`), full
write-up in [[the ALTIVEC-SHIFT-ROTATE-BUGS doc]].

---

## Session 11 — Experiment 1: Mac OS 9.2.1 on the 1.1 ROM (2026-06-09)

### The 1.1 ROM is NewWorld, not OldWorld

**Critical finding:** The file `1998-07-21 - Mac OS ROM 1.1.rom` is detected by
`rom_detect_type()` as **ROMTYPE_NEWWORLD (type 5)**, not ROMTYPE_GOSSAMER (type 4).
The nanokernel ID string at ROM offset `ROM_NANOKERNEL_ID_OFFSET` matches `"NewWorld"`.

This means the "Upgrade Card" metaphor (keep OldWorld ROM, shim CPU identity) was based on
a misunderstanding: we already HAVE a NewWorld ROM. The 1.1 ROM IS a NewWorld Mac OS ROM
file — version 1.1, circa 1998, from the first generation of NewWorld machines (iMac G3).
All CLAUDE.md references to this as "OldWorld" were incorrect. The "OldWorld" label likely
conflated the ROM file format (NewWorld CHRP/parcels) with the hardware generation the file
was designed for.

**Impact on strategy:** The "upgrade card" framing doesn't describe what we're doing. Real
G3→G4 upgrade cards didn't bypass ROM version checks — they upgraded the CPU on machines
that already had sufficient ROMs. Our real situation is: we have a **1998 NewWorld ROM** (v1.1)
trying to boot a **2001 System** (9.2.1). The gap is ROM version/vintage, not ROM type.

### gestaltMachineType patch — no effect

Patched UniversalInfo offset 0x60 from 0x3d to 0x196 (406) in the NEWWORLD branch of
`rom_patches.cpp` (line ~1856), gated on `SS_NW_MODEL`. Confirmed the patch fires via
`[NW-MODEL] UniversalInfo gestaltMachineType patched to 406 (0x196)` in stderr. The boot
stall is byte-identical: same 10,169 compiled blocks, same DR Emulator hot PCs, same
dialog: "This startup disk will not work on this Macintosh model."

**Why it doesn't help:** On NewWorld, ALL machines report gestaltMachineType=406. It's the
universal NewWorld value, documented as "informational only." The 9.2.1 System file's
compatibility check doesn't discriminate on this field — 406 is what every NewWorld machine
already returns.

### device-tree model/compatible — no effect (earlier finding, confirmed)

`SS_NW_MODEL=1` injects `model=PowerMac3,1` + `compatible` into the Name Registry device
tree (name_registry.cpp lines 94–120). Also no effect on the "will not work" dialog.

### The check is in the System file, not the ROM

The error string "This startup disk will not work on this Macintosh model" was found in the
System file on the disk image (multiple copies at offsets 0x653c417, 0x11d49217, 0x18fcf8c4),
NOT in the ROM dump. It is NOT in the 4MB ROM image at all.

The dialog is drawn by 68k code running through the DR Emulator — the hot PCs during the
stall (0x504a51c0 at 58.7%, 0x504b37d0 at 40.5%) are DR Emulator dispatch table entries,
confirmed by disassembly (lbz/b and addi/b patterns = 68k opcode dispatch). The System has
progressed through nanokernel init, Toolbox init (QuickDraw, Window Manager, Dialog Manager
all working), and is spinning in a modal dialog event loop.

### The check runs AFTER Toolbox init

The error dialog is a standard Mac alert — rendered by Dialog Manager over a gray desktop.
This means the check happens during System startup, AFTER the basic Toolbox is initialized,
but BEFORE extensions load or the Finder starts. The code that performs the check is likely
in the System file's `INIT` or boot-sequence resource, not in the ROM.

### What the check probably tests

The gibbly/BoxFlag mechanism (System 7.1–7.5 era) is retired by Mac OS 8.0. The 9.2.1
System file likely checks for:
- **ROM file version** — the "Mac OS ROM" file has a version resource; version 1.1 may be
  below 9.2.1's minimum
- **ROM feature flags** — specific capabilities (parcels, Toolbox exports) present in later
  ROM versions but absent in v1.1
- **Nanokernel version** — a version field in the Kernel Data Page (KDP)
- **Some other ROM characteristic** that differs between the 1998 v1.1 ROM and the 2000+ ROMs
  that shipped alongside Mac OS 9.x

The wack0/universal-tbxi-patchset solves the OPPOSITE problem (old OS on new ROM) and
is not applicable.

### Experiment 1 outcome: blocked, needs deeper RE

The "upgrade card" approach of patching identity fields (gestaltMachineType, device-tree
model/compatible) cannot satisfy the 9.2.1 compatibility check. The check probes for
ROM environment characteristics that the 1998 v1.1 ROM lacks.

**Catch-22:** The 9.2.1 Installer app also refuses to run ("This program cannot run on
your computer") — even from Mac OS 9.0.4, which boots fine on this ROM. So we can't
use the normal install/upgrade path to get 9.2.1 onto the disk.

**Experiment assets:**
- Prefs: `/tmp/exp_921_on_11.prefs` (HD boot from pathB_upgrade.dsk, 1.1 ROM)
- Disk: `/Users/Shared/macemu/pathB_upgrade.dsk` (9.0.4 installed, 9.2.1 System Folder
  contents swapped in)
- Logs: `/tmp/exp_pathB_mach406.log`, `/tmp/jit_diag.20260609-202443.52394.log`

### Wrong-branch patch debugging (meta-lesson)

The initial gestaltMachineType patch was placed in the **GOSSAMER branch** (line ~1885) of
the UniversalInfo patching code, because CLAUDE.md labeled the 1.1 ROM as "OldWorld."
The diagnostic `fprintf` confirming the patch never appeared. Adding a ROM-type diagnostic
print (`[ROMPATCH] ROM type detected: 5 (NewWorld)`) revealed the ROM routes through the
NEWWORLD branch instead. Moving the patch to the NEWWORLD branch (line ~1853) made it fire.

**Lesson:** Don't trust documentation labels about ROM types — verify with `rom_detect_type()`
output. A "Mac OS ROM 1.1" file is not the same thing as a "1.1 OldWorld ROM."

### Real upgrade cards did NOT bypass OS version gates

Research into real G3→G4 upgrade cards (Sonnet, NewerTech, XLR8) shows they **never** shipped
OS version enablers. Their software extensions handled CPU-specific concerns only: AltiVec
activation, clock speed reporting, NVRAM patches. The Mac OS 9.2 installer rejected machines
based on **ROM type/machine model**, not CPU. A G4 in a beige G3 didn't change the machine's
ROM type or Gestalt ID.

The tool that actually enabled Mac OS 9.2 on unsupported (OldWorld) machines was **OS 9 Helper**
— a community hack that patched **3 resources in the installer** to remove the machine-model
whitelist. It was pure installer surgery, independent of any hardware upgrade. Once installed,
9.2.x ran fine on OldWorld hardware.

**Max OS by ROM type:**
- OldWorld (beige G3, 9500, etc.): officially Mac OS 9.1; 9.2.x via OS 9 Helper hack
- NewWorld (B&W G3, iMac G3, etc.): Mac OS 9.2.2 natively, no hack needed

The B&W G3 (PowerMac1,1) runs 9.2.2 natively — it's NewWorld. No upgrade card needed.

**Impact:** The "upgrade card" metaphor is doubly broken: (1) our ROM is already NewWorld,
and (2) real upgrade cards didn't solve OS compatibility anyway. The real parallel is
OS 9 Helper's installer-patch approach — bypass the check, not fake the identity.

### Two barriers, not one

The 9.2.1 compatibility check has TWO separate gates:
1. **The installer** ("This program cannot run on your computer") — refuses to launch even
   from Mac OS 9.0.4 on the 1.1 ROM. This is a CFM/launch-time check in the installer app.
2. **The System file** ("This startup disk will not work on this Macintosh model") — rejects
   the machine at early boot when we manually copy 9.2.1 System Folder contents. This is
   68k code in the System file, running after Toolbox init.

OS 9 Helper patched the **installer** gate (gate 1). But we bypassed gate 1 by manually
copying the System Folder. We're stuck at gate 2 — the System file's own boot-time check,
which is a different code path from the installer check.
