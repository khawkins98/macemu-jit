> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Phase 2: macOS arm64 W^X Compliance & JIT Addressing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make SheepShaver's JIT actually execute on macOS arm64: MAP_JIT-compliant code cache, a working opcode-harness regression gate, and DIRECT_ADDRESSING-correct JIT codegen.

**Architecture:** Three sequenced workstreams, each gated by the jit-test harness: (2a) MAP_JIT + write-protect toggling for the JIT code cache — the single blocker shared by JIT boot, the ROM harness, and the opcode harness; (2b) port the SS_TEST harness RAM allocation from REAL to DIRECT addressing so the harness becomes the regression gate; (2c) convert the aarch64 JIT's load/store codegen from REAL (identity) to DIRECT (NATMEM_OFFSET base register) addressing — the centerpiece, verified opcode-family by opcode-family.

**Tech Stack:** C++ (kpx_cpu emulator core), AArch64 instruction emission (`ppc-jit.cpp`), POSIX/Mach memory APIs (`mmap`/`MAP_JIT`/`pthread_jit_write_protect_np`/`sys_icache_invalidate`).

**Working repo:** `~/Documents/git/macemu-jit`, branch `macos-arm64`.

**Prerequisites already in place (Phase 1 + research, see LEARNINGS.md):**
- Native arm64 build works; interpreter runtime confirmed executing PPC code with RAM at `NATMEM_OFFSET` (DIRECT addressing works for the interpreter)
- CD-image open fixed (`9bc215cd`) — interpreter boot is unblocked at the I/O layer
- Verified: RWX mmap → EPERM; RWX+MAP_JIT → works (ad-hoc-signed binary, no entitlement needed)
- Verified: __PAGEZERO cannot be shrunk on arm64 → low-memory REAL_ADDRESSING is impossible; DIRECT is the only path
- The aarch64 JIT hardcodes REAL addressing in load/store codegen (zero NATMEM_OFFSET references)

**Commit rule:** Never add a `Co-Authored-By` trailer. Upstream style: `fix:`/`feat:` prefix, root cause in body.

**Regression gates:** `SheepShaver/jit-test/run.sh` before every commit touching emulator/JIT code. LEARNINGS.md updated with every task.

**Upstream compatibility rule:** every Apple-specific behavior goes behind small helpers or `#if defined(__APPLE__)` guards that compile to the existing behavior on Linux. We want these patches to be upstreamable.

---

### Task 2a: MAP_JIT + write-protect toggling for the JIT code cache

**Files:**
- Modify: `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/jit-target-cache.hpp` (cache mmap, ~lines 30-46)
- Modify: `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` (code emission/patching paths that write to the cache)

- [ ] **Step 1: Inventory cache allocation and all cache-write sites**

```bash
cd ~/Documents/git/macemu-jit
grep -n "mmap\|mprotect" SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/jit-target-cache.hpp
grep -n "jit_cache_alloc\|patch_chain_sites\|chain_code\|memcpy.*cache\|emit\|code_ptr" SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp | head -40
```

Identify: (a) the allocation site, (b) every place bytes are written into cache memory (block compilation, runtime back-patching of chain sites), (c) where compiled code is invoked (the dispatch call into the cache).

- [ ] **Step 2: Add Apple JIT helpers to jit-target-cache.hpp**

```cpp
#if defined(__APPLE__) && defined(__aarch64__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#define JIT_CACHE_MAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT)
static inline void jit_cache_begin_write(void) { pthread_jit_write_protect_np(0); }
static inline void jit_cache_end_write(void *addr, size_t len) {
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(addr, len);
}
#else
#define JIT_CACHE_MAP_FLAGS (MAP_PRIVATE | MAP_ANONYMOUS)
static inline void jit_cache_begin_write(void) {}
static inline void jit_cache_end_write(void *addr, size_t len) {
    /* existing icache flush behavior for Linux arm64 stays as-is */
    (void)addr; (void)len;
}
#endif
```

Adapt names/placement to the file's existing style. Check whether the file already has an icache flush (`ic ivau` asm or `__builtin___clear_cache`) — keep Linux behavior identical, route Apple through `sys_icache_invalidate`.

- [ ] **Step 3: Use MAP_JIT in the allocation; bracket every write site**

- Allocation: replace the flags with `JIT_CACHE_MAP_FLAGS` (keep PROT_READ|PROT_WRITE|PROT_EXEC — with MAP_JIT this is allowed).
- Every write site found in Step 1 gets `jit_cache_begin_write()` before and `jit_cache_end_write(start, len)` after. Batch at the largest sensible granularity (whole-block compile = one bracket; one chain-site patch = one bracket) — do NOT toggle per instruction.

- [ ] **Step 4: Build + run the harness**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver/src/Unix && make -j8 2>&1 | tail -3
cd ~/Documents/git/macemu-jit/SheepShaver && ./jit-test/run.sh 2>&1 | grep METRIC
```

Expected: still `pass=0` (the SS_TEST RAM blocker is Task 2b) BUT the failure mode must change — verify by running one vector manually:

```bash
cd ~/Documents/git/macemu-jit/SheepShaver/src/Unix
SS_TEST_HEX="38600005" SS_TEST_DUMP=1 SS_TEST_JIT=1 ./SheepShaver 2>&1 | head -5
```

Before 2a: `SS_TEST: cannot allocate RAM in low 4GB` (and JIT init failure in boot logs).
After 2a: the JIT cache must allocate successfully — check the boot path too:

```bash
SS_USE_JIT=1 timeout_or_perl_alarm 10 ./SheepShaver 2>&1 | head -3
```

Expected: NO `PPC-JIT-A64: failed to allocate ... code cache` line.

- [ ] **Step 5: Run the ROM harness (it shares the fix)**

The rom-harness has its own RWX mmap (`rom-harness.cpp:1138-1147`). Apply the same MAP_JIT treatment there (or split its region into RW data + MAP_JIT code if that's how it uses the memory — read the code first).

```bash
cd ~/Documents/git/macemu-jit/SheepShaver/rom-harness && make && ./rom-harness "/Users/khawkins/Downloads/New_World_Mac_Roms/<oldest rom>" --count=1000 2>&1 | tail -5
```

Expected: it runs (score will be meaningless on a CHRP New World ROM — that's fine, "it executes without EPERM" is the gate).

- [ ] **Step 6: Commit + LEARNINGS.md**

```bash
cd ~/Documents/git/macemu-jit
git add SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ SheepShaver/rom-harness/rom-harness.cpp LEARNINGS.md
git commit -m "fix: use MAP_JIT and write-protect toggling for the JIT code cache on macOS

macOS arm64 W^X policy rejects RWX anonymous mappings with EPERM. MAP_JIT
mappings are allowed (no entitlement needed for ad-hoc-signed builds);
writes must be bracketed with pthread_jit_write_protect_np() and followed
by sys_icache_invalidate(). Linux behavior is unchanged."
```

---

### Task 2b: Port SS_TEST harness RAM allocation to DIRECT addressing

**Files:**
- Modify: `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` (~lines 935-984, `ss_run_opcode_test` RAM setup)

- [ ] **Step 1: Understand how the main (working) path allocates guest RAM**

```bash
grep -n "vm_acquire\|RAMBase\|NATMEM_OFFSET\|ram_base" SheepShaver/src/Unix/main_unix.cpp | head -30
grep -n "RAMBaseHost\|RAMBase" SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp | head -30
```

The interpreter boot proved the main path works on macOS (RAM at 0x4000xxxxxxxx). Find exactly how it computes the host mapping address and how RAMBase (guest) and RAMBaseHost (host) relate under DIRECT_ADDRESSING. Mirror that in the test path.

- [ ] **Step 2: Rewrite the SS_TEST allocation**

Replace the "mmap at 0x10000000 hint, bail if > 4GB, RAMBase = (uint32)host" logic with the DIRECT model: map test RAM the same way main_unix.cpp maps guest RAM (at `NATMEM_OFFSET + guest_base` or via the same vm_acquire wrapper), set RAMBase to the guest address, RAMBaseHost to the host pointer, and drop both the low-4GB check and PROT_EXEC. Keep the Linux path working: guard with the same conditions the main path uses (DIRECT vs REAL is a compile-time property — on Linux REAL builds the old logic must remain).

- [ ] **Step 3: Run the harness — interpreter vectors must now pass**

```bash
cd ~/Documents/git/macemu-jit/SheepShaver && ./jit-test/run.sh 2>&1 | grep METRIC
```

Expected: interpreter vectors pass (substantial movement from 0). JIT vectors will still fail — their codegen reads wrong addresses until 2c. Record exact numbers in LEARNINGS.md. If interpreter vectors do NOT pass, debug before proceeding — Task 2c is unverifiable without this gate.

- [ ] **Step 4: Commit + LEARNINGS.md**

---

### Task 2c: aarch64 JIT DIRECT_ADDRESSING (base-register) codegen

**Files:**
- Modify: `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp` (EA/load/store codegen + prologue ~line 3570)
- Possibly: `SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-codegen-aarch64.h` (register-offset LDR/STR encodings if not present)

This is the centerpiece and the largest task. Work opcode-family by opcode-family with the harness as the gate.

- [ ] **Step 1: Inventory all guest-memory access sites**

```bash
grep -n "emit_load_ea_base" SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp
grep -nE "a64_(ldr|str)[bhw]?_.*RTMP" SheepShaver/src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp | head -60
```

Build a list: integer loads (lwz/lbz/lhz/lha + u/x forms), integer stores (stw/stb/sth + u/x forms), byte-reversed (lwbrx/sthbrx), atomics (lwarx/stwcx.), strings (lswi/stswi), FP (lfs/lfd/stfs/stfd + x forms), AltiVec (lvx/stvx...). Note which emit helper each goes through.

- [ ] **Step 2: Decide and implement the base-register strategy**

Recommended: a callee-saved register (e.g. x21 — verify it's free; the prologue at ~3570 saves x20=RSTATE) holding `NATMEM_OFFSET`, loaded once in the JIT entry/dispatcher prologue. Add it to the prologue/epilogue save-restore. Define `RGUESTBASE` alongside the existing register defines.

On Linux (REAL addressing, RAM mapped low) the same code works with the base register holding 0 — so this does NOT need an #ifdef in the codegen itself; only the value loaded differs (`NATMEM_OFFSET` on DIRECT builds, 0 on REAL builds). That keeps one codegen path for upstream.

- [ ] **Step 3: Add register-offset load/store emitters (if missing)**

Check `ppc-codegen-aarch64.h` for `LDR Wt, [Xn, Xm]` (register-offset) encodings. If only immediate-offset forms exist, add the register-offset encodings (LDR/LDRB/LDRH/LDRSH/STR/STRB/STRH, plus FP/SIMD variants). Each is a single 32-bit instruction encoding — follow the existing encoder macro style.

- [ ] **Step 4: Convert one family at a time, harness after each**

Order (simplest first, each followed by `./jit-test/run.sh` + commit if vectors for that family flip to passing and nothing regresses):
1. `lwz`/`stw` (D-form word load/store) — proves the pattern
2. Remaining integer D-forms (lbz/lhz/lha/stb/sth + update forms)
3. Indexed forms (lwzx/stwx/...)
4. Byte-reversed + atomics + strings
5. FP loads/stores
6. AltiVec loads/stores

Each conversion: EA computed as before (32-bit, zero-extended into the EA temp), access becomes register-offset against RGUESTBASE. Centralize in helpers (`emit_guest_ldr_w(dst, ea_reg)` etc.) so each opcode handler is a one-line change.

- [ ] **Step 5: Final gate + boot attempt**

- All 209 vectors pass in interpreter AND JIT mode
- JIT-mode boot attempt: `SS_USE_JIT=1 ./SheepShaver` with the user's ROM/CD — record how far it gets vs the interpreter
- Tight-loop benchmark if feasible (compare against upstream's ~737 MIPS Linux number)

- [ ] **Step 6: Commit + LEARNINGS.md + update design doc Phase 2 status**

---

### Task 2d: Visual boot confirmation (needs user or Screen Recording permission)

- [ ] Interpreter-mode boot with the user watching (or after granting Screen Recording to the terminal): confirm Happy Mac → Welcome splash → Mac OS 8.6 installer desktop. Record boot time.
- [ ] If 2c completed: same in JIT mode, compare boot times.
- [ ] Update design doc: Phase 2 exit criteria check.

---

## Risks

| Risk | Mitigation |
|---|---|
| Write-protect toggling breaks runtime back-patching (chain sites patched while another thread executes cache code) | pthread_jit_write_protect_np is per-thread; the executing thread keeps X, the patching thread takes W. Verify SheepShaver's JIT is single-threaded (likely) — if so, no issue |
| MAP_FIXED at NATMEM_OFFSET collides with dyld/ASLR mappings | 0x400000000000 is far above ASLR ranges (verified working in interpreter boot); main path already handles this |
| x21 not actually free in the JIT register convention | Inventory register usage first (Step 2c-2); fall back to loading the base from the CPU state struct per block if no register is free |
| Register-offset addressing changes flag/extension semantics subtly (e.g. lha sign-extension) | Harness gates every family; 209 vectors cover all forms including sign-extension cases |
| Upstream divergence while we work | Sync upstream/master before starting; these files change rarely upstream (last touched 2026-05-17) |

## Out of scope for Phase 2

- Re-enabling lazy CR0 / register allocation (upstream containment) — Phase 3
- The 25 failing ROM blocks / OldWorld ROM acquisition — Phase 3
- App bundle, Hardened Runtime signing (+ allow-jit entitlement), notarization — Phase 4
- BasiliskII (68k) equivalents of all of the above
