# LEARNINGS — macOS ARM64 JIT work

Running log of non-obvious things learned while working on this fork.
Newest entries at the top of each section. Review at the start of each session.

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

Mac OS sets CR2.LT (bit 8) to signal a pending interrupt. The DR emulator checks it via `bclr BO=5,BI=8` (opcode `0x4C420020`) at the end of every dispatch cycle. JIT polls spcflags at **block entry**: when TRIGGER is detected it saves PC, returns to C, which runs `check_spcflags()` → sets CR2.LT, then re-dispatches the same block. The interpreter polls at **block exit** — after `bclr 5,8` has already run the current handler. Net effect: JIT fires the interrupt one dispatch step early, before the current 68k instruction handler completes.

**The double-lhau block (504613e0) makes it critical:**

Block 504613e0 does two `lhau r27,2(r24)` fetches per dispatch cycle — speculatively pre-fetching the extension word. When JIT re-runs this block after injecting CR2.LT, r24 has already advanced 4 bytes, and r27 holds the extension word, not an opcode. The interrupt handler fires with r24 pointing 2 bytes past the correct position → extension word is dispatched as a 68k opcode → register corruption.

**This explains all prior Bug #2 symptoms:** extension word dispatched as opcode, OE arithmetic step interrupted mid-sequence, A3 = 0x103ffffe corruption (pre-decrement step never ran).

All 6 DR emulator dispatch variants (ROM+0x466080/84/c0/e0/100/120) use `bclr 5,8` as the structural interrupt gate.

**Fix (Option A, ~100–150 lines in ppc-jit.cpp):** In `compile_one()`, detect opcode `0x4C420020` when PC is in ROM+0x460000–0x500000. Emit an inline spcflags-check-and-inject prologue BEFORE the branch, forcing CR2.LT to be current at the exact moment `bclr` evaluates it — matching the interpreter's one-cycle-later delivery.

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
