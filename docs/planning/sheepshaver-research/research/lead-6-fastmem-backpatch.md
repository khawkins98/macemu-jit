# Lead 6 — Fastmem + SIGSEGV Fault Backpatching

> **Status:** 📖 Reference / archive · **Created:** 2026-06-02 · **Updated:** 2026-06-04
> **Why this doc exists:** Research lead: fastmem + SIGSEGV fault backpatching (closed) — verdict in backlog.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../../../CONTRIBUTING.md) → "Documentation Lifecycle")._


Study of Dolphin's `JitArm64_BackPatch.cpp` fastmem fault-backpatching scheme and
whether it is applicable to the SheepShaver PPC→ARM64 JIT under our
DIRECT_ADDRESSING model (`host = NATMEM_OFFSET + (uint32)guest`, NATMEM_OFFSET =
0x400000000000).

Compiled 2026-06-02. Dolphin source verified verbatim against
`master:Source/Core/Core/PowerPC/JitArm64/JitArm64_BackPatch.cpp` (fetched, not paraphrased).

---

## 1. Verified Dolphin implementation

Dolphin's scheme: emit a bare fast LDR/STR at the access site; if it faults (MMIO /
unmapped / cache-inhibited), a host fault handler rewrites the faulting instruction
in place into a `BL slow_path`. One-shot — once patched, that site never faults again.

### Fast-path emission

`EmitBackpatchRoutine()` records the byte span of the fast access and registers it
in a map keyed by the *end* address (`fast_access_end`). Verbatim
(`JitArm64_BackPatch.cpp:57-167`):

```cpp
void JitArm64::EmitBackpatchRoutine(u32 flags, MemAccessMode mode, ARM64Reg RS, ARM64Reg addr,
                                    BitSet32 gprs_to_push, BitSet32 fprs_to_push,
                                    bool emitting_routine)
{
  ...
  const bool emit_fast_access = mode != MemAccessMode::AlwaysSlowAccess;
  const bool emit_slow_access = mode != MemAccessMode::AlwaysFastAccess;
  ...
  const u8* fast_access_start = GetCodePtr();
  ...
  if (emit_fast_access)
  {
    ARM64Reg memory_base = MEM_REG;
    ARM64Reg memory_offset = addr;
    ...
    else   // fastmem: bare access, no bounds check
    {
      if (flags & BackPatchInfo::FLAG_SIZE_32)
        LDR(RS, memory_base, memory_offset);
      ...
      ByteswapAfterLoad(this, &m_float_emit, RS, RS, flags, true, false);
    }
  }
  const u8* fast_access_end = GetCodePtr();

  if (emit_slow_access)
  {
    if (emit_fast_access)
    {
      in_far_code = true;
      SwitchToFarCode();                       // slow path lives in pre-emitted "far code"
      if (jo.fastmem && !emitting_routine)
      {
        FastmemArea* fastmem_area = &m_fault_to_handler[fast_access_end];
        fastmem_area->fast_access_code = fast_access_start;
        fastmem_area->slow_access_code = GetCodePtr();
      }
    }
    ...
```

Note: the fast path is `LDR(RS, memory_base, memory_offset)` with no comparison or
branch. `MEM_REG` (X28) holds the guest memory base; the bounds/validity of the
address is enforced *only* by the host page tables — an invalid access SIGSEGVs.

### Fault-handler lookup + in-place patch under W^X

`HandleFastmemFault()` is called from Dolphin's host fault handler. Verbatim
(`JitArm64_BackPatch.cpp:330-360`):

```cpp
bool JitArm64::HandleFastmemFault(SContext* ctx)
{
  const u8* pc = reinterpret_cast<const u8*>(ctx->CTX_PC);
  auto slow_handler_iter = m_fault_to_handler.upper_bound(pc);

  // no fastmem area found
  if (slow_handler_iter == m_fault_to_handler.end())
    return false;

  const u8* fastmem_area_start = slow_handler_iter->second.fast_access_code;
  const u8* fastmem_area_end = slow_handler_iter->first;

  // no overlapping fastmem area found
  if (pc < fastmem_area_start)
    return false;

  const Common::ScopedJITPageWriteAndNoExecute enable_jit_page_writes;
  ARM64XEmitter emitter(const_cast<u8*>(fastmem_area_start), const_cast<u8*>(fastmem_area_end));

  emitter.BL(slow_handler_iter->second.slow_access_code);

  while (emitter.GetCodePtr() < fastmem_area_end)
    emitter.NOP();

  m_fault_to_handler.erase(slow_handler_iter);

  emitter.FlushIcache();

  ctx->CTX_PC = reinterpret_cast<std::uintptr_t>(fastmem_area_start);
  return true;
}
```

Key mechanics:
- `m_fault_to_handler` is a `std::map<const u8*, FastmemArea>` keyed by the *end* of
  the fast-access span; `upper_bound(pc)` finds the span whose end is just past the
  faulting PC, then `pc >= fast_access_code` confirms the PC is inside it.
- `Common::ScopedJITPageWriteAndNoExecute` is the RAII W^X toggle — on Apple Silicon
  this wraps `pthread_jit_write_protect_np()` (the nesting-counter version is Lead 2).
- The fast access span is overwritten with `BL slow_access_code` + `NOP` padding,
  then `FlushIcache()` (i-cache invalidation, = `sys_icache_invalidate` on macOS).
- `CTX_PC` is rewound to the start of the (now-patched) span and execution resumes;
  the `BL` runs the slow path and the site never faults again.
- The map entry is `erase`d after patching — the area is now permanently slow.

This requires the fast and slow forms to occupy the *same byte length* (slow form =
one `BL`, padded with NOP). Dolphin guarantees this because the fast access is a
fixed-size instruction sequence and the far-code slow path is reachable by a single
`BL` (±128 MB).

---

## 2. Our current memory access paths (file:line)

### Guest load/store emission — bare, unconditional, no slow path

The SheepShaver JIT emits exactly the Dolphin *fast path* and nothing else. EA is
computed into a temp (`RTMP0`), then a single register-offset access against
`RMEMBASE` (x19). Examples in
`src/kpx_cpu/src/cpu/jit/aarch64/ppc-jit.cpp`:

| PPC op | line | emitted |
|--------|------|---------|
| `lwzx` (23) | 1305 | `a64_ldr_w_reg(RTMP1, RMEMBASE, RTMP0)` + `REV` |
| `stwx` (151) | 1318 | `REV` + `a64_str_w_reg(RTMP1, RMEMBASE, RTMP0)` |
| `lbzx`/`stbx` | 1533/1540 | `a64_ldrb_reg` / `a64_strb_reg` |
| `lhzx`/`sthx` | 1545/1554 | `a64_ldrh_reg` + `REV16` |
| `lwbrx`/`stwbrx` (534/662) | 1644/1651 | `a64_ldr_w_reg`/`a64_str_w_reg` (no REV) |

`RMEMBASE` is loaded once per block with `emit_load_mem_base()` →
`emit_load_imm64(RMEMBASE, JIT_MEM_BASE)` where
`JIT_MEM_BASE = (uint64_t)NATMEM_OFFSET` (ppc-jit.cpp:293-294, 504-508).

### How the 32-bit guest address gets masked — for free, via UXTW

The register-offset encoding is `a64_ldr_w_reg` =
`emit32(0xB8606800 | rm<<16 | rn<<5 | rt)`
(`src/kpx_cpu/src/cpu/jit/aarch64/ppc-codegen-aarch64.h:151`). The `option` field in
`0x...6800` is `011b` = **UXTW**: the low 32 bits of `Xm` (the EA temp) are
zero-extended and added to `Xn` (RMEMBASE). So
`host = NATMEM_OFFSET + (uint32)EA` is realized purely by the addressing mode — no
explicit `AND ea, 0xFFFFFFFF` is ever emitted, and the *entire* 4 GiB guest address
space is covered by the fast path. **This is exactly Dolphin's `jo.fastmem` path,
already in place.**

### What is mapped into the flat NATMEM space

`main_unix.cpp` maps RAM, ROM, and (via `Mac2HostAddr`) everything else into the
single `NATMEM_OFFSET + guest` window with `vm_acquire_fixed` /
`vm_mac_acquire_fixed`:
- RAM at `RAMBase` (RW+EXEC), ROM at `ROMBase = 0x50000000` (main_unix.cpp:188,
  1095-1207).
- Framebuffer, low-memory globals, KernelData, DR cache — all fixed-mapped in the
  same window.

So a guest load/store to a *valid* Mac address hits a real host page and the bare
LDR/STR just works. There is **no per-access bounds check to eliminate** — we never
had one.

### What happens on a non-RAM / unmapped access today

There is no JIT slow path and no helper exit for loads/stores. An access that misses
all mapped regions raises a host SIGSEGV/Mach exception, handled by
`sigsegv_handler(sigsegv_info_t*)` in
`src/kpx_cpu/sheepshaver_glue.cpp:809-879`:

```cpp
sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip)
{
#if ENABLE_VOSF
  if (Screen_fault_handler(sip)) return SIGSEGV_RETURN_SUCCESS;   // framebuffer dirty-tracking
#endif
  const uintptr addr = (uintptr)sigsegv_get_fault_address(sip);
  if ((addr - (uintptr)ROMBaseHost) < ROM_SIZE)
    return SIGSEGV_RETURN_SKIP_INSTRUCTION;                       // writes to ROM: ignore
  const uint32 pc = cpu->pc();
  bool mac_fault = (pc in ROM/RAM/DR_CACHE);
  if (mac_fault) {
    // a handful of HARD-CODED interpreter-PC fixups, e.g.:
    if (pc == ROMBase + 0x48e080 && (cpu->gpr(8) == 0xf3012002 ...))
      return SIGSEGV_RETURN_SKIP_INSTRUCTION;                     // MacOS 8 serial driver probe
    ... // ZeroPage writes, ignoresegv pref
  }
  // otherwise:
  enter_mon(); QuitEmulator(); return SIGSEGV_RETURN_FAILURE;     // crash
}
```

Two things matter here:
1. The fault handler is **trap-by-PC**, and the PCs it recognizes are *guest* PCs
   (`cpu->pc()`), checked against `ROMBase + 0x...`. These are probes of hardware
   addresses like `0xf3012002` (serial), which a MacOS startup path deliberately
   touches expecting it to fail. They are handled by *skipping the guest
   instruction*, not by re-dispatching memory.
2. Anything not on that allowlist → `enter_mon()` + `QuitEmulator()`: **a hard
   crash, not a guest DSI exception.**

### Hardware access is trap-driven, not MMIO-fault-driven

The architectural reason MMIO faults are rare: SheepShaver does **not** let Mac OS
touch real hardware registers through memory. Device access is intercepted by
**EMUL_OP trap opcodes patched into ROM** and dispatched in C++ via
`EmulOp(M68kRegisters*, uint32 pc, int selector)`
(`src/emul_op.cpp:65`): XPRam (`OP_XPRAM*`), NVRAM, the Sony/disk/serial drivers
(`OP_SONY_*`, `OP_DISK_*`), video, etc. The guest never executes a load/store to a
device register in the common path — it executes a patched trap that returns to C++.
The handful of `0xf301xxxx` probes in the SIGSEGV handler are the exceptions that
slip past patching, and they are deliberately made to fault-and-skip.

---

## 3. Applicability analysis

**Fastmem-with-backpatching is essentially already done for us — and the
backpatching half has nothing to patch.**

- **The fast path is the only path we emit.** DIRECT_ADDRESSING + the UXTW addressing
  mode give us Dolphin's `jo.fastmem` behaviour for the full 4 GiB guest space with
  zero bounds checks. There is no slow path, no comparison, no branch to optimize
  away. The structural win Dolphin gets from backpatching (turning a checked access
  into an unchecked one) is a win we already have unconditionally.

- **There is no hot stream of MMIO faults to convert into `BL slow_path`.** Hardware
  is reached through EMUL_OP traps + driver patches (emul_op.cpp), not through
  faulting memory accesses. The only recurring faults are:
  - **Framebuffer / VOSF** (`Screen_fault_handler`) — already a dedicated,
    purpose-built dirty-page tracker. It *wants* to keep faulting (each fault marks a
    page dirty for the next blit); backpatching it to a permanent slow path would
    **break** dirty tracking. This is the opposite of what backpatch wants.
  - **Known ROM probe PCs** — finite, ~6 of them, already handled by PC match.

- **A "JIT-direct MMIO" feature would be solving a problem we don't have.** Dolphin
  needs fast MMIO because GameCube/Wii titles bang hardware registers (GX FIFO, etc.)
  in hot loops. Mac OS under SheepShaver does not — it calls Toolbox/driver routines
  that we already short-circuit in C++.

**The one real gap this lead surfaces is a correctness issue, not a perf win:** an
unmapped guest access from JIT code lands in `sigsegv_handler`, fails the allowlist,
and **crashes the emulator** (`enter_mon`/`QuitEmulator`) instead of raising a guest
DSI/ISI exception the way real PPC hardware would. The interpreter has the same
behaviour, so JIT doesn't regress it — but if we ever want faithful
data-storage-interrupt semantics, that is where the work is, and it is unrelated to
fastmem performance.

---

## 4. What it would take on macOS arm64 (if we ever did it)

If a future need arose (e.g. faithful DSI delivery, or genuinely hot direct-MMIO):

1. **Per-access span tracking.** Record `(fast_start, fast_end) → slow_path` for each
   emitted load/store, keyed by end PC in a `std::map` — mirroring `m_fault_to_handler`.
   Our JIT currently emits no slow path, so each would need a pre-emitted far-code
   stub (call into a C++ `ppc_read32/ppc_write32`-style helper that runs the guest
   exception path).
2. **Hook the existing Mach exception server.** We already have a full Mach
   `EXC_BAD_ACCESS` handler thread (`sigsegv.cpp:620+`, `handleExceptions`,
   `catch_mach_exception_raise_state_identity`). The backpatch entry point would slot
   into the same dispatch that currently calls `sigsegv_handler`, *before* the
   crash path: look up the faulting host PC in the span map; if found, patch.
3. **W^X patch under MAP_JIT.** The code cache is `MAP_JIT`. Patching from the fault
   handler requires `pthread_jit_write_protect_np(false)` → emit `BL`+NOP →
   `pthread_jit_write_protect_np(true)` → `sys_icache_invalidate(start, len)`. This is
   exactly `ScopedJITPageWriteAndNoExecute` + `FlushIcache`. **Pairs directly with
   Lead 2** (the nesting-counter W^X toggle) since the handler is a nested emit caller.
4. **Equal-length fast/slow forms.** The fast access span must be ≥ the size of one
   `BL` (4 bytes) so it can be overwritten in place. Our single-instruction
   `a64_ldr_w_reg` is exactly 4 bytes — a `BL` fits with zero NOP padding. Good.
5. **Thread / reentrancy safety.** The Mach handler runs on a dedicated exception
   thread (`exc_thread`), patching code another thread is executing. Dolphin gets
   away with this because the faulting thread is stopped at the fault; we'd need the
   same guarantee (the faulting CPU thread is suspended while the exception thread
   patches). Must also handle the case where the fault thread *is* the patching path.

---

## 5. Effort, risk, payoff

| | |
|---|---|
| **Effort** | High. Requires per-access span map, far-code slow stubs for every load/store form, Mach-handler integration, W^X-in-handler, and i-cache invalidation. None of the slow-path infrastructure exists today. |
| **Risk** | High. Signal/Mach-handler code patching live JIT memory under W^X is the single subtlest area in any dynarec; race conditions and i-cache staleness are hard to reproduce and debug. VOSF interaction is a correctness trap (backpatching the framebuffer would silently break screen updates). |
| **Payoff** | **Near zero for performance.** We already emit the fast path; there is no checked access to eliminate and no hot MMIO fault stream. The only payoff is *correctness* — faithful DSI/ISI delivery instead of an emulator crash on a genuinely bad guest access — and that does not need backpatching at all (a plain helper-exit on the rare fault would do). |

---

## 6. Recommendation

**Do not implement fastmem backpatching. Mark Lead 6 closed as "already satisfied /
not applicable."**

Rationale:
- The fast half of Dolphin's scheme (unchecked `LDR/STR [base + UXTW(ea)]`) is
  *already* what our JIT emits, courtesy of DIRECT_ADDRESSING + the UXTW addressing
  mode. We get the performance Dolphin's backpatching exists to achieve, with none of
  the signal-handler machinery.
- The backpatch half exists to make *checked or trapping* MMIO accesses fast after
  first fault. SheepShaver has no hot MMIO-via-memory: hardware is reached through
  EMUL_OP traps and driver patches (`emul_op.cpp`), and the framebuffer fault path
  (VOSF) deliberately wants to keep faulting. There is nothing to convert to a
  one-shot `BL slow_path`.

Salvage value from this study:
1. **Verbatim confirmation** that our memory model matches Dolphin's best path —
   useful as a correctness cross-check and to retire the lead with confidence.
2. **The W^X / Mach-handler patching pattern** (`ScopedJITPageWriteAndNoExecute` +
   `FlushIcache`, `upper_bound` span lookup) is the reusable nugget. If we ever do
   live code patching from a fault handler for *any* reason, copy this shape, and do
   Lead 2 (nesting-counter W^X) first.
3. **A noted correctness gap (not a fastmem task):** an unmapped guest access from JIT
   currently crashes via `enter_mon`/`QuitEmulator` rather than raising a guest DSI.
   If faithful exception delivery ever matters, fix it with a simple helper-exit on
   the rare fault — not with backpatching.

Net: keep as a closed reference lead. Engineering attention is better spent on
Leads 2 (W^X nesting counter), 4 (OE-form audit), 7 (BRK tripwire), and 3 (lazy
carry), per the existing investigation order.
