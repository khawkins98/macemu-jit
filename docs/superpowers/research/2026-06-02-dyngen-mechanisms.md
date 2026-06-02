# Dyngen JIT Mechanisms (x86/x86_64) — How It Solves Our Three aarch64 Gaps

Research date: 2026-06-02. READ-ONLY analysis of the legacy QEMU-derived dyngen JIT
in SheepShaver. All file:line references are to the `macos-arm64` branch.

**One-sentence summary:** Dyngen does NOT call a compile function per block. Compiled
blocks are stitched directly host→host ("direct block chaining"), and **every block's
entry point begins with an spcflags poll** that either falls through into the block body
(no pending work) or returns to the C dispatcher (interrupt pending). That single entry
guard is simultaneously the answer to GAP 1 (dispatch cost) and GAP 2 (interrupts breaking
chains). For GAP 3, dyngen applies **no special-casing whatsoever** to ROM or the 68k
emulator, and it **never mixes interpreter and JIT execution of the same PC** — that mode
mixing is unique to our aarch64 path and is what the in-tree comment already blames for 68k
corruption.

Relevant files:
- `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp` — execute loop, chain resolver, invalidate
- `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-translate.cpp` — `compile_block`, branch emission, epilogue
- `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-dyngen.cpp` — `gen_start` (the entry guard), `gen_bc`
- `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-dyngen-ops.cpp` — `op_spcflags_check`, `op_branch_chain_*`
- `SheepShaver/src/kpx_cpu/src/cpu/jit/basic-dyngen.{hpp,cpp}` — `execute()` trampoline, `op_execute`
- `SheepShaver/src/kpx_cpu/src/cpu/jit/basic-dyngen-ops.cpp` — `op_execute` prologue, `op_jmp_*`
- `SheepShaver/src/kpx_cpu/src/cpu/jit/dyngen-exec.h` — `DYNGEN_FAST_DISPATCH`, `__op_jmp0/1`
- `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-blockinfo.hpp` — `link_info`, `MAX_TARGETS`, `INVALID_PC`
- `SheepShaver/src/kpx_cpu/src/cpu/jit/jit-config.hpp` — `DYNGEN_DIRECT_BLOCK_CHAINING`

> Build note: this dyngen JIT is **x86/x86_64-only** and is **not compiled on aarch64**
> (`DYNGEN_FAST_DISPATCH` is defined only for `__powerpc__`/`__i386__`/`__x86_64__`,
> dyngen-exec.h:111-116). It is reference architecture, not live code on our target.

---

## 1. Dispatch — exact mechanism

GAP 1 and GAP 2 are **one mechanism viewed from two angles.** The steady-state dispatcher
*is* the chaining. There are two regimes:

### Fast path (steady state): host→host, no C, no hash lookup

Once blocks are compiled and chained, control flows entirely in native code. Block A's exit
branch is patched to point at block B's `entry_point`; B's exit at C's; etc. There is **no
`my_block_cache.find()` and no function call** between chained blocks. The only per-transition
cost is the entry guard (the spcflags test described in §2). See §2 for the patch mechanism.

### Slow path (re-entry into C): the `codegen.execute()` trampoline + find loop

The C dispatcher is touched only when a block deliberately returns (`gen_exec_return`), which
happens on: pending spcflags, an indirect branch whose target isn't chained (BCLR/BCCTR), an
unchainable branch target, or a full translation cache. The re-entry loop is in
`powerpc_cpu::execute()`:

ppc-cpu.cpp:597-628
```c
if (use_jit) {
    block_info *bi = my_block_cache.find(pc());
    if (bi == NULL)
        bi = compile_block(pc());
    for (;;) {
        for (;;) {
            codegen.execute(bi->entry_point);          // enter native code
            if (!spcflags().empty()) {
                if (!check_spcflags())  goto return_site;
                if (spcflags().test(SPCFLAG_JIT_EXEC_RETURN)) { ... break; } // cache invalidated
            }
            if ((bi = my_block_cache.find(pc())) == NULL)  break;   // next block not compiled
        }
        bi = compile_block(pc());                       // compile the missing block
    }
}
```

`codegen.execute()` is a tiny trampoline, NOT a compile call:

basic-dyngen.hpp:269-275
```c
inline void basic_dyngen::execute(uint8 *entry_point) {
    typedef void (*func_t)(uint8 *, dyngen_cpu_base);
    func_t func = (func_t)execute_func;     // execute_func == op_execute
    func(entry_point, parent_cpu);
}
```

`execute_func` is generated once at init (basic-dyngen.cpp:45-46: `execute_func = gen_start();
gen_op_execute();`). `op_execute` saves the host VCPU register globals to a stack array, installs
`this_cpu`, then jumps straight into the block; blocks run with guest T0/T1/A0/CPU resident in
fixed host registers. `gen_exec_return` jumps to `op_exec_return_offset`, which restores those
globals and returns to C:

basic-dyngen-ops.cpp:250-262
```c
void OPPROTO op_execute(uint8 *entry_point, basic_cpu *this_cpu) {
    func_t func = (func_t)entry_point;
    const int n_slots = 16 + 4;
    volatile uintptr stk[n_slots];
    stk[n_slots - 1] = (uintptr)CPU; ...               // save host VCPU globals
    CPU = this_cpu;
    DYNGEN_SLOW_DISPATCH(entry_point);                 // jump into the block
    func();  // never reached; forces compiler to record the return address
```

### Per-block overhead, dyngen vs. our aarch64

| | dyngen | our aarch64 |
|---|---|---|
| Chained transition (hot) | entry spcflags test (a few instrs), no call, no lookup | full `ppc_jit_aarch64_compile()` call (hash lookup inside 4KB-frame fn) every block |
| Re-enter from C | `op_execute` trampoline + `my_block_cache.find()` | same heavyweight compile() |

Our aarch64 dispatcher pays the heavyweight cost **every block** precisely because it has no
chaining: each block returns to C, which re-dispatches. Adding chaining (§4) removes both the
call and the lookup from hot transitions — the same fix closes GAP 1 and GAP 2.

---

## 2. Chaining — exact mechanism + how interrupts break chains

`DYNGEN_DIRECT_BLOCK_CHAINING` is enabled unconditionally under dyngen (jit-config.hpp:56-59).

### 2a. Per-block data: `link_info li[MAX_TARGETS]`

ppc-blockinfo.hpp:46-53
```c
struct link_info {
    uint8 *  jmp_resolve_addr;   // trampoline that compiles+patches the target on first hit
    uint8 *  jmp_addr;           // address of the native branch displacement to patch
    uint32   jmp_pc;             // guest PC of the target block
};
static const uint32 INVALID_PC = 0xffffffff;
link_info li[MAX_TARGETS];
```

`MAX_TARGETS` == 2 (basic-dyngen.hpp:237, `MAX_JUMPS = 2`): a two-way conditional branch chains
both its taken (`li[0]` = tpc) and fall-through (`li[1]` = npc) edges. `init()` sets all
`jmp_pc = INVALID_PC` (ppc-blockinfo.hpp:71-74).

### 2b. When chaining is allowed

Only in-page jumps or jumps into ROM (read-only):

ppc-translate.cpp:62-68
```c
static inline bool direct_chaining_possible(uint32 bpc, uint32 tpc) {
#ifndef DYNGEN_FAST_DISPATCH
    return false;
#endif
    return ((bpc ^ tpc) >> 12) == 0 || is_read_only_memory(tpc);
}
```

(In-page so the source block's validity tracks the target's at page granularity; ROM because it
is permanently valid.) Set during branch emission, e.g. for `BC` at ppc-translate.cpp:530-544 it
records `li[0].jmp_pc = tpc` (and `li[1].jmp_pc = npc` for conditional/CTR branches), then
emits `gen_bc(..., use_direct_block_chaining)`.

### 2c. The chained-branch op and the resolver trampoline

For chained branches `gen_bc` emits `op_branch_chain_1` / `op_branch_chain_2`, which are bare
jumps to the link slots:

ppc-dyngen-ops.cpp:685-697
```c
void OPPROTO op_branch_chain_1(void) { DYNGEN_FAST_DISPATCH(__op_jmp0); }
void OPPROTO op_branch_chain_2(void) {
    if (T1) DYNGEN_FAST_DISPATCH(__op_jmp0);
    else    DYNGEN_FAST_DISPATCH(__op_jmp1);
    dyngen_barrier();
}
```

`__op_jmp0/1` are the relocatable branch displacements; their addresses are captured into
`dg.jmp_addr[i]` when the op is copied into the block. At first execution they point at a
**resolver trampoline** generated in the block epilogue:

ppc-translate.cpp:1583-1601
```c
if (use_direct_block_chaining) {
    func_t func = (func_t)&powerpc_cpu::call_compile_chain_block;
    for (int i = 0; i < block_info::MAX_TARGETS; i++) {
        if (bi->li[i].jmp_pc != block_info::INVALID_PC) {
            uint8 *p = dg.gen_align(16);
            dg.gen_mov_ad_A0_im(((uintptr)bi) | i);    // stuff block ptr | slot index into A0
            dg.gen_invoke_CPU_A0_ret_A0(func);          // call compile_chain_block(this, A0)
            dg.gen_jmp_A0();                            // jump to returned entry point
            bi->li[i].jmp_addr = dg.jmp_addr[i];
            bi->li[i].jmp_resolve_addr = p;
            dg_set_jmp_target_noflush(bi->li[i].jmp_addr, bi->li[i].jmp_resolve_addr);
        }
    }
}
```

(Note the array double-use: `gen_start` transiently uses `jmp_addr[0]` for the entry-guard skip
then nulls it (ppc-dyngen.cpp:65-66); the chaining epilogue later reuses `jmp_addr[i]` for the
chain-exit displacements (1595). Same array, two sequential roles.)

The resolver compiles the target on demand and **back-patches the branch** so the second and
all later traversals jump straight to the target — no resolver, no C:

ppc-cpu.cpp:560-580
```c
void *call_compile_chain_block(powerpc_cpu *the_cpu, block_info *sbi)
{ return the_cpu->compile_chain_block(sbi); }

void *compile_chain_block(block_info *sbi) {
    const int n = ((uintptr)sbi) & 3;             // slot index packed in low bits
    sbi = (block_info *)(((uintptr)sbi) & ~3L);
    const uint32 tpc = sbi->li[n].jmp_pc;
    block_info *tbi = my_block_cache.find(tpc);
    if (tbi == NULL) tbi = compile_block(tpc);
    dg_set_jmp_target(sbi->li[n].jmp_addr, tbi->entry_point);   // patch branch → target entry
    return tbi->entry_point;
}
```

Patching is a single relative-displacement write (basic-dyngen.hpp:28-41).

### 2d. THE CRITICAL PART — how interrupts break chains

**Every block entry point begins with an spcflags poll.** This is emitted by `gen_start`, which
runs at the top of `compile_block` (ppc-translate.cpp:167, `bi->entry_point = dg.gen_start(...)`):

ppc-dyngen.cpp:58-68
```c
uint8 *powerpc_dyngen::gen_start(uint32 pc) {
    uint8 *p = basic_dyngen::gen_start();
    gen_op_spcflags_check();           // if spcflags==0, skip the next two ops (jump to body)
    gen_op_set_PC_im(pc);              // otherwise: record guest PC and...
    gen_exec_return();                 // ...return to the C dispatcher
    dg_set_jmp_target_noflush(jmp_addr[0], gen_align());  // wire the skip target = block body
    jmp_addr[0] = NULL;
    return p;                          // entry_point = p (the spcflags_check)
}
```

The check itself is a conditional fast-dispatch:

ppc-dyngen-ops.cpp:619-631
```c
#if defined(__x86_64__)
#define FAST_COMPARE_SPECFLAGS_DISPATCH(SPCFLAGS, TARGET) \
        asm volatile ("test %0,%0 ; jz " #TARGET : : "r" (SPCFLAGS))
#endif
void OPPROTO op_spcflags_check(void) {
    FAST_COMPARE_SPECFLAGS_DISPATCH(powerpc_dyngen_helper::spcflags().get(), __op_jmp0);
}
```

So the control flow at every chained transition is:

```
block A body --(op_branch_chain)--> B.entry_point == op_spcflags_check
    spcflags == 0  ?  jump to B's body (stay in native code, fully chained)
    spcflags != 0  ?  set_PC_im(B.pc); exec_return  ->  back to C dispatcher
```

When the C dispatcher regains control it runs `check_spcflags()` (ppc-cpu.cpp:517-557), which
delivers the interrupt via `HandleInterrupt`, then re-enters native code at the (now correct)
PC. **The interrupt path itself does nothing to the chains** — it only writes the spcflags word;
the next block entry polls it. This is *passive* chain-breaking by polling, exactly the
mechanism our aarch64 chaining lacks.

### 2e. Negative findings (state these explicitly)

- `check_spcflags()` and the trigger-interrupt path do **NOT** unchain anything. They write/read
  the spcflags word; chains break only because the next entry guard polls it.
- `powerpc_block_info::invalidate()` (ppc-cpu.cpp:954-971) **does** reset jump targets back to
  their resolvers (`dg_set_jmp_target(tli->jmp_addr, tli->jmp_resolve_addr)`), but this is called
  **only on code modification**, never on interrupt delivery. Its callers are `invalidate_cache`
  / `invalidate_cache_range` (ppc-cpu.cpp:938-997), driven by icbi/isync / self-modifying code.
  Do not conflate the two unchaining stories: interrupts → passive poll; code edits → active
  target reset.

So the design is **not** subject to the "chained loop never services interrupts" bug that forced
us to disable aarch64 chaining: the entry guard guarantees a return to C the first time spcflags
becomes nonzero after any block boundary.

---

## 3. 68k emulator / mixed execution — what dyngen does

### 3a. No special-casing of ROM or the 68k emulator — at all

The only ROM awareness is `is_read_only_memory()`, used solely to (a) permit cross-page chaining
into ROM and (b) file compiled ROM blocks on a "dormant" list:

ppc-translate.cpp:49-56
```c
static inline bool is_read_only_memory(uintptr addr) {
#ifdef SHEEPSHAVER
    if ((addr - ROMBase) < ROM_AREA_SIZE) return true;
#endif
    return false;
}
```

ppc-translate.cpp:1609-1612
```c
if (is_read_only_memory(bi->pc))
    my_block_cache.add_to_dormant_list(bi);   // ROM blocks survive normal cache eviction
else
    my_block_cache.add_to_active_list(bi);
```

There is **no exclusion** of the 68k-emulator region (our `ROMBase+0x460000` cutoff has no
dyngen analogue), no different block-ending rules, no CR-bit awareness. Dyngen compiles the ROM
68k interpreter exactly like any other PPC code, and it works — because of §3b.

### 3b. Dyngen NEVER mixes interpreter and JIT execution of the same PC

When `use_jit` is true, a given PC is **always** executed compiled. The interpreter loop
(`do_interpret`, ppc-cpu.cpp:871-895) is reachable **only when `use_jit` is false**. There is no
"complete vs. incomplete block" gate and no per-block fallback to the interpreter dispatch loop.

What *is* reused is the per-opcode interpreter **handler functions** — but they are always called
**inline from within the compiled block**, never from the interpreter loop. Any opcode dyngen
can't natively translate falls to `do_generic`, which emits a call to `ii->execute.ptr()` (the
same handler the interpreter would use) embedded in the block:

ppc-translate.cpp:1512-1552
```c
default:                       // Direct call to instruction handler
  do_generic:
    func = (func_t)ii->execute.ptr();
  do_invoke:
    cg_context.pc = dpc; cg_context.opcode = opcode; ...
    compile_status = compile1(cg_context);          // try native codegen first
    switch (compile_status) {
    case COMPILE_FAILURE:
    case COMPILE_EPILOGUE_OK:
        if ((dpc - sync_pc) > sync_pc_offset) { ...; dg.gen_set_PC_im(dpc); }  // sync guest PC
        sync_pc_offset += 4;
        dg.gen_invoke_CPU_im(func, opcode);          // inline call to interpreter handler
        compile_status = COMPILE_CODE_OK;            // block still "compiled"
        break;
    }
```

The `gen_set_PC_im` "sync points" exist so those inline handlers observe a correct guest PC —
**not** to switch execution modes. `compile_block` always produces a runnable block (worst case,
a sequence of inline handler calls), so dyngen never bounces a PC between two execution engines.

### 3c. Contrast with our aarch64 path — this is the likely 68k bug

Our path genuinely **does** switch modes per-PC:
- GATE 2 only executes blocks with `jblk.complete` (ppc-cpu.cpp:767, 720-723); incomplete blocks
  fall to `skip_jit`.
- `skip_jit` runs the **interpreter** for that PC (ppc-cpu.cpp:821-864), then may hand back to
  JIT on the next block (the `ppc_jit_aarch64_is_compilable` break at 858).

So the same code address can run compiled on one visit and interpreted on another. The in-tree
comment already fingers this for 68k corruption:

ppc-cpu.cpp:744-754
```
* lives the ROM's built-in 68k emulator ... That emulator is itself an interpreter whose
* dispatch protocol polls a CR bit injected asynchronously by HandleInterrupt(); JIT-compiling
* its dispatch/handler blocks changes interrupt-delivery interleaving in ways that corrupt
* multi-step 68k instruction emulation (observed: skipped immediate-word consumption, guest
* jumps to garbage).
```

The replication target is therefore **uniform execution + correct interrupt interleaving**, NOT
68k special-casing. Dyngen avoids the corruption not by excluding the 68k emulator but by (1)
always compiling it and (2) guaranteeing — via the entry spcflags guard at every block boundary —
that an interrupt injected by `HandleInterrupt` is observed at a deterministic point (a block
boundary, with guest PC synced) rather than at the unpredictable point where our skip_jit handoff
crosses between engines mid-emulated-instruction.

---

## 4. Mechanisms to replicate in aarch64 — prioritized

Dependency ordering matters: **(a) is a hard prerequisite for (b).** Enabling chaining without
an entry guard reintroduces exactly our current "chained loop never services interrupts" bug.

### (a) Entry-prologue spcflags poll + return-to-dispatcher — MEDIUM. **Do this first.**
Mirror `gen_start` (ppc-dyngen.cpp:58-68): every compiled block's entry point begins with a load
of the spcflags word and a conditional branch:
- spcflags == 0 → fall through to block body
- spcflags != 0 → store guest PC into `regs.pc`, return to the C dispatcher (which already calls
  `check_spcflags()` and re-enters)

This is the safety valve. It also lets us *test* the design before adding chaining: with the
guard in place but chaining still off, behavior is unchanged but every block proves it returns to
C whenever spcflags is set.

### (b) Direct block chaining with resolver/patch sites — MEDIUM–LARGE. Depends on (a).
Port the `link_info` model (ppc-blockinfo.hpp:46-53) and the resolve-then-patch trampoline
(`compile_chain_block`, ppc-cpu.cpp:565-580; epilogue, ppc-translate.cpp:1583-1601):
- Each block records up to 2 outgoing edges (taken/fall-through) with guest target PC.
- Branch exits initially jump to a resolver stub that compiles the target and back-patches the
  branch displacement to the target's entry point.
- Restrict chaining to in-page or ROM targets (`direct_chaining_possible`, ppc-translate.cpp:62-68).
- On `invalidate_range`, reset patched branches back to their resolvers (the
  `powerpc_block_info::invalidate()` analogue, ppc-cpu.cpp:954-971) so stale chains can't survive
  a code edit. We already evict JIT blocks on range invalidation (ppc-cpu.cpp:988-996); this
  extends that to un-patching inbound chain edges.
- AArch64 specifics: patch a B-imm displacement (±128 MB range; if a target is farther, fall back
  to an indirect jump or refuse to chain). Bracket each patch with
  `pthread_jit_write_protect_np` + `sys_icache_invalidate` per the MAP_JIT contract.

Together (a)+(b) eliminate the per-block `ppc_jit_aarch64_compile()` call and hash lookup from hot
transitions — closing GAP 1 and GAP 2 with one change.

### (c) Uniform always-compile with inline handler calls — LARGE. The real 68k fix (GAP 3).
Remove the per-PC mode switching:
- Make the JIT always produce a runnable block. For opcodes lacking native codegen, emit an
  inline call to the existing interpreter handler with a synced guest PC (dyngen's `do_generic` /
  `gen_invoke_CPU_im` + `gen_set_PC_im`, ppc-translate.cpp:1516-1551). This removes the GATE 2
  "complete-only" containment and the `skip_jit` interpreter fallback, so no PC ever alternates
  between engines.
- Once uniform execution + the entry guard (a) are in place, the 68k-emulator ROM exclusion at
  ppc-cpu.cpp:756-760 should become removable: the corruption stems from mode-mixing and
  nondeterministic interrupt interleaving, both of which (a)+(c) eliminate. Validate with the
  parity harness before deleting the exclusion.

Effort/risk note: (c) is the largest but is the one that actually addresses the 68k corruption.
(a)+(b) are the performance wins and are independently shippable, with (a) being low-risk and
high-leverage on its own.
