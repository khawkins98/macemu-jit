# M1 Task 6 — Mach Fault Path Integration Dossier

Prepared by recon agent, 2026-06-10. All line numbers are from current HEAD on the
`macos-arm64` branch in the machine-layer-m1 worktree. Line numbers may drift a few
lines after new commits but *symbol names are stable*.

---

## 1. sigsegv.h — enum, struct, and insertion points

### 1a. `sigsegv_return_t` enum (verbatim, sigsegv.h:153-157)

```cpp
// SIGSEGV handler return state
enum sigsegv_return_t {
  SIGSEGV_RETURN_SUCCESS,
  SIGSEGV_RETURN_FAILURE,
  SIGSEGV_RETURN_SKIP_INSTRUCTION
};
```

**Insertion point for `SIGSEGV_RETURN_STATE_MODIFIED`** — insert after line 156
(`SIGSEGV_RETURN_SKIP_INSTRUCTION`), before the closing `};`:

```
Context (3 lines before):
  154:   SIGSEGV_RETURN_SUCCESS,
  155:   SIGSEGV_RETURN_FAILURE,
  156:   SIGSEGV_RETURN_SKIP_INSTRUCTION
INSERT:   SIGSEGV_RETURN_STATE_MODIFIED,   // handler mutated the thread state in place (Mach only)
  157: };
```

### 1b. `sigsegv_info_t` struct (verbatim, sigsegv.h:137-149)

```cpp
struct sigsegv_info_t {
	sigsegv_address_t addr;
	sigsegv_address_t pc;
#ifdef HAVE_MACH_EXCEPTIONS
	mach_port_t thread;
	bool has_exc_state;
	SIGSEGV_EXCEPTION_STATE_TYPE exc_state;
	mach_msg_type_number_t exc_state_count;
	bool has_thr_state;
	SIGSEGV_THREAD_STATE_TYPE thr_state;
	mach_msg_type_number_t thr_state_count;
#endif
};
```

The thread-state member is `thr_state` of type `SIGSEGV_THREAD_STATE_TYPE`.

**On macOS arm64** (config.h:117-119 under the `#elif defined(__aarch64__)` branch):
```
SIGSEGV_THREAD_STATE_TYPE   = arm_thread_state64_t
SIGSEGV_THREAD_STATE_FLAVOR = ARM_THREAD_STATE64
SIGSEGV_THREAD_STATE_COUNT  = ARM_THREAD_STATE64_COUNT
```
`arm_thread_state64_t` is `_STRUCT_ARM_THREAD_STATE64`. The `__x[]` array and PAC-aware
`arm_thread_state64_get_pc` / `arm_thread_state64_set_pc_fptr` are part of this type.

**IMPORTANT — thr_state lifecycle on arm64 (see Plan Corrections §A below):**

`handle_badaccess` sets `SI.has_thr_state = false` at sigsegv.cpp:2721 and enters the
switch at line 2786 **without fetching thread state first**. The only pre-fetch at
2725-2784 is inside `#if defined(__APPLE__) && defined(__x86_64__)` — the arm64 build
skips it entirely. The first (and only) lazy fetch on the SKIP path is at lines 2795-2796:

```cpp
// sigsegv.cpp:2794-2796 (SKIP branch, inside #ifdef HAVE_MACH_EXCEPTIONS):
		if (!SIP->has_thr_state)
			mach_get_thread_state(SIP);
```

Therefore when the glue handler returns `SIGSEGV_RETURN_STATE_MODIFIED`, `sip->thr_state`
will contain **uninitialized stack storage** unless `mach_get_thread_state` has been called
first. `sigsegv_get_thread_state()` **must perform the lazy fetch** before returning the
pointer. See Plan Corrections §A for the precise implementation adjustment.

### 1c. `mach_get_thread_state` / `mach_set_thread_state` (verbatim, sigsegv.cpp:2621-2639)

```cpp
// sigsegv.cpp:2621-2630
static void mach_get_thread_state(sigsegv_info_t *SIP)
{
	SIP->thr_state_count = SIGSEGV_THREAD_STATE_COUNT;
	kern_return_t krc = thread_get_state(SIP->thread,
										 SIGSEGV_THREAD_STATE_FLAVOR,
										 (natural_t *)&SIP->thr_state,
										 &SIP->thr_state_count);
	MACH_CHECK_ERROR(thread_get_state, krc);
	SIP->has_thr_state = true;
}

// sigsegv.cpp:2632-2639
static void mach_set_thread_state(sigsegv_info_t *SIP)
{
	kern_return_t krc = thread_set_state(SIP->thread,
										 SIGSEGV_THREAD_STATE_FLAVOR,
										 (natural_t *)&SIP->thr_state,
										 SIP->thr_state_count);
	MACH_CHECK_ERROR(thread_set_state, krc);
}
```

Both are `static` — NOT visible outside sigsegv.cpp. The new accessor and STATE_MODIFIED
branch must live inside sigsegv.cpp.

### 1d. `handle_badaccess` STATE_MODIFIED insertion point (sigsegv.cpp:2786-2818)

The switch at line 2786 currently has three cases:

```cpp
// sigsegv.cpp:2786-2817 (abbreviated)
	switch (SIGSEGV_FAULT_HANDLER_INVOKE(SIP)) {
	case SIGSEGV_RETURN_SUCCESS:
		return true;

#if HAVE_SIGSEGV_SKIP_INSTRUCTION
	case SIGSEGV_RETURN_SKIP_INSTRUCTION:
		...skip logic, mach_set_thread_state(SIP)...
		return true;
		break;
#endif
	case SIGSEGV_RETURN_FAILURE:
		...dump...
		break;
	}
```

**No `-Wswitch`/`-Wswitch-enum` in the build flags** (Makefile CXXFLAGS = `-g -O2
-I/opt/homebrew/include -mdynamic-no-pic`, no `-W` additions). The switch has no `default:`
and the existing SKIP case is `#if`-guarded. The new STATE_MODIFIED case can follow the
same pattern.

**Insert after the SKIP block and before `case SIGSEGV_RETURN_FAILURE:`** (between lines 2808
and 2810). Exact 3-line context:

```cpp
// context:
	// 2808:         break;
	// 2809: #endif
INSERT:
#ifdef HAVE_MACH_EXCEPTIONS
	case SIGSEGV_RETURN_STATE_MODIFIED:
		// Handler mutated thread state in sip->thr_state in place; write it back.
		// (Mach only: state was fetched by sigsegv_get_thread_state before the
		// handler ran — see sigsegv_get_thread_state.)
		mach_set_thread_state(SIP);
		return true;
#endif
	// 2810: case SIGSEGV_RETURN_FAILURE:
```

**Non-Mach builds:** the `#ifdef HAVE_MACH_EXCEPTIONS` guard means the case doesn't
exist in UNIX-signal builds. The compiler never sees an unhandled enum value there.
No other switch statements in the file enumerate `sigsegv_return_t` — grep confirms the
switch at line 2786 is the only one.

### 1e. `sigsegv_get_thread_state` — insertion point in sigsegv.cpp

Add near `sigsegv_get_fault_address` / `sigsegv_get_fault_instruction_address` (after
line 2639, still inside the `#ifdef HAVE_MACH_EXCEPTIONS` block). Exact context:

```cpp
// 2638: }
// 2639: }    ← closing brace of mach_set_thread_state
// 2640: #endif
INSERT (inside the same #ifdef block, just before 2640 #endif):
```

**Correct implementation** (see Plan Corrections §A — must perform lazy fetch):

```cpp
// Mach path only: raw ARM_THREAD_STATE64 of the faulting thread (mutable in place;
// written back by handle_badaccess when the handler returns SIGSEGV_RETURN_STATE_MODIFIED).
// Performs a lazy mach_get_thread_state if not already fetched.
// Returns NULL on non-Mach builds or if no thread is available.
void *sigsegv_get_thread_state(sigsegv_info_t *SIP)
{
#if defined(HAVE_MACH_EXCEPTIONS) && defined(_STRUCT_ARM_THREAD_STATE64)
    if (!SIP->has_thr_state)
        mach_get_thread_state(SIP);
    return &SIP->thr_state;
#else
    (void)SIP;
    return NULL;
#endif
}
```

Add the corresponding declaration to `sigsegv.h` after line 179
(`sigsegv_get_fault_instruction_address`):

```cpp
// Mach path only: returns a pointer to the ARM_THREAD_STATE64 of the faulting thread.
// The state is fetched lazily on first call. Returns NULL on non-Mach/non-arm64 builds.
extern void *sigsegv_get_thread_state(sigsegv_info_t *sip);
```

### 1f. Build impact — BasiliskII

BasiliskII has its **own** copy of sigsegv.cpp / sigsegv.h at
`BasiliskII/src/CrossPlatform/` (not shared; `diff` shows only whitespace-level
divergence). Modifying `SheepShaver/src/CrossPlatform/sigsegv.cpp` does **not** touch
BasiliskII. The `#ifdef HAVE_MACH_EXCEPTIONS` guard means x86/Linux/non-Mach
SheepShaver builds are textually unaffected.

---

## 2. `sheepshaver_glue.cpp` `sigsegv_handler` — first 40 lines (with line numbers)

```cpp
// sheepshaver_glue.cpp:919-998
919: sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip)
920: {
921: #if ENABLE_VOSF
922: 	// Handle screen fault
923: 	extern bool Screen_fault_handler(sigsegv_info_t *sip);
924: 	if (Screen_fault_handler(sip))
925: 		return SIGSEGV_RETURN_SUCCESS;
926: #endif
927:
928: 	const uintptr addr = (uintptr)sigsegv_get_fault_address(sip);
929:
930: #if HAVE_SIGSEGV_SKIP_INSTRUCTION
931: 	// Ignore writes to ROM
932: 	if ((addr - (uintptr)ROMBaseHost) < ROM_SIZE)
933: 		return SIGSEGV_RETURN_SKIP_INSTRUCTION;
934:
935: 	// Get program counter of target CPU
936: 	sheepshaver_cpu * const cpu = ppc_cpu;
937: 	const uint32 pc = cpu->pc();
938: 	
939: 	// Fault in Mac ROM or RAM?
940: 	bool mac_fault = (pc >= ROMBase && pc < (ROMBase + ROM_AREA_SIZE)) || (pc >= RAMBase && pc < (RAMBase + RAMSize)) || (pc >= DR_CACHE_BASE && pc < (DR_CACHE_BASE + DR_CACHE_SIZE));
941: 	if (mac_fault) {
942:
943: 		// Legacy PC-keyed skip hacks (installer probes, serial-driver device
944: 		// probes). PARAVIRTUAL ONLY: on the newworld fidelity profile these
945: 		// would silently eat MMIO accesses the bus must see
946: 		// (MACHINE-LAYER-PLAN.md section 2b).
947: 		if (!MachineProfileIsNewWorld()) {
948:
949: 			// "VM settings" during MacOS 8 installation
950: 			if (pc == ROMBase + 0x488160 && cpu->gpr(20) == 0xf8000000)
951: 				return SIGSEGV_RETURN_SKIP_INSTRUCTION;
952:
953: 			// MacOS 8.5 installation
954: 			else if (pc == ROMBase + 0x488140 && cpu->gpr(16) == 0xf8000000)
955: 				return SIGSEGV_RETURN_SKIP_INSTRUCTION;
956:
957: 			// MacOS 8 serial drivers on startup
958: 			else if (pc == ROMBase + 0x48e080 && (cpu->gpr(8) == 0xf3012002 || cpu->gpr(8) == 0xf3012000))
959: 				return SIGSEGV_RETURN_SKIP_INSTRUCTION;
960: [... more serial hacks through line 972 ...]
961: 		}
962: 		// Ignore writes to the zero page (SheepMem; both profiles)
963: 		if ((uint32)(addr - SheepMem::ZeroPage()) < (uint32)SheepMem::PageSize())
964: 			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
965: 		// Ignore all other faults, if requested. PARAVIRTUAL ONLY:
966: 		if (!MachineProfileIsNewWorld() && PrefsFindBool("ignoresegv"))
967: 			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
968: 	}
969: #else
970: #error "FIXME: You don't have the capability to skip instruction within signal handlers"
971: #endif
```

### 2a. How `addr` and `pc` are obtained

- `addr` (line 928): `sigsegv_get_fault_address(sip)` → returns a **host virtual address**
  (`void *`). On macOS arm64 with `DIRECT_ADDRESSING`, host address = `NATMEM_OFFSET +
  guest_addr`. So `addr` is a 64-bit host address in the `0x400000000000+` range.
- `pc` (line 937): `cpu->pc()` → the **PPC guest PC**, a 32-bit value in the guest 32-bit
  address space (e.g. `0x50312250` in ROM, `0x10004000` in RAM).

**Address space context:** `NATMEM_OFFSET = 0x400000000000` (config.h:430). `DIRECT_ADDRESSING`
is active (sysdeps.h:95-96 gates on `NATMEM_OFFSET` being defined). `VMBaseDiff =
NATMEM_OFFSET` (vm.hpp:196). The idiom `host → guest` is: `guest = (uint32)(addr -
VMBaseDiff)`, which is `(uint32)((uintptr)host_addr - NATMEM_OFFSET)`.

**VMBaseDiff visibility in sheepshaver_glue.cpp:** The file includes `cpu_emulation.h`
(line 22), which includes `cpu/vm.hpp` (cpu_emulation.h:62), which defines `VMBaseDiff`
as `const uintptr VMBaseDiff = NATMEM_OFFSET` (vm.hpp:196) — visible as a `const uintptr`
in translation units that include the chain. Do NOT redeclare it; use it directly.

**Exact conversion expression for Task 6 Step 3:**
```cpp
uint32_t gaddr = (uint32_t)((uintptr_t)addr - VMBaseDiff);
```
`addr` is already a `uintptr` at line 928; the cast makes the arithmetic explicit.

**Note on line 932 (ROM-write skip):** The ROM-write check uses `ROMBaseHost` (a host
pointer), not `VMBaseDiff` arithmetic, to stay in host-address space and avoid a 32-bit
wraparound. The MMIO dispatch block must run **before** line 932 (ROM-write check) because
a fault in device space (`0xF3000000+`) would not match `ROMBaseHost` and would fall
through to the legacy serial hacks or be swallowed by `ignoresegv`. The correct insertion
point is:

```
After line 928 (addr computed):
  928:   const uintptr addr = (uintptr)sigsegv_get_fault_address(sip);
  929:                                                             ← INSERT HERE
  930: #if HAVE_SIGSEGV_SKIP_INSTRUCTION
  931:   // Ignore writes to ROM
  932:   if ((addr - (uintptr)ROMBaseHost) < ROM_SIZE)
```

The insertion block (from the M1 plan) is correct here. The `#include "mmio_bus.h"` goes
at the top of sheepshaver_glue.cpp alongside the existing includes.

---

## 3. main_unix.cpp second `sigsegv_handler` — which is live?

### 3a. Declaration context (main_unix.cpp:308-319)

```cpp
#if EMULATED_PPC
extern void emul_ppc(uint32 start);
extern void init_emul_ppc(void);
extern void exit_emul_ppc(void);
sigsegv_return_t sigsegv_handler(sigsegv_info_t *sip);   // ← extern declaration, glue's fn
#else
extern "C" void sigusr2_handler_init(int sig, siginfo_t *sip, void *scp);
extern "C" void sigusr2_handler(int sig, siginfo_t *sip, void *scp);
static void sigsegv_handler(int sig, siginfo_t *sip, void *scp);  // ← local fn, different sig
static void sigill_handler(int sig, siginfo_t *sip, void *scp);
#endif
```

### 3b. Handler installation (main_unix.cpp:738-744)

```cpp
#else                                        // ← this is the EMULATED_PPC arm
	// Install SIGSEGV handler for CPU emulator
	if (!sigsegv_install_handler(sigsegv_handler)) {
	    sprintf(str, GetString(STR_SIG_INSTALL_ERR), "SIGSEGV", strerror(errno));
	    ErrorAlert(str);
	    return false;
	}
```

The `#if !EMULATED_PPC` block at lines 696-737 installs the POSIX `sigaction`-based
handler; the `#else` (EMULATED_PPC, lines 738-744) calls `sigsegv_install_handler` with
the **glue handler** declared at line 314.

### 3c. Definitive resolution

`EMULATED_PPC = 1` on macOS arm64 (config.h:12). Therefore:

- `main_unix.cpp:740` installs `sheepshaver_glue.cpp`'s `sigsegv_handler` (the one with
  `sigsegv_return_t` signature) via the Mach exception machinery.
- `main_unix.cpp:2275` (`static void sigsegv_handler(int, siginfo_t*, void*)`) is
  **inside `#if !EMULATED_PPC`** and is **dead code** on macOS arm64 — it never compiles
  into the executable.

**M1 must hook only the glue handler** (`sheepshaver_glue.cpp:919`). The
main_unix.cpp:2275 handler is not compiled and requires no changes for MMIO dispatch.

**Corollary for Task 5 gate widening:** The gate edits at `main_unix.cpp:2309` and
`main_unix.cpp:2477` (inside the dead `#if !EMULATED_PPC` handler) have **no runtime
effect** on macOS arm64. The only gates that matter are `sheepshaver_glue.cpp:947` and
`sheepshaver_glue.cpp:981`. (They are still worth updating for correctness on
non-EMULATED_PPC Linux builds where the main_unix.cpp handler is the live one — but for
the macOS arm64 target they are a no-op.)

---

## 4. SS_TEST_HEX execution path and bus initialization

### 4a. Early exit before all init

`main_unix.cpp:1158-1162`:
```cpp
	/* Early opcode test mode: bypass all SheepShaver init if SS_TEST_HEX is set */
	if (getenv("SS_TEST_HEX") && *getenv("SS_TEST_HEX")) {
		extern bool ss_run_opcode_test(void);
		ss_run_opcode_test();
		return 0;
	}
```

This returns at the **very top of `main()`** (after only Linux ASLR handling). It runs
**before**:
- `PrefsInit()` (line 1292)
- `MachineProfileInit()` (line 1296)
- `sigsegv_install_handler()` (line 740, in `InstallSignalHandlers()`)
- The RAM/ROM mmap + MMIO bus bring-up block (~line 1502)
- `MMIOBusActivate()` (never called)

`ss_run_opcode_test()` allocates its own minimal RAM and creates a `sheepshaver_cpu`
directly. No Mach handler is installed. `mmio_bus_active` stays `false`.
`MachineUsesMMIOBus()` is never called (MachineProfileInit not run).

### 4b. Implication for Task 6 Step 4 smoke

**The plan's Task 6 Step 4 smoke command:**
```bash
SS_MMIO_BUS=1 SS_TEST_HEX=38601234 SS_TEST_JIT=1 make test-opcodes
```
**will NOT work as described.** The `SS_TEST_HEX` path exits main before the bus init.
`[MMIO] bus active ...` will never be printed. `MMIOBusDumpStats` at atexit prints
nothing (n_regions==0). The vector result will be correct (the test-mode path doesn't
touch device space), making this smoke a false pass — it proves nothing about the fault
path.

### 4c. Recommended replacement smoke (standalone unit test approach)

The cheapest valid functional smoke for Task 6 is a **standalone unit test**
`test_mmio_machfault.cpp` (following M0's/Tasks 1-4's pattern), which:

1. Calls `MMIOBusRegister()` + `MMIOBusActivate()` to set up a bus.
2. Constructs a synthetic `arm_thread_state64_t` with:
   - `__pc` pointing to a buffer containing a known JIT-emitted instruction
     (e.g. `0xB8604800 | (1<<16) | (19<<5) | 1` = `LDR w1, [x19, w0, UXTW]`, with
     `x19` irrelevant since we're calling directly, not through a Mach fault)
   - `__x[0]` = the device guest address (e.g. `0xF3012002`)
3. Calls `MMIOMachFaultDispatch(0xF3012002, &ts)` directly.
4. Asserts that the device read callback was invoked, `ts.__x[1]` holds the expected
   value, and `ts.__pc` advanced by 4.

**One caveat:** The `arm_thread_state64_get_pc` / `arm_thread_state64_set_pc_fptr` PAC
macros require pointer-auth capability at runtime. In a standalone clang++ test (no
entitlements, no JIT mapping) the PAC operations are NOPs on non-PAC hardware. Verify
with a `#if __has_feature(ptrauth_calls)` probe in the test comment; on the current M-
series dev machine this is safe.

The `make test-jit` (350/350 paravirtual check) from the plan is the correct gate for
"paravirtual still works" and should remain Step 4's primary check.

---

## 5. Summary of precise insertion points

| Location | File:line | What changes |
|---|---|---|
| `sigsegv_return_t` enum | `sigsegv.h:156` | Add `SIGSEGV_RETURN_STATE_MODIFIED` after `SKIP_INSTRUCTION` |
| `sigsegv_get_thread_state` decl | `sigsegv.h:179` | Add `extern void *sigsegv_get_thread_state(sigsegv_info_t *sip);` |
| `sigsegv_get_thread_state` impl | `sigsegv.cpp:2639` (after mach_set_thread_state closing brace, before `#endif`) | New function with lazy fetch (see §1e) |
| STATE_MODIFIED case in switch | `sigsegv.cpp:2808` (after SKIP `break;`, before FAILURE `case`) | New `#ifdef HAVE_MACH_EXCEPTIONS` case block |
| MMIO dispatch block | `sheepshaver_glue.cpp:928` (after addr= line, before ROM-write check at 932) | New `if (mmio_bus_active)` block |
| `#include "mmio_bus.h"` | `sheepshaver_glue.cpp` top | Add to existing include block |

---

## Plan Corrections Needed

### A. CRITICAL: `sigsegv_get_thread_state` must perform lazy fetch (not just return pointer)

**Plan text (Task 6 Step 1b):** "Implementation returns the `sip->thr_state` ARM_THREAD_STATE64
storage under `HAVE_MACH_EXCEPTIONS` + `_STRUCT_ARM_THREAD_STATE64`, else NULL."

**What the code actually does:** `handle_badaccess` (sigsegv.cpp:2721) sets
`SI.has_thr_state = false`. On arm64 there is **no** thread-state fetch before the user
handler is invoked — the pre-fetch block at lines 2725-2784 is inside `#if defined(__APPLE__)
&& defined(__x86_64__)` and is skipped entirely on arm64. The user handler switch at line
2786 fires with `has_thr_state == false`.

If `sigsegv_get_thread_state` simply returns `&SIP->thr_state` without fetching, the
caller reads uninitialized stack memory — `ts.__pc` will be garbage, the decode will fail
or decode a random instruction, and `ts.__x[acc.rt]` will be written to garbage. The
emulator will then crash or produce silently wrong results when `mach_set_thread_state`
writes the corrupted state back.

**Required fix:** `sigsegv_get_thread_state` must call `mach_get_thread_state(SIP)` if
`!SIP->has_thr_state`, mirroring the lazy pattern already used in the SKIP branch
(lines 2795-2796). The implementation in §1e above already incorporates this fix.

### B. Task 6 Step 4 smoke using `SS_TEST_HEX` is invalid

**Plan text:** "Then a 5-second bus smoke: run the harness single-vector path with the
bus forced on — `SS_MMIO_BUS=1 SS_TEST_HEX=38601234 SS_TEST_JIT=1 make test-opcodes`
Expected: vector passes; stderr shows `[MMIO] bus active …`"

**Reality:** `SS_TEST_HEX` causes main() to return at line 1162, before `PrefsInit`,
`MachineProfileInit`, `sigsegv_install_handler`, and the bus bring-up block. The bus is
never activated. The Mach handler is never installed. `[MMIO] bus active` is never
printed. The test exits 0 (vector passes) purely because it never touches device space —
it is a false pass that proves nothing about the fault path.

**Required fix:** Replace the `SS_TEST_HEX` smoke with a `test_mmio_machfault.cpp`
standalone unit test that calls `MMIOMachFaultDispatch` directly (see §4c for the
full design). The `make test-jit` (350/350) check is sufficient for the
"paravirtual unchanged" gate and should remain the primary build smoke.

### C. Plan's "file shared with BasiliskII" framing is imprecise

**Plan text (Task 6 Step 1):** "This file is shared with BasiliskII: keep every addition
inside `#ifdef HAVE_MACH_EXCEPTIONS`..."

**Reality:** BasiliskII has its **own** separate copy of `sigsegv.cpp`/`sigsegv.h` at
`BasiliskII/src/CrossPlatform/`. The two files are structurally identical but
independently maintained (diff shows only whitespace-level divergence). Modifying
`SheepShaver/src/CrossPlatform/sigsegv.cpp` does not affect BasiliskII's copy. The
`#ifdef HAVE_MACH_EXCEPTIONS` guard advice is still good hygiene for
cross-platform correctness within SheepShaver's own build matrix (Linux/x86/etc.), but
the motivation is not "shared file with BasiliskII."

### D. Task 5 gate edits at main_unix.cpp:2309/2477 are runtime no-ops on macOS arm64

Not a blocking error, but worth noting: those two lines live inside `#if !EMULATED_PPC`
(the dead handler). The actual runtime gates that must change are only
`sheepshaver_glue.cpp:947` and `sheepshaver_glue.cpp:981`. The main_unix.cpp gate edits
matter for Linux/PPC non-emulated builds only.
