> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Wave 0 — MMU/SR wall unblock: SR[16] + MSR stored state + low-memory mapping

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cross (or precisely characterize) the `0x50326050–0x50326068` MMU/SR boot wall per `docs/planning/machine/M5-MMU-SR-WALL-ANALYSIS.md` §6: add `sr[16]` + `msr` stored state to the CPU core (the SPRG-fix pattern — store the write, return the read), give the low-physical hole (`~0x200a0`) a backing mapping on the newworld profile, run the §7 probe boots, and record the identity-vs-non-identity verdict that gates M3a's plan. Paravirtual stays byte-identical (test-jit 350/350).

**Architecture:** Two `powerpc_registers` fields appended LAST (`sr[16]`, `msr` — after `srr1`; clean PPC recompile required); interpreter decode for `mtsr`/`mtsrin`/`mfsr`/`mfsrin`/`mtmsr` (currently `execute_illegal`) + `mfmsr` re-sourced from stored state (cold value 0xf072 preserves today's behavior exactly); a **single profile-sized Low Memory acquire** (`vm_mac_acquire_fixed(0, newworld ? 0x100000 : 0x3000)` — rev 2 C2: the Mach VM backend page-truncates a 0x3000-based second acquire into an overlap failure). **MSR remains stored-only this wave** — nothing consults MSR[DR/EE] for behavior (that is M3a); `rfi` is untouched. **JIT: three native cases neutralized to interpreter fallback** (rev 2 C1 — `ppc-jit.cpp` compiles `mfsr`(595)/`mfsrin`(659) as return-0 and `mfmsr`(83) as hardcoded 0xf072; left native, the wall routine's `mfsrin` never reads the stored state and the fix is dead under JIT). Harness gains two round-trip vectors + the batch-mode per-vector reset extended to the trailing supervisor block so the gate actually covers the new state.

**Tech Stack:** C++ interpreter slow paths, autoconf build, SS_PROBE_PC no-recompile probes for acceptance.

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/M5-MMU-SR-WALL-ANALYSIS.md` (whole memo) | The design: §1 faulting instruction, §4 gap inventory + citations, §5 M1-composition proof, §6 the two-part fix + the honest sufficiency conditional, §7 the probe scripts |
| `docs/planning/machine/M3-INTERRUPT-RECON.md` §2, §6 | MSR-absent/mfmsr-0xf072/rfi facts; `powerpc_registers` offset table + append-last constraint |
| MEMORY `stale_build_struct_offsets` | header change ⇒ clean PPC recompile before trusting test-jit |

## Codebase facts (verified 2026-06-10)

- **`powerpc_registers` tail** (`ppc-registers.hpp:256–271`): the MUST-stay-LAST block currently ends `… bat[16]; srr0; srr1;`. New fields append after `srr1`. These trailing fields are interpreter-accessed by name only — no JIT offset macros needed.
- **Decode mechanism (rev 2 — three files per new op):** the static `powerpc_ii_table[]` lives in `ppc-decode.cpp` (entry shape `{name, EXECUTE_0(handler), PPC_I(ENUM), X_form, 31, xo, CFLOW_NORMAL}` — copy the `mfmsr` entry at **ppc-decode.cpp:728–732**); each op also needs a new `PPC_I(...)` constant in `ppc-instructions.hpp` (alphabetical enum ending at `PPC_I(MAX)`, ~:366) and a member decl in `ppc-cpu.hpp` (precedent `execute_mfmsr` at :451). `init_decoder_entry` (ppc-decode.cpp:1838–1844) auto-dispatches X_form/31/xo. Operand extraction: plain handlers, no operand templates — `rS_field`/`rB_field` (ppc-bitfields.hpp:80–82), `SR_field` = bits 12–15 **already exists** (ppc-bitfields.hpp:199).
- **mtsr/mtsrin/mfsr/mfsrin/mtmsr today**: no decode entries → `execute_illegal`; the PC+=4 no-op comes from the **default-true `ignoreillegal` pref** (`prefs_items.cpp:107`) — all configs, not just lenient ones (rev 2). Side effects lost by decoding: env-gated `[STUB-TRACE]` xo counters + the `SS_LOG_ILLEGAL` mtmsr/MSR[VEC] log (:160–176) — replicate the latter in the new mtmsr handler (it's live AltiVec-probe telemetry). xo numbers verified against in-tree `ppc-dis.c`: mfmsr=83, mtmsr=146, mtsr=210, mtsrin=242, mfsr=595, mfsrin=659.
- **`mfmsr` today** returns hardcoded `0xf072` (`ppc-execute.cpp:1277–1281`). **Cold-state contract: `msr` inits to `0xf072`.** Nuance (rev 2): 0xf072 already has DR(0x10) set, so the wall routine's `ori r20,r23,0x10; mtmsr` stores 0xf072 unchanged — post-Wave-0 mfmsr through that path is byte-identical. `interrupt()`'s `gpr(11)=0xf072` (sheepshaver_glue.cpp:663) is an independent literal; nothing compares it to stored msr.
- **JIT (rev 2 C1 — the plan's original "no JIT cases" claim was FALSE):** `ppc-jit.cpp:2711–2719` natively compiles `mfsr`(595)/`mfsrin`(659) → constant 0 and `mfmsr`(83) → constant 0xf072. mtmsr/mtsr/mtsrin have no cases (fall back). Task 1 changes the three read cases to `return false` so all six ops are interpreter-sourced. Paravirtual byte-identity holds: interp cold values (0, 0, 0xf072) equal today's JIT constants.
- **Register init**: `powerpc_cpu::init_registers()` (**ppc-cpu.cpp:1034–1048**) zeroes gpr/fpr/…/sprg (sprg loop :1047 with the "must start clean" comment) but **NOT sdr1/bat/srr0/srr1 — pre-existing malloc-garbage cold-state bug** (the regs struct comes from raw-malloc `operator new`, ppc-cpu.cpp:1228–1255); zero them in passing. Runs on both emulator + harness paths (constructor → initialize :1159 → init_registers :1175).
- **Batch-harness footgun (rev 2):** the batch path's per-vector CPU reset (sheepshaver_glue.cpp:1287–1291) resets GPR/LR/CTR/CR/XER/FP/VR only — msr/sr would persist across batch vectors while legacy mode gets a fresh process each time. Extend the per-vector reset to the trailing supervisor block (sprg/sdr1/bat/srr0/srr1/sr/msr) before adding the round-trip vectors.
- **Low Memory area**: `vm_mac_acquire_fixed(0, 0x3000)` at `main_unix.cpp:1605` inside `if (!memory_mapped_from_zero)` (provably always false on macOS arm64 — config.h: PAGEZERO_HACK undef, MEM_BULK never defined) + the `#if` guards. **(rev 2 C2)** the backend is Mach `vm_allocate` (`HAVE_MACH_VM`, vm_alloc.cpp:363–382): a fixed acquire at 0x3000 page-TRUNCATES to 0x0 (16K pages) and overlap returns KERN_NO_SPACE → the original two-mapping design bricks the boot. Also note the existing (0,0x3000) acquire already rounds up to 0x4000 on this host — guest 0x3000–0x3fff is silently backed today. Fix = ONE profile-sized acquire. Init order verified: PrefsInit :1446 → MachineProfileInit :1450 → the acquire :1605 — the profile gate is live there.
- **`ss_rpc_is_mapped`** (`main_unix.cpp:983–1011`) enumerates mapped guest ranges and already has a newworld block (:996) — add the 0x0–0x100000 newworld range (also makes RPC mem_read usable for the §7.3 descriptor probe).
- **Paravirtual byte-identity surfaces to watch**: (a) `ss_stub_ill31` counters stop counting these xo's once decoded — stub-counter output changes ONLY under `SS_LOG_ILLEGAL`-style diagnostics, not in REGDUMPs; (b) any harness vector exercising mtsr/mtmsr/mfsr would change REGDUMP — **verify none exists** (`grep -iE "mtsr|mtmsr|mfsr" SheepShaver/jit-test/run.sh` expected empty; record in Task 1); (c) `mfmsr` cold value preserved via the 0xf072 init.
- **Acceptance asset**: `/tmp/m2accept.prefs` (9.0.1 ROM, newworld) from M2; probe recipes in M5-analysis §7 (SS_PROBE_PC syntax in CLAUDE.md).

## File map

| File | Status | Responsibility |
|---|---|---|
| `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-registers.hpp` | modify | `sr[16]` + `msr` appended last, with comments in the established style |
| `SheepShaver/src/kpx_cpu/src/cpu/ppc/ppc-execute.cpp` (+ decode table site, likely `ppc-cpu.cpp` or the instr table in ppc-execute) | modify | Decode + store/return handlers for mtsr/mtsrin/mfsr/mfsrin/mtmsr; mfmsr from stored state; cold init |
| `SheepShaver/src/Unix/main_unix.cpp` | modify | newworld-gated low-mem extension after the existing 0x3000 acquire |
| docs: `M5-MMU-SR-WALL-ANALYSIS.md` (§7 results), `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` §2.8 rung-2 row, `CHANGELOG.md`, `MACHINE-LAYER-PLAN.md` (one status line) | modify | Probe results + rung verdict recorded |

**Parallelization:** Tasks 1 and 2 touch disjoint files and can run as parallel implementers (only Task 1 runs the emulator build; Task 2's gate is deferred to Task 3). Tasks 3–5 sequential.

**Standing rules:** clean PPC recompile after the header change (stale-.o footgun); paravirtual byte-identity is the contract (legacy `make test-jit` authoritative; batch for iteration); emulator boots are user-coordinated; commit per task.

---

### Task 1: SR[16] + MSR stored state (CPU core)

**Files:** `ppc-registers.hpp`, `ppc-execute.cpp` (+ the decode-table site where `mfmsr` is registered — locate it first)

- [ ] **Step 1:** Append to the MUST-stay-LAST block in `ppc-registers.hpp` (after `srr1`, matching the existing comment style):

```cpp
	uint32 sr[16];				// Segment registers SR0-15 (mtsr/mtsrin/mfsr/mfsrin). Previously
								// dropped via execute_illegal; the NK's MMU/segment-fault service
								// routines (ROM 0x325c00-0x326160) write an SR and READ IT BACK
								// (mfsrin after mtsrin) - dropped writes made that read garbage.
								// Stored state only: nothing consults SRs for translation (V=P).
	uint32 msr;					// MSR. Previously absent (mfmsr hardcoded 0xf072, mtmsr dropped).
								// Stored state only in Wave 0 - MSR[DR/EE] semantics land in M3a.
								// COLD VALUE MUST BE 0xf072 (init in powerpc_cpu init/reset) so
								// mfmsr-before-any-mtmsr is byte-identical to the old hardcode.
```

- [ ] **Step 2 (rev 2):** In `powerpc_cpu::init_registers()` (`ppc-cpu.cpp:1034–1048`, next to the sprg zero-loop at :1047): set `regs().msr = 0xf072;`, zero `sr[0..15]`, **and zero the pre-existing uninitialized trailing fields `sdr1`/`bat[16]`/`srr0`/`srr1`** (malloc-garbage cold-state bug, same class the sprg comment records). Runs on both emulator + harness paths (verified: constructor → initialize → init_registers).

- [ ] **Step 3 (rev 2 — three files per op):** For each of mtmsr(146)/mtsr(210)/mtsrin(242)/mfsr(595)/mfsrin(659):
  - `ppc-instructions.hpp`: add `PPC_I(MTMSR)` etc. to the alphabetical enum.
  - `ppc-cpu.hpp` (~:451): add the `execute_*` member decls (precedent: `execute_mfmsr`).
  - `ppc-decode.cpp`: add table entries copying the `mfmsr` entry at :728–732 (`{name, EXECUTE_0(h), PPC_I(E), X_form, 31, xo, CFLOW_NORMAL}`).
  - `ppc-execute.cpp` handlers (plain field extraction, no operand templates):
    - `mtmsr`: `regs().msr = gpr(rS_field::extract(opcode));` + replicate the `SS_LOG_ILLEGAL` MSR[VEC] log from :167–171 (live AltiVec-probe telemetry — don't orphan it).
    - `mfmsr` (existing): `operand_RD::set(this, opcode, regs().msr);` (cold value identical to the old hardcode).
    - `mtsr`: `regs().sr[SR_field::extract(opcode)] = gpr(rS_field::extract(opcode));` (`SR_field` exists, ppc-bitfields.hpp:199).
    - `mtsrin`: `regs().sr[gpr(rB_field::extract(opcode)) >> 28] = gpr(rS_field::extract(opcode));`
    - `mfsr` / `mfsrin`: the read counterparts via `operand_RD::set`.
    All end with `increment_pc(4)`.

- [ ] **Step 3b (rev 2 C1 — JIT neutralization):** In `ppc-jit.cpp:2711–2719`, change the native cases for **595 (mfsr), 659 (mfsrin), 83 (mfmsr)** to `return false` (interpreter fallback), with a comment: "Wave 0: SR/MSR are stored state now — single-source through the interpreter; the old native constants (0 / 0xf072) equal the interpreter's cold values, so paravirtual is byte-identical." mtmsr/mtsr/mtsrin already fall back.

- [ ] **Step 4 (rev 2 — make the gate see the new state):**
  (a) Extend the batch-mode per-vector CPU reset (`sheepshaver_glue.cpp:1287–1291`) to also reset the trailing supervisor block: sprg/sdr1/bat/srr0/srr1 to 0, msr to 0xf072 — without this, batch and legacy REGDUMPs diverge for any vector reading state a previous vector wrote.
  (b) Add two round-trip vectors to `SheepShaver/jit-test/run.sh` following the table's format rules (read the harness-integrity rules in jit-test/README.md first): an mtmsr→mfmsr round trip and an mtsrin→mfsrin round trip (deterministic: write an immediate-built value, read it back). Run the harness self-validation. Record the new total (352).
  (Pre-check confirmed: zero existing vectors touch these ops — by mnemonic AND hex grep.)

- [ ] **Step 5:** **Clean PPC recompile** (header changed): remove the kpx_cpu objects under src/Unix explicitly (or `make clean` there), `make build-ss`, then `SS_HARNESS_BATCH=1 make test-jit && make test-jit` → both **352/352**. Do NOT trust an incremental build after a registers-header change.

- [ ] **Step 6:** Commit — `feat(cpu): Wave 0 - SR0-15 + MSR stored state (store-the-write/return-the-read; MSR cold 0xf072; JIT mfsr/mfsrin/mfmsr fall back; +2 round-trip vectors)`

### Task 2: Low-memory hole mapping (newworld-gated)

**Files:** `SheepShaver/src/Unix/main_unix.cpp`

- [ ] **Step 1 (rev 2 C2 — single profile-sized acquire, NOT a second mapping):** the Mach VM backend (`HAVE_MACH_VM`) page-truncates a fixed acquire at 0x3000 down to 0x0 and fails on overlap (KERN_NO_SPACE) — a second acquire bricks the boot. Instead, resize the ONE existing Low Memory acquire (`main_unix.cpp:1605`):

```cpp
		// Create Low Memory area. Wave 0 (M5-MMU-SR-WALL-ANALYSIS §6): on the
		// newworld profile, extend it to cover the NK's low-physical descriptor
		// region (~0x200a0 lwbrx/stwx probes, second access at +0x200b0, and the
		// absolute lbz at 0x3f00) - real hardware backs this with the first 128KB
		// of DRAM. Paravirtual keeps the historical 0x3000 (ignoresegv ate these
		// accesses there; mapping them would change behavior). Single acquire:
		// page-aligned start, page-size-agnostic, no Mach overlap hazard.
		const uint32 lowmem_size = MachineProfileIsNewWorld() ? 0x100000 : 0x3000;
		if (vm_mac_acquire_fixed(0, lowmem_size) < 0) {
			sprintf(str, GetString(STR_LOW_MEM_MMAP_ERR), strerror(errno));
			ErrorAlert(str);
			goto quit;
		}
		lm_area_mapped = true;
		if (MachineProfileIsNewWorld())
			fprintf(stderr, "[WAVE0] low memory extended to 0x0-0x100000 (NK low-physical descriptors)\n");
```
(Keep the block inside the existing `if (!memory_mapped_from_zero)` + `#if` guards — verified always-reached on macOS arm64. Init order verified: MachineProfileInit at :1450 precedes this.)

- [ ] **Step 2 (rev 2 — definitive):** add the newworld `0x0–0x100000` range to `ss_rpc_is_mapped` (`main_unix.cpp:983–1011`, which already has a `MachineProfileIsNewWorld()` block at :996) so diagnostics/RPC classify it — this also makes RPC `mem_read` usable for the §7.3 descriptor probe.

- [ ] **Step 3:** Build + fast gate (`make build-ss && SS_HARNESS_BATCH=1 make test-jit`) — paravirtual untouched (gated block).

- [ ] **Step 4:** Commit — `feat(machine): Wave 0 - newworld low-memory hole mapping (0x3000-0x100000)`

### Task 3: Non-regression checkpoint

- [ ] `make -C SheepShaver/src/machine test` → 8/8. `make test-jit` (legacy, authoritative) → 350/350. `make test-opcodes` → inert (no new stderr classes on paravirtual). `make e2e-test` → 122 passed. `make e2e` (paravirtual lifecycle — **coordinate with user**) → PASS.
- [ ] Any failure: stop, systematic-debugging.

### Task 4: Acceptance — probe boots (user-coordinated, M5-analysis §7)

Prefs (recreate if `/tmp/m2accept.prefs` is gone):
`printf 'rom /Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom\nramsize 268435456\nnogui true\nmachine newworld\n' > /tmp/m2accept.prefs`

Probe caveats (rev 2): SS_PROBE_PC hooks block entry **on the JIT path only** — never probe with `SS_USE_JIT=0`; keep probes **register-only** (the `[0xADDR]`/`[rN:SIZE]` memory dumps read guest memory unguarded — a probe of an unmapped address crashes the host) until after this wave's mapping is confirmed live.

- [ ] **Boot A (the wall test):** `SS_ROM_LENIENT=1 SS_ROM_SKIP_JUMP68K=1 perl -e 'alarm 120; exec @ARGV' ./src/Unix/SheepShaver --config /tmp/m2accept.prefs` — classify into one of THREE outcome classes (rev 2 — the routine's post-fix flow was disassembled: with the page zero-filled it loads code 0, takes neither the 0x40 nor ≥0x41 branch, logs fault-code 0 into a pending queue at KDP+0x910/0x912, restores SR/MSR and returns cleanly):
  - **(a) SIGSEGV gone, new frontier further on** — success class; record the new PC (candidates: a sibling routine in 0x325c00–0x326160, the DR-Emulator handoff ~0x5046f900, or genuinely further).
  - **(b) SIGSEGV gone but the handler re-enters forever** (the dispatcher re-invokes it on the same un-resolved fault — watch for a runaway `[PROBE]` visit count at 0x50325f00/0x50326050, M5 §7.4) — the wall became a loop, NOT success; record the KDP+0x910 queue behavior.
  - **(c) still faults at 0x50326068** — the mechanism claim is wrong; probe per §7.1 and report. Do not hack further.
- [ ] **Boot B (probe values):** `SS_PROBE_PC='0x50326050:r20,r22,r23;0x50325f68:r21,r22,r24' …` per §7.1 — capture r22 (EA base), r23 (saved MSR), the SR save/program values. Resolves identity-vs-non-identity (REDTEAM question D).
- [ ] **Assert:** no paravirtual change; `[WAVE0]` line present on newworld; outcome class recorded with evidence.

### Task 5: Docs + verdict recording

- [ ] `M5-MMU-SR-WALL-ANALYSIS.md`: append a "§8 Wave 0 results" section — probe values, identity verdict (or "deferred: wall moved without needing the determination"), the new frontier PC.
- [ ] `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` §2.8: rung-2 row updated (extended scope landed; outcome).
- [ ] `MACHINE-LAYER-PLAN.md`: one status-line note (Wave 0 landed between M2 and M3; M5's rung-2 partially pre-paid).
- [ ] `CHANGELOG.md` entry; `LEARNINGS.md` only if the probe produced a non-obvious finding.
- [ ] Commit — `docs: Wave 0 MMU/SR wall results`

---

## Self-review record

- M5-analysis §6 coverage: sr[16]+msr stored ✓ (T1); low-mem mapping ✓ (T2, newworld-gated — the memo didn't flag the paravirtual-ignoresegv behavior-change hazard; this plan adds the gate); probe boots ✓ (T4 = §7.1–7.5 subset); rung verdict recorded ✓ (T5). MSR semantics deliberately NOT consulted (M3a). `rfi` untouched (M3a). No JIT work (verified single-sourced).
- Sharp edges flagged: clean-recompile rule (T1 S5); mfmsr cold-value contract 0xf072 (T1 S1/S2); xo numbers must be verified against the PEM before wiring (T1 S3); the lenient/ignore path that currently no-ops these ops disappears once decoded — meaning on PARAVIRTUAL a guest mtmsr/mtsr that previously hit execute_illegal→abort (without lenient flags) now silently stores; this is strictly more permissive and byte-identical on all green-gate paths (no harness vector; T1 S4 verifies), but record it in the commit body.

## Rev 2 corrections (red-team-lite, 2026-06-10 — 2 parallel code-verified reviewers)

**CRITICAL:**
- **C1** — the original "no JIT changes" premise was FALSE: `ppc-jit.cpp:2711–2719` natively
  compiles mfsr/mfsrin → 0 and mfmsr → 0xf072; under JIT the wall routine's `mfsrin` would
  never read the stored state and the wave couldn't test its own hypothesis. Fixed: Task 1
  Step 3b neutralizes the three read cases to interpreter fallback (cold values identical →
  paravirtual byte-identical); the harness was also blind to all six ops, so Step 4 adds two
  round-trip vectors + extends the batch per-vector reset to the trailing supervisor block
  (batch-vs-legacy REGDUMP equivalence would otherwise break).
- **C2** — the original two-mapping design bricked the newworld boot: Mach `vm_allocate`
  page-truncates a fixed acquire at 0x3000 to 0x0 and returns KERN_NO_SPACE on overlap.
  Fixed: ONE profile-sized Low Memory acquire (`0x100000` on newworld, `0x3000` paravirtual).

**MAJOR/MINOR (folded):** decode = three files per op (PPC_I enum + ppc-cpu.hpp decl +
ppc-decode.cpp table entry; exact precedents cited); init site is `init_registers()`
ppc-cpu.cpp:1047 (also fixes the pre-existing sdr1/bat/srr0/srr1 malloc-garbage cold state);
the no-op path is the default-true `ignoreillegal` pref (all configs); the SS_LOG_ILLEGAL
mtmsr/MSR[VEC] log is replicated in the new handler (live telemetry); 0xf072-has-DR-set
nuance recorded (the wall routine's mtmsr stores 0xf072 unchanged — byte-identical reads);
`interrupt_copy`'s field list deliberately excludes the trailing supervisor block (benign for
stored-only state; M3a revisits); Task 4 gained outcome class (b) handler-re-entry-loop
(post-fix flow disassembled: zero-filled descriptor → fault-code 0 → pending queue at
KDP+0x910), the probes-are-JIT-only + register-only caveats, and the prefs recreate recipe;
`ss_rpc_is_mapped` range addition made definitive; xo numbers verified against in-tree
ppc-dis.c.

**Verified-OK highlights:** init order (PrefsInit → MachineProfileInit → acquire) correct at
the insertion point; guards provably always-reached on macOS arm64; no layout collisions for
0x0–0x100000 (RAM/ROM/KDP/DR/SheepMem/MMIO all elsewhere); nothing depends on the current
0x200a0 abort beyond it being the wall marker; append-after-srr1 safe (layout asserts all
precede the trailing block; SS_JIT_VERIFY uses sizeof); SR_field already exists.
