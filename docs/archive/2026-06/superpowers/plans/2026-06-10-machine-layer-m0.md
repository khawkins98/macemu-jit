> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Machine Layer M0 — Profile Plumbing + Machine Description Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land milestone M0 of `docs/planning/MACHINE-LAYER-PLAN.md`: a pref-selected machine profile (`paravirtual` default / `newworld`), the `SS_NW_*` env-gate sprawl consolidated under it, the legacy serial-skip hacks + `ignoresegv` disabled on the fidelity profile, the paravirtual path byte-identical — plus the two M0 design artifacts (Core99 machine description, ROM-patch audit table).

**Architecture:** A new `machine/` module owns one process-wide `MachineProfile` enum, initialized once after `PrefsInit` from pref + env (with legacy `SS_NW_TRAMPOLINE` mapping). All former `getenv("SS_NW_*")` sites and the to-be-gated hack sites consult this module. The decision logic is a pure function, unit-tested by a standalone test binary (no emulator build needed). The two docs are research artifacts produced from the spike results + the codebase itself.

**Tech Stack:** C++ (emulator), standalone clang++ test binary, autoconf build (`config.status` regen), capstone/grep for the doc tasks.

**Non-regression contract (applies to every task):** the paravirtual profile (default, no env vars) must behave byte-identically. Gates: `make build-ss` succeeds, `make test-jit` stays score=100, and a final `make e2e` checkpoint (Task 5; needs a GUI session — sanctioned isolated-config exception).

**Key references:**
- Spec: `docs/planning/MACHINE-LAYER-PLAN.md` (M0 row in §3; §2b "disables ignoresegv + legacy serial-skips"; §2a machine description; M0 ROM-patch audit)
- Spike results: `docs/planning/spikes/SPIKE-S1-QEMU-GATE-CHECK.md`, `SPIKE-S3-STALL-DEVICE-PROBE.md`
- Build note: after editing `SheepShaver/src/Unix/Makefile.in`, regenerate with `cd SheepShaver/src/Unix && ./config.status Makefile` (config.status is present; no full reconfigure needed).
- Commit style: project uses `feat(...)/fix(...)/docs(...)` prefixes; NEVER put backticks in `git commit -m` — use `git commit -F-` heredoc for multi-line messages.

---

## File structure

| File | Responsibility |
|---|---|
| Create `SheepShaver/src/include/machine_profile.h` | Public API: `MachineProfile` enum, pure `MachineProfileParse()`, `MachineProfileInit()`, `CurrentMachineProfile()`, `MachineProfileIsNewWorld()`, `MachineEnvFlag()` |
| Create `SheepShaver/src/machine/machine_profile.cpp` | Implementation; the only file that reads the `machine` pref / `SS_MACHINE` / legacy `SS_NW_TRAMPOLINE` |
| Create `SheepShaver/src/machine/test_machine_profile.cpp` | Standalone unit test (assert-based, exit 0/1) |
| Create `SheepShaver/src/machine/Makefile` | Builds + runs the standalone test (`make -C SheepShaver/src/machine test`) |
| Modify `SheepShaver/src/prefs_items.cpp` | Register `machine` pref + default `"paravirtual"` |
| Modify `SheepShaver/src/Unix/Makefile.in` | Add `../machine/machine_profile.cpp` to SRCS |
| Modify `SheepShaver/src/Unix/main_unix.cpp` | Call `MachineProfileInit()` after `PrefsInit`; consolidate 1 `SS_NW_TRAMPOLINE` site; gate serial-skips + `ignoresegv` |
| Modify `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` | Consolidate 2 `SS_NW_TRAMPOLINE` + 1 `SS_NW_SYNTH_ENTRY` sites; gate serial-skips + `ignoresegv` |
| Modify `SheepShaver/src/rom_patches.cpp` | Consolidate 4 `SS_NW_TRAMPOLINE` + 1 `SS_NW_MODEL` sites |
| Modify `SheepShaver/src/name_registry.cpp` | Consolidate 1 `SS_NW_MODEL` site |
| Create `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` | §2a artifact: address map, interrupt tree, device-tree skeleton |
| Create `docs/planning/machine/ROM-PATCH-AUDIT.md` | M0 audit: device-neutralizing ROM patches classified |
| Modify `docs/planning/MACHINE-LAYER-PLAN.md`, `CHANGELOG.md` | Bookkeeping: M0 → ✅ |

**Profile precedence (the design decision, locked here):**
1. `SS_MACHINE` env (`paravirtual` / `newworld`) — explicit, wins over everything.
2. Legacy `SS_NW_TRAMPOLINE` env set (non-empty) → `newworld` + deprecation warning. Preserves every existing diagnostic workflow verbatim.
3. `machine` pref (default `"paravirtual"`).
4. Unknown values → `paravirtual` + warning (never abort: a typo must not brick a boot).

**What is NOT gated in M0** (deliberate): the zero-page-write skip in the sigsegv handler (SheepMem behavior, both profiles need it) and the "VM settings"/installer `0xf8000000` skips' *semantics* — they are gated together with the serial skips because they are the same class of PC-keyed hack, but if a fidelity boot later needs an installer path, that's an M1+ device/bus decision, not M0's.

---

### Task 1: `machine_profile` module — pure logic + standalone test (TDD)

**Files:**
- Create: `SheepShaver/src/include/machine_profile.h`
- Create: `SheepShaver/src/machine/machine_profile.cpp`
- Create: `SheepShaver/src/machine/test_machine_profile.cpp`
- Create: `SheepShaver/src/machine/Makefile`

- [ ] **Step 1: Write the header (API contract first)**

`SheepShaver/src/include/machine_profile.h`:

```cpp
/*
 *  machine_profile.h - Machine Layer profile selection (MACHINE-LAYER-PLAN.md M0)
 *
 *  Two machine profiles:
 *    MACHINE_PARAVIRTUAL - today's proven path (ROM patches + HLE shims). Default.
 *    MACHINE_NEWWORLD    - the Core99-class fidelity profile (MMIO bus, device
 *                          models, supervisor environment), grown milestone by
 *                          milestone. Selecting it disables the legacy PC-keyed
 *                          serial-skip hacks and the ignoresegv blanket skip.
 *
 *  Precedence: SS_MACHINE env > legacy SS_NW_TRAMPOLINE env (deprecated alias
 *  for newworld) > "machine" pref > default paravirtual. Unknown values fall
 *  back to paravirtual with a warning (never abort).
 */

#ifndef MACHINE_PROFILE_H
#define MACHINE_PROFILE_H

enum MachineProfile {
	MACHINE_PARAVIRTUAL = 0,
	MACHINE_NEWWORLD    = 1
};

// Pure decision function (unit-tested standalone; no emulator dependencies).
// pref / env_machine may be NULL. legacy_nw_trampoline = SS_NW_TRAMPOLINE set.
// On deprecated/unknown input, *warning receives a static message (else NULL).
extern MachineProfile MachineProfileParse(const char *pref,
                                          const char *env_machine,
                                          bool legacy_nw_trampoline,
                                          const char **warning);

// Process-wide state. Init once, immediately after PrefsInit().
extern void MachineProfileInit(void);
extern MachineProfile CurrentMachineProfile(void);
extern bool MachineProfileIsNewWorld(void);

// Centralized diagnostic env flags (SS_NW_MODEL, SS_NW_SYNTH_ENTRY, ...):
// true iff the variable is set, non-empty, and not "0".
extern bool MachineEnvFlag(const char *name);

#endif // MACHINE_PROFILE_H
```

- [ ] **Step 2: Write the failing test**

`SheepShaver/src/machine/test_machine_profile.cpp`:

```cpp
/*
 *  test_machine_profile.cpp - standalone unit test for MachineProfileParse /
 *  MachineEnvFlag. Builds without the emulator (MACHINE_PROFILE_STANDALONE_TEST).
 *  Run: make -C SheepShaver/src/machine test
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "machine_profile.h"

static int failures = 0;

#define CHECK(cond) do { \
	if (cond) { printf("PASS: %s\n", #cond); } \
	else      { printf("FAIL: %s (line %d)\n", #cond, __LINE__); failures++; } \
} while (0)

int main()
{
	const char *warn;

	// Defaults: nothing set -> paravirtual, no warning
	warn = (const char *)1;
	CHECK(MachineProfileParse(NULL, NULL, false, &warn) == MACHINE_PARAVIRTUAL);
	CHECK(warn == NULL);

	// Pref selects
	CHECK(MachineProfileParse("paravirtual", NULL, false, &warn) == MACHINE_PARAVIRTUAL);
	CHECK(MachineProfileParse("newworld", NULL, false, &warn) == MACHINE_NEWWORLD);
	CHECK(warn == NULL);

	// Unknown pref -> paravirtual + warning (never abort)
	CHECK(MachineProfileParse("g3beige", NULL, false, &warn) == MACHINE_PARAVIRTUAL);
	CHECK(warn != NULL);

	// SS_MACHINE env wins over pref
	CHECK(MachineProfileParse("paravirtual", "newworld", false, &warn) == MACHINE_NEWWORLD);
	CHECK(MachineProfileParse("newworld", "paravirtual", false, &warn) == MACHINE_PARAVIRTUAL);

	// Unknown env -> fall through to pref + warning
	CHECK(MachineProfileParse("newworld", "bogus", false, &warn) == MACHINE_NEWWORLD);
	CHECK(warn != NULL);

	// Legacy SS_NW_TRAMPOLINE -> newworld + deprecation warning
	CHECK(MachineProfileParse(NULL, NULL, true, &warn) == MACHINE_NEWWORLD);
	CHECK(warn != NULL);
	CHECK(MachineProfileParse("paravirtual", NULL, true, &warn) == MACHINE_NEWWORLD);

	// Explicit SS_MACHINE beats legacy flag
	CHECK(MachineProfileParse(NULL, "paravirtual", true, &warn) == MACHINE_PARAVIRTUAL);

	// MachineEnvFlag: set/unset/zero/empty
	setenv("MP_TEST_FLAG", "1", 1);
	CHECK(MachineEnvFlag("MP_TEST_FLAG") == true);
	setenv("MP_TEST_FLAG", "0", 1);
	CHECK(MachineEnvFlag("MP_TEST_FLAG") == false);
	setenv("MP_TEST_FLAG", "", 1);
	CHECK(MachineEnvFlag("MP_TEST_FLAG") == false);
	unsetenv("MP_TEST_FLAG");
	CHECK(MachineEnvFlag("MP_TEST_FLAG") == false);

	printf(failures ? "RESULT: %d FAILURES\n" : "RESULT: ALL PASS\n", failures);
	return failures ? 1 : 0;
}
```

`SheepShaver/src/machine/Makefile`:

```makefile
# Standalone unit test for machine_profile (no emulator build needed).
CXX      ?= clang++
CXXFLAGS ?= -std=c++11 -Wall -g
CPPFLAGS  = -I../include -DMACHINE_PROFILE_STANDALONE_TEST

test: test_machine_profile
	./test_machine_profile

test_machine_profile: test_machine_profile.cpp machine_profile.cpp ../include/machine_profile.h
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_machine_profile.cpp machine_profile.cpp

clean:
	rm -f test_machine_profile

.PHONY: test clean
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `make -C SheepShaver/src/machine test`
Expected: build FAILURE — `machine_profile.cpp: No such file or directory` (the implementation doesn't exist yet).

- [ ] **Step 4: Write the implementation**

`SheepShaver/src/machine/machine_profile.cpp`:

```cpp
/*
 *  machine_profile.cpp - Machine Layer profile selection
 *  See docs/planning/MACHINE-LAYER-PLAN.md (M0) and include/machine_profile.h.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "machine_profile.h"

#ifndef MACHINE_PROFILE_STANDALONE_TEST
#include "sysdeps.h"
#include "prefs.h"
#endif

static MachineProfile g_profile = MACHINE_PARAVIRTUAL;

static bool parse_one(const char *s, MachineProfile *out)
{
	if (s == NULL)
		return false;
	if (strcmp(s, "paravirtual") == 0) { *out = MACHINE_PARAVIRTUAL; return true; }
	if (strcmp(s, "newworld") == 0)    { *out = MACHINE_NEWWORLD;    return true; }
	return false;
}

MachineProfile MachineProfileParse(const char *pref, const char *env_machine,
                                   bool legacy_nw_trampoline, const char **warning)
{
	*warning = NULL;
	MachineProfile p;

	// 1. SS_MACHINE env: explicit, wins over everything.
	if (env_machine != NULL && *env_machine != '\0') {
		if (parse_one(env_machine, &p))
			return p;
		*warning = "SS_MACHINE has an unknown value (expected paravirtual|newworld) - ignored";
		// fall through to the next source
	}

	// 2. Legacy SS_NW_TRAMPOLINE: deprecated alias for the newworld profile.
	if (legacy_nw_trampoline) {
		*warning = "SS_NW_TRAMPOLINE is deprecated - use SS_MACHINE=newworld or the 'machine' pref";
		return MACHINE_NEWWORLD;
	}

	// 3. The "machine" pref (registered default: "paravirtual").
	if (pref != NULL && *pref != '\0') {
		if (parse_one(pref, &p))
			return p;
		*warning = "machine pref has an unknown value (expected paravirtual|newworld) - using paravirtual";
	}

	// 4. Default.
	return MACHINE_PARAVIRTUAL;
}

bool MachineEnvFlag(const char *name)
{
	const char *v = getenv(name);
	return v != NULL && *v != '\0' && strcmp(v, "0") != 0;
}

#ifndef MACHINE_PROFILE_STANDALONE_TEST
void MachineProfileInit(void)
{
	const char *warning = NULL;
	g_profile = MachineProfileParse(PrefsFindString("machine"),
	                                getenv("SS_MACHINE"),
	                                getenv("SS_NW_TRAMPOLINE") != NULL,
	                                &warning);
	if (warning)
		fprintf(stderr, "[MACHINE] WARNING: %s\n", warning);
	fprintf(stderr, "[MACHINE] profile=%s\n",
	        g_profile == MACHINE_NEWWORLD ? "newworld" : "paravirtual");
}
#endif

MachineProfile CurrentMachineProfile(void)
{
	return g_profile;
}

bool MachineProfileIsNewWorld(void)
{
	return g_profile == MACHINE_NEWWORLD;
}
```

Note: in the standalone test build there is no `MachineProfileInit` (it needs prefs); the test exercises the pure functions only. `g_profile` defaults to `MACHINE_PARAVIRTUAL`, so the accessors are safe even pre-init.

- [ ] **Step 5: Run the test to verify it passes**

Run: `make -C SheepShaver/src/machine test`
Expected: `RESULT: ALL PASS`, exit 0.

- [ ] **Step 6: Commit**

```bash
cd /Users/khawkins/Documents/git/macemu-jit
git add SheepShaver/src/include/machine_profile.h SheepShaver/src/machine/
git commit -m "feat(machine): M0 machine-profile module with standalone unit test"
```

---

### Task 2: Register the `machine` pref and wire `MachineProfileInit` into startup

**Files:**
- Modify: `SheepShaver/src/prefs_items.cpp` (table ~line 28-80; `AddPrefsDefaults` ~line 89)
- Modify: `SheepShaver/src/Unix/Makefile.in` (SRCS list, ~line 68)
- Modify: `SheepShaver/src/Unix/main_unix.cpp` (~line 1291)

- [ ] **Step 1: Add the pref item**

In `SheepShaver/src/prefs_items.cpp`, inside `common_prefs_items[]` (after the `{"rom", ...}` entry at ~line 45), add:

```cpp
	{"machine", TYPE_STRING, false,     "machine profile: paravirtual (default) or newworld"},
```

In `AddPrefsDefaults()` (after the `PrefsAddBool("ignoresegv", true);` line at ~line 108), add:

```cpp
	// Machine Layer profile (docs/planning/MACHINE-LAYER-PLAN.md). paravirtual =
	// today's proven path; newworld = the Core99 fidelity profile (M0+).
	PrefsAddString("machine", "paravirtual");
```

- [ ] **Step 2: Add the source file to the build**

In `SheepShaver/src/Unix/Makefile.in` ~line 68, the SRCS continuation line currently reads:

```
    ../rom_patches.cpp ../rsrc_patches.cpp ../emul_op.cpp ../ui_introspect.cpp ../name_registry.cpp \
```

Change it to:

```
    ../rom_patches.cpp ../rsrc_patches.cpp ../emul_op.cpp ../ui_introspect.cpp ../name_registry.cpp \
    ../machine/machine_profile.cpp \
```

Then regenerate the Makefile:

```bash
cd SheepShaver/src/Unix && ./config.status Makefile
```

- [ ] **Step 3: Call `MachineProfileInit` after `PrefsInit`**

In `SheepShaver/src/Unix/main_unix.cpp`: add the include near the other local includes at the top of the file (it already includes `"prefs.h"`):

```cpp
#include "machine_profile.h"
```

At ~line 1291, change:

```cpp
	// Read preferences
	PrefsInit(vmdir, argc, argv);
```

to:

```cpp
	// Read preferences
	PrefsInit(vmdir, argc, argv);

	// Resolve the machine profile (MACHINE-LAYER-PLAN.md M0): pref + env,
	// before anything consults it (ROM patching, sigsegv policy, NW gates).
	MachineProfileInit();
```

- [ ] **Step 4: Build and verify the log line**

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver && make build-ss
```

Expected: clean build. Then verify the default profile prints without booting (use the harness path, which runs the full init):

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver && \
  SS_TEST_HEX=38601234 SS_TEST_JIT=0 make test-opcodes 2>&1 | grep '\[MACHINE\]'
```

Expected output: `[MACHINE] profile=paravirtual`

And the env override + legacy mapping:

```bash
SS_MACHINE=newworld SS_TEST_HEX=38601234 SS_TEST_JIT=0 make test-opcodes 2>&1 | grep '\[MACHINE\]'
# Expected: [MACHINE] profile=newworld
SS_NW_TRAMPOLINE=1 SS_TEST_HEX=38601234 SS_TEST_JIT=0 make test-opcodes 2>&1 | grep '\[MACHINE\]'
# Expected: [MACHINE] WARNING: SS_NW_TRAMPOLINE is deprecated ...
#           [MACHINE] profile=newworld
```

(If `make test-opcodes` does not reach prefs init in this configuration, fall back to: `printf 'rom /Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom\nramsize 268435456\nnogui true\n' > /tmp/m0.prefs && timeout 5 ./SheepShaver --config /tmp/m0.prefs 2>&1 | grep MACHINE` — a <5s init-only run, killed before boot proper; `pkill -9 -x SheepShaver` after.)

- [ ] **Step 5: Run the JIT gate**

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver && make test-jit 2>&1 | tail -3
```

Expected: `METRIC ... score=100` (count unchanged from current baseline — run `make harness-count` if unsure).

- [ ] **Step 6: Commit**

```bash
cd /Users/khawkins/Documents/git/macemu-jit
git add SheepShaver/src/prefs_items.cpp SheepShaver/src/Unix/Makefile.in SheepShaver/src/Unix/main_unix.cpp
git commit -m "feat(machine): register machine pref and init profile at startup"
```

---

### Task 3: Consolidate the `SS_NW_*` env gates through the module

Every `getenv("SS_NW_TRAMPOLINE")` becomes `MachineProfileIsNewWorld()`; every `getenv("SS_NW_MODEL")` / `SS_NW_SYNTH_ENTRY` read becomes `MachineEnvFlag(...)`. Behavior is preserved exactly (legacy env still maps to the profile via Task 1's precedence). The 9 sites:

**Files:**
- Modify: `SheepShaver/src/rom_patches.cpp` (4× `SS_NW_TRAMPOLINE`, 1× `SS_NW_MODEL`)
- Modify: `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` (2× `SS_NW_TRAMPOLINE`, 1× `SS_NW_SYNTH_ENTRY`)
- Modify: `SheepShaver/src/Unix/main_unix.cpp` (1× `SS_NW_TRAMPOLINE`)
- Modify: `SheepShaver/src/name_registry.cpp` (1× `SS_NW_MODEL`)

- [ ] **Step 1: Enumerate the exact sites (they may have drifted)**

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver/src
grep -rn 'getenv("SS_NW_' --include='*.cpp' .
```

Expected: 9 hits across the 4 files above. If the count differs, list them all and convert every one.

- [ ] **Step 2: Convert each site**

Add `#include "machine_profile.h"` to each of the 4 files (near their existing includes of `"prefs.h"` or `"sysdeps.h"`).

Conversion patterns — apply to every hit from Step 1:

```cpp
// BEFORE (typical SS_NW_TRAMPOLINE site, e.g. sheepshaver_glue.cpp HandleInterrupt):
static const bool nw_tramp = (ROMType == ROMTYPE_NEWWORLD && getenv("SS_NW_TRAMPOLINE"));
// AFTER:
static const bool nw_tramp = (ROMType == ROMTYPE_NEWWORLD && MachineProfileIsNewWorld());

// BEFORE (typical bare check, e.g. rom_patches.cpp):
if (getenv("SS_NW_TRAMPOLINE")) {
// AFTER:
if (MachineProfileIsNewWorld()) {

// BEFORE (SS_NW_MODEL value-check pattern, rom_patches.cpp patch_68k universal_info):
const char *nw = getenv("SS_NW_MODEL");
if (nw && *nw && *nw != '0') {
// AFTER:
if (MachineEnvFlag("SS_NW_MODEL")) {

// BEFORE (SS_NW_SYNTH_ENTRY in sheepshaver_glue.cpp):
... getenv("SS_NW_SYNTH_ENTRY") ...
// AFTER (preserve the site's existing null/value semantics):
... MachineEnvFlag("SS_NW_SYNTH_ENTRY") ...
```

NOTE: `MachineProfileInit()` runs right after `PrefsInit` (Task 2), which precedes ROM loading/patching and `init_emul_ppc` — every converted site executes after init. Do NOT convert the `getenv("SS_NW_TRAMPOLINE") != NULL` read *inside* `machine_profile.cpp` itself (that one IS the legacy mapping).

- [ ] **Step 3: Verify no stragglers**

```bash
grep -rn 'getenv("SS_NW_' --include='*.cpp' /Users/khawkins/Documents/git/macemu-jit/SheepShaver/src
```

Expected: exactly 1 hit — `machine/machine_profile.cpp` (the legacy mapping).

- [ ] **Step 4: Build + gate**

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver && make build-ss && make test-jit 2>&1 | tail -3
```

Expected: clean build, `score=100`.

- [ ] **Step 5: Commit**

```bash
cd /Users/khawkins/Documents/git/macemu-jit
git add SheepShaver/src/rom_patches.cpp SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp \
        SheepShaver/src/Unix/main_unix.cpp SheepShaver/src/name_registry.cpp
git commit -m "refactor(machine): consolidate SS_NW_* env gates through machine_profile"
```

CAUTION: `sheepshaver_glue.cpp` and `rom_patches.cpp` carry uncommitted pre-existing experimental changes (Path A scaffolding, `[NW-INT]` logging). Commit ONLY the hunks this task touches if possible (`git add -p` is unavailable to agents non-interactively — instead, keep your edits minimal and commit the whole files ONLY after confirming with the user, or stage via `git diff`/`git apply` of your own hunks). If in doubt: stop and ask the user how to handle the mixed files.

---

### Task 4: Gate the legacy serial-skip hacks + `ignoresegv` on the fidelity profile

Per MACHINE-LAYER-PLAN §2b: on the `newworld` profile, a skip firing before bus dispatch would silently eat a device access — so the PC-keyed skip hacks and the `ignoresegv` blanket skip must be OFF. On `paravirtual` they stay exactly as-is.

**Files:**
- Modify: `SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` (~lines 940-975: skip block + `ignoresegv`)
- Modify: `SheepShaver/src/Unix/main_unix.cpp` (~lines 2300-2335: skip block; ~line 2466: `ignoresegv`)

- [ ] **Step 1: Gate the sigsegv-handler skip block in `sheepshaver_glue.cpp`**

At ~line 940, the block starts with `if (mac_fault) {` followed by the chain of PC-keyed skips ("VM settings", "MacOS 8.5 installation", serial drivers, DR-cache variants), then the zero-page skip, then `ignoresegv`. Restructure so the PC-keyed skips and `ignoresegv` are paravirtual-only, while the zero-page skip stays unconditional:

```cpp
	if (mac_fault) {

		// Legacy PC-keyed skip hacks (installer probes, serial-driver device
		// probes). PARAVIRTUAL ONLY: on the newworld fidelity profile these
		// would silently eat MMIO accesses the bus must see
		// (MACHINE-LAYER-PLAN.md section 2b).
		if (!MachineProfileIsNewWorld()) {

			// "VM settings" during MacOS 8 installation
			if (pc == ROMBase + 0x488160 && cpu->gpr(20) == 0xf8000000)
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;

			// MacOS 8.5 installation
			else if (pc == ROMBase + 0x488140 && cpu->gpr(16) == 0xf8000000)
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;

			// MacOS 8 serial drivers on startup
			else if (pc == ROMBase + 0x48e080 && (cpu->gpr(8) == 0xf3012002 || cpu->gpr(8) == 0xf3012000))
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;

			// MacOS 8.1 serial drivers on startup
			else if (pc == ROMBase + 0x48c5e0 && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;
			else if (pc == ROMBase + 0x4a10a0 && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;

			// MacOS 8.6 serial drivers on startup (with DR Cache and OldWorld ROM)
			else if ((pc - DR_CACHE_BASE) < DR_CACHE_SIZE && (cpu->gpr(16) == 0xf3012002 || cpu->gpr(16) == 0xf3012000))
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;
			else if ((pc - DR_CACHE_BASE) < DR_CACHE_SIZE && (cpu->gpr(20) == 0xf3012002 || cpu->gpr(20) == 0xf3012000))
				return SIGSEGV_RETURN_SKIP_INSTRUCTION;
		}

		// Ignore writes to the zero page (SheepMem; both profiles)
		if ((uint32)(addr - SheepMem::ZeroPage()) < (uint32)SheepMem::PageSize())
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;

		// Ignore all other faults, if requested. PARAVIRTUAL ONLY: on the
		// newworld profile an unexpected fault must abort loudly, not be
		// silently skipped (MACHINE-LAYER-PLAN.md section 2b / section 6).
		if (!MachineProfileIsNewWorld() && PrefsFindBool("ignoresegv"))
			return SIGSEGV_RETURN_SKIP_INSTRUCTION;
	}
```

(Keep the surrounding `#else / #error` lines untouched. Note the zero-page skip changes from `else if` to `if` because the preceding chain is now inside its own block — preserve exact comparison expressions.)

- [ ] **Step 2: Gate the duplicate skip block in `main_unix.cpp`**

At ~line 2300, wrap the identical chain ("VM settings" through the two DR-cache entries, each ending in `r->pc() += 4; ... return;`) in the same guard:

```cpp
		// Legacy PC-keyed skip hacks - PARAVIRTUAL ONLY (see sheepshaver_glue.cpp
		// sigsegv handler; MACHINE-LAYER-PLAN.md section 2b).
		if (!MachineProfileIsNewWorld()) {
			// "VM settings" during MacOS 8 installation
			if (r->pc() == ROMBase + 0x488160 && r->gpr(20) == 0xf8000000) {
				...existing chain verbatim through the second DR_CACHE entry...
			}
		}
```

i.e. insert the `if (!MachineProfileIsNewWorld()) {` line before the first `// "VM settings"` comment and the closing `}` after the final DR-cache `return;` + its closing brace, re-indenting the chain by one level. Do not alter any condition or body.

At ~line 2466, change:

```cpp
		// Ignore illegal memory accesses?
		if (PrefsFindBool("ignoresegv")) {
```

to:

```cpp
		// Ignore illegal memory accesses? (paravirtual only - the newworld
		// profile must abort loudly on unexpected faults)
		if (!MachineProfileIsNewWorld() && PrefsFindBool("ignoresegv")) {
```

- [ ] **Step 3: Build + gates**

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver && make build-ss && make test-jit 2>&1 | tail -3
```

Expected: clean build, `score=100`.

- [ ] **Step 4: Commit**

```bash
cd /Users/khawkins/Documents/git/macemu-jit
git add SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp SheepShaver/src/Unix/main_unix.cpp
git commit -m "feat(machine): disable legacy serial-skip hacks and ignoresegv on the newworld profile"
```

(Same mixed-file caution as Task 3 for `sheepshaver_glue.cpp`.)

---

### Task 5: Paravirtual non-regression checkpoint (system-level)

**Files:** none (verification only)

- [ ] **Step 1: Offline gates one more time, from clean**

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver && make build-ss && make test-jit 2>&1 | tail -3 && make -C src/machine test
```

Expected: build OK, `score=100`, `RESULT: ALL PASS`.

- [ ] **Step 2: E2E lifecycle smoke (sanctioned isolated-config boot; needs a logged-in GUI session)**

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver && make e2e
```

Expected: `PASS: clean lifecycle: booted to Finder, clean shutdown, exit 0`, and the run log contains `[MACHINE] profile=paravirtual`. If no GUI session is available, STOP and ask the user to run `make e2e` — do not skip this gate silently; M0 is not done until it passes.

- [ ] **Step 3: Verify the deprecation path still boots the NW diagnostic config (optional, cheap)**

This confirms legacy workflows still work (profile mapping, not behavior change): run the documented NW diagnostic boot once and check the log shows the deprecation warning + `profile=newworld`, and that the `[NW-TRAMP]` lines still appear:

```bash
printf 'rom /Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom\nramsize 268435456\nnogui true\n' > /tmp/m0nw.prefs
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver && pkill -9 -x SheepShaver; \
  SS_NW_TRAMPOLINE=1 SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1 timeout 30 ./SheepShaver --config /tmp/m0nw.prefs 2>&1 | \
  grep -E '\[MACHINE\]|\[NW-TRAMP\]' | head -5; pkill -9 -x SheepShaver
```

Expected: the WARNING line, `profile=newworld`, and at least one `[NW-TRAMP]` line (same as before this change).

---

### Task 6: Machine description artifact — `CORE99-MACHINE-DESCRIPTION.md`

The §2a contract everything in M1–M5 implements against. This is a research/writing task built from the spike results, SheepShaver's own AddrMap patch, and QEMU's mac99 machine as conformance reference.

**Files:**
- Create: `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md`

- [ ] **Step 1: Gather the source data (commands, not vibes)**

```bash
# 1. SheepShaver's own NewWorld AddrMap (what we already tell the guest):
sed -n '1890,1950p' /Users/khawkins/Documents/git/macemu-jit/SheepShaver/src/rom_patches.cpp
# 2. Spike S3's verified identities/offsets:
cat /Users/khawkins/Documents/git/macemu-jit/docs/planning/spikes/SPIKE-S3-STALL-DEVICE-PROBE.md
# 3. QEMU's Core99 machine source (device bases, IRQ numbers) - read on GitHub:
#    https://github.com/qemu/qemu/blob/master/hw/ppc/mac_newworld.c
#    https://github.com/qemu/qemu/blob/master/hw/misc/macio/macio.c
#    (WebFetch these; cite the file+line for every address you record)
# 4. The 9.0.1 ROM's actual probes: docs/planning/spikes/SPIKE-S1-QEMU-GATE-CHECK.md
#    + HANDOFF-NEWWORLD-SUPERVISOR-MMU.md section 1.7.1 (check_work SCC init sequence)
```

- [ ] **Step 2: Write the document**

Create `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` with exactly these sections, each row carrying a source citation (file:line or URL):

```markdown
# Core99 Machine Description (Machine Layer §2a contract)

> Status / created / purpose header (match house style of MACHINE-LAYER-PLAN.md)
> Versioned contract: M1-M5 implement against THIS file; change it by PR, not drift.

## 1. Physical address map
| Region | Base | Size | Kind (trapped-MMIO / mapped-aperture / RAM / ROM) | Source |
- RAM 0x00000000, ROM 0x50000000 (SheepShaver placement - cite main_unix.cpp/ROMBase)
- MacIO (KeyLargo) container base 0xF3000000 - cite QEMU mac_newworld.c + our AddrMap
- ESCC legacy/compat SCC base 0xF3012000 (ch A ctrl +2, data +6) - cite S3 + AddrMap
- ESCC MacIO-native base 0xF3013000 (if the ROM uses it - check S3/QEMU; record both)
- VIA-Cuda base 0xF3016000, register stride 0x200 - cite S3 + AddrMap
- OpenPIC base (QEMU mac99: 0xF3040000 region - verify in mac_newworld.c and cite)
- DBDMA channel block base (verify in macio.c and cite) - stubs-only until M4
- Boot framebuffer aperture (placeholder base; finalized in M5 - mark as such explicitly)

## 2. Interrupt tree
| Source device | PIC input # | Source |
(OpenPIC inputs for ESCC, VIA/Cuda, DBDMA per QEMU mac_newworld.c - cite each. Note
DEC is a CPU-internal exception, NOT a PIC input - MACHINE-LAYER-PLAN section 2c.)

## 3. Device-tree skeleton
The node list the M5 Trampoline synthesis publishes: mac-io node + children
(escc, via-cuda, nvram), cpu node, memory node, display node (HLE video - MUST NOT
advertise DBDMA channels per section 2f). For each node: name, device_type,
reg (from section 1), interrupts (from section 2). Cross-check property names against
name_registry.cpp (what SheepShaver already fakes) and cite.

## 4. What the 9.0.1 ROM actually probes (evidence-driven scope fence)
Table: probe site -> device/register -> evidence (S3 disassembly, check_work WR
sequence from HANDOFF 1.7.1, serial monitor loop). This is the M1 "implement exactly
this" fence - registers not in this table get abort-loudly stubs.

## 5. Open questions
(anything the sources disagree on; one line each with the experiment that settles it)
```

Fill every table row from the Step 1 sources. Any value you cannot source: put it in §5 Open questions instead of guessing — an honest gap beats a confident wrong address (this project has flip-flopped on device identities three times; see SPIKE-S3).

- [ ] **Step 3: Self-check**

Every address row has a citation; the §4 fence covers both S3 consumers (serial monitor + check_work); the display node carries the no-DBDMA constraint; DEC explicitly excluded from the PIC tree.

- [ ] **Step 4: Commit**

```bash
cd /Users/khawkins/Documents/git/macemu-jit
git add docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md
git commit -m "docs(machine): Core99 machine description - the M0 contract artifact"
```

---

### Task 7: ROM-patch audit — `ROM-PATCH-AUDIT.md`

Rev-3 finding #16: device models behind patched-out guest init are dead code. Classify every device-neutralizing ROM patch.

**Files:**
- Create: `docs/planning/machine/ROM-PATCH-AUDIT.md`

- [ ] **Step 1: Enumerate the patch sites**

```bash
cd /Users/khawkins/Documents/git/macemu-jit/SheepShaver/src
# All named find_rom_data patterns + their patch bodies:
grep -n "static const uint8 .*_dat\[\]" rom_patches.cpp
# The known device-neutralizing cluster (from the rev-3 review):
grep -n "via_init\|scc_init\|cuda_init\|gc_mask\|run_diags\|powermac_id\|nvram" rom_patches.cpp | head -40
```

Also read `docs/planning/PATCH-68K-SHIM-INVENTORY.md` (the 84 `patch_68k` patterns are already inventoried there — do NOT re-derive them; reference + classify).

- [ ] **Step 2: Write the audit document**

Create `docs/planning/machine/ROM-PATCH-AUDIT.md`:

```markdown
# ROM-Patch Audit - device-neutralizing patches vs the Machine Layer

> Purpose: MACHINE-LAYER-PLAN M0 deliverable (rev-3 finding #16). On the newworld
> fidelity profile, each device model is dead code until the patch that NOPs the
> guest's init of that device is retired. This table assigns every such patch a
> disposition and an owning milestone.

## Disposition key
- KEEP - harmless/required on both profiles (e.g. EMUL_OP trap inserts for HLE we keep)
- RETIRE@Mx - must be skipped on the newworld profile when milestone Mx's device
  model lands (the milestone's DoD asserts the un-patched init completes)
- REPLACE@Mx - the patch is the paravirtual stand-in for a device model
- PARAVIRTUAL-ONLY - applies only on the paravirtual profile by nature (skip on newworld from M0)

## PatchROM / patch_nanokernel / patch_68k device-neutralizing patches
| Patch (name_dat / site) | rom_patches.cpp line | What it neutralizes | Disposition | Owning milestone |
(one row per site found in Step 1; at minimum: via_init/via_init2/via_init3 [~1916-1944],
scc_init [~2121-2135], cuda_init [~2390], GC interrupt-mask writes [~2337], run_diags,
powermac_id, the NVRAM cluster, the serial-skip-adjacent patches. Verify each line
number by reading the code - the ~values here are from the rev-3 review and may drift.)

## patch_68k shims (referenced, not re-derived)
One paragraph: PATCH-68K-SHIM-INVENTORY.md remains the per-pattern inventory; M6 owns
triage. Note here only the shims that touch DEVICES (Cuda/SCC/VIA/NVRAM rows from that
inventory) with the same disposition columns.

## How dispositions get enforced
The mechanism (profile check inside PatchROM vs a patch-table flag) is an M1 design
decision - this doc only assigns dispositions. Note the candidate mechanisms.
```

Fill the main table by reading each patch site (Step 1 line numbers as starting points). For each: quote the 1-line effect (e.g. "NOPs the 68k `via_init` routine so the guest never programs VIA timers"), then assign: VIA cluster → RETIRE@M3 (VIA model lands M3... unless the M1 VIA-timer surface needs it — check SPIKE-S3's recommendation and note the dependency), SCC init → RETIRE@M1, Cuda init → RETIRE@M3 (Cuda is a loud stub in M1 — flag this tension explicitly if the un-patched init touches the stub), GC/interrupt-mask → RETIRE@M3, NVRAM cluster → RETIRE@M4, run_diags/powermac_id → KEEP or PARAVIRTUAL-ONLY with one sentence of reasoning each.

- [ ] **Step 3: Commit**

```bash
cd /Users/khawkins/Documents/git/macemu-jit
git add docs/planning/machine/ROM-PATCH-AUDIT.md
git commit -m "docs(machine): ROM-patch audit - device-neutralizing patch dispositions for M1-M6"
```

---

### Task 8: Bookkeeping — mark M0 done

**Files:**
- Modify: `docs/planning/MACHINE-LAYER-PLAN.md` (M0 row in §3; header status line)
- Modify: `CHANGELOG.md` (top of file, house style: date + [SheepShaver] tag)

- [ ] **Step 1: Update the plan**

In `docs/planning/MACHINE-LAYER-PLAN.md`: M0 row — prefix the milestone name with ✅ and append: `**DONE <date>** — machine pref + profile module (unit-tested), SS_NW_* consolidated, skips/ignoresegv gated, paravirtual verified byte-identical (test-jit 100 + e2e PASS), CORE99-MACHINE-DESCRIPTION.md + ROM-PATCH-AUDIT.md landed.` Update the header status line: `spikes S1–S3 complete` → `M0 complete; next: M1`.

- [ ] **Step 2: CHANGELOG entry**

Add at the top of `CHANGELOG.md`, matching the existing entry format (check the current top entry's exact style first):

```
[date] [SheepShaver] Machine Layer M0: pref-selected machine profile
(paravirtual default / newworld), SS_NW_* env gates consolidated under it
(SS_NW_TRAMPOLINE deprecated alias), legacy serial-skip hacks + ignoresegv
disabled on the newworld profile, standalone unit test
(make -C src/machine test). Design artifacts: CORE99-MACHINE-DESCRIPTION.md,
ROM-PATCH-AUDIT.md. Paravirtual non-regression: test-jit score=100, e2e PASS.
See docs/planning/MACHINE-LAYER-PLAN.md (M0).
```

- [ ] **Step 3: Commit**

```bash
cd /Users/khawkins/Documents/git/macemu-jit
git add docs/planning/MACHINE-LAYER-PLAN.md CHANGELOG.md
git commit -m "docs(machine): mark Machine Layer M0 complete"
```
