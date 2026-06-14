> **ARCHIVED 2026-06-14** — Moved to archive during the Reku pass (2026-06).
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone partial — M12 Wave0+Wave1 landed, frontier captured; superseded by M13
>

# M12 — Display pixels: get Mac OS to write to the framebuffer aperture

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Get Mac OS 9.x running in the NewWorld fidelity profile to write at least one non-zero
pixel to the framebuffer aperture at 0x81000000, producing `[FB-DIRTY] non_zero_pixels=N` (N > 0)
on exit.

**Architecture:** Three sequential concerns — (1) characterize the post-EXT frontier crash that
kills the boot at ~0.3s; (2) use the QEMU rig to map how many walls separate the current frontier
from display manager init; (3) deliver a working ndrv to the OF display node so the Display Manager
can write pixels once the boot reaches that stage. Implementation tasks are provisional and will be
scoped by Task-0 answers.

**Tech Stack:** C/C++ (SheepShaver), PPC static RE (capstone, lldb), QEMU mac99 differential rig,
ring-walk.py, ss-slot-boot.sh.

---

## Status: PARTIAL COMPLETE / CLOSED (2026-06-13 session 7)

**Tasks A (Wave0) and B (VideoDriverStub) COMPLETE. Task C (pixel gate) FAIL. Task D (Wave1) COMPLETE.**
- Wave0 (`ddbd8d79`): NW lowmem 1MB→32MB — fixes ea=0x010020c8.
- Wave1 (`348544cd`): 0xFF000000–0xFFFFFFFF anonymous zero — fixes ea=0xFFFFEFD0.
- `irq_fired=0`, `[FB-DIRTY]=0`. Blocker: CGRP routes EXT → ROM+0xED08 which hits A-trap 0xA9A8
  before the Mac OS Trap Dispatch Table is loaded. **M13 input.**
- All harness/machine/e2e gates pass (353/353, machine ALL PASS, e2e-test 122, make e2e PASS).

> **Rev 2:** post-red-team (PROCESS + TECHNICAL). BINDING amendments in §amendments below.
> **Process:** `docs/MILESTONE-WORKFLOW.md`. Read `docs/AGENT-CONTEXT.md` at task start.
> Predecessor: M11 COMPLETE (2026-06-13). Branch: `macos-arm64`.

---

## Authoritative inputs

| Doc | Section used |
|-----|-------------|
| `docs/HANDOFF.md` | §M11 acceptance, frontier state, non-determinism caveat |
| `docs/AGENT-CONTEXT.md` | Constants, boot recipes, instrument caveats, gate tiers |
| `docs/planning/machine/FRAMEBUFFER-RECON.md` | §2.3 (cofb ndrv in parcels), §4 (node spec), §5 (options + tripwires) |
| `docs/planning/superpowers/plans/2026-06-13-m11-framebuffer.md` | Task addendum (T-F1/T-F2/T-F5 results, accepted contracts) |
| `SheepShaver/src/name_registry.cpp` | DoPatchNameRegistry, VideoDriverStub injection |
| `SheepShaver/src/include/rom_decode.hpp` | Parcel decode pipeline |
| `SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py` | QEMU differential rig (behavioral oracle) |
| `SheepShaver/tools/ring-walk.py` | Ring dump analysis |

---

## Codebase facts (re-verify before use)

- Aperture mapped at `fb_aperture_base = 0x81000000` (16 MB) under `ss_m11_fb && MachineProfileIsNewWorld()`.
  `[FB-DIRTY]` atexit scan already present in `main_unix.cpp`. [STATIC from M11]
- OF display node published with `address/width/height/linebytes/depth/compatible="cofb"` when
  `ss_m11_fb=true` (`name_registry.cpp:374-399`). The `screen` alias also published
  (`name_registry.cpp:404-408`). [STATIC]
- `DoPatchNameRegistry()` is triggered by `OP_NAME_REGISTRY` EMUL_OP (planted by `patch_68k()`).
  For the 9.0.1 parcels ROM, the pattern was found (relocated at 0x2fa, DISK-PATH-RECON §6) but
  **`OP_NAME_REGISTRY` firing on the newworld boot has never been confirmed.** [STATIC — re-verify]
- `VideoDriverStub` (PEF, `VideoDriverStub.i`) is injected unconditionally as
  `driver,AAPL,MacOS,PowerPC` at `name_registry.cpp:371`. This fires ONLY if `DoPatchNameRegistry`
  runs. If it does NOT run on newworld, the video node has no ndrv via this path. [STATIC]
  **`compatible="cofb"` and the VideoDriverStub COEXIST on the same node without structural
  conflict** — they are set in different blocks (line 371 unconditional; cofb inside ss_m11_fb
  guard). No cofb-vs-stub contention in the published registry. [TECHNICAL verified Rev 2]
- Prop parcels (including the `cofb` display ndrv at ROM file 0x213859) are currently discarded
  by `SheepShaver/src/include/rom_decode.hpp` (per FRAMEBUFFER-RECON §2.3 and §3.1). No parcel
  grafting exists. [STATIC — re-verify rom_decode.hpp for current code]
- Frontier state (HANDOFF 2026-06-13): NW all-on boot reaches `[DR68K] first instruction` and
  `EXT delivered #1`, then ~50/50 clean park vs SIGSEGV at variable `ea` (0x100000,
  0x55590000, etc.). Boot exits ~0.3s. No display-manager-adjacent code has ever been observed.
  [STATIC from HANDOFF]
- Gate tiers: per-commit = build + `SS_HARNESS_BATCH=1 make test-jit` (353/353) + `make -C src/machine test`.
  Per-task = + plain `make test-jit` + `make e2e-test`. Paravirtual gate: `make e2e` when change
  touches non-gated shared code. [STATIC from AGENT-CONTEXT]

---

## Task 0 — BINDING Recon

**Rule:** ALL blocking answers must be filled in the addendum below before ANY implementation
task begins. The addendum is the gate; a TODO entry in the addendum = blocked.

### Blocking-answer table

| Question | Blocks | Budget | Fallback |
|----------|--------|--------|---------|
| Q-M1: Frontier crash character | All impl tasks | 3 boot slots + ring-walk | If ea/PC pattern unclear: record partial, set M12 stop-rule to treat crash as the primary task |
| Q-M2: QEMU wall census | Task B scope | 2 QEMU sessions (50s each) | Use ROADMAP M11+ estimate ("~3-4 milestone-class efforts"); note uncertainty in task B |
| Q-M3: ndrv delivery path | Task B implementation | 1 static RE session on name_registry + rom_decode | If unclear: default to VideoDriverStub injection via OF tree (proven for paravirtual) |
| Q-M4: Does OP_NAME_REGISTRY fire on newworld? | Task B (name registry vs direct OF path) | `SS_EMULOP_COUNTS=1` one boot | If it doesn't fire: newworld uses OF device tree directly; name_registry path is moot |

### Q-M1: Frontier crash character — what is killing the boot at ~0.3s?

**Question:** What 68k PC (r24), PPC block PC, and ea appear at the crash? Is the crash address
display-adjacent (0x81000000 range) or in some other subsystem? Is the park side hitting a known
NK idle structure?

**Method:**
```bash
# Run 3 boot slots with ring + r24-ring capture, then ring-walk --r24-flow
SS_JIT_TRACE_RING=1 SS_DR_R24_RING=1 \
SheepShaver/tools/ss-slot-boot.sh --label m12-t0-crash-A --timeout 30 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M11_FB=1'

# For the crash case: ring-walk the crash
python3 SheepShaver/tools/ring-walk.py <rundir>/jit_diag.log \
    --r24-flow --window <last-200>:end

# For the park case: check nw-northstar verdict
cd SheepShaver && make nw-northstar
```

Run 3 slots. On any SIGSEGV run: dump the last 100 r24 transitions with `--r24-flow`. Record:
- Last 5 r24 values before crash
- PPC crash PC (from SIGSEGV handler output)
- ea value
- Whether ea is in 0x81000000 range (display), 0x68xxx000 range (NK structures), or elsewhere

**Tag:** [PROBE✓]. Record in addendum Q-M1.

**Budget:** 3 × 30s slot boots + 15 min ring analysis. Hard stop.

**Fallback:** If crash is non-deterministic (< 50% reproduction), record "crash not reliably reproducible, ea=variable" and proceed — the park side may be more productive for display work.

### Q-M2: QEMU wall census — how many walls between current frontier and display init?

**Question:** In a working QEMU mac99 9.0.1 boot, what does the guest do between EXT delivery and
the first pixel write to the VGA LFB? What MMIO addresses are touched? Roughly how many "walls"
(blocked exception handlers, missing device models) are between the current frontier and display init?

**Method:**
```bash
# Run the QEMU rig with a longer timeout; capture device-tree + monitor output
bash SheepShaver/tools/qemu-rig.sh --timeout 60

# Query display node properties (from T-F1, already known)
python3 SheepShaver/tools/qemu-mon.py --sock /tmp/qemu-rig-*/mon.sock \
    'info mtree' | grep -A2 81000000

# Set a watchpoint on the LFB base to catch first write (requires gdbstub):
# QEMU_EXTRA_ARGS="-gdb tcp::1234 -S" bash SheepShaver/tools/qemu-rig.sh --timeout 90
# Then: gdb -> target remote :1234 -> watch *(int*)0x81000000 -> c -> bt
```

If the QEMU gdb approach is too complex, an alternative: simply run the rig for 60s and record
how far the guest gets (does it reach Finder? System 9 splash? gray screen only?). This tells us
roughly how long the display path is.

**Answer format:** "QEMU mac99 9.0.1 reaches X at Ys; first pixel write observed/not observed; display init appears to need: [list]."

**Tag:** [PROBE✓]. Record in addendum Q-M2.

**Budget:** 2 QEMU sessions (60s each). Hard stop.

**Caveat (load-bearing):** QEMU's display model is not our machine model. Use it for "what does
Mac OS do on a working boot" not for "what addresses to implement." Never cite QEMU MMIO addresses
as reference values for our machine layer.

### Q-M3: ndrv delivery path — how should the cofb ndrv reach the display node?

**Question:** For the newworld fidelity profile, where does the Display Manager look for the
ndrv for our video node? Options are: (a) `driver,AAPL,MacOS,PowerPC` property on the OF device
tree node (the paravirtual path), (b) cofb ndrv grafted from parcels (prop parcel at ROM file
0x213859), (c) something else. What is viable given our current infrastructure?

**Method (static RE):**

1. Check if prop parcels are currently discarded:
   ```bash
   grep -n "prop_parcel\|cofb\|graft\|discard" SheepShaver/src/include/rom_decode.hpp | head -20
   grep -n "prop.*parcel\|ndrv.*graft" SheepShaver/src/rom_patches.cpp | head -10
   ```

2. Check if the OF device tree (the one Mac OS reads, not the name registry) has a path for
   injecting `driver,AAPL,MacOS,PowerPC`:
   ```bash
   grep -n "driver.*AAPL\|ndrv\|PowerPC.*prop" SheepShaver/src/rom_patches.cpp | head -20
   grep -n "driver.*AAPL\|video.*prop" SheepShaver/src/machine/core99.cpp | head -20
   ```

3. Determine if `DoPatchNameRegistry` runs on the newworld path (see Q-M4). If NOT, then
   `name_registry.cpp:371` (VideoDriverStub injection) is dead code for newworld — and we
   need a different injection path.

**Tag:** [STATIC]. Record in addendum Q-M3.

**Budget:** 1 static RE session (30 min). Hard stop.

### Q-M4: Does OP_NAME_REGISTRY fire on the newworld boot?

**Question:** Does the 68k EMUL_OP `OP_NAME_REGISTRY` actually execute during a 9.0.1 all-on
NewWorld boot? If not, the `DoPatchNameRegistry()` / `VideoDriverStub` path is dead for newworld.

**Method:**
```bash
# Run one all-on boot with EMULOP counting
SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M11_FB=1 SS_EMULOP_COUNTS=1 \
SheepShaver/tools/ss-slot-boot.sh --label m12-t0-emulop --timeout 60

# Check boot.log for OP_NAME_REGISTRY count
grep "OP_NAME_REGISTRY\|name_reg\|OP_NAME" <rundir>/boot.log
```

**Tag:** [PROBE✓]. Record in addendum Q-M4.

**Budget:** 1 boot slot (60s). Hard stop.

---

## Task 0 addendum (fill before starting Task A/B/C)

### Q-M1 result [PROBE✓]

3 boots run (`SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M11_FB=1 SS_JIT_TRACE_RING=1`, 30s each).

| Boot | EXIT | Crash | ea | dec_expiries |
|------|------|-------|----|-------------|
| 1 | 133 (SIGTRAP/SIGSEGV) | YES | **0x010020c8** | 7 (very early) |
| 2 | 1 (SIGTERM) | NO | — | 2020 (healthy) |
| 3 | 1 (SIGTERM) | NO | — | 22751 (healthy) |

- **Crash frequency:** 1/3 observed
- **ea decode:** 0x010020c8 = 16 MB + 0x20c8. Falls in the **unmapped gap** between the
  1 MB newworld lowmem window (0x0–0xFFFFF) and RAMBase. `r3 = 0x010020c8` at crash —
  the DR emulator is using r3 as an EA for a load/store.
- **68k PC at crash:** r24 = 0x5000eee0 (ROM+0xeee0), inside a tight loop ROM+0xed00–0xef00.
  The R24 ring also shows transient non-ROM values 0x20f00000 and 0x0000003f — these are
  data values in r24, not misrouted PCs.
- **PPC crash PC:** 0x12f646ef8 (JIT cache — compiled code for ROM+0xeee0 region)
- **SS_DR_R24_RING:** Available (1,259,519 transitions captured).
- **Crash is display-adjacent:** NO. ea=0x010020c8 is in the unmapped gap above lowmem;
  display aperture is 0x81000000–0x81FFFFFF.
- **Conclusion:** Intermittent (EXT-timing-driven). When the crash does not occur, the boot
  parks cleanly with dec_expiries≥2020 — real boot progress well past the 0.3s window.
  The crash is at ROM+0xeee0 reading from an unmapped gap; likely a pointer into RAM that
  the Trampoline/NK synthesis has not staged (see LEARNINGS 2026-06-12 DEC-storm pattern).

### Q-M2 result [PROBE✓]

QEMU rig ran 90s. **Boot reached Finder** (Mac OS 9.2.1 with 9.0.1 ROM). VRAM at 0x81000000
contains non-zero pixel data confirming Mac OS wrote pixels.

**Boot stages between EXT delivery and display init (observed in QEMU):**
1. NK booting — 68k world not yet started (~8–10s)
2. Ticks counter (0x168) goes non-zero → 68k started, time-manager running
3. `*(0x64)` transitions to RAM-resident handler → System fully loaded
4. VBL chain (0x6e4) populated → Display Manager initialized, VBL tasks running
5. VRAM non-zero → pixels written

**Key finding:** Mac OS uses the OFW display descriptor (base address + rowBytes + bounds from
the device tree) → NK reads it → sets `screenBits.baseAddr` → QuickDraw inits → pixel writes
go directly to VRAM. **No ATI/Rage128 MMIO wall.** Mac OS wrote pixels to a generic VGA LFB
via the OFW descriptor alone.

**Rough wall estimate: ~2 walls** (not 5+):
1. OFW/NK framebuffer handoff: NK must find valid display descriptor in our synthesized tree
2. Cuda/VIA ticks: `*(0x168)` must be non-zero before System inits QuickDraw (clean boots
   already show dec_expiries≥2020, suggesting tick machinery works)

### Q-M3 result [STATIC]

- **Prop parcels discarded:** YES — `rom_decode.hpp` only processes `parcel_type == 'rom '`;
  all other parcel types silently skipped. No grafting path.
- **Core99.cpp:** Does NOT exist at `SheepShaver/src/machine/core99.cpp`. No OF tree direct
  injection path via a machine-model layer.
- **MachineProfileIsNewWorld gating in name_registry:** NOT GATED. The video node creation
  and VideoDriverStub injection at line 371 run unconditionally — no profile guard.
- **cofb + VideoDriverStub coexistence:** They COEXIST without conflict. Both properties are
  set on the same video node. The M11 comment ("blocks OS overlay") refers to NOT adding a
  SECOND `driver,AAPL,MacOS,PowerPC` inside the ss_m11_fb block — the one at line 371 is
  already present. (Rev 2 A1 amendment confirms.)
- **NativeOp/NATIVE_VIDEO_DO_DRIVER_IO:** LIKELY-SAFE — profile-agnostic thunk.
- **Recommended path:** B1 (name registry). The VideoDriverStub is already injected at line 371.
  The question is whether the Display Manager calls it (depends on Q-M4 + display init chain).

### Q-M4 result [PROBE✓]

- **SS_EMULOP_COUNTS:** Implemented but counts PPC emulops only; used `SS_PROBE_68K=0x500002fa`
  instead (probe at the OP_NAME_REGISTRY patch site).
- **OP_NAME_REGISTRY patch site:** 0x500002fa (relocated from expected range, found via
  lenient whole-image fallback; confirmed by SS_ROM_PATCH_TRACE).
- **OP_NAME_REGISTRY fires:** **YES** — `SS_PROBE_68K=0x500002fa` fired `match=1/8`.
  Immediately after the probe: `[M11-FB] name registry: video node address=0x81000000 640x480x32`.
- **DoPatchNameRegistry runs:** CONFIRMED. The video node is registered at 0x81000000 640×480×32
  on the newworld all-on boot. VideoDriverStub is live in the name registry.
- **Timing:** Fires early in the 68k boot sequence — after DR emulator entry and DEC delivered #3,
  **before** the first EXT interrupt.
- **Conclusion:** Path B1 is live. Task B (ndrv injection) is effectively **already implemented**
  for the basic case. The Display Manager will find the VideoDriverStub in the name registry.
  The remaining question is whether the Display Manager calls `kOpenCommand` → `VideoOpen`, and
  whether `screen_base` (the paravirtual buffer, redirected to the aperture in M11) equals 0x81000000.

---

## Implementation tasks

> **Task-0 addendum complete (2026-06-13).** Blocking-answer table filled. Tasks scoped
> based on findings. Summary: Task B (ndrv) is effectively already done — VideoDriverStub
> is registered via OP_NAME_REGISTRY + DoPatchNameRegistry, confirmed to fire on newworld.
> Task A (crash fix) is the primary remaining work. See §amendments A2–A3 for preconditions.

### Task A — Frontier crash fix (if Q-M1 shows a fixable crash)

**Precondition:** Q-M1 complete; crash characterized; crash is not in display-adjacent code.

**If the crash is in display code:** that means the display path is actually reachable and Task B
should focus on the ndrv. Document the crash PC and ea as Task B context.

**If the crash is in NK/68k bootstrap code (most likely):** trace the root cause via the known
pattern: "check which Trampoline-era global is zero/uninitialized" (LEARNINGS 2026-06-12, DEC storm
lesson). Preferred fix: `SS_SEED_MEM` no-recompile experiment to confirm hypothesis, then
stage the missing global in `sheepshaver_glue.cpp` trampoline synthesis.

**Gate:** `SS_M12_FIX_A=1` (default OFF). Change inside `MachineProfileIsNewWorld()` guard.

**Acceptance criterion:** 3/3 consecutive 60s all-on boots reach `[DR68K] first instruction`
without SIGSEGV; `dec_expiries > 5` (boot is not parking immediately).

**Files:** `SheepShaver/src/Unix/sheepshaver_glue.cpp` (trampoline synthesis), possibly
`SheepShaver/src/rom_patches.cpp` (if a staging hook is needed).

**Stop rule:** if the crash root cause requires more than ONE new global staged OR a new device
model, this is a separate milestone. Declare M12 blocked, file M12a, and re-scope.

### Task B — ndrv delivery verification (Q-M4: already live; verify DM calls it)

**Precondition (A2):** Q-M1 addendum filled AND Q-M3 filled AND Q-M4 filled. (All filled 2026-06-13.)
Task A complete (A3 addendum entry present).

**Q-M4 confirmed Path B1 (OP_NAME_REGISTRY fires).** VideoDriverStub is already live.
No code changes needed to the ndrv injection path.

**Verification steps only:**

1. Confirm `screen_base` equals `fb_aperture_base` (0x81000000) in the newworld+ss_m11_fb
   profile. In M11, `the_buffer` was redirected to `fb_host_ptr` (aperture host pointer);
   `screen_base = Host2MacAddr(the_buffer)` → should be 0x81000000.
   ```bash
   grep -n "the_buffer.*fb_host\|screen_base\|Host2MacAddr.*the_buffer\|fb_host_ptr" \
       SheepShaver/src/SDL/video_sdl3.cpp SheepShaver/src/Unix/main_unix.cpp | head -20
   ```

2. Verify no code path resets `screen_base` to the old paravirtual pool address after the
   aperture redirect (the aperture is a different host pointer from the 80MB reserved pool).

3. Add a log line in `VideoOpen` (video.cpp) that prints the recorded `saveBaseAddr`:
   `[M12-VIDEO] VideoOpen: saveBaseAddr=0x%08x` — this confirms the DM called kOpenCommand
   and the stub recorded the aperture address. Gate: `SS_M12_NDRV=1`.

If screen_base ≠ 0x81000000 in the newworld profile, this IS code work: redirect the
screen_base assignment inside the ss_m11_fb guard in `video_sdl3.cpp`.

**Gate:** `SS_M12_NDRV=1` (default OFF, depends on Task A).

**Acceptance criterion:** With `SS_M12_FIX_A=1 SS_M12_NDRV=1`, a 60s all-on boot shows a
`[M11-FB] name registry: video node` line in boot.log (or equivalent OF-tree injection confirmed
via `SS_ROM_PATCH_TRACE=1`). If the EMULOP path fires, this is automatic. If the OF-tree path
is needed, an explicit log line from the injection hook.

**Files:** `SheepShaver/src/name_registry.cpp` (Path B1) or
`SheepShaver/src/machine/core99.cpp` + `SheepShaver/src/Unix/sheepshaver_glue.cpp` (Path B2).

**Stop rule:** If the Display Manager requires a device service the ndrv stub doesn't provide
(e.g., a real MMIO call to an unknown address), record the specific failure and file it as M13.
The milestone FAILS if the ndrv can't respond to at least `kOpenCommand` → `VideoOpen`.

### Task C — Pixel gate verification

**Precondition:** Tasks A + B complete. `[FB-DIRTY]=0` must still be the baseline on a 30s boot.

**Method:**
```bash
# Run acceptance boot with extended timeout (180s for display init to complete)
SS_M12_FIX_A=1 SS_M12_NDRV=1 SS_M11_FB=1 \
SheepShaver/tools/ss-slot-boot.sh --label m12-accept --timeout 180

# Check [FB-DIRTY] scan
grep "FB-DIRTY" <rundir>/boot.log
```

**Gate:** `SS_M12_PIXELS=1` = gate combining all three conditions: `SS_M11_FB=1 SS_M12_FIX_A=1 SS_M12_NDRV=1`.

**Acceptance criterion (PASS):** `[FB-DIRTY] non_zero_pixels=N` with N > 0 within 180s.

**Diagnostic (not a gate):** Visual gray screen or happy-Mac in VNC/SDL window.

**FAIL condition:** `[FB-DIRTY] non_zero_pixels=0` after 180s → milestone fails; record
boot frontier at time of failure as M13 input.

---

## Acceptance

```bash
# 1. Harness (must stay 353/353)
cd SheepShaver && SS_HARNESS_BATCH=1 make test-jit

# 2. Machine unit tests
make -C src/machine test  # ALL PASS

# 3. Pixel gate (PASS criterion)
SS_M12_PIXELS=1 \
SheepShaver/tools/ss-slot-boot.sh --label m12-final --timeout 180
# → grep "FB-DIRTY.*non_zero_pixels=[^0]" in boot.log

# 4. NewWorld observe (report-only)
cd SheepShaver && make nw-northstar
# → record [NW-PROG verdict] — REGRESSED(...) = real problem

# 5. Paravirtual regression (REQUIRED: Tasks A/B touch trampoline/name-registry — non-gated risk)
cd SheepShaver && make e2e
```

**PASS:** harness 353/353, machine tests ALL PASS, `[FB-DIRTY] non_zero_pixels > 0`, `make e2e` green.
**FAIL:** pixel gate missing → do not close; record frontier as M13 input.

---

## Stop rules

1. **Crash root cause requires > 1 global staged or a new device model** → STOP Task A, declare
   M12 blocked, file M12a as a separate milestone.
2. **Display Manager demands a service the ndrv stub can't provide** (MMIO call, mode list, etc.)
   → STOP Task B, record specific failure, file as M13. Milestone does NOT close.
3. **Second falsification of the same contract** (per MILESTONE-WORKFLOW §7) → STOP, re-scope, re-plan.
4. **Tempting wrong fix (HLE shortcut):** do NOT host-side fake a pixel write to claim the gate.
   The gate requires genuine Mac OS execution writing to 0x81000000. A synthetic host-side
   `memset(fb_host_ptr, 0x88, 1)` would pass the grep but violates the milestone's purpose.

---

## Self-review / tensions flagged

- **The frontier crash may be display-unrelated.** If Q-M1 shows the crash is in NK/68k init
  code (not display), Task A becomes a prerequisite that has nothing to do with the display path.
  M12 may effectively become "fix the next boot wall, then M13 is display init." The PASS
  criterion still requires pixels — if the wall is too deep, the milestone may need to be split.
- **VideoDriverStub for newworld.** The stub was built for the paravirtual profile (it calls
  `NativeOp → VideoDoDriverIO`). Whether `NativeOp` is valid in the newworld execution
  environment (post-M10 CGRP delivery regime) is unverified. Q-M4 + Task B must verify this.
  If `NativeOp` is not safe in newworld, a different ndrv delivery path (parcels graft) may be
  needed — that is a larger scope change.
- **No red-team round run yet.** This is Rev 1. The PROCESS + TECHNICAL red-team review must
  run before any implementation task starts (per MILESTONE-WORKFLOW §2).

---

## Red-team record

**Run:** 2026-06-13, two parallel reviewers (PROCESS + TECHNICAL).

**PROCESS findings (CRITICAL→resolved):**
- [CRITICAL] Task B precondition allowed "crash confirmed display-adjacent" as an escape hatch
  without a concrete addendum artifact. Fixed in §amendments: Task B now requires an explicit
  addendum entry in Q-M1 stating the crash PC/ea or "no reproducible crash."
- [CRITICAL] Pixel gate grep (`[^0]`) was vacuously passable and couldn't distinguish genuine
  Mac OS writes. Fixed: acceptance step #3 now requires boot duration >10s AND `[DR68K] first
  instruction` present in the log before the FB-DIRTY check.
- [CRITICAL] Task A had no completion artifact — only prose acceptance criteria. Fixed: Task A
  now requires a Task A addendum entry with literal `pass=3/3 SIGSEGV=0 dec_expiries=N`.
- [CRITICAL] Q-M3 fallback was an implicit implementation decision. Fixed: fallback now
  explicitly states "scope Task B to Path B1 only" and marks Q-M3 as "scope-reduced, not
  answered."
- [MODERATE] SS_M12_PIXELS prose alias → spelled out as three explicit env vars in acceptance.
- [MODERATE] Stop rule 1 gameable by counting struct fields as one. Fixed: count logical
  missing-initialization units (each zero/unset host-staged value = one unit).
- [MODERATE] Task B acceptance log string not verified in source → grep step added to Task B.
- [MODERATE] No per-task boot attempt budgets → added to Tasks A and B.
- [LOW] Q-M2 easy-path format was yes/no → minimum answer format added.
- [LOW] No DOCS task → Task D added.

**TECHNICAL findings (resolved):**
- [WRONG] Plan's Task B Path B1 concern ("cofb blocks VideoDriverStub") was a misread of the
  M11 comment. The comment says "don't add a SECOND `driver,AAPL,MacOS,PowerPC` inside the
  ss_m11_fb block." The unconditional injection at line 371 and `compatible="cofb"` coexist
  without conflict. Fixed: codebase facts clarified; Task B Path B1 concern removed.
- [VERIFIED] `SS_EMULOP_COUNTS=1` is a real implemented env var (kpx_cpu/sheepshaver_glue.cpp:719–741).
- [VERIFIED] Prop parcels discarded by rom_decode.hpp (only `'rom '` type processed).
- [VERIFIED] Frontier state described accurately.
- [UNVERIFIABLE] `SS_DR_R24_RING` env var existence not confirmed → Task 0 Q-M1 method now
  includes a static check step before running.

---

## §amendments (BINDING, Rev 2)

### A1 — Codebase fact correction: cofb + VideoDriverStub coexistence

**Correction:** The plan's Task B/Path B1 concern ("verify cofb does not block VideoDriverStub")
was based on a misread of the M11 comment at `name_registry.cpp:376–378`. The comment says
"don't add a SECOND `driver,AAPL,MacOS,PowerPC` inside the ss_m11_fb block" — not that
`compatible="cofb"` blocks the unconditional injection at line 371. The video node has
BOTH the VideoDriverStub driver property AND `compatible="cofb"` simultaneously. No structural
conflict exists in the published name registry. Remove the Path B1 cofb-conflict investigation.

### A2 — Task B precondition made concrete

**Replaces Task B's precondition prose.** Task B may start ONLY when:
- Q-M1 addendum is filled with either: (a) `crash_pc=0xNNNNNNNN ea=0xNNNNNNNN` OR
  (b) `crash_not_reproducible: confirmed via 3 boots` AND
- Q-M3 addendum is filled AND Q-M4 addendum is filled.

"Crash confirmed display-adjacent" is NOT a valid Task B precondition unless Q-M1 also records
the specific ea (in the 0x81000000–0x81FFFFFF range) AND a DR68K r24 value that is inside
known Mac OS display-manager code (0x50XXXXXX ROM range or a loaded ndrv).

### A3 — Task A completion artifact required

**Adds to Task A acceptance.** Before starting Task B, fill in the plan addendum:
```
Task A result: pass=N/3  SIGSEGV=M/3  dec_expiries_median=K  date=YYYY-MM-DD
```
No implementation task may start without this literal entry.

### A4 — Acceptance step #3 strengthened

**Replaces acceptance step #3.** The pixel gate command and pass criteria are:

```bash
# Run with all-on gates
SS_M11_FB=1 SS_M12_FIX_A=1 SS_M12_NDRV=1 \
SheepShaver/tools/ss-slot-boot.sh --label m12-final --timeout 180

# PASS requires ALL of:
# 1. Boot ran for > 10s (not an immediate crash)
grep -c "." <rundir>/boot.log | awk '$1 > 50'   # proxy for non-trivial run
# 2. 68k world started (durable marker)
grep -q "\[DR68K\] first instruction" <rundir>/boot.log
# 3. Pixels written
grep -qE "\[FB-DIRTY\] non_zero_pixels=[1-9][0-9]*" <rundir>/boot.log
```
**All three checks must pass.** The gate does NOT pass if only the pixel grep matches.

### A5 — Q-M3 fallback made explicit

**Replaces Q-M3 fallback.** If Q-M3 cannot determine the delivery path within budget, mark
Q-M3 addendum as "UNCLEAR — scope reduced." Task B is then scoped to Path B1 ONLY
(verify existing VideoDriverStub injection runs on newworld; no new injection path). This is
a scope reduction, not a resolution. Document what was unclear.

### A6 — SS_DR_R24_RING verification step added to Q-M1

**Adds to Q-M1 method, before running boots:**
```bash
# Confirm SS_DR_R24_RING is implemented before relying on it
grep -rn "DR_R24_RING\|dr_r24_ring" SheepShaver/src/ --include="*.cpp" --include="*.h"
```
If not found: use `SS_JIT_TRACE_RING=1` only and drop the `--r24-flow` flag from ring-walk
(fall back to `--find-pc` with the crash PC from the SIGSEGV output).

### A7 — Per-task boot attempt budget

- **Task A:** max 6 boot slots total (investigation + acceptance). If 3/3 clean is not achieved
  in 6 slots, invoke stop rule 1.
- **Task B:** max 4 boot slots total (verification + acceptance). If the ndrv log signal is
  not observed in 4 slots, invoke stop rule 2.

### A8 — Task D: DOCS close-out added

**New task (must run after acceptance, before milestone close):**

- [ ] Add CHANGELOG entry (component `[SheepShaver/machine]`, acceptance numbers, gate name)
- [ ] Update DIAGNOSTICS.md with `SS_M12_FIX_A` and `SS_M12_NDRV` knob entries
- [ ] Update ROADMAP.md M12 row: status → COMPLETE, acceptance summary
- [ ] Update AGENT-CONTEXT.md frontier block
- [ ] Update HANDOFF.md current-state table
- [ ] Grep for stale "M12 next" or "pixel gate deferred to M12" claims; update or archive
- [ ] Add LEARNINGS entry for any non-obvious finding (especially frontier crash root cause)

### A9 — Q-M2 minimum answer format

**Replaces the Q-M2 "if too complex, skip to simple alternative" prose.** The simple
alternative MUST produce: "QEMU mac99 9.0.1 reaches [specific observable state, e.g. gray
screen / happy-Mac / Finder] at approximately Ns; [count]-or-more boot stages observed
between `[DR68K]`-equivalent and display init." A yes/no on Finder reach is not acceptable.

### A10 — Stop rule 1 clarified

**Replaces stop rule 1 wording.** A "missing initialization unit" is defined as: each
distinct host-side value that must be staged (written into guest memory or a kernel structure)
to allow the guest to proceed past a wall. A single struct that holds two independently-necessary
values counts as TWO units. If Task A requires staging more than one such unit, stop rule 1 fires.
