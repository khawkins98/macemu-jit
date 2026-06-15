> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# M3 — Interrupt & Exception Architecture Recon Dossier

> **Status:** 📋 Recon complete (2026-06-10) — code-verified groundwork for M3
> (MACHINE-LAYER-PLAN §2d, the XL "big rock"). Read this before M3's writing-plans round.
> · **Branch:** `macos-arm64` · **Target:** macOS arm64, `EMULATED_PPC=1`, `USE_AARCH64_JIT`
> · **Method:** verbatim code + `file:line`. Line numbers may drift a few lines; symbol names
> are stable. Companion to M1's recon dossiers (`docs/superpowers/plans/m1-task6-recon.md`).

This dossier mines the *actual* interrupt/exception code so M3's plan starts from facts, not
the §2d sketch. Six sections, then **plan corrections vs §2d**, then **ranked open design
decisions**.

---

## 1. Today's interrupt path, end to end (verbatim)

### 1a. Which `TriggerInterrupt` is live? — the spcflags path, NOT SIGUSR2

There are three `TriggerInterrupt` definitions. **On macOS arm64 (`EMULATED_PPC=1`) the live
one is in `sheepshaver_glue.cpp`; the SIGUSR2 path is dead code.**

`sheepshaver_glue.cpp:1841-1851` (LIVE):
```cpp
void TriggerInterrupt(void)
{
	idle_resume();
#if 0
  WriteMacInt32(0x16a, ReadMacInt32(0x16a) + 1);
#else
  // Trigger interrupt to main cpu only
  if (ppc_cpu)
	  ppc_cpu->trigger_interrupt();
#endif
}
```

`main_unix.cpp:2333-2341` (DEAD on this target — guarded `#if !EMULATED_PPC`):
```cpp
#if !EMULATED_PPC
void TriggerInterrupt(void)
{
	if (ready_for_signals) {
		idle_resume();
		pthread_kill(emul_thread, SIGUSR2);
	}
}
#endif
```
(A third copy lives in `Windows/main_windows.cpp:701` — not this platform.)

`trigger_interrupt()` (`ppc-cpu.hpp:524-529`) just sets a flag — **no signal, no thread kill**:
```cpp
inline void powerpc_cpu::trigger_interrupt()
{
#if PPC_CHECK_INTERRUPTS
	spcflags().set(SPCFLAG_CPU_TRIGGER_INTERRUPT);
#endif
}
```

**Consequence:** the entire live interrupt-raise path on macOS arm64 is *purely spcflags*.
`sigusr2_handler` / `sigusr2_handler_init` (`main_unix.cpp:310-311` decls, all inside
`#if !EMULATED_PPC`) are never compiled. See Correction C — §2g item 4's "retire SIGUSR2
with the nested-execute path" is a **no-op on this target**.

### 1b. The 60 Hz tick thread (the interrupt source)

`main_unix.cpp:2137` `tick_func` (the "60Hz thread (really 60.15Hz)"), interrupt-raise tail at
`main_unix.cpp:2198-2208`:
```cpp
		// Pseudo Mac 1Hz interrupt, update local time
		if (++tick_counter > 60) {
			tick_counter = 0;
			WriteMacInt32(0x20c, TimerDateTime());
		}

		// Trigger 60Hz interrupt
		if (ReadMacInt32(XLM_IRQ_NEST) == 0) {
			SetInterruptFlag(INTFLAG_VIA);
			TriggerInterrupt();
		}
```
So the tick thread (a third thread, distinct from emul/CPU thread) sets `INTFLAG_VIA` and calls
the glue `TriggerInterrupt` → `trigger_interrupt()` → `SPCFLAG_CPU_TRIGGER_INTERRUPT`.

### 1c. `check_spcflags` — the spcflags state machine (`ppc-cpu.cpp:1415-1459`)

```cpp
bool powerpc_cpu::check_spcflags()
{
	if (spcflags().test(SPCFLAG_CPU_EXEC_RETURN)) {
		spcflags().clear(SPCFLAG_CPU_EXEC_RETURN);
		return false;
	}
#ifdef SHEEPSHAVER
	if (spcflags().test(SPCFLAG_CPU_HANDLE_INTERRUPT)) {
		spcflags().clear(SPCFLAG_CPU_HANDLE_INTERRUPT);
		static bool processing_interrupt = false;
		if (!processing_interrupt) {
			processing_interrupt = true;
			powerpc_registers r;
			powerpc_registers::interrupt_copy(r, regs());
			HandleInterrupt(&r);
			powerpc_registers::interrupt_copy(regs(), r);
			processing_interrupt = false;
			...
		}
	}
	if (spcflags().test(SPCFLAG_CPU_TRIGGER_INTERRUPT)) {
		spcflags().clear(SPCFLAG_CPU_TRIGGER_INTERRUPT);
		spcflags().set(SPCFLAG_CPU_HANDLE_INTERRUPT);
	}
#endif
	...
	return true;
}
```
Two-step: TRIGGER → (next poll) HANDLE → `HandleInterrupt`. `check_spcflags` is called from the
interpreter loop at `ppc-cpu.cpp:1513, 2243, 2402, 2458` (block boundaries / dispatch).

**`processing_interrupt` (line 1424) is a HandleInterrupt re-entry guard ONLY** — it does not
track nesting across `interrupt()`/`execute_68k`/`execute_macos_code`/`execute_emul_op`, each of
which runs a nested `execute()` whose block-boundary poll still fires. See §3c and open-decision #2.

### 1d. The centerpiece — `sheepshaver_cpu::interrupt()` (`sheepshaver_glue.cpp:617-690`)

> **Plan line-number drift:** §2d cites `sheepshaver_glue.cpp:595`; the function is actually at
> **617** (the EMUL_OP default block is at 591-610). Record in Corrections.

```cpp
// Handle MacOS interrupt
void sheepshaver_cpu::interrupt(uint32 entry)
{
	...
	// Save program counters and branch registers      ← the PC/LR/CTR/SP save
	uint32 saved_pc = pc();
	uint32 saved_lr = lr();
	uint32 saved_ctr= ctr();
	uint32 saved_sp = gpr(1);

	// Initialize stack pointer to SheepShaver alternate stack base   ← STACK SWAP
	gpr(1) = SignalStackBase() - 64;

	// Build trampoline to return from interrupt        ← nested-execute return sentinel
	SheepVar32 trampoline = POWERPC_EXEC_RETURN;

	// Prepare registers for nanokernel interrupt routine ← the KDP register-save FAKE
	WriteMacInt32(KERNEL_DATA_BASE + 0x004, gpr(1));
	WriteMacInt32(KERNEL_DATA_BASE + 0x018, gpr(6));

	gpr(6) = ReadMacInt32(KERNEL_DATA_BASE + 0x65c);
	assert(gpr(6) != 0);
	WriteMacInt32(gpr(6) + 0x13c, gpr(7));
	WriteMacInt32(gpr(6) + 0x144, gpr(8));
	WriteMacInt32(gpr(6) + 0x14c, gpr(9));
	WriteMacInt32(gpr(6) + 0x154, gpr(10));
	WriteMacInt32(gpr(6) + 0x15c, gpr(11));
	WriteMacInt32(gpr(6) + 0x164, gpr(12));
	WriteMacInt32(gpr(6) + 0x16c, gpr(13));

	gpr(1)  = KernelDataAddr;
	gpr(7)  = ReadMacInt32(KERNEL_DATA_BASE + 0x660);
	gpr(8)  = 0;
	gpr(10) = trampoline.addr();
	gpr(12) = trampoline.addr();
	gpr(13) = get_cr();

	// rlwimi. r7,r7,8,0,0
	uint32 result = op_ppc_rlwimi::apply(gpr(7), 8, 0x80000000, gpr(7));
	record_cr0(result);
	gpr(7) = result;

	gpr(11) = 0xf072; // MSR (SRR1)                      ← the fake SRR1 / MSR value
	cr().set((gpr(11) & 0x0fff0000) | (get_cr() & ~0x0fff0000));

	// Enter nanokernel
	if (ROMType == ROMTYPE_NEWWORLD) { ... [NW-INT] enter log ... }
	execute(entry);                                      ← NESTED execute() + EMUL_RETURN trampoline
	if (ROMType == ROMTYPE_NEWWORLD) { ... [NW-INT] return log ... }

	// Restore program counters and branch registers     ← PC/LR/CTR/SP restore
	pc() = saved_pc;
	lr() = saved_lr;
	ctr()= saved_ctr;
	gpr(1) = saved_sp;
}
```

Key facts for M3:
- The "register save" is a **hand-rolled emulation** of what the nanokernel's exception prologue
  expects (writes into KDP `+0x004/+0x018` and the `+0x65c`-pointed block at `+0x13c..+0x16c`),
  not a real exception. `gpr(11) = 0xf072` is the *hardcoded fake SRR1/MSR* the handler reads.
- The handler returns via the `POWERPC_EXEC_RETURN` (= `POWERPC_EMUL_OP | 1`,
  `sheepshaver_glue.cpp:127`) sentinel placed in `gpr(10)`/`gpr(12)`; the nested `execute(entry)`
  (line 673) runs until that sentinel sets `SPCFLAG_CPU_EXEC_RETURN`, which `check_spcflags`
  (line 1417) turns into `return false` (exit the nested loop).
- PC/LR/CTR/SP are saved (625-628) and *restored* (682-685) by the host — the guest handler does
  not `rfi` back; the host fakes the resume. **This is exactly what §2d replaces.**

### 1e. Where `interrupt()` is called + the nanokernel entry addresses

`HandleInterrupt` (`sheepshaver_glue.cpp:1853-1986`) dispatches by `XLM_RUN_MODE`:
```cpp
void HandleInterrupt(powerpc_registers *r)
{
#ifdef USE_SDL_VIDEO
	// We must fill in the events queue in the same thread that did call SDL_SetVideoMode()
	SDL_PumpEvents();                                    ← SDL_PumpEvents lives HERE (line 1857)
#endif
	if (int32(ReadMacInt32(XLM_IRQ_NEST)) > 0)
		return;
	...
	switch (ReadMacInt32(XLM_RUN_MODE)) {
	case MODE_68K:
		WriteMacInt16(ReadMacInt32(KERNEL_DATA_BASE + 0x67c), 1);
		... WriteMacInt32(0x16a, ReadMacInt32(0x16a) + 1);   // tick Ticks
		// NewWorld idle-loop wake (the [NW-INT] tick-50 injection hack):
		if (nw_tramp) { ... if (nw_tick >= 50) {
			DisableInterrupt();
			ppc_cpu->interrupt(ROMBase + 0x312b1c);          ← NEWWORLD entry
		} }
		break;
#if INTERRUPTS_IN_NATIVE_MODE
	case MODE_NATIVE:   // DEAD for NewWorld 9.x ROMs (comment 1924-1926)
		if (r->gpr[1] != KernelDataAddr) {
			...
			if (ROMType == ROMTYPE_NEWWORLD)
				ppc_cpu->interrupt(ROMBase + 0x312b1c);      ← NEWWORLD entry
			else
				ppc_cpu->interrupt(ROMBase + 0x312a3c);      ← OLDWORLD entry
		}
		break;
#endif
#if INTERRUPTS_IN_EMUL_OP_MODE
	case MODE_EMUL_OP:  // runs a full 68k interrupt routine via Execute68k()
		... Execute68k(proc, &r); ...
		break;
#endif
	}
}
```

**Nanokernel exception entry points (the working `interrupt()` ABI for §4 direct-entry):**
- NewWorld: `ROMBase + 0x312b1c`
- OldWorld: `ROMBase + 0x312a3c`

The `[NW-INT]` block (1891-1919) is the env-gated injection hack §2d/M3 deletes. `SDL_PumpEvents`
is at **`sheepshaver_glue.cpp:1857`**, inside `HandleInterrupt`, with a hard thread constraint
(comment 1856): "We must fill in the events queue in the same thread that did call
SDL_SetVideoMode()." See open-decision #3 — relocation is not a free move.

---

## 2. MSR / SRR0 / SRR1 / rfi / sc state today

| State | Status | Where |
|---|---|---|
| **MSR** | **ABSENT** — no MSR field in `powerpc_registers` | `ppc-registers.hpp:203-271` (no `msr`) |
| `mfmsr` | **STUB** — returns hardcoded `0xf072`, ignores any stored state | `ppc-execute.cpp:1277-1281` |
| `mtmsr` | **DROPPED** — falls into `execute_illegal` (op31/XO146); only logged under `SS_LOG_ILLEGAL` | `ppc-execute.cpp:148-207`, log path 167-171 |
| **SRR0** | **STORED** (`regs().srr0`), read/written by SPR 26 | `ppc-registers.hpp:269`; mfspr 1319-1321; mtspr 1387-1389 |
| **SRR1** | **STORED** (`regs().srr1`), read/written by SPR 27 | `ppc-registers.hpp:270`; mfspr 1322-1324; mtspr 1390-1392 |
| **rfi** | **PARTIAL STUB** — `pc() = srr0` only; **does NOT restore MSR from SRR1** | `ppc-execute.cpp:1530-1533` |
| **sc** | **STUB → illegal + ad-hoc PC bumps** (double-increment bug class) | `ppc-execute.cpp:1116-1124`; JIT `ppc-jit.cpp:3693` |

`execute_mfmsr` verbatim (`ppc-execute.cpp:1277-1281`):
```cpp
void powerpc_cpu::execute_mfmsr(uint32 opcode)
{
	operand_RD::set(this, opcode, 0xf072);   // hardcoded — no MSR state exists
	increment_pc(4);
}
```

`execute_rfi` verbatim (`ppc-execute.cpp:1530-1533`):
```cpp
void powerpc_cpu::execute_rfi(uint32 opcode)
{
	pc() = regs().srr0;        // SRR1→MSR restore is MISSING
}
```

`execute_syscall` verbatim (`ppc-execute.cpp:1116-1124`):
```cpp
void powerpc_cpu::execute_syscall(uint32 opcode)
{
#ifdef SHEEPSHAVER
	execute_illegal(opcode);   // <-- (A)
#else
	cr().set_so(0, execute_do_syscall && !execute_do_syscall(this));
#endif
	increment_pc(4);           // <-- (B)
}
```

**The `sc` double-increment bug class (§2d), precisely — it is *conditional*, three branches in
`execute_illegal` (`ppc-execute.cpp:148-207`):**
1. **Normal boot** (no ignore flag): `execute_illegal` reaches `abort()` at line 206 — `sc`
   *crashes*, never returns to (B).
2. **`ignoreillegal` pref set:** `execute_illegal` does `increment_pc(4); return;` (lines 188 /
   193-196) → returns to `execute_syscall` (B) which does `increment_pc(4)` again ⇒ **PC bumped
   twice (+8), skipping the instruction after `sc`.** This is the bug class.
3. **Test mode** (`SS_TEST_HEX`/`SS_TEST_HEX_FILE`): sets `SPCFLAG_CPU_EXEC_RETURN; return;`
   (181-185) — short-circuits before (B); no double bump.

**Both engines route `sc` the same way:** the AArch64 JIT does not natively emit `sc` —
`ppc-jit.cpp:3693` `case 17: /* sc — system call: fall back so interpreter raises it */ return
false;` — so it falls to the interpreter `execute_syscall` → `execute_illegal`.

**Documented confirmation (cite):** the double-increment is recorded in
`HANDOFF-NEWWORLD-SUPERVISOR-MMU.md:81` (§1.7.2): *"The idle task's `sc` polling is dead (`sc` =
`execute_illegal` no-op, PC += 8, skips `cmpwi r3,0`)."* (No `git log`/`LEARNINGS.md` entry uses
the literal phrase "double-increment"; the HANDOFF entry is the canonical cite. The `PC += 8`
matches branch (2) above.) §2d's fix — make `sc` a real exception (SRR0/SRR1 + vector + `rfi`) —
structurally eliminates this.

---

## 3. The block-boundary delivery point (precise-interrupt point §2d relies on)

### 3a. JIT poll — `emit_entry_spcflags_poll` (`ppc-jit.cpp:1368-1385`)

Confirmed at the line the task flagged (~107 is the contract comment; emitter is at **1368**):
```cpp
static void emit_entry_spcflags_poll(uint32_t block_start_pc) {
	/* LDR  Wtmp0, [RSTATE, #PPCR_SPCFLAGS]  — load spcflags.mask */
	a64_ldr_w_imm(RTMP0, RSTATE, PPCR_SPCFLAGS);
	/* AND  Wtmp0, Wtmp0, #PPCR_SPCFLAGS_POLL_MASK (0x0F) — actionable bits only */
	emit32(0x12000C00 | (RTMP0 << 5) | RTMP0);
	/* CBZ  Wtmp0, <body> — skip the return sequence when no flags pending. */
	uint32_t *cbz_loc = jit_code_ptr;
	emit32(0); /* placeholder CBZ */
	/* Flags pending: store block start PC and return to the dispatcher. */
	emit_load_imm32(RTMP0, (int32_t)block_start_pc);
	a64_str_w_imm(RTMP0, RSTATE, PPCR_PC);
	emit_bare_epilogue();
	...
}
```
Contract (comment `ppc-jit.cpp:107-122, 430-439`): emitted at **every block chain entry**
(`chain_entry_start`), the precise-interrupt point. Poll mask `PPCR_SPCFLAGS_POLL_MASK = 0x0F`
(line 439) = `EXEC_RETURN(1) | TRIGGER_INTERRUPT(2) | HANDLE_INTERRUPT(4) | ENTER_MON(8)`;
`SPCFLAG_JIT_EXEC_RETURN` (bit 16) is deliberately excluded. On a pending flag the block stores
its start PC to `PPCR_PC` and returns to the C dispatcher, which runs `check_spcflags`.

**Critical chaining invariant for M3** (comment `ppc-jit.cpp:3262-3267, 3354-3356`): backward
intra-block branches must return to the dispatcher (not native-branch to `insn_code_offset[i]`)
or they'd bypass the poll and starve interrupt delivery. M3's real-delivery design must preserve
this "every backward edge re-polls" property.

### 3b. Interpreter poll

`check_spcflags()` (`ppc-cpu.cpp:1415`) is called at block boundaries / dispatch:
`ppc-cpu.cpp:1513`, `2243` (`if (!check_spcflags()) goto return_site;`), `2402`,
`2458` (`if (!spcflags().empty() && !check_spcflags())`). Same actionable-bit semantics as the
JIT poll (they share `check_spcflags`).

### 3c. Nested-execute contexts (the §2d deliverability risk surface) — where a depth counter would live

Every host-driven entry calls a nested `execute()`; the block-boundary poll fires **inside** all
of them, so an external interrupt can today be delivered mid-context (the §2d-rev3 "`rfi` into a
host C++ continuation" hazard):

| Nested context | File:line | Nested `execute()` |
|---|---|---|
| `interrupt()` (the handler itself) | `sheepshaver_glue.cpp:617` | `execute(entry)` line 673 |
| `execute_68k()` | `sheepshaver_glue.cpp:693` | `execute(gpr(29))` line 754 |
| `execute_macos_code()` | `sheepshaver_glue.cpp:789` | `execute(proc)` line 819 |
| `execute_emul_op()` → `EmulOp()` | `sheepshaver_glue.cpp:317` / 359 | (EmulOp may re-enter Execute68k) |

The existing `static bool processing_interrupt` (`ppc-cpu.cpp:1424`) guards **only**
`HandleInterrupt` re-entry — it does **not** span these. So §2d's "external interrupts deliverable
only at depth-1 block boundaries" needs a **new nesting depth counter** wrapping these four entry
points (increment on enter, decrement on the host restore), checked at the poll. `processing_interrupt`
is too narrow to reuse. (See open-decision #2.)

---

## 4. The vector-base question (§2d)

### 4a. What is at guest 0x0100–0x0F00 today

Under DIRECT_ADDRESSING (V=P), guest physical 0x0000–0x0FFF is **68k low-memory globals**, not a
PPC exception vector page. Confirmed writes that land there:
- `WriteMacInt32(0x16a, …)` — **`Ticks`** incremented on every VBL: `sheepshaver_glue.cpp:1884`
  (live), `1845` (`#if 0`). The nanokernel spin-waits poll `Ticks` directly (comment 1878-1884:
  cites `0x5031040c`, `0x50313d34`).
- `WriteMacInt32(0x20c, TimerDateTime())` — `Time` low-memory global, tick thread
  `main_unix.cpp:2201`.
- KernelData / low-mem setup in ROM patches: `rom_patches.cpp:795` (`LA_EmulatorData` at KDP
  `0xa4`), `rom_patches.cpp:1245` ("KernelData (0x300-0x324)… flat virtual=physical… no live
  SRs/BATs").

So **PPC exceptions vectoring to physical 0x0100–0x0F00 would collide with these live 68k
globals** — exactly the §2d collision. There is no vector page mapped today; delivery is faked by
`interrupt()` jumping straight to the handler entry (§1e).

### 4b. Direct-entry option — the known handler entry points

The §2d option (i) "direct-entry" dispatches to the nanokernel's known handler entry recovered
from the working `interrupt()` ABI:
- **NewWorld handler entry: `ROMBase + 0x312b1c`** (`sheepshaver_glue.cpp:1916, 1939`).
- **OldWorld handler entry: `ROMBase + 0x312a3c`** (`sheepshaver_glue.cpp:1941`).
- The full `interrupt()` ABI contract (KDP writes, `gpr(1)=KernelDataAddr`, fake `gpr(11)=0xf072`
  SRR1, `POWERPC_EXEC_RETURN` trampoline in `gpr(10)/gpr(12)`) is §1d above.

HANDOFF cross-ref (`HANDOFF-NEWWORLD-SUPERVISOR-MMU.md:81, 135, 367`): injection via
`ppc_cpu->interrupt(ROMBase + 0x312b1c)` enters the handler correctly and traverses interrupt
prologue → scheduler (`0x503242a8`) → dispatch (`0x503244cc → 0x50318000`) → idle task
(`0x50324f04`). EXEC_RETURN trampoline at `0x5058f5c8`. This is the proven mechanism direct-entry
formalizes — the smaller delta §2d recommends starting with.

---

## 5. New since the plan was written — composition with M1's machinery + the MMU/SR wall

### 5a. M1 MMIO bus — what's registered today (`main_unix.cpp:1696-1726`)

```cpp
void *want = (void *)(uintptr_t)(NATMEM_OFFSET + 0xF3000000ull);   // PROT_NONE reservation
...
SCCReset(&scc, 0xF3012000);
...
bool ok = MMIOBusRegister(0xF3000000, 0x80000, MMIO_TRAPPED, &macio_stub_dev)   // MacIO container (loud stub)
       && MMIOBusRegister(0xF3012000, 0x1000, MMIO_TRAPPED, &scc_dev)            // SCC 8530
       && MMIOBusRegister(0xF3016000, 0x2000, MMIO_TRAPPED, &via_dev);           // VIA 6522
MMIOBusActivate();   // mmio_bus.cpp:52 — active iff n_regions > 0
```
Bus core: `machine/mmio_bus.cpp` (`MMIOBusRegister` line 27). Region kinds `MMIO_TRAPPED` /
mapped-aperture. **No PIC region is registered yet** — M3 must add the OpenPIC.

### 5b. Where M3 composes with M1

- **OpenPIC region (M3's to register):** per `CORE99-MACHINE-DESCRIPTION.md:47, 97` the KeyLargo
  OpenPIC sits at **`0xF3040000` (MacIO + 0x40000)**, inside the already-reserved MacIO container
  (`0xF3000000–0xF3080000`). M3 registers a new `MMIO_TRAPPED` region there with the OpenPIC
  model. **§5 Q6 (region size) is still OPEN** — `CORE99-MACHINE-DESCRIPTION.md:172-173`: "not
  pinned in `macio.c`; read QEMU `hw/intc/openpic.c` `OPENPIC_MODEL_KEYLARGO` register window
  before registering." Note the current MacIO-stub region (`0xF3000000+0x80000`) *overlaps* the
  intended OpenPIC offset — M3 must split/carve the stub so the OpenPIC region wins dispatch.
- **PIC → CPU wiring:** `CORE99-MACHINE-DESCRIPTION.md:74` — OpenPIC `OPENPIC_OUTPUT_INT` drives
  the CPU external-interrupt input, which in M3 becomes a PIC→spcflags assertion delivered at the
  block-boundary poll (§3). Input numbers (ESCC/VIA = 0x24/0x25/0x19) are **QEMU-only, unverified**
  (`CORE99-MACHINE-DESCRIPTION.md:178`).
- **DEC ≠ PIC:** `CORE99-MACHINE-DESCRIPTION.md:78-80` confirms §2c — the decrementer is a
  CPU-internal exception (vector 0x900), never through the OpenPIC. M2's virtual clock raises the
  DEC *condition*; M3 delivers it on the CPU-internal path, separate from PIC inputs.
- **M2 clock already partly landed:** `execute_mfspr`/`execute_mtspr` now honor DEC/TBL/TBU via
  `g_virt_clock` on the newworld profile (`ppc-execute.cpp:1341-1426`); M1 shipped a minimal DEC
  tick. M3's DEC delivery builds on this.

### 5c. The 0x50326050 MMU/SR wall (CHANGELOG 2026-06-10) — M3/M5 territory

`CHANGELOG.md:40-63`: after the M1 NK-boot-ceiling fix (`d8932203`), boot now SIGSEGVs at
**`0x50326050–0x50326068`** — `lwbrx` of hardcoded physical `0x200a0` (below RAMBase) inside the
NK's **MMU/segment-fault handler**: `mtdbatl/mtdbatu`, `mtsrin`, `mtmsr` translation toggling,
byte-reversed PTE accesses. This is the genuine SR/BAT/supervisor-environment wall. **M3 directly
intersects it:** that handler does `mtmsr` (translation toggle) — which today is a **dropped
no-op** (§2). M3's MSR model (EE/PR/IP/translation bits) and M5's SR/BAT stored state are what let
this handler behave. The `mtsrin`/`mtdbat*` are the rung-2 SR/BAT stored-state work (§2e / M5),
but the `mtmsr` translation-enable semantics are M3's MSR model.

---

## 6. `powerpc_registers` layout — offsets the JIT hardcodes (append-last constraint)

Struct: `ppc-registers.hpp:203-271`. JIT-hardcoded byte offsets (`ppc-jit.cpp:412-429`), pinned
by `static_assert`s in `ppc-cpu.cpp:484-492`:

| Field | Offset | JIT macro | Notes |
|---|---|---|---|
| `gpr[n]` | `n*4` | `PPCR_GPR(n)` | |
| `gpr_hi[n]` | `128 + n*4` | `PPCR_GPR_HI(n)` | |
| `fpr[n]` | `256 + n*8` | `PPCR_FPR(n)` | |
| `cr` | 1024 | `PPCR_CR` | |
| `xer` | 1028 | `PPCR_XER` (+SO/OV/CA/CNT 1028-1031) | |
| `fpscr` | 1040 | `PPCR_FPSCR` | |
| `lr` | 1044 | `PPCR_LR` | |
| `ctr` | 1048 | `PPCR_CTR` | |
| `pc` | 1052 | `PPCR_PC` | poll stores block PC here |
| `spcflags` | 1056 | `PPCR_SPCFLAGS` | `static_assert == 1056` (ppc-cpu.cpp:484) |
| `reserve_valid` | 1060 | `PPCR_RESERVE_VALID` | `static_assert == 1060` |
| `reserve_addr` | 1064 | `PPCR_RESERVE_ADDR` | `static_assert == 1064` |

**After `reserve_*` come the "MUST stay LAST" fields** (`ppc-registers.hpp:256-270`): `sprg[4]`,
`sdr1`, `bat[16]`, `srr0`, `srr1`. The struct comment (256-257): *"MUST stay LAST: the JIT
hardcodes byte offsets… Appending here shifts nothing the JIT references."* These trailing fields
are accessed **by name in the interpreter only** — JIT `mfspr`/`mtspr` for SPRG/SDR1/BAT/SRR fall
back to the interpreter (comment 262-263), so their exact offsets are irrelevant to codegen.

**Constraint for M3's new fields (MSR, and any DEC mirror):** append **after** `srr1` (last),
never between `reserve_addr` and `sprg`, and **require a clean PPC recompile** (the known
stale-build / struct-offset footgun — MEMORY `stale_build_struct_offsets`). MSR will be
interpreter-accessed (slow-path `mfmsr`/`mtmsr`/`rfi`/exception dispatch), so no new JIT offset
macro is needed unless M3 wants an inline MSR.EE check in the poll (then add a pinned macro +
`static_assert`).

---

## What M3's plan must correct vs MACHINE-LAYER-PLAN §2d's sketch

1. **`interrupt()` is at `sheepshaver_glue.cpp:617`, not 595** (§2d cites 595). Line drift.
2. **SIGUSR2 is already dead on this target** (Correction-grade). §2g item 4 ("the SIGUSR2
   mechanism is retired with the nested-execute path in M3") is a **no-op on macOS arm64**:
   `EMULATED_PPC=1` selects glue's `TriggerInterrupt` → `trigger_interrupt()` →
   `SPCFLAG_CPU_TRIGGER_INTERRUPT`; the `pthread_kill(emul_thread, SIGUSR2)` path
   (`main_unix.cpp:2333`) and `sigusr2_handler` are all `#if !EMULATED_PPC` dead code. The live
   interrupt-raise path is purely spcflags. There is nothing to retire here — M3 should state this
   and scope the "retire SIGUSR2" item to Linux/non-EMULATED_PPC builds only (or drop it).
3. **SRR0/SRR1 already exist as stored state** (§2d says "real save/restore semantics so the
   guest handler returns" as if from scratch). The fields are present (`ppc-registers.hpp:269-270`)
   and `mtspr`/`mfspr` 26/27 work (`ppc-execute.cpp:1319-1324, 1387-1392`). M3's actual gap is:
   (a) **MSR does not exist at all** (no field; `mfmsr` hardcoded `0xf072`; `mtmsr` dropped), and
   (b) **`rfi` only restores PC, not MSR from SRR1** (`ppc-execute.cpp:1530-1533`). The save-side
   (`sc`/exception writing SRR0/SRR1) is what's missing, not the storage.
4. **The `sc` double-increment is conditional, not unconditional.** Normal boot *aborts* in
   `execute_illegal`; only `ignoreillegal` produces the +8 double-bump; test mode short-circuits.
   M3's "structurally fixes the double-increment" claim is correct but the bug only manifests
   under `ignoreillegal` (or the HANDOFF idle-task path that treats `sc` as a dead no-op).
5. **`processing_interrupt` is NOT the depth mechanism §2d's deliverability rule needs.** It guards
   only `HandleInterrupt` re-entry; it does not span `interrupt()`/`execute_68k`/
   `execute_macos_code`/`execute_emul_op`. §2d's depth-1 rule needs a *new* nesting counter (§3c).
6. **The MacIO-stub region overlaps the OpenPIC offset.** §2d/§2a assume a clean PIC region at
   MacIO+0x40000, but today `0xF3000000+0x80000` (the loud MacIO stub) already covers
   `0xF3040000`. M3 must carve the stub or register the OpenPIC region with dispatch precedence.
   And §5 Q6 (OpenPIC region size) is still open (`CORE99-MACHINE-DESCRIPTION.md:172`).
7. **`SDL_PumpEvents` relocation has a thread constraint** (§2d says "move to a host-side
   cadence" as if trivial). It must run on the thread that called `SDL_SetVideoMode`
   (`sheepshaver_glue.cpp:1856`). The target cadence must honor that, or pump from the SDL-owning
   thread.
8. **The 0x50326050 wall couples M3 to M5 immediately.** The first thing real delivery will hit is
   the NK segment/MMU handler that does `mtmsr` translation toggling — so M3's MSR model and M5's
   SR/BAT stored state are co-dependent at that wall, not cleanly separable as the milestone split
   implies. M3 should expect to land a minimal `mtmsr` (translation/EE bits as stored state) even
   before M5's full SR/BAT work, or that handler still no-ops.

---

## Ranked open design decisions

**1. Vector-base experiment shape (§2d, M3-start, allowed to fail back to direct-entry).**
Recommend the bounded experiment be framed as: *can a single special-cased mapping of the
exception vector page (0x0100–0x0F00) coexist with the live 68k low-memory globals (`Ticks`@0x16a,
`Time`@0x20c, KernelData 0x300-0x324)?* — option (ii). **Start with option (i) direct-entry**
(dispatch to `ROMBase+0x312b1c` via the proven `interrupt()` ABI, §4b) because it is the smallest
delta from the mechanism that already enters the handler correctly (HANDOFF-confirmed). The
experiment's pass bar: real PIC→spcflags→handler→`rfi` round-trip with no `interrupt()` host fake.
Highest-risk, decided first per §3 sequencing.

**2. Depth-1 deliverability mechanism (§2d-rev3).** A new nesting depth counter incremented at the
four nested-`execute()` entry points (§3c: `interrupt()`, `execute_68k`, `execute_macos_code`,
`execute_emul_op`) and checked at the block-boundary poll: external PIC interrupts deliver only
when depth==1; otherwise latch pending and drain on return to depth 1. Where it lives: a member of
`sheepshaver_cpu` (or a thread-local), set in glue alongside the existing PC/LR/CTR save/restore.
Do NOT extend `processing_interrupt` — it's the wrong granularity. M3's harness adds a test vector
that asserts an interrupt raised mid-EMUL_OP is deferred, not delivered into the host frame.

**3. SDL_PumpEvents relocation target (§2d host-integration relocation).** Constraint: must run on
the `SDL_SetVideoMode` thread (`sheepshaver_glue.cpp:1856`). Candidate targets, ranked: (a) the
existing 60 Hz tick thread *if* it is the SDL-owning thread (verify — likely not; SDL window is
typically the main thread); (b) a dedicated host-cadence pump on the video/main thread driven by a
timer; (c) keep the pump where it is but decouple it from interrupt *delivery* (delivery moves to
the PIC path; pump stays a cosmetic host-cadence call). Option (c) is the smallest delta and avoids
the thread-affinity hazard — recommend it unless event latency proves unacceptable.

---

*End of M3 interrupt-architecture recon dossier.*
