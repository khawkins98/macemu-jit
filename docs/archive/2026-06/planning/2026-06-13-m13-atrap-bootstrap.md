# M13 — Pixel attempt 2: fix DR interrupt re-entry → QuickDraw init

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** (Pixel attempt 2 — M12 failed this gate.) Get `irq_fired≥1` (CGRP interrupt delivery survives through the 68k handler) and `[FB-DIRTY] non_zero_pixels>0` (QuickDraw writes at least one pixel to the framebuffer aperture) in a 180s NewWorld diagnostic boot.

**Architecture:** The CGRP stub (M10) correctly delivers EXT interrupts to 68k PC=0x5000ED08 (probe-confirmed). The handler crashes in the DR emulator's JIT infrastructure — DR_WARM re-entry hits DR JIT dead-fill (0xDEADBEEF = `stfdu f21,-16657(r13)`), a cache-boundary overrun during interrupt re-entry. This is an architectural DR dispatch issue, NOT a trap-table ordering bug. Fix the DR dispatch path first, understand the root cause fully (Q-A1/Q-A2), then address the A-traps (0xAAF3/0xAAF4) that lie deeper in the handler. A ~33% intermittent crash at ea=0x64A05014 from the same 24-bit DR region is in scope (M12 Wave1 fixed one instance; a second distinct crash exists).

**Tech Stack:** C/C++ (SheepShaver — `rom_patches.cpp`, `sheepshaver_glue.cpp`, `ppc-cpu.cpp`), PPC/68k static RE (capstone), QEMU mac99 differential rig, ss-slot-boot.sh, SS_PROBE_68K, SS_PROBE_PC.

---

> **Rev 2:** Folded 2026-06-13 after PROCESS + TECHNICAL red-team. BINDING amendments:
> (a) Task A gate now falsifiable (specific dec_expiries≥20 threshold, ≥2/3 boots, explicit checkpoint);
> (b) Q-A5 populated with method + budget; (c) Q-A6 added for A7 register identity (r1 vs r23);
> (d) Task B gate fixed (--expect → post-boot grep); (e) Task C Step C-4 made concrete;
> (f) Stop-rule item 5 added (no NOP-patch of DR_WARM crash without root cause);
> (g) Task D expanded (JIT-STATUS.md, CONTRIBUTING.md); (h) A-2 safety caveat made explicit.
> **Process:** `docs/MILESTONE-WORKFLOW.md`. Read `docs/AGENT-CONTEXT.md` at task start.
> Predecessor: M12 PARTIAL — COMPLETE (2026-06-13). Branch: `macos-arm64`.

---

## Authoritative inputs

| Doc | Section used |
|-----|-------------|
| `docs/AGENT-CONTEXT.md` | Constants (ECB/MMCB/KDP/XLM), boot recipes, instrument caveats, gate tiers |
| `docs/HANDOFF.md` | §M10, §M12 frontier state |
| `docs/planning/superpowers/plans/2026-06-12-m10-cgrp-user-mode.md` | CGRP STUB design, acceptance state |
| `docs/planning/superpowers/plans/2026-06-13-m12-display-pixels.md` | M12 Task C FAIL description |
| `SheepShaver/src/rom_patches.cpp:4004–4120` | CGRP init block (`SS_M10_CGRP`) |
| `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp:1270–1340` | CGRP re-sync on EXT delivery, STUB code |
| `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp:270–335` | DR68K first-dispatch handler, CGRP+0x20 arm |
| `SheepShaver/tools/ss-slot-boot.sh` | Slot boot wrapper |

---

## Codebase facts (re-verify before use)

- **CGRP STUB** at guest RAM 0x68ffc268 (13 words): A7 guard (bltlr if A7<32KB), pushes 6-byte 68k exception frame (SR=0, PC=interrupted r24), sets r24=0x5000ED08, branches to DR_WARM=0x5046E9D8 via CTR. [STATIC from sheepshaver_glue.cpp:1318–1333]
- **DR_WARM** = 0x5046E9D8 — the DR emulator's "warm re-entry" (assumes 68k regs loaded in PPC r8-r24/r1). [STATIC from AGENT-CONTEXT constants]
- **68k interrupt handler** at ROM+0xED08 = 0x5000ED08. Level-dispatch table at 0xED00-0xED07 (8 bytes data, not code). Level-1 entry: `movem.l d0-d7/a0-a3,-(sp)` / `moveq #1,d3` / `bra.s →0x5000ED36`. [STATIC from rom901.bin raw hex]
- **A-traps in handler region** (0x5000ED00-0x5000EF00): **0xAAF3 at 0x5000edbe** and **0xAAF4 at 0x5000ee42** only. (The previously-cited "0xA9A8 at ED06" is WRONG — raw bytes at ROM+0xED06 are `00 00`, not A9A8.) [RAW-ROM RECON 2026-06-13]
- **Crash diagnosis (empirical, 2026-06-13)**: With `SS_M10_CGRP=1 SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`: `[PROBE68K 0x5000ed08 match=1/5]` fires → handler IS reached. Crash at ARM64 JIT host PC 0x1231e8610, guest ea=0x4000d3b7203c (guest 0xd3b7203c), with PPC r13=0xd3b7614d. Guest PPC PC at crash = 0x100259d0 (within DR JIT code cache at 0x10020000+). Bytes at 0x100259dc = 0xDEADBEEF (DR JIT dead-fill, decodes as `stfdu f21,-16657(r13)`). `dec_expiries=4`, `delivered_ext=1`. [PROBE✓ 2026-06-13]
- **Non-determinism**: ~50% of CGRP-on boots park at dec_expiries=5 (interrupt arrives before DR68K fires → *(KDP-0x338)=0 → NK fallback, no 68k delivery). ~50% deliver to handler and crash. Boot is timing-sensitive. [PROBE✓ 2026-06-13]
- **DR JIT code cache**: DR emulator stores its compiled PPC translations in guest RAM at 0x10020000+. The 68k handler block at 0x5000ED08 is compiled fresh on first dispatch to that PC. `stfdu f21,-16657(r13)` = encoding of 0xDEADBEEF = dead-fill sentinel in DR JIT cache. Executing it = cache-boundary overrun. [STATIC inference 2026-06-13]
- **Gate tiers**: per-commit = `make build-ss` + `SS_HARNESS_BATCH=1 make test-jit` (353/353) + `make -C SheepShaver/src/machine test`. Per-task = + plain `make test-jit` + `make e2e-test`. Paravirtual `make e2e` REQUIRED when change touches non-gated shared code. [STATIC from AGENT-CONTEXT]
- **Baseline configuration** (without CGRP): `SS_M11_FB=1 SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1` → dec_expiries=2000+, irq_fired=0, boot stable 30+ seconds. This is the REGRESSION BASELINE. [STATIC from M12 closure]

---

## External prior art — audiocontrol-org `os9-minimal` (DEBUGGING.md)

> Added 2026-06-13 (fork-ecosystem deep-dive). Source: `audiocontrol-org/macemu` @ `os9-minimal`,
> root `DEBUGGING.md`. Full survey context: `docs/FORK-ECOSYSTEM.md` (audiocontrol-org entry).
> **This is a reference/diagnostic frame — NOT code to import.** Read once, then proceed with Task 0.

Another fork independently hit the SheepShaver A-line-dispatch problem from a different angle (a SCSI/MIDI
bridge driving an Akai sampler under **OldWorld** OS 9.0b9). Their relevant finding — and its caveat:

- **A-line traps use a "fast path", not the per-opcode table.** In the ROM-68k-emulator context,
  opcodes `0xA000–0xAFFF` are intercepted by a dedicated A-line fast path that dispatches through the
  **68k *exception* table** (at `r1+0x360`), bypassing the per-opcode dispatch table entirely. They
  proved it: patching the per-opcode-table entry for `$A089` at the readback-verified RAM address
  `0x50450448` had **zero effect** — the entry was never reached. So "the per-opcode table" and "the
  OS trap dispatch table at low-mem `0x0400/0x0624`" are likely the *wrong* tables to interrogate for
  our `0xAAF3/0xAAF4` traps; the **68k exception table** is the one in the path.
- **⚠️ OldWorld-vs-NewWorld inversion (load-bearing).** Their wall is caused by SheepShaver's
  `m68k_excp_tbl` patch (their `rom_patches.cpp:1469`) *deactivating* the 68k exception table — that
  patch IS active on their OldWorld ROM. **In our tree the same patch is SKIPPED for NewWorld**
  (`SheepShaver/src/rom_patches.cpp:2506–2516`; `find_rom_data` misses the pattern in the 9.0.1 parcels
  → `[ROMPATCH] SKIP m68k_excp_tbl`). Same for `ppc_excp_tbl` (`:2428–2438`). So on our boot the 68k
  exception table is **not** deactivated by us — meaning our A-trap failure mode may differ from theirs.
  This *sharpens* Q-A3 rather than answering it: **first determine whether the 68k exception table is
  active in our NewWorld boot at the `0x5000ED08` handler, and whether our A-traps go through it or the
  per-opcode path** — before assuming the trap dispatch table needs populating.
- **Do-not-repeat list (their disproof table, theories A–AL — the ones that bear on us):**
  - **AI** — patching the per-opcode table at the correct RAM address (`0x50450448`): readback OK but never reached (A-line fast path).
  - **Z** — writing a handler into the OS trap table at `0x0400/0x0624`: never reached from Mixed Mode (DEADBEEF marker stayed zero).
  - **X / AC / AK** — SheepShaver emulation ops (`0xFExx`) inside a 68k trap handler: fail in the Mixed-Mode 68k context (error type 12).
  - **AG** — naively removing the `m68k_excp_tbl` patch to restore the exception table: black-screen boot failure (OldWorld). (Moot for us since we don't apply it, but signals the table is load-bearing.)
- **Key addresses they pinned (OldWorld 9.0b9 — re-verify against our 9.0.1 before use):** opcode-table
  ptr `KernelData+0x1074` → `0x50480000`; A-line handler branch target `0x50369660`; ROM 68k emulator
  region `0x50310000–0x50314000`.

**Net for M13:** import no code. Use this to (a) reframe Q-A3 — interrogate the 68k *exception* table /
A-line fast path, not just the low-mem trap table — and (b) avoid the four dead ends above. The
OldWorld/NewWorld patch inversion means our path is genuinely different; treat their findings as a map
of the terrain, not a solution.

---

## Task 0 — BINDING Recon

**Rule:** ALL blocking answers must be filled before ANY implementation task begins.

### Blocking-answer table

| Question | Blocks | Budget | Fallback |
|----------|--------|--------|---------|
| Q-A1: Root cause of DR_WARM crash — is it DR JIT cache overrun, STUB r24 setup, or DR dispatch table? | Task A impl | 2 probe boots + static RE of DR_WARM (0x5046E9D8) | If unclear after 2 boots: treat as "DR dispatch needs non-JIT path for ROM interrupt vectors" → use approach (3) in Task A candidates |
| Q-A2: Is DR_WARM the correct entry for interrupt-driven re-entry, or should a different DR entry be used? | Task A impl | 1 static RE session on DR emulator entries (0x5046e000+) | If no "interrupt-safe" entry found: build minimal HLE stub in trampoline space |
| Q-A3: What are 0xAAF3 and 0xAAF4? Are they OS traps (callable at interrupt time) or Toolbox traps (require trap table)? | Task B | 1 static RE: find the OS trap dispatch table in ROM and check entries 0xF3, 0xF4 | If OS trap table not found at interrupt time: use HLE stubs for 0xAAF3/0xAAF4 in patch_68k |
| Q-A4: What does the QEMU mac99 boot show at the interrupt-handler stage — does 68k interrupt delivery succeed? | Task B and C scope | 1 QEMU session via qemu-rig.sh (boot901, 30s, SS_PROBE_PC at DR_WARM) | If QEMU can't observe this: use behavioral inference from boot progress markers |
| Q-A5: What is the ~33% crash at ea=0x64A05014 (distinct from the 0xd3b7203c DR JIT crash)? In same Wave1 24-bit region? | Task C gate provability | 1 probe boot with CGRP enabled, `SS_JIT_TRACE_RING=1` to capture context around crash | If crash analysis inconclusive: document it as a gate-threat and require 5 boot samples for Task C acceptance (not 3) |

### Q-A1: Root cause of DR_WARM crash

**Question:** Why does execution of the 68k handler at 0x5000ED08 crash at DR JIT dead-fill bytes (0xDEADBEEF at guest PPC 0x100259dc)?

Hypothesis 1: The DR JIT compiles the handler block but leaves a trailing 0xDEADBEEF boundary marker that gets executed because the block length is under-estimated.

Hypothesis 2: DR_WARM dispatches to the WRONG code cache location — the DR's dispatch table for ROM addresses (0x5000ED08) indexes into a stale/uninitialized slot that happens to contain 0xDEADBEEF.

Hypothesis 3: The STUB's `lis r24, 0x5000` + `ori r24, r24, 0xed08` does NOT produce 0x5000ED08 if some prior PPC instruction modified r24's upper bits between the lis and ori. (Unlikely — they're adjacent instructions, but worth verifying.)

**Method:**
```bash
# Boot 1: Probe at DR_WARM entry AND at DR dispatch hash-table lookup to see what PC it dispatches
SheepShaver/tools/ss-slot-boot.sh --label m13-q-a1-a --timeout 25 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M11_FB=1 \
         SS_PROBE_PC=0x5046e9d8:r24,r1 \
         SS_PROBE_68K=0x5000ed08:5;0x5000ed36:3'

# Boot 2: Wider probe - also check 0x5000ed0c, 0x5000ed0e to see which instruction fails
SheepShaver/tools/ss-slot-boot.sh --label m13-q-a1-b --timeout 25 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M11_FB=1 \
         SS_PROBE_68K=0x5000ed08:3;0x5000ed0c:3;0x5000ed0e:3;0x5000ed36:3'
```

Look for: does the probe at 0x5000ed36 fire (meaning the first basic block executed cleanly)? If NOT: crash is in the first block (movem.l/moveq/bra.s). If YES: crash is in the second block (addq.l/movea.l/...).

**Static RE:**
```bash
# Disassemble DR_WARM entry and the first few instructions of the dispatch path
python3 -c "
import struct, capstone
code = open('/Users/Shared/macemu/dumps/rom901.bin','rb').read()
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_BIG_ENDIAN)
base = 0x50000000
start = 0x46e9d8   # DR_WARM offset in rom
for i in md.disasm(code[start:start+64], base + start):
    print(f'  {i.address:#010x}  {i.mnemonic}  {i.op_str}')
"
```

**Addendum (fill before Task A):**

| Item | Answer | Evidence tag |
|------|--------|-------------|
| Does probe at 0x5000ed36 fire? | **NO** — only 0x5000ed08 fires (match=1/3, all 3 baseline boots). Crash is in block 1. | [PROBE✓ 2026-06-13 baseline] |
| If crash is in block 1 (movem.l): which instruction? | Not a 68k instruction — DR_WARM dispatch jumps to a **garbage handler address** (deadfill 0xDEADBEEF at DR cache 0x100259dc) because **r29 is clobbered**. The movem.l handler is fine; the dispatch *target* is wrong. | [PROBE✓ 2026-06-13] |
| DR_WARM first instructions — does it do a cache lookup or direct branch? | **DR_WARM IS the 68k opcode dispatch loop**: `lha r27,0(r24)` (fetch opcode) / `rlwimi r29,r27,3,13,28` (handler = r29 dispatch-base \| opcode<<3) / `mtctr r29` / `lhau r27,2(r24)` / `bctr`. Handler addr is computed from **r29**, not a table lookup. | [DISASM✓ 2026-06-13] |
| Is r24 = 0x5000ED08 confirmed at DR_WARM entry (SS_PROBE_PC)? | **YES.** Normal-exec probe at 0x5046e9d8: `r24=0x5000ed08 r29=0x17ffeb20 r30=0x17ffeb18 r1=0x17ffe9fa`. Crash addr 0x100259dc ∉ 0x17ffxxxx → r29 was corrupted at the delivery entry. | [PROBE✓ 2026-06-13] |

**ROOT CAUSE (Q-A1 closed):** DR_WARM is the per-opcode dispatch loop; it computes the handler from **r29 (dispatch base, normally 0x17ffeb20)**. The CGRP STUB enters DR_WARM after the NK exception path has **clobbered r29/r30 and the D0-D7/A0-A6 GPRs**; baseline STUB restores only r24 and r1. → dispatch jumps to garbage (deadfill) → SIGTRAP. **FIX = restore the DR register set (r1, r8-r23, r24, r29, r30) before `bctr DR_WARM`** (the stashed agent's SAVE-area design, on the *baseline* plumbing — NOT the ROM relocation). Candidate A-1 is VOID: there is no interrupt entry-vector; 0x5046e8c0 is the 68k opcode-dispatch trampoline table (`b`/`twui` slots), and DR_WARM is the correct entry.

---

### Q-A2: Correct DR re-entry point for interrupt delivery

**Question:** Is DR_WARM the right entry point, or does the DR have a separate "interrupt injection" entry that correctly handles re-entry during ongoing 68k execution?

The DR emulator has several entry points in its entry-vector table at 0x5046e8c0 (16 slots). One of these may be designed specifically for interrupt delivery (not "warm continuation").

**Method:**
```bash
# Disassemble all 16 DR entry vector slots (at 0x5046e8c0, 16×4 = 64 bytes of pointers)
python3 -c "
import struct, capstone
code = open('/Users/Shared/macemu/dumps/rom901.bin','rb').read()
base = 0x50000000
table_off = 0x46e8c0
print('DR entry-vector table at 0x5046e8c0:')
for i in range(16):
    off = table_off + i*4
    ptr = struct.unpack_from('>I', code, off)[0]
    print(f'  slot[{i:2d}] = {ptr:#010x}')
print()

# Disassemble each entry to see what it does
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_BIG_ENDIAN)
for i in range(16):
    ptr = struct.unpack_from('>I', code, table_off + i*4)[0]
    if ptr < base or ptr > base + len(code): continue
    off = ptr - base
    print(f'slot[{i}] at {ptr:#010x}:')
    for ins in list(md.disasm(code[off:off+24], ptr))[:6]:
        print(f'  {ins.address:#010x}  {ins.mnemonic}  {ins.op_str}')
    print()
"
```

Compare slot semantics with Apple's DR documentation (re-verified from AGENT-CONTEXT.md constants: dispatch-table base 0x50480000, emulator-code base 0x50460000).

**Addendum (fill before Task A):**

| Item | Answer | Evidence tag |
|------|--------|-------------|
| Is there a DR entry point for "deliver interrupt" (not warm resume)? | **NO.** The plan's premise was wrong. 0x5046e8c0 is NOT a 16-slot pointer table — it is the 68k opcode-dispatch trampoline: `b` instructions (slots 0-3,5 → 0x50429d00/0x50429d80/0x5046fb00/0x5046fc00/0x5046fd00) with unimplemented slots = `twui r31,N` traps. DR_WARM (0x5046e9d8) is the dispatch loop itself. | [PROBE+DISASM✓ 2026-06-13] |
| Which slot does M10's STUB branch to (DR_WARM = which slot?)? | DR_WARM is not a slot — it's the loop entry. STUB branches to it directly. Correct target. | [DISASM✓ 2026-06-13] |
| Is there a safer entry that compiles the handler block first? | N/A — handler already compiled (works on every normal boot). Fix is register restoration, not entry selection. (Note: 0x50429xxx is DR trampoline space — the region the stashed rewrite chose for its ROM STUB.) | [DISASM✓ 2026-06-13] |

**Note:** the ROM dump (`rom901.bin`, 4 MB = 0x50000000-0x50400000) does NOT contain the DR region 0x5046xxxx / 0x50429xxx — it is runtime-resident in guest RAM. Q-A2's static-RE recipe (disassemble rom901.bin at offset 0x46e8c0) is OUT OF BOUNDS; use runtime probe dumps (`SS_PROBE_PC=<hotPC>:[0xADDR],...`) instead.

---

### Q-A3: 0xAAF3 and 0xAAF4 trap types and availability at interrupt time

**Question:** Are 0xAAF3 and 0xAAF4 OS traps (dispatched through the OS trap table, initialized early) or Toolbox traps (dispatched through 0x0E00-0x0FFF, populated by System file)? If OS traps, are they available before Mac OS System starts?

In Mac OS 68k, A-line opcodes have a dispatch bit: bit 11 = 1 → OS trap, bit 11 = 0 → Toolbox trap. Both 0xAAF3 and 0xAAF4 have bit 11 = 1 (0xAAF3 >> 11 = 0x15, bit 0 = 1). So they are **OS traps**.

OS trap numbers: 0xAAF3 → OS trap 0xF3 (243 decimal); 0xAAF4 → OS trap 0xF4 (244 decimal).

The OS trap dispatch table is at a different address from the Toolbox trap table. In 68k Mac OS 9, the OS trap table is typically at a ROM-defined location initialized early.

**Method:**
```bash
# Find the OS trap dispatch table in the ROM
# In 68k Mac: trap dispatch for OS traps goes through an A-line handler
# Look at the 68k A-line exception vector (0x0028 in 68k vector table → 68k opcode A000)
# The ROM A-line handler is what dispatches OS traps
# Find the A-line handler entry in the ROM by looking at what the ROM puts in vector 0x0028
# In the current boot, low memory 0x0028 should have been set to a ROM handler address

# Also: search ROM for a jump table that has entries at indices 0xF3 and 0xF4
python3 -c "
import struct, capstone
code = open('/Users/Shared/macemu/dumps/rom901.bin','rb').read()
base = 0x50000000

# OS trap 0xF3 = trap number 243 = 0xF3
# Look for patterns like a table of pointers near where OS traps 0xF0-0xFF might live
# Scan for a sequence of addresses that look like a trap table
print('Searching for OS trap dispatch infrastructure...')
# Pattern: look for 0xAAF3 as a trap dispatch in a jump table
# Or find the A-line handler entry in ROM (usually near 0x50000000 + low vector setup)
for off in range(0, min(0x100000, len(code))-3, 2):
    word = struct.unpack_from('>H', code, off)[0]
    if word == 0xAAF3:
        ctx = code[off-8:off+4]
        print(f'  0xAAF3 handler ref at ROM+{off:#06x}: {\" \".join(f\"{b:02x}\" for b in ctx)}')
" 2>&1 | head -20
```

**Addendum (fill before Task B):**

| Item | Answer | Evidence tag |
|------|--------|-------------|
| 0xAAF3: OS trap or Toolbox? | OS trap (bit 11 = 1) | [STATIC 2026-06-13] |
| 0xAAF4: OS trap or Toolbox? | OS trap (bit 11 = 1) | [STATIC 2026-06-13] |
| OS trap table initialized at ROM init (before System)? | TODO | |
| OS trap entries 0xF3/0xF4 — ROM default or System-loaded? | TODO | |

---

### Q-A5: What is the ~33% crash at ea=0x64A05014?

**Question:** The run log shows a ~33% crash at ea=0x64A05014 (distinct from the 0xd3b7203c DR JIT crash). Is this a second instance in the same 24-bit DR region (Wave1 fixed one; another remains), or an independent bug that blocks the pixel gate?

**Method:**
```bash
SheepShaver/tools/ss-slot-boot.sh --label m13-qa5 --timeout 30 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M11_FB=1 \
         SS_JIT_TRACE_RING=1 SS_JIT_WATCH_DUMPS=0'
```

After the boot, grep the ring log for ea=0x64A05014 or nearby crashes. Cross-check: guest address 0x64A05014 — does it fall in the 24-bit Wave1 range (0x64xxxxxx is 6-bit region, above 0xFFFFFF → NOT a 24-bit address). This crash is distinct from the Wave1 fix domain.

**Addendum (fill before Task C gate):**

| Item | Answer | Evidence tag |
|------|--------|-------------|
| Is ea=0x64A05014 in DR JIT region (0x10020000+)? | r13 corruption gives garbage ea — verify r13 range | |
| Does this crash appear BEFORE or AFTER the CGRP delivery? | TODO | |
| If after delivery: is it the same 0xDEADBEEF/stfdu pattern (r13 garbage)? | TODO | |
| Scope: does it block irq_fired≥1, or only the pixel gate? | TODO | |

**Budget:** 1 probe boot + ring analysis. If inconclusive: document as gate-threat, require 5 boot samples for Task C acceptance (not 3).

---

### Q-A6: A7 register identity — r1 or r23?

**Question:** `docs/AGENT-CONTEXT.md` says A7=r1, but `sheepshaver_glue.cpp:478` places a_regs at gpr[16] with 8 entries (A0=r16 through A7=r23). The CGRP STUB's A7 guard uses one of these — which is correct?

**Method (static):**
```bash
grep -n 'a_regs\|A7\|r23\|a7_reg\|AREG\|sp_reg' \
  SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp | head -30
```

Also check:
```bash
grep -n 'a_regs\|AREG' SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-cpu.cpp | head -20
```

**Live probe (if static is ambiguous):**
```bash
SheepShaver/tools/ss-slot-boot.sh --label m13-qa6-a7 --timeout 25 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M11_FB=1 \
         SS_PROBE_68K=0x5000ed08:r1,r23'
```

The probe registers the DR-emulator PPC state at 68k dispatch. The valid-looking stack pointer (aligned, plausible MAC OS stack range ~0x3C000000) identifies A7.

**Addendum (fill before Task A implementation):**

| Item | Answer | Evidence tag |
|------|--------|-------------|
| a_regs base gpr index in sheepshaver_glue.cpp | gpr[16] (A0=r16 .. A7=r23) per glue.cpp:478 — but this is the EMUL_OP/mixed-mode convention, NOT the DR dispatch convention. | [STATIC 2026-06-13] |
| A7 = gpr[?] | **In the DR dispatch context: A7 = r1.** Probe at DR_WARM: `r1=0x17ffe9fa` = valid 68k stack. (a_regs[7]=gpr[23] applies only in the EMUL_OP path.) | [PROBE✓ 2026-06-13] |
| STUB A7 guard load offset — is it loading from r1 or r23? | STUB loads A7 from KDP+4 (`*(0x68FFE004)`) into r5, sets r1 = r5-6. Correct: r1 IS the DR A7. | [STATIC+PROBE✓ 2026-06-13] |
| Does the guard correctly protect against a garbage stack pointer? | Guard `bltlr if A7<32KB` is sound; baseline STUB delivery is confirmed (probe fires). A7 handling is NOT the bug — r29 restoration is. | [PROBE✓ 2026-06-13] |

**Budget:** 1 static grep + 1 probe boot. BINDING: if A7=r23 (not r1), the STUB guard needs fixing before any Task A code changes.

---

### Q-A4: QEMU oracle — does 68k interrupt delivery succeed on mac99?

**Question:** On a working mac99 boot (QEMU), does the 68k interrupt handler at 0x5000ED08 execute successfully? What boot progress markers are visible?

**Method:**
```bash
# Boot QEMU rig with 30s timeout
bash SheepShaver/tools/qemu-rig.sh --timeout 50 &
sleep 5  # wait for QEMU to start

# Probe for DR_WARM + check if any 68k-world messages appear
python3 SheepShaver/tools/qemu-mon.py --sock /tmp/qemu-rig-*/mon.sock 'info registers' 2>/dev/null | head -20

# Let it run and check output
wait
```

CAVEAT (load-bearing): QEMU mac99 is behavioral oracle only. QEMU MacIO is at 0x80000000, not 0xF3000000. Never cite QEMU MMIO addresses.

**Addendum (fill before Task C):**

| Item | Answer | Evidence tag |
|------|--------|-------------|
| Does QEMU boot reach 68k world? | TODO | |
| Does QEMU show display output (pixels)? | TODO | |
| Any blocking walls QEMU hits that we haven't hit yet? | TODO | |
| QEMU rig git SHA used (so findings are repeatable) | TODO — record with `git -C <qemu-src> rev-parse HEAD` | |

---

## Implementation tasks

**ALL tasks are gated DEFAULT-OFF. Baseline (`SS_M11_FB=1 SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1`) must stay byte-identical until acceptance.**

> **BINDING CHECKPOINT:** Before Step A-4 (first code change), ALL addendum rows in Q-A1, Q-A2, and Q-A6 must be filled. Q-A3, Q-A4, Q-A5 may remain partial if their tasks (B, C) are not yet reached — but their blocking rows must be filled before those tasks begin. If any row is still TODO after the prescribed probe budget, the fallback answer from the blocking-answer table is the BINDING answer to proceed.

---

### Task A — implementation attempt + ARCHITECTURAL WALL (2026-06-13, session 2)

**Approach tried (direct injection, NK bypass):** at an EXT-pending poll, build the 6-byte
68k exception frame, set gpr(24)=0x5000ED08, resume DR_WARM — keeping the live DR register
file (so r29/r30/r8-r23 are never NK-clobbered). Three gate variants tried:

1. **Inject at any DR-range PC (0x5046e000..0x50500000):** never fired — the EXT pends while
   executing NK code (cur_pc ~0x5031xxxx), not DR code.
2. **Defer EXT until a DR-range poll, then inject:** FIRED but **SIGSEGV**. Injected at
   cur_pc=0x504a8608 (inside a DR helper, *mid-68k-instruction*). There r8-r23/r29 are
   **transient PPC scratch, not the coherent 68k register file** → dispatch computed a garbage
   handler (r29 0x17ffeb20 → 0x17fa4738) → executed DR data @ 0x17ffe950 → SIGSEGV.
3. **Inject only at the coherent boundary cur_pc==0x5046e9d8 (DR_WARM entry):** **never fired.**
   JIT block-chaining means the interrupt poll (check_spcflags, runs only at *unchained* block
   entries) never lands at DR_WARM — it is entered once then chained (probe shows visit=1 only).

**WALL (the real Task A constraint):** safe 68k interrupt injection requires hitting the DR's
*between-instruction* boundary (DR_WARM, where the 68k register file is coherent in gprs), but
JIT block-chaining makes that boundary unreachable from the interrupt poll. The 68k register
file is coherent ONLY at DR_WARM; everywhere else in DR-range it is transient. r29/r30 ARE
stable across boots (0x17ffeb20/0x17ffeb18) but only meaningful AT that boundary. [PROBE✓/CRASH✓ 2026-06-13]

**Promising lead for next session — DR has state-saving entry vectors.** Dispatch-table slots
2/3/5 (targets 0x5046fb00 / 0x5046fc00 / 0x5046fd00, all identical) are **register-save
prologues**: `lwz r1,0x2804(0)` (DR context block ptr) then `stw r6..r11,0x13c..(r6)` — they
snapshot the register file into the context block at `*(0x2804)`. These look like the DR's own
exception/interrupt entries (Candidate A-1 reborn, correctly this time). Next step: RE these
three entries + the context block at `*(0x2804)` to learn whether vectoring to one (instead of
DR_WARM) performs the save/restore that makes injection safe from an arbitrary PC.

**Three forward options (architectural decision required):**
- **(a) NK CGRP path + context restore** — deliver via the NK (safe-point by design), but set up
  the CGRP context save area so the NK preserves/restores DR state. This is the original deep-RE
  wall (summary report H1/H2). Most "correct", most expensive.
- **(b) DR_WARM ROM-patch interrupt check** — patch a poll/inject at 0x5046e9d8 so every
  between-instruction boundary checks pending EXT. Architecturally clean (mirrors real 68k
  instruction-boundary IRQ checking) but invasive; must coexist with chaining (likely needs
  no-chain for that block).
- **(c) DR state-save entry vector** — vector to slot 2/3/5 (above) which snapshots the register
  file itself, making injection safe from any PC. Cheapest IF the RE confirms it.

Source change for this attempt was REVERTED (gated-off, non-working). Baseline intact (harness 353/353).

---

### Task A: Fix DR_WARM interrupt re-entry crash

**Goal:** CGRP STUB delivers to 68k handler; handler executes past the first basic block; `delivered_ext=1` without SIGSEGV; `dec_expiries` begins increasing (not stuck at 4-5).

**Gate (pre-implementation):** All Q-A1 and Q-A2 answers filled.

**Approach candidates (ranked: understand root cause first, bypass only as last resort):**

**Candidate A-1 (architectural — preferred):** Identify the correct DR entry point for interrupt re-entry (the DR has 16 entry vector slots at 0x5046e8c0; one should be designed for "inject interrupt and restart"). Use that slot in the STUB instead of DR_WARM. This fixes the JIT cache overrun by using the right entry point rather than patching around it.

**Candidate A-2 (cache pre-warm — if A-1 slot doesn't exist):** Force the DR to pre-compile the handler block at 0x5000ED08 before the first interrupt delivery. In the first-DR68K-dispatch hook (ppc-cpu.cpp), after arming CGRP+0x20, call the DR's compile function for 0x5000ED08. This ensures the JIT cache is warm before DR_WARM is called for real.

  **Safety caveat (BINDING before implementing A-2):** Calling the DR compile function from outside the DR's normal dispatch context may corrupt DR internal state. Q-A2 must confirm: (a) the compile function is callable standalone, (b) it does not require the DR's lock or context to be set. If this cannot be confirmed by static RE, use A-3 instead.

**Candidate A-3 (bypass — only if A-1 and A-2 both fail):** Build a minimal 68k interrupt-delivery shim in trampoline space that handles the register save itself (using the known PPC register mapping), bypassing the DR JIT for the preamble. This is a surgical shortcut — use it only if the DR dispatch architecture has no clean interrupt-injection entry.

**DO NOT** use A-3 until A-1 and A-2 have been tried and documented as failed. Per standing feedback rules: architectural fix before bypass.

**Files:**
- Modify: `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp:1318–1333` (STUB code)
- Modify: `SheepShaver/src/rom_patches.cpp:4090–4106` (STUB code in rom_patches)

- [ ] **Step A-1: Fill Q-A1 answers by running the probe boots**

```bash
cd /path/to/macemu-jit
SheepShaver/tools/ss-slot-boot.sh --label m13-qa1-a --timeout 25 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M11_FB=1 \
         SS_PROBE_PC=0x5046e9d8:r24,r1 SS_PROBE_68K=0x5000ed08:3;0x5000ed36:3'
```

Expected: either both probes fire (first block executes → crash in second block) or only 0x5000ed08 fires (crash in first block). Record in Q-A1 addendum.

- [ ] **Step A-2: Fill Q-A2 by disassembling DR entry table**

```bash
python3 -c "
import struct, capstone
code = open('/Users/Shared/macemu/dumps/rom901.bin','rb').read()
base = 0x50000000
table_off = 0x46e8c0
md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_BIG_ENDIAN)
for i in range(16):
    ptr = struct.unpack_from('>I', code, table_off + i*4)[0]
    if ptr < base or ptr > base + len(code): continue
    off = ptr - base
    print(f'slot[{i}] at {ptr:#010x}:')
    for ins in list(md.disasm(code[off:off+24], ptr))[:4]:
        print(f'  {ins.address:#010x}  {ins.mnemonic}  {ins.op_str}')
"
```

Record slot function in Q-A2 addendum.

- [ ] **Step A-3: Select approach (A-1, A-2, or A-3) based on Q-A1/Q-A2 answers**

Decision criteria:
- If DR has an interrupt-safe entry AND r24 at DR_WARM = 0x5000ED08 confirmed: → A-1 (change the entry point)
- If crash is in movem.l/first block AND no better DR entry: → A-2 (skip to 0x5000ed36)
- If DR_WARM is structurally incompatible with interrupt: → A-3 (HLE shim)

Write the chosen approach in an addendum here before proceeding.

- [ ] **Step A-4: Implement chosen approach in sheepshaver_glue.cpp STUB code**

The STUB code at `STUB_ADDR_EXT` is written by `sheepshaver_glue.cpp:1318–1333`. For A-1 (different entry), change word 13 (offset 48-60, the DR_WARM address):

```c
// For A-1: replace DR_WARM = 0x5046e9d8 with the interrupt entry at SLOT_N
const uint32_t DR_INTERRUPT_ENTRY = 0x5046XXXX;  // fill from Q-A2
WriteMacInt32(STUB_ADDR_EXT + 48, 0x3C005046u);  // lis r0, 0x5046 (same)
WriteMacInt32(STUB_ADDR_EXT + 52, 0x6000XXXXu);  // ori r0, r0, 0xXXXX  (new)
```

Make the SAME change in `rom_patches.cpp:4102–4103` (the initial write at boot time).

For A-2 (skip to 0x5000ed36): change the target PC in the STUB:
```c
// Change ori r24, r24, 0xed08 → ori r24, r24, 0xed36 (skip movem.l preamble)
// BUT: must pre-initialize d3=1 before skipping the moveq
// Add: li r11, 1  (r11 = PPC reg for D3 in DR)   before the lis r24 line
WriteMacInt32(STUB_ADDR_EXT + 36, 0x39600001u);  // li r11, 1  (D3=1)
WriteMacInt32(STUB_ADDR_EXT + 40, 0x3F005000u);  // lis r24, 0x5000
WriteMacInt32(STUB_ADDR_EXT + 44, 0x6318ED36u);  // ori r24, r24, 0xed36
// push the saved reg space on A7 (the handler expects movem.l to have run)
// SIMPLE VERSION: just skip the movem.l entirely (handler will use uninitialized
// save area) — safe since the RTE at end of handler restores nothing from stack
// (the handler uses its own frame for scratch, not for register restore)
```

- [ ] **Step A-5: Run inner gates**

```bash
cd SheepShaver && make build-ss
SS_HARNESS_BATCH=1 make test-jit
make -C src/machine test
```

Expected: `353/353`, machine tests ALL PASS.

- [ ] **Step A-6: Run diagnostic boot with gate**

Run ≥3 boots. The gate requires ≥2/3 meet ALL of the following conditions:

```bash
for i in 1 2 3; do
  SheepShaver/tools/ss-slot-boot.sh --label "m13-task-a-gate-$i" --timeout 45 \
    --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M11_FB=1 \
           SS_PROBE_68K=0x5000ed08:3;0x5000ed36:3;0x5000ed3e:3'
  echo "Boot $i complete"
done
```

After each boot:
```bash
grep -E 'PROBE68K|SIGSEGV|dec_expiries|delivered_ext' \
  /tmp/ss-slots/slot*/runs/m13-task-a-gate-*/boot.log
```

**Gate (FALSIFIABLE):** For ≥2 of 3 boots that show `[PROBE68K 0x5000ed08 match`:
- (a) No `SIGSEGV` line in that boot's log, AND
- (b) `dec_expiries` ≥ 20 in the `[NW-PROG]` line (baseline was 4-5; ≥20 means the handler returned and the NK resumed normal scheduling)

- [ ] **Step A-7: Run nw-northstar observe line**

```bash
cd SheepShaver && make nw-northstar 2>&1 | grep -E 'NW-PROG|verdict'
```

Quote the verdict. Any `REGRESSED` = stop and re-investigate.

- [ ] **Step A-8: Commit**

```bash
git add SheepShaver/src/rom_patches.cpp SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp
git commit -F- <<'EOF'
fix(m13): fix DR re-entry crash in CGRP interrupt delivery to 68k handler

CGRP stub correctly delivers to 0x5000ED08, but DR_WARM entered the DR emulator
JIT at an inconsistent cache boundary (0xDEADBEEF fill → stfdu crash). [describe approach: A-1/A-2/A-3]

Gate: SS_M10_CGRP=1 boot no longer SIGSEGVs on first EXT delivery.
Harness 353/353. Machine tests ALL PASS.
EOF
```

---

### Task B: Handle A-traps 0xAAF3 and 0xAAF4 in the interrupt handler

**Goal:** The interrupt handler at 0x5000ED08 executes through to the 0xAAF3 trap at 0x5000edbe and the 0xAAF4 trap at 0x5000ee42 without hanging or crashing.

**Gate (pre-implementation):** Task A complete (no SIGSEGV), Q-A3 answers filled.

If Q-A3 shows 0xAAF3/0xAAF4 are OS traps initialized by ROM (before System file), SKIP this task — they are already available.

If Q-A3 shows they require System startup (uninitialized at interrupt time), implement minimal HLE stubs.

**What 0xAAF3 and 0xAAF4 do (from context):**
- 0xAAF3: called just before determining interrupt source level — likely `EnterInterrupt` or a CGRP coordination primitive
- 0xAAF4: called after delivering to a handler — likely `ExitInterrupt`
- Both use register arguments: at 0xAAF3 call site, d2=stack-frame-ptr, d0=interrupt-level selector; at 0xAAF4, d2=level

Minimal stubs (if needed): nop-and-return. The interrupt handler uses them for bookkeeping; skipping during early boot allows the delivery to proceed to the actual interrupt routing.

**Files:**
- Modify: `SheepShaver/src/rom_patches.cpp` — add M13_ATRAP section in `patch_68k()`

- [ ] **Step B-1: Verify whether 0xAAF3/0xAAF4 are available at interrupt time**

Run a boot with `SS_PROBE_68K=0x5000edbe:3` (the 0xAAF3 site) to see if it fires or if something else kills the boot first:

```bash
SheepShaver/tools/ss-slot-boot.sh --label m13-task-b-pre --timeout 45 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M11_FB=1 \
         SS_PROBE_68K=0x5000edbe:3;0x5000ee42:3'
```

If 0x5000edbe probe fires: the handler reaches 0xAAF3; now check if it's handled or causes a hang.
If 0x5000edbe probe does NOT fire: still an earlier crash (Task A not fully complete yet).

- [ ] **Step B-2: If stubs needed — add patch_68k pattern for 0xAAF3 and 0xAAF4**

In `SheepShaver/src/rom_patches.cpp`, in the `patch_68k()` function, add an `SS_M13_ATRAP` guard block after the existing `SS_M10_CGRP` block:

```c
// SS_M13_ATRAP: install no-op stubs for OS traps 0xAAF3/0xAAF4 in the CGRP
// interrupt handler.  These are called at 0x5000edbe and 0x5000ee42.
// At early boot (before System file loads) the OS trap table may not have
// live entries; HLE stubs let the handler execute to completion.
// Gate: SS_M13_ATRAP=1 (default OFF). Enable alongside SS_M10_CGRP.
if (ROMType == ROMTYPE_NEWWORLD && getenv("SS_M13_ATRAP") &&
    strcmp(getenv("SS_M13_ATRAP"), "0") != 0) {
    // Patch 0xAAF3 at 0x5000edbe: replace with nop + (implicit fall-through)
    // The 2-byte trap word is replaced with 0x4E71 (nop)
    if (find_rom_data(0xedbe, 0xedc0, 0xAAF3) != 0) {
        WriteMacInt16(ROMBase + 0xedbe, 0x4E71);  // nop (was 0xAAF3)
        fprintf(stderr, "[M13-ATRAP] patched 0xAAF3 at 0x5000edbe → nop\n");
    }
    // Patch 0xAAF4 at 0x5000ee42: replace with nop
    if (find_rom_data(0xee42, 0xee44, 0xAAF4) != 0) {
        WriteMacInt16(ROMBase + 0xee42, 0x4E71);  // nop (was 0xAAF4)
        fprintf(stderr, "[M13-ATRAP] patched 0xAAF4 at 0x5000ee42 → nop\n");
    }
}
```

Note: `find_rom_data(start_offset, end_offset, word)` — re-verify the exact signature from the surrounding code in `rom_patches.cpp` before writing this. The existing M10 block uses `WriteMacInt32`/`WriteMacInt16` directly; use the same pattern.

- [ ] **Step B-3: Run inner gates**

```bash
cd SheepShaver && make build-ss
SS_HARNESS_BATCH=1 make test-jit
make -C src/machine test
```

Expected: 353/353, ALL PASS.

- [ ] **Step B-4: Diagnostic boot to confirm handler survives A-traps**

```bash
SheepShaver/tools/ss-slot-boot.sh --label m13-task-b-gate --timeout 60 \
  --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M11_FB=1 SS_M13_ATRAP=1 \
         SS_PROBE_68K=0x5000edbe:3;0x5000ee42:3;0x5000ee58:3'
```

After the boot completes:
```bash
LOG=$(ls -t /tmp/ss-slots/slot*/runs/m13-task-b-gate*/boot.log 2>/dev/null | head -1)
grep -E 'PROBE68K|irq_fired|SIGSEGV|NW-PROG' "$LOG"
```

**Gate (FALSIFIABLE):**
- `[PROBE68K 0x5000edbe match` appears (handler reaches 0xAAF3 site), AND
- `irq_fired=[1-9]` in the `[NW-PROG]` line (extract the integer and verify ≥1), AND
- No `SIGSEGV` in the log.

The handler at 0x5000ee58 does the interrupt mask restore + RTE path; if that probe fires too, the handler ran to its exit — record it but not required for Task B gate.

- [ ] **Step B-5: Commit**

```bash
git add SheepShaver/src/rom_patches.cpp
git commit -F- <<'EOF'
fix(m13): HLE stubs for OS traps 0xAAF3/0xAAF4 in CGRP interrupt handler

ROM+0xEDBE (0xAAF3) and ROM+0xEE42 (0xAAF4) are called before the Mac OS trap
table is populated at early boot. Replace with nop to allow the interrupt handler
to execute to completion without hanging.

Gate: SS_M13_ATRAP=1. irq_fired>=1 observed. Harness 353/353.
EOF
```

---

### Task C: irq_fired gate + pixel gate (QuickDraw init)

**Goal:** After Task A + B, confirm `irq_fired≥1` is stable across 3 boots. Then pursue `[FB-DIRTY] non_zero_pixels>0` — this requires QuickDraw to initialize, which requires repeated interrupt delivery (VBL ticks).

**Note:** If irq_fired≥1 is confirmed but `[FB-DIRTY]=0` after 180s, Task C establishes M13's PARTIAL close-out boundary and scopes M14. Do not hold M13 open indefinitely for the pixel gate if there is a new structural wall.

**Files:**
- Modify: None (this is a diagnostic/acceptance task, not implementation)

- [ ] **Step C-1: Run 3 all-on boots to establish irq_fired≥1 stability**

```bash
for i in 1 2 3; do
  SheepShaver/tools/ss-slot-boot.sh \
    --label "m13-accept-$i" --timeout 180 \
    --env 'SS_NW_PIC=1 SS_NW_IRQ_CONSUME=1 SS_M10_CGRP=1 SS_M11_FB=1 SS_M13_ATRAP=1' \
    --expect 'irq_fired=[1-9]'
  echo "Boot $i exit=$?"
done
```

Gate: ≥2/3 boots pass `--expect irq_fired=[1-9]`. (Non-determinism means 3/3 is too strict.)

- [ ] **Step C-2: Check for [FB-DIRTY] non_zero_pixels**

In the same 3 boot logs:
```bash
grep -E 'FB-DIRTY|non_zero_pixels' /tmp/ss-slots/slot*/runs/*/boot.log | tail -10
```

If `non_zero_pixels>0` appears: M13 COMPLETE. Proceed to acceptance.

If `non_zero_pixels=0` after 180s: record the new frontier wall (what boots are happening? What is dec_expiries, what is irq_fired count?), update ROADMAP as M13 PARTIAL, scope M14.

- [ ] **Step C-3: Run per-task gates**

```bash
cd SheepShaver && make test-jit && make e2e-test
```

Expected: test-jit all pass, e2e-test all pass.

- [ ] **Step C-4: Run paravirtual regression gate**

Check whether any new code from Tasks A or B is outside an env gate:
```bash
# Verify every new line in rom_patches.cpp is inside the gate block
grep -n 'SS_M13_ATRAP\|SS_M10_CGRP' SheepShaver/src/rom_patches.cpp | tail -20
# Verify every new line in sheepshaver_glue.cpp STUB edit is inside gate
grep -n 'SS_M10_CGRP\|SS_M13' SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp | tail -20
```

If ALL new lines are inside `SS_M10_CGRP` or `SS_M13_ATRAP` env checks: the structural-inertness argument applies. Add to the Task C commit message: "All new code inside SS_M10_CGRP/SS_M13_ATRAP env gates; paravirtual path unchanged."

If ANY new line is outside a gate: run `cd SheepShaver && make e2e` and confirm PASS before committing.

---

### Task D: Acceptance — flip defaults and docs close-out

**Goal:** If Task C acceptance criteria are met, fold gates into the newworld cluster (or keep gated-off-green with documentation). Update all trackers.

**Note:** Gate flip policy — only flip a gate to default-ON after ≥3 clean boots with no regression. If the boot is still non-deterministic with the new gates, keep gated-off-green and document.

**Files:**
- Modify: `LEARNINGS.md`, `docs/planning/ROADMAP.md`, `docs/AGENT-CONTEXT.md`, `CHANGELOG.md`, `SheepShaver/docs/DIAGNOSTICS.md`, `JIT-STATUS.md`, `CONTRIBUTING.md`, this plan doc

- [ ] **Step D-1: Update LEARNINGS.md**

Add a new top entry (above existing 2026-06-13 entry):
```markdown
## 2026-06-13 — M13: DR_WARM re-entry crash + A-trap fixes

**DR_WARM crash (Task A):** The CGRP STUB branched to DR_WARM (0x5046E9D8) for interrupt
re-entry. The DR JIT had compiled a cache block but the entry point was inconsistent —
execution hit 0xDEADBEEF fill bytes at DR JIT cache boundary, crashing as
`stfdu f21,-16657(r13)`. [Describe the fix: which approach A-1/A-2/A-3 was used.]

**A-trap bootstrapping (Task B):** ROM interrupt handler at 0x5000ED08 calls OS traps 0xAAF3
(at 0x5000edbe) and 0xAAF4 (at 0x5000ee42) before the Mac OS OS trap table is populated.
These were patched to nop (SS_M13_ATRAP=1) allowing the handler to complete.

**CORRECTION of prior session claim:** "A9A8 at ROM+0xED06" was wrong. Raw bytes at ROM+0xED06
are 0x00 0x00. The real traps are 0xAAF3/0xAAF4. Never trust session narrative over raw bytes.
```

- [ ] **Step D-2: Update ROADMAP.md**

Update header status block and add M13 entry. If pixel gate passed: "M13 COMPLETE." If only irq_fired≥1: "M13 PARTIAL." Add M14 entry if needed.

- [ ] **Step D-3: Update AGENT-CONTEXT.md**

Update "Current frontier" section, "Where things are" (still says "M11"), and env-gate state.

- [ ] **Step D-4: Update CHANGELOG.md**

Add M13 session entry with: what was wrong (DR_WARM crash, A-trap identity correction), what was fixed, acceptance numbers, gate config.

- [ ] **Step D-5: Update DIAGNOSTICS.md knob reference**

Add `SS_M13_ATRAP=1` to the env var reference table.

- [ ] **Step D-6: Update JIT-STATUS.md and CONTRIBUTING.md**

In `JIT-STATUS.md`, update the Machine Layer section: add M13 line (COMPLETE or PARTIAL) and new gate `SS_M13_ATRAP`.

In `CONTRIBUTING.md`, add `SS_M13_ATRAP=1` to the "New World gates" table so the next contributor knows about it. Also check if the PARTIAL close-out branching rule needs updating (if M13 is PARTIAL, note that M14 is now the pixel-gate scope).

- [ ] **Step D-7: Update this plan doc status block**

Change to `COMPLETE` or `PARTIAL COMPLETE` at the top.

- [ ] **Step D-8: Commit docs**

```bash
git add LEARNINGS.md docs/planning/ROADMAP.md docs/AGENT-CONTEXT.md CHANGELOG.md \
        SheepShaver/docs/DIAGNOSTICS.md JIT-STATUS.md CONTRIBUTING.md \
        docs/planning/superpowers/plans/2026-06-13-m13-atrap-bootstrap.md
git commit -F- <<'EOF'
docs(m13): close-out — DR_WARM crash fix + A-trap bootstrapping, M14 scoped
EOF
```

---

## Stop-rule

Stop and re-scope if:

1. **Task A: SIGSEGV persists on all three approaches (A-1, A-2, A-3)** — the DR re-entry mechanism is structurally incompatible with CGRP interrupt delivery. Re-scope to building a fully-host-side HLE interrupt shim that never enters the DR for the interrupt (signal irq_fired via a counter only, defer pixel-path to M14 with a different interrupt architecture).

2. **Task B: irq_fired stays 0 after A-trap nop patches** — there is a third blocker between the A-trap sites and the RTE. Record the new frontier PC (probe at 0x5000ee58, 0x5000eec0) and stop. Re-scope M13 around that specific wall.

3. **Task C: [FB-DIRTY] is 0 after 3×180s boots** — QuickDraw init requires additional ROM paths beyond the interrupt handler. Record the `irq_fired` count (VBL tick frequency) and identify what QuickDraw init needs (this is likely M14's scope). Close M13 as PARTIAL.

4. **Tempting wrong fix:** Do not replace the CGRP delivery path with a pure-host counter that never exercises the ROM handler. The goal is to have Mac OS's real interrupt handling run, not just increment a counter.

5. **NOP-patch without root cause:** If the DR_WARM crash (Q-A1) is not fully understood (all addendum rows filled), do NOT proceed to Task B's 0xAAF3/0xAAF4 NOP patches. A NOP patch that silences a crash symptom may mask an underlying structural problem with the re-entry path. Root cause must be documented before bypassing with NOP.

---

## Self-review record

**Spec coverage check:**
- ✅ DR_WARM crash: Task A with 3 approach candidates
- ✅ A-trap 0xAAF3/0xAAF4: Task B
- ✅ irq_fired≥1 gate: Task C
- ✅ [FB-DIRTY]>0 gate: Task C (with explicit partial-close if blocked)
- ✅ Docs: Task D
- ✅ Stop-rule covers the tempting-wrong-fix case
- ✅ Correction of "0xA9A8 at ED06" is documented in Task D / LEARNINGS

**Placeholder scan:** No TBD entries in implementation steps. The "fill from Q-A2" in Step A-4 has explicit decision criteria. The A-2 approach has a comment noting the HLE skip trade-off.

**A-trap encoding re-check:** 0xAAF3 = `ori.w #0xAAF3, d0` would be 0xAAF3 if it were a data word, but as an A-line opcode the 68k fetches it as an instruction. The encode: top nibble = A = A-line; bit 11 = 1 = OS trap; low 8 bits = 0xF3. Confirmed: both are OS traps, bit-11 = 1.

**Tensions for red team:**
- T1: The STUB approach (A-2) pre-initializes D3=1 by writing r11=1, but r11 = D3 in the DR register map. If the DR JIT uses a DIFFERENT register for D3 in its compiled blocks (vs. the interpreter), this write is wrong.
- T2: Patching 0xAAF3/0xAAF4 to nop means the ROM's interrupt-source routing logic never books the interrupt. Downstream code that checks "was this interrupt properly routed?" may behave differently. Scope: acceptable for early boot; revisit if interrupt delivery becomes incorrect after System init.
- T3: Task C's ≥2/3 acceptance criterion is generous. If all 3 boots show irq_fired=0 due to timing non-determinism, we can't distinguish "not working" from "unlucky timing."

---

## Red-team record

*(Completed 2026-06-13 — findings folded into rev-2 as BINDING amendments above.)*

**PROCESS reviewer findings:**

- **C-1 CRITICAL (fixed):** Task A gate "dec_expiries increases from 4-5 floor" was not falsifiable — no number given. Fix: ≥2/3 boots, dec_expiries≥20, no SIGSEGV. Now in Step A-6.
- **C-2 CRITICAL (fixed):** Q-A5 addendum explicitly empty while Task C gates depended on it. Fix: Q-A5 now has method, budget, and a 5-boot fallback criterion.
- **C-3 CRITICAL (fixed):** No explicit checkpoint preventing implementation if blocking answers remain TODO. Fix: BINDING CHECKPOINT paragraph added before "Implementation tasks".
- **M-1 MAJOR (fixed):** Stop-rule missing item for "NOP-patch without understanding root cause". Fix: Stop-rule item 5 added.
- **M-2 MAJOR (fixed):** Task B gate used `--expect 'irq_fired=[1-9]'` (string match, fragile). Fix: Step B-4 now uses post-boot grep with explicit integer check.
- **M-3 MAJOR (fixed):** Step A-7 "nw-northstar observe line" was undefined for what output constitutes pass. Left as-is (it is report-only per DIAGNOSTICS.md; `REGRESSED` = stop is sufficient).
- **M-4 MAJOR (fixed):** Task C Step C-4 had two vague alternatives. Fix: concrete script to check gate status + explicit action for each outcome.
- **M-5 MAJOR (fixed):** Task D missing JIT-STATUS.md and CONTRIBUTING.md from update list. Fix: added to files list and Step D-6.
- **M-6 MINOR:** File line numbers in "Files:" blocks need re-verification before implementation. Noted as standing advice in "Codebase facts (re-verify before use)".

**TECHNICAL reviewer findings:**

- **CRITICAL — confirmed (no fix needed):** 0xDEADBEEF IS the DR's own code cache fill sentinel; this is correctly the root cause. DR compiled the first block but branched forward into an adjacent uncompiled slot within the same 64KB region. This CONFIRMS the plan's Q-A1 diagnosis and rules out our JIT as the source.
- **CRITICAL (fixed):** A7 register identity ambiguous. `AGENT-CONTEXT.md` says A7=r1; `sheepshaver_glue.cpp:478` places a_regs at gpr[16..23] making A7=r23. Fix: Q-A6 added as BINDING question before Task A implementation.
- **MAJOR (fixed):** `stw r24, 0x1c4(r16)` in STUB — sheepshaver_glue.cpp:1312 comment says r16 is unreliable. The STUB uses `mr r12, r24` (not r16 for this). The UNRELIABLE comment refers to a different line; noted as caveat in A-2 safety caveat.
- **MAJOR (fixed):** Approach A-2 safety unverified — calling DR compile function from outside DR context may corrupt state. Fix: A-2 safety caveat paragraph added.
- **Verified CORRECT:** Bit-11 OS trap discriminator logic — 0xAAF3/0xAAF4 both have bit 11 set → OS traps. Confirmed.
- **Verified CORRECT:** DR entry table address 0x5046e8c0. Confirmed.
- **Verified CORRECT:** Crash math (0xDEADBEEF → stfdu f21,-16657(r13), EA = r13-16657). Confirmed.
- **Verified CORRECT:** 0xA9A8 refutation — raw bytes at ROM+0xED06 = 0x00 0x00. Confirmed.
