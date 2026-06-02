# Implementation Backlog — JIT & Emulation Improvements

Consolidated, implementation-ready work items from the 2026-06-02 research effort
(Dolphin deep-dives + ARM64 dynarec / Mac video landscape surveys). Each item is written
so an agent can pick it up cold: what to change, where, how to verify, and what to read first.

**Source documents** (read the relevant one before starting an item):
- `../EMULATOR-RESEARCH-LEADS.md` — verdicts, roadmap, landscape synthesis
- `lead-1` … `lead-7-*.md` — Dolphin deep-dives with verified code
- `landscape-2-arm64-dynarec-projects.md` — MAME / oaknut / dynarmic / Box64 / FEX / Rosetta
- `landscape-3-classic-mac-video-accel.md` — video acceleration architecture comparison

**Verification baseline for ALL JIT items:** `cd SheepShaver && make test-opcodes` must stay
at score 100 (209 vectors, each run in interpreter AND JIT mode with REGDUMPs diffed).
For boot-level changes also run `make run-jit` and confirm boot to desktop.

**⚠ Coordination note (as of 2026-06-02):** another agent has uncommitted work in
`ppc-jit.cpp` (OE-form inlining, chain-site, epilogue changes), `ppc-cpu.cpp`, `ppc-jit.h`,
and `jit-test/run.sh`. Before editing those files, check `git status` / `git diff` and
rebase these item descriptions onto whatever has landed. Line numbers below are from the
working tree on 2026-06-02 and will drift.

---

## Tier A — Correctness fixes (small, do first)

### A1. Fix `adde` carry-out (VERIFIED BUG)

- **File:** `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`, case 138 (~line 1430)
- **Bug:** carry-in is added with a non-flag-setting `ADD`; `emit_write_xer_ca_from_carry()`
  then reads the C flag left over from the earlier `ADDS`. When adding CA wraps the result
  (e.g. rA+rB=0xFFFFFFFF, CA=1 → result 0), recorded CA=0 but the architecturally correct
  CA=1. There is also a dead `MRS NZCV` into RTMP2 that is never used.
- **Fix (designed, ready to apply):**
  ```c
  case 138: /* adde rD,rA,rB (rD = rA + rB + CA) */
      /* Materialize CA_in into the host C flag, then one ADCS computes
       * rA + rB + CA with the correct carry-out. */
      emit_read_xer_ca(RTMP2);
      emit32(0x71000400 | (RTMP2 << 5) | 0x1F); /* CMP W(RTMP2), #1 → C = CA_in */
      emit_load_gpr(RTMP0, ra);
      emit_load_gpr(RTMP1, rb);
      emit32(0x3A000000 | (RTMP1 << 16) | (RTMP0 << 5) | RTMP0); /* ADCS rA+rB+C */
      emit_store_gpr(RTMP0, rd);
      emit_write_xer_ca_from_carry();
      if (op & 1) lazy_update_cr0(RTMP0);
      return true;
  ```
  Encodings: `CMP Wn,#1` = `SUBS WZR,Wn,#1` = `0x71000400 | (Rn<<5) | 0x1F`;
  `ADCS Wd,Wn,Wm` = `0x3A000000 | (Rm<<16) | (Rn<<5) | Rd`.
- **Test vector to add** (`SheepShaver/jit-test/run.sh`, carry-wrap edge case):
  ```bash
  # adde carry-out edge: CA=1 and rA+rB=0xFFFFFFFF → result 0, CA_out must be 1
  # li r3,-1; addic r0,r3,1 (CA=1, r0=0); li r4,0; adde r5,r4,r3
  T_adde_carry_wrap="3860FFFF 30030001 38800000 7CA41914"
  TEST_ORDER+=(adde_carry_wrap)
  ```
- **Verify:** new vector FAILS (JIT≠interpreter) before fix, passes after; all 209+ still pass.
- **Detail:** `lead-3-lazy-carry.md` §"pre-existing adde bug"

### A2. Fix `subfe` carry-out (same bug class as A1)

- **File:** same, case 136 (~line 1358)
- **Bug:** identical pattern — `ADDS ~rA+rB` then non-flag `ADD` of CA drops the second carry.
- **Fix:** same shape as A1 (CMP to set C, then `MVN` + `ADCS ~rA+rB+C`). The `MVN`
  (`0x2A2003E0 | (Rm<<16) | Rd`) does not affect flags, so emit it between the CMP and ADCS.
- **Test vector:**
  ```bash
  # subfe carry-out edge: CA=1 and ~rA+rB=0xFFFFFFFF (rA==rB) → result 0, CA_out must be 1
  # li r3,-1; addic r0,r3,1 (CA=1); lis r4,0x1234; ori r4,r4,0x5678; subfe r5,r4,r4
  T_subfe_carry_wrap="3860FFFF 30030001 3C801234 60845678 7CA42110"
  TEST_ORDER+=(subfe_carry_wrap)
  ```
- **Note:** `addme` (234) and `subfme` (232) already use the correct 64-bit-sum technique;
  `addze` (202) / `subfze` (200) do a single ADDS so they are correct. Only 136/138 are broken.

### A3. Stop silently mis-executing `mullwo` (VERIFIED BUG)

- **File:** same, ~line 1068: `case 715: /* mullw (with OE bit) */` aliased onto `case 235`
- **Bug:** `mullwo` compiles to a plain `MUL` that never sets XER OV/SO — silent wrong results.
- **Fix:** delete the `case 715:` line (and add a comment). Blocks containing `mullwo` then
  fall back to the interpreter, same policy as `divwo`/`divwuo`.
- **Verify:** harness stays at 100. Optionally add a `mullwo` overflow vector
  (e.g. `0x8000 * 0x10000`) and confirm JIT and interpreter agree (both via interpreter
  fallback after the fix).
- **Detail:** `lead-4-oe-form-punt.md` §"latent bug"

### A4. Instrument chain-site pool exhaustion

- **File:** same, `record_chain_site()` (~line 176): `if (chain_site_pool_next >= JIT_CHAIN_SITE_POOL) return;`
- **Problem:** when the 16384-entry pool fills, chain sites are silently dropped — runtime
  block chaining silently degrades on long runs. Never reported anywhere.
- **Fix (minimal):** add a counter (`jit_chain_sites_dropped`) incremented on that early
  return, reported wherever the existing JIT counters are printed (see `jit_report_misses()` /
  the periodic block-count report). Decision about growing the pool comes later, with data.
- **Verify:** build + boot; counter visible in report output.
- **Detail:** `lead-7-dispatch-linking.md` §"chain design comparison"

### A5. Fix stale documentation constants

- **File:** `CLAUDE.md` (repo root — note: gitignored, local-only) and any docs that repeat it
- Block cache is **32768 buckets / 65536 pool** (`JIT_BC_BUCKETS`/`JIT_BC_POOL`,
  ppc-jit.cpp:52-54), not 8192.
- XER byte offsets are **SO=1028, CA=1030** (`PPCR_XER_SO`/`PPCR_XER_CA`, ppc-jit.cpp:281-283),
  not 900/902.

---

## Tier B — Cheap performance/robustness wins

### B1. `emit_update_cr0` cleanup (~18 → ~8 instructions)

- **File:** ppc-jit.cpp, `emit_update_cr0()` (~line 538)
- **What:** replace the branchy/packed CR0 update with CSET (LT/GT/EQ from NZCV after a
  `CMP result, #0`) + BFI into the packed CR word + OR-in of XER.SO.
- **Why:** record-form (`Rc=1`) ops are everywhere; this path is emitted by nearly every
  integer op via `lazy_update_cr0`.
- **Verify:** harness 100 (CR0 vectors exist); boot to desktop.
- **Detail:** `lead-1-cr-64bit-representation.md` §"Tier 1 recommendation"

### B2. Port a `LogicalImm` encoder; use immediates in `rlwinm`/`rlwimi`

- **What:** port Dolphin's dependency-free ARM64 bitmask-immediate encoder
  (`Arm64Emitter.h:525-601`, GPLv2+ → compatible) or oaknut's MIT equivalent, re-typed to
  our `int` register convention. Then rewrite the `rlwinm`/`rlwimi` mask paths
  (ppc-jit.cpp:~2154-2197) to emit `AND Wd,Wn,#imm` instead of materialize-mask + reg-reg AND.
- **Why:** PPC rotate-mask immediates are always contiguous-bit-run masks — exactly what
  ARM64 logical immediates encode. Saves 2-5 instructions on one of the most common PPC idioms.
- **Verify:** harness 100 (rlwinm/rlwimi vectors exist: `rlwimi_insert` etc.); boot.
- **Detail:** `lead-5-arm64-emitter.md` §"targeted crib"

### B3. Hoist W^X toggle out of `patch_chain_sites()` loop

- **File:** ppc-jit.cpp, `patch_chain_sites()` (~line 168-190)
- **What:** the loop currently brackets each patched word with
  `jit_cache_begin_write()`/`jit_cache_end_write()`. Hoist one bracket around the whole loop
  (model: `invalidate_range` at ~3783/3799 already does this correctly).
- **⚠ Invariant:** `sys_icache_invalidate` must still cover every patched address — pass the
  full range to one invalidate call, or invalidate per-word but toggle once.
- **Verify:** harness 100; boot; confirm chained execution still works (JIT block counters).
- **Detail:** `lead-2-wx-nesting-counter.md` §"Option A"

### B4. Debug-gated BRK tripwire on destroyed blocks

- **File:** ppc-jit.cpp, block invalidation paths (~line 179, ~3801 — where `code` is nulled)
- **What:** under `#ifdef JIT_DEBUG` (or an env var), overwrite the first word of an
  invalidated block's code with `BRK #0x123` (0xD4202460 + imm) instead of just nulling the
  cache entry. Any stale inbound chain then traps loudly instead of executing garbage.
- **Why:** our `patched` side-table chain design has exactly this failure mode exposed.
- **Verify:** boot with the flag on — no BRK hits in normal operation. Then artificially
  invalidate a hot block and confirm the trap fires.
- **Detail:** `lead-7-dispatch-linking.md` §"BRK tripwire analysis"

### B5. Host hardware cursor (video, zero guest changes)

- **Files:** `SheepShaver/src/Unix/video_x.cpp` / SDL video path
- **What:** render the mouse cursor host-side (SDL cursor or overlay) instead of relying on
  the guest drawing it into the framebuffer, decoupling pointer latency from VOSF refresh.
- **Why:** the single most noticeable interactivity improvement available; it was QEMU's
  first acceleration step too.
- **Verify:** boot, move mouse — pointer is smooth even when the guest is busy; no double
  cursor (guest cursor must be hidden via the existing video driver control calls if possible).
- **Detail:** `landscape-3-classic-mac-video-accel.md` §"first wins"

---

## Tier C — Strategic projects (larger, sequence after A+B)

### C1. AOT-compile the Mac ROM (attacks JIT residency — highest expected impact)

- **The problem it solves:** T2 counter data (LEARNINGS.md, commit 7030a441) shows execution
  abandons the JIT loop after ~15 s and stays in the interpreter; the JIT gate is re-entered
  only ~1/sec. This gates every other throughput optimization.
- **The idea (from Rosetta 2's playbook):** the Mac ROM is immutable
  (`vm_protect`ed READ|EXECUTE after patches, see ppc-jit.cpp ROM-range comments) and is the
  dominant execution target. Instead of compiling ROM blocks on demand — and falling back to
  the interpreter whenever an uncompiled block is hit — scan and compile all reachable ROM
  code ahead of time (at startup, or as a background pass), so ROM execution always has a
  compiled block available.
- **Concrete first step:** instrument *why* the interpreter loop doesn't re-enter the JIT
  (ppc-cpu.cpp:~712-786, the `SS_USE_JIT` gate and `skip_jit:` loop) — is it because blocks
  are missing, or because the gate is only checked on certain events? The answer determines
  whether AOT compilation or a gate restructure is the right fix. **Do this before writing
  any AOT machinery.**
- **Verify:** T2-style counter read (lldb on a running boot) shows sustained JIT-cache
  residency; wall-clock boot time / Finder responsiveness improves.
- **Detail:** `lead-7-dispatch-linking.md` (residency data) + `landscape-2` §Rosetta 2

### C2. Study/lift from MAME's PPC DRC

- **What:** MAME's `src/devices/cpu/powerpc/ppcdrc.cpp` (PPC603/604/750 recompiler frontend)
  and `src/devices/cpu/drcbearm64.cpp` (ARM64 backend) are **BSD-3-Clause** — legally
  vendorable into our GPLv2 tree.
- **How to use:** not a wholesale replacement. Read `ppcdrc.cpp` for per-instruction
  semantics of the exact CPUs we emulate (a second correctness oracle besides QEMU, but with
  ARM64-codegen-shaped answers). Lift specific sequences where ours are weaker (FP, FPSCR
  handling, paired ops). Long-term: evaluate whether MAME's UML IR + drcbearm64 is a viable
  next-generation backend for SheepShaver.
- **First step:** an agent reads ppcdrc.cpp's handling of the 10 opcodes we currently punt
  on most (get the list from `jit_report_misses()` output during a boot) and writes a
  comparison doc.
- **Detail:** `landscape-2-arm64-dynarec-projects.md` §MAME

### C3. Paravirtual video acceleration protocol (`.ndrv` extension)

- **What:** extend SheepShaver's existing virtual display driver (`src/video.cpp` control/
  status protocol) with accelerated primitives — rect fill, copy/blit, scroll — executed
  host-side. MoL proved this design works for Mac OS 9; QEMU's `qemu_vga.ndrv` (GPL-2.0) is
  the living reference for the guest-side driver structure.
- **Sequence:** B5 (host cursor) first → VOSF dirty-rect tightening → then this.
- **Do NOT:** emulate real ATI silicon (DingusPPC path) — unproven everywhere and GPL-3.0.
- **Detail:** `landscape-3-classic-mac-video-accel.md` (architecture comparison table)

### C4. oaknut W^X spike (`DualCodeBlock`)

- **What:** oaknut (MIT, header-only) keeps separate RW and RX mappings of the same physical
  code cache, eliminating `pthread_jit_write_protect_np` toggling entirely. Spike: does the
  dual-mapping approach (`vm_remap` / `MAP_SHARED` tricks) coexist with MAP_JIT on current
  macOS arm64? (Apple has historically restricted this — the spike's job is a yes/no.)
- **If yes:** replaces Lead 2/B3 entirely and removes a whole class of W^X bugs.
- **Detail:** `landscape-2-arm64-dynarec-projects.md` §oaknut

---

## Rejected / closed leads (do not implement)

| Lead | Why rejected |
|------|--------------|
| OE-form punt to interpreter (Dolphin Lead 4) | Workload inversion: Mac OS's ROM 68K emulator makes OE forms hot. Inlining them is what achieved boot-to-desktop. Keep the hybrid: hot add/sub/neg inline, cold div punted. |
| Fastmem fault backpatching (Dolphin Lead 6) | Already satisfied by DIRECT_ADDRESSING; Mac hardware access is EMUL_OP traps, not MMIO faults — nothing to backpatch. Only salvage: a helper-exit for unmapped-address DSI correctness. |
| Wholesale Arm64Emitter vendoring (Lead 5) | API mismatch + dependency tail. Crib `LogicalImm` only (B2). |
| Full 64-bit CR representation (Lead 1) | Needs a CR register cache to pay off; mfcr regresses badly with CR in memory. Do B1 instead; revisit only after a register cache exists. |
| Lazy carry state machine (Lead 3) | Payoff capped (GPRs are memory-resident); blocked on the open truncation-epilogue corruption. Fix A1/A2 first; revisit behind a flag after profiling. |
| ATI/real-silicon video emulation | Unproven in every emulator; GPL-3.0 license hazard. |

---

## Suggested execution order

```
A1 → A2 → A3 (one branch: carry+mullwo fixes, with new harness vectors)
A4, A5            (trivial, can ride along)
C1 first step      (instrument residency — INFORMATION, gates everything)
B1, B2, B3, B4    (independent; can be parallel agents, each gated on harness 100)
B5                (video, independent of JIT work)
C1 full / C2 / C3 / C4  (sequence by what C1's instrumentation reveals)
```
