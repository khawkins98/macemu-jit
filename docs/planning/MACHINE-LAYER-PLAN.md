# The Machine Layer — a designed NewWorld fidelity profile

> **Status:** 🟢 Approved architecture (rev 4) — **spikes done; M0 + M1 + M2 complete; Wave 0 MMU/SR wall CROSSED (2026-06-10); M3a ✅ complete (2026-06-10)**: real PPC exception model landed — exc_core (OEA masks, sc/rfi real semantics, EE-edge re-raise), DEC delivery hook (KDP shim, execute_depth deliverability rule, in-place poll), [NW-INT] deleted, HandleInterrupt fenced; **first real PPC exception ever delivered**: `[EXC] DEC delivered #1: restart=50429b40 srr1=0000f072 msr=00001040 -> entry=50412b1c`; cold-MSR EE=0 fix; boot-frontier identified as the NK Thud debug console (designed wake = serial character, not timer — EE stays honestly masked there); **end-to-end demo** (a2dd1ff8): SS_SCC_RX_INJECT fed one CR at T+25s → JIT compile counter 781→791 (10 new blocks); M2 scheduler → M1 bus/backpatch → SCC Rx → check_work → console — every machine-layer milestone composing; **next: M3b** (OpenPIC + Cuda/ADB + external-source wiring + SDL_PumpEvents relocation + patch retirements + RTC + deliverability harness vector + nested-execute path completion + boot-past-console question)
> · **Created:** 2026-06-10
> · **Updated:** 2026-06-10 (rev 2+3: two adversarial review rounds — §8; rev 4: spike results
> folded in — §3 spikes, M1/M2/M7 re-scoped, §9 re-score #1: capability ~70–75%; post-M1 spike:
> NK ceiling at 0x503123fc root-caused + fixed d8932203; new frontier is the 0x50326050
> MMU/segment-handler wall — M3/M5 territory)
> **Decision (2026-06-10):** Stop extending the ROM-patching/paravirtualization approach toward
> NewWorld and Mac OS 9.2.x one bug at a time. Instead, build the thing SheepShaver never had:
> a **real machine-model layer** — MMIO bus, virtual clock, device models, interrupt/exception
> architecture, supervisor environment — as a first-class, testable subsystem, landed as a second
> **machine profile** beside the proven paravirtual path.
>
> **This is the new primary approach for ROADMAP D3** ("break the 9.0.4 ceiling").

---

## 0. Lineage — what this supersedes and what it builds on

This document is the synthesis of the Path A / Path B research arc. It **supersedes the
strategy** in the documents below; their analysis, data, and reverse-engineering remain the
foundation and are referenced throughout, not discarded.

| Ancestor doc | Relationship |
|---|---|
| `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` (Path A) | **Superseded as a path, harvested as components.** Its §2.7 hybrid insight (LLE for boot, HLE for runtime) is this plan's core principle. Its §3 MMU rung-ladder becomes §2e here. Its §2.8 obstacle map remains the per-wall reference — and is now **explicitly inherited by milestone M6** (rev-2 correction: device models alone were never the blocking walls on the 9.0.1 path). The env-gated scaffolding (`SS_NW_TRAMPOLINE`, synthetic ECB stub, SegMap/PMDT spikes) are prototypes the Machine Layer replaces with designed components. |
| `UPGRADE-CARD-PATH.md` (Path B) | **Superseded as a strategy, kept as a tactical tool.** The 4-byte System-file gate bypass and the SYSTEM-BOOT-GATES RE are permanently useful. Its key finding — the post-splash **SCC serial stall** — is the Machine Layer's first concrete consumer (M1). The "upgrade card" metaphor survives in corrected form: the cards we slot in are **device models and supervisor components**, not identity shims (identity patches were measured insufficient — UPGRADE-CARD-PATH §2.1). |
| `NEW-WORLD-ROM-SUPPORT-PLAN.md` | Historical context for the NewWorld ROM work; staged parcels analysis still valid as reference. |
| `PATCH-68K-SHIM-INVENTORY.md` | Path A artifact; the HLE-shim reference for milestone M6's shim triage and for whichever shims the fidelity profile keeps (§2f). |
| `docs/planning/sheepshaver-research/SYSTEM-BOOT-GATES.md` | Active reference — gate anatomy + DSAT format, reusable across System versions. |
| `DINGUSPPC-EVALUATION-PLAN.md` | Resolved the license question (GPLv2-or-later + GPLv3 → combined GPLv3, feasible). DingusPPC is promoted from "ideas-first reference" to **primary device-model donor** (§4). Caveat (rev 2): its device models hang off its `TimerManager`/event-scheduler globals — porting them means building our own virtual clock first (§2c / M2). |
| `MMU-NANOKERNEL-MP-PLAN.md`, `MMU-DEFERRAL-REDTEAM.md` | Deep-dive references for §2e; the deferral verdict stands (classic Mac OS is morally V=P — no translation engine needed). One carve-out discovered in review: exception-vector delivery may need a single special-cased low-memory mapping (§2d). |
| `COMPATIBILITY-PAYOFF-DINGUSPPC-REVISIT.md` | Its "make 8.6–9.0.4 run wanted software usably" verdict is unaffected — that work proceeds on the **paravirtual** profile in parallel. |

**Why the pivot from both paths:** Path A devolved into ROM-specific byte-pattern RE with no
unifying design (violating architecture-first); Path B proved that identity/gate patches cannot
close a *structural* gap (a 1998 ROM environment vs a 2001 System's expectations) — its own
frontier (the SCC stall) is literally a missing device model. Both paths independently arrived
at the same wall: **SheepShaver has no machine model.** This plan builds one.

---

## 1. Goal and governing decisions

**End state (Phase-3 "widen emulation," ROADMAP project arc):** a full-stack classic PowerPC Mac
emulation platform — Mac OS 8.6 → **9.2.2** — benefiting from the native AArch64 JIT, with
designed foundations for raw hardware handling, power management, and eventually Metal-mapped
graphics.

Decisions made 2026-06-10 (with Ken):

1. **Destination is B, gait is C.** The platform vision ("model the complete PowerPC Mac")
   justifies building permanent components — but we build them **in the order 9.2.x demands**,
   no speculative generality.
2. **Classic Mac OS only; door architecturally ajar for OS X.** Classic Mac OS is morally V=P,
   so the DIRECT_ADDRESSING/NATMEM identity-mapped JIT fast path stays. We do **not** build a
   softmmu. The memory-access seam (MMIO bus) is designed so a translation layer *could* slot
   in later; we never pay for it now.
3. **Strangler-fig dual profile (Approach A).** The new work lands as a second, pref-selected
   **machine profile**. Today's path is frozen as `paravirtual` and remains the shipping
   default with all gates green. The `newworld` fidelity profile grows beside it, component by
   component, A/B-testable against the known-good path.
4. **Chassis stays SheepShaver.** Evaluated and rejected: porting our JIT into DingusPPC
   (near-rewrite of the JIT's memory/dispatch model; abandons our host-integration and
   verification investment) and adopting QEMU as substrate (TCG replaces our JIT — the
   project's reason to exist). Both are demoted to supporting roles: DingusPPC = device-model
   donor, QEMU = behavioral oracle.

   *Identity check — this is NOT a QEMU fork with JIT ambitions.* QEMU's posture is full LLE
   (every device real, guest opaque, softmmu, TCG). SheepShaver's posture — which we keep —
   is a paravirtualized OS runtime (HLE video/disk/network/FS, identity-mapped memory, our
   JIT, first-class host integration). The Machine Layer adds a **thin LLE crust at the
   firmware boundary only**: one SCC, a VIA timer surface, a PIC, NVRAM, a clock, honest
   exception delivery — perhaps 2–3% of a QEMU-style machine model, fenced to exactly the
   registers the ROM provably probes (machine-description §4). Everything above the firmware
   stays paravirtual, permanently (§2f). **Anti-drift rule: the day a milestone proposes
   modeling hardware the ROM doesn't probe, the answer is no — that's rebuilding QEMU,
   badly; use QEMU (we do — as the oracle).**
5. **(rev 2) The fidelity profile models exactly ONE machine: Core99-class (Power Mac G3
   B&W/G4 era — KeyLargo MacIO + OpenPIC), the machine QEMU `mac99` and DingusPPC both model
   well and the 9.0.1 ROM targets.** The 1.1 ROM expects Heathrow/Paddington-class hardware —
   a *different* machine; supporting both would mean two address maps, two PICs, two device
   trees. The 1.1-ROM/9.2.1 tactical path (Path B's frontier) gets **only standalone device
   model(s) at the addresses the stall loop actually polls** — not the full profile.
   *(rev 3 caveat — RESOLVED by spike S3 (rev 4): the stall is the ROM's serial test monitor
   polling SCC ch A (0xF3012002) with a VIA 6522 T2-timeout escape (0xF3016000) — r18=VIA,
   r19=SCC; M1's scope is SCC + VIA timer/IFR. And per spike S1, the 1.1-ROM path is a bus
   testbed only — 9.2 audits CFM boot fragments that only parcels ROMs provide, so 9.2-on-1.1
   is structurally capped regardless of devices.)*

---

## 2. Architecture

```
┌─────────────────────────────────────────────────┐
│ Host integration (SDL3, VNC, E2E, prefs)        │  ← unchanged
├─────────────────────────────────────────────────┤
│ CPU core: AArch64 JIT + interpreter              │  ← fast path unchanged; exception/
│   memory access stays identity-mapped            │    SPR slow paths gain a profile seam
├──────────────────┬──────────────────────────────┤
│ Profile:         │ Profile: NEWWORLD FIDELITY    │
│ PARAVIRTUAL      │  (new, Core99-class machine)  │
│ (today's path,   │  ┌─────────────────────────┐ │
│  frozen & green) │  │ Machine description      │ │
│                  │  │ MMIO bus + virtual clock │ │
│  ROM patches +   │  │ Device models:           │ │
│  HLE shims +     │  │  SCC · VIA/Cuda · PIC ·  │ │
│  EMUL_OPs        │  │  NVRAM · MacIO · DBDMA   │ │
│                  │  │ Interrupt/exception      │ │
│                  │  │  architecture (MSR/SRR)  │ │
│                  │  │ Supervisor env:          │ │
│                  │  │  MMU bookkeeping ·       │ │
│                  │  │  Trampoline synthesis    │ │
│                  │  └─────────────────────────┘ │
│                  │  HLE shims kept for runtime   │
└──────────────────┴──────────────────────────────┘
```

New code lives in `SheepShaver/src/machine/` — explicit, testable components replacing the
diffuse patchwork of ROM patches, fake memory pages, and env-gated hacks.

### 2a. Machine description (the contract everything implements)

An explicit, versioned description of the modeled machine: physical address map (MacIO base,
device offsets), interrupt tree (which device raises which PIC input), and the **device-tree
skeleton** the synthesized firmware handoff publishes to the guest. This is a *design artifact
produced in M0*, because every later component implements against it — device addresses (M1,
M4), interrupt routing (M3), and critically the **video node**: the HLE video path must appear
as a plausible display device in the tree (the future `.ndrv`/Metal seam — §2f), and
HLE-backed devices must NOT advertise capabilities (e.g. DBDMA channels) that we don't model.

### 2b. MMIO bus (the keystone)

A registry of guest physical address ranges → device objects with
`read(addr, size)` / `write(addr, size, value)` handlers.

**Three dispatch paths, by access origin** *(rev 2 — the original "the existing SIGSEGV
machinery already decodes faulting accesses" was wrong; on macOS arm64 the handler is
`pc += 4` with no operand decode — `sigsegv.cpp:2564`. rev 3 — a third access class was
missed entirely)*:

- **JIT path — fault + decode.** Device pages are left unmapped inside the NATMEM
  reservation; a SIGSEGV/Mach-exception handler decodes the faulting **JIT-emitted** access
  and dispatches to the bus. This requires building (M1): an **AArch64 load/store decoder**
  for the specific access forms the JIT emits (`ppc-jit.cpp` emits clean
  `LDR/STR Wt, [RMEMBASE, Xidx]` + separate `REV` — a deliberately restricted, decodable set;
  the JIT memory-emitters become the *contract* the decoder tests against), register
  writeback via Mach thread state, and an explicit **endianness contract**: the injected
  value must be raw big-endian, because the JIT's `REV` executes after the resumed load.
- **Interpreter path — software range check.** Interpreter memory accesses are
  compiler-generated host code (undecodable), and nested `execute()` contexts (interrupt
  handlers, EMUL_OPs) run in the interpreter — exactly where device touches from handlers
  happen. The interpreter's memory accessors get an address-range check that calls the bus
  directly. Both paths land on the same device handler. *(rev 3)* The range check is
  **compiled/branch-gated per profile** so the paravirtual interpreter path (incl. all
  nested-execute contexts and the `SS_USE_JIT=0` baseline) pays nothing — and the bench
  gates must include an interpreter-mode measurement to prove it, since the §3 gates
  otherwise only measure JIT.
- ***(rev 3)* Host-accessor path — explicit bus entry points.** A large access class is
  host-side C++ via `ReadMacInt*`/`WriteMacInt*`/`Mac2HostAddr`: EMUL_OP handlers,
  `Execute68k`, HLE shims (serial/disk/video), ROM patching — and our own debug tooling
  (`SS_PROBE_PC` `[rN:SIZE]` dumps, `SS_JIT_WATCH_ADDR` reads). Any of these touching a
  trapped page faults at arbitrary compiler-generated code — undecodable, so under the
  abort-loudly rule a guest driver handing a device address to an HLE shim (or a developer
  probing 0xF3xxxxxx) would hard-abort the emulator. Contract: host code accesses device
  space only through explicit `bus_read`/`bus_write`; debug builds assert-on-MMIO-range in
  `Mac2HostAddr`; the probe/watch tools route through the bus or refuse device ranges loudly.

**Region kinds** *(rev 2)*: the bus API distinguishes **trapped-MMIO** regions (unmapped,
fault-dispatched — device registers) from **mapped-aperture** regions (real memory, direct
access, dirty-tracked — the future Metal framebuffer). Trap-per-write framebuffers are
impossible; baking the distinction into the API now avoids a bus redesign at the Metal
milestone.

**Polling-loop strategy** *(rev 2 — the original "boot-time polling is not perf-critical" was
contradicted by our own data: the nanokernel idle loop IS a device poll, observed at ~2.6M
iter/s. rev 3 — costed honestly: this build uses **Mach exceptions**, not in-thread signals;
each fault is a Mach message + thread suspend + 2× get/set state = microseconds, so a MHz-rate
poll through the fault path is ~hours-per-second of wall time, not merely "slow")*:
**(a) JIT backpatch is in M1 scope, not in reserve** — a known-faulting access site gets
patched to call the bus directly (Dolphin's approach); the fault path is the *discovery*
mechanism, the backpatch is the steady state for hot sites; **(b) idle-detection** — when a
device read polls "no work" N consecutive times, the device may request a host-side
sleep/yield (also the future power-management hook). The bus logs per-region fault rates so
any storm is visible, not theorized.

**Constraints:** 16 KB host-page granularity (trapped regions must be 16 KB-aligned; fine for
the 0xF30xxxxx device block, but means a "device pointer" aimed into mapped RAM can never
trap — consumers like `[KDP-0x900]` must point at real unmapped device addresses). The
fidelity profile **disables** `ignoresegv` and the legacy hardcoded PC/register serial-skip
hacks (`sheepshaver_glue.cpp` sigsegv handler) — a skip firing before bus dispatch would
silently eat a device access. MMIO regions are excluded from `SS_JIT_VERIFY` replay (device
reads are side-effecting — clear-on-read/FIFO-pop — and must never be double-executed).

**Cost model:** zero on the RAM/ROM fast path — those stay raw JIT loads/stores. **OS X door
(decision 2):** the bus is the memory-access seam; if translation is ever needed it slots in
here.

### 2c. Virtual clock & event scheduler *(rev 2 — new component; was entirely missing)*

**M2 landed (2026-06-10):** `virt_clock` (`SheepShaver/src/machine/virt_clock.cpp`) and `event_sched` (`src/machine/event_sched.cpp`) — the latter is a port of DingusPPC `core/timermanager.{cpp,h}` @ `92bb6d10549529f9f4031a85c2bc136149535bdc` (GPL-3.0) — are the live production implementation of this section.

There is no guest time infrastructure today: DEC reads return 0 (or the `SS_SYNTH_DEC` hack),
`mtspr DEC` is **dropped**, and timing is the host 60 Hz tick thread. Every device model needs
a time base: VIA timers, SCC baud/timeouts, DBDMA completion, and the CPU's own
timebase/decrementer. Component: a guest-visible **TB/DEC** honoring `mtspr`/`mfspr` (mapped
to host monotonic time at a fixed ratio), plus a **host event scheduler** (ordered timer
queue) device models hang callbacks on. This is also the porting prerequisite for DingusPPC
device code, which is written against its `TimerManager` equivalent.

Note the architectural split this enforces: **DEC is a CPU-internal exception (vector 0x900)
— it never passes through the PIC.** VIA/device timers raise PIC inputs. The two paths are
distinct in §2d.

### 2d. Interrupt & exception architecture *(rev 2 — promoted to the central component; it was
mis-scattered across milestones as if a side effect)*

Today's `interrupt()` (`sheepshaver_glue.cpp:595`) is a host-managed context switch: it fakes
the nanokernel's register-save into the KDP context block, swaps stacks, runs a **nested
`execute()`** terminated by an EMUL_RETURN trampoline, and restores PC/LR/CTR — with
`SDL_PumpEvents` running inside `HandleInterrupt`. The fidelity profile replaces this with
real delivery: device → PIC → CPU exception → guest handler → `rfi` resume.

What that actually requires (the honest list):
- **MSR model:** EE (external-interrupt enable) gating, PR, IP — stored state the
  CPU core consults at interrupt-delivery points (spcflags are checked at block boundaries —
  that is the precise-interrupt point; acceptable for classic Mac OS).
- **SRR0/SRR1 + `rfi`:** real save/restore semantics so the guest handler returns into JIT
  execution. (This also structurally fixes the `sc` double-increment bug class — `sc` becomes
  a real exception instead of `execute_illegal` + ad-hoc PC bumps.)
- **Vector-base decision:** PPC exceptions vector to physical 0x0100–0x0F00 — which under
  identity mapping **collides with 68k low-memory globals** (`Ticks` at 0x16a, the RTE stubs,
  etc.). On real hardware the nanokernel maps 68k-virtual 0 away from physical 0; we defer
  real translation. Two candidate resolutions, to be settled by a bounded experiment at the
  start of M3: **(i) direct-entry** — the PIC/exception layer dispatches straight to the
  nanokernel's known handler entry points (recovered from the working `interrupt()` ABI),
  skipping the vector page entirely; or **(ii) one special-cased mapping** for the vector
  page only (the single rung-3 carve-out). Start with (i) — it is the smaller delta from the
  proven mechanism.
- **Host-integration relocation:** `SDL_PumpEvents` and friends move out of the interrupt
  path to a host-side cadence.
- ***(rev 3)* Deliverability rule — real exceptions vs the nested-execute HLE we keep:**
  EMUL_OPs/HLE (§2f) still run via nested `execute()` terminated by EMUL_RETURN trampolines.
  A PIC assertion delivered *inside* a nested context would have the guest handler `rfi`
  "back" into a continuation that is a host C++ stack frame — undefined, the `sc`
  double-increment bug class one layer up. Rule: **external interrupts are deliverable only
  at depth-1 block boundaries**; nested HLE contexts execute as if MSR.EE were masked, with
  a pending-latch drained on return to depth 1. M3's harness additions include a test vector
  for exactly this.
- **CPU-core honesty** *(rev 2 — the original "CPU core unchanged" claim was false)*: `sc`,
  DEC, MSR, SRR0/1 live in shared CPU files (`ppc-execute.cpp`, `ppc-jit.cpp` SPR cases).
  The profile seam goes *inside* the CPU core on **slow paths only** (exception dispatch, SPR
  access) — a profile check at those sites, never per-memory-op. Standing rule: any new
  `powerpc_registers` fields (MSR, SRR0/1, DEC) are appended **last** (the JIT hardcodes
  struct offsets) and require a clean PPC recompile (known stale-build footgun).

### 2e. Supervisor environment (the rung-ladder, continued as components)

- **Rung 2 — SR/BAT as stored state** (S, general correctness): store what `mtspr`/`mtsr`
  writes, return it on read-back, like the SPRG0–3 fix already harvested from Path A.
- **Synthesized Trampoline handoff:** the **recovered-by-RE** register/KDP/device-tree state
  Open Firmware leaves for the nanokernel *(rev 2: not "documented" — no public source states
  the Trampoline's post-conditions; our contract comes from Path A's wedge/RE work and has
  known soft spots, e.g. the SegMap episode)*. Built by ABI contract as a designed
  initializer, publishing the §2a device tree. Promoted from the env-gated
  `SS_NW_TRAMPOLINE` writes.
- **MMU bookkeeping only:** rungs 3+ (pre-seeded HTAB, shadow arenas, softmmu) stay deferred
  per MMU-DEFERRAL-REDTEAM — except the possible single vector-page carve-out (§2d).

### 2f. What stays HLE on purpose

Video (→ the future Metal surface, advertised through the §2a device tree as a display node),
disk, ethernet, sound, file system. These are *runtime* paths where paravirtualization is the
feature, not the compromise — it's where the speed is and where host integration (and
eventually Metal) hooks in. The hybrid split is permanent architecture, not a transition
state: **LLE for what the ROM probes, HLE for what the OS uses.** Constraint from §2a: the
device tree must not advertise LLE capabilities behind HLE devices (no phantom DBDMA channels
on nodes we service by HLE).

### 2g. Execution & locking model *(rev 3 — new section; rev 2 had zero words on threading)*

This build handles faults via **Mach exceptions on a dedicated handler thread**
(`HAVE_MACH_EXCEPTIONS`, `sigsegv.cpp:727` spawns `handleExceptions`, which mutates the
suspended CPU thread via `thread_get/set_state`). So the machine layer's contexts are:

| Context | Thread | Touches device state? |
|---|---|---|
| JIT-path MMIO (fault dispatch) | **Mach exception-handler thread**, CPU thread suspended mid-instruction | yes |
| Interpreter/host-accessor MMIO | emul (CPU) thread | yes |
| Event-scheduler callbacks (VIA timers, DBDMA completion) | tick/timer thread | yes |
| Interrupt assertion (PIC → CPU) | any of the above | spcflags only |

Rules:
1. **One lock per device** (or finer): all device-state mutation goes through the owning
   device's lock, regardless of entry path. Device handlers must be small and never call
   out into emulator subsystems while holding their lock.
2. **The Mach-handler thread takes no foreign locks** — no malloc, no stdio, no JIT-compile
   lock; anything possibly held by the suspended thread deadlocks the emulator. Device
   handlers reachable from the fault path must satisfy this (lock-only-their-device,
   pre-allocated memory, deferred logging via ring buffer).
3. **Interrupt assertion is atomic-with-ordering**: PIC → spcflags uses the existing atomic
   spcflags mechanism (release/acquire), safe from any thread; delivery happens only at the
   CPU thread's block-boundary poll (§2d).
4. **SIGUSR2 interplay**: the legacy tick→`pthread_kill(emul_thread, SIGUSR2)` interrupt
   path can land while the CPU thread is Mach-suspended; on the fidelity profile the SIGUSR2
   mechanism is retired with the nested-execute path (M3) — until then, fidelity-profile
   testing documents the suspension window as a known hazard.
5. **lldb caveat**: a debugger attach contends the EXC_BAD_ACCESS exception port — debugging
   the bus with lldb perturbs the bus. Bus diagnostics must be log/telemetry-first (consistent
   with the existing single-attach VBL rule in LEARNINGS).

---

## 3. Milestones *(rev 2 — re-cut after review: M1 grew, the clock got its own milestone, the
interrupt/exception redesign is now explicitly the big rock, and the inherited Path A walls
got their own milestone instead of hiding inside "integration")*

Each milestone is independently valuable and gated; the paravirtual profile's gates
(`make test-jit`, `make e2e`, bench history) stay green throughout — the standing
non-regression contract.

**Pre-M0 spikes** — ✅ **ALL THREE COMPLETE (2026-06-10, same day — see
`docs/planning/spikes/`)**. Results, each of which changed the plan (rev 4):

- ✅ **S1 — QEMU gate-check** (`SPIKE-S1-QEMU-GATE-CHECK.md`): **gates PASS natively on the
  9.0.1 ROM** — the unpatched 9.2.1 installer (all `_SysError` sites A9C9-intact, verified)
  boots to Finder under QEMU mac99 with the 2001 "Mac OS ROM 9.0.1" swapped in. **And the
  gate-2 probe (~0x7E24) is not a model check at all: it is a CFM boot-fragment audit** —
  `Gestalt('mach')` selects a checklist, then verifies DebugLib/InterfaceLib/Math64Lib/
  MPLibrary/… fragments via `GetResource('fovr'/'sfvr'/'nlib')` + CFM lookups — state the
  9.0.1 ROM's `prcl` parcels provide and **the 1.1 ROM structurally cannot** (no parcels, no
  fragment names). Consequences: (a) M7's path is settled — native 9.0.1 ROM, no ROM-swap
  rung, 4-byte bypass kept only for the residual $76-on-HD-copy case; (b) **Path B was
  structurally doomed**, retroactively explaining why identity patches never worked; (c) M1
  consumer (a) (9.2-on-1.1) is re-framed as a **bus/SCC testbed only** — its endgame is
  capped by the missing parcels, it is not a route to 9.2. Bonus: `macos921.dsk` actually
  contains Mac OS **8.6**, not 9.2.1.
- ✅ **S2 — Mach fault-decode spike** (`SPIKE-S2-MACH-FAULT-DECODE.md`, working code in
  `spikes/s2-mach-fault-decode/`): **keystone validated end-to-end, first try** — PROT_NONE
  page → Mach EXC_BAD_ACCESS on a handler thread → decode the exact JIT form
  (`LDR Wt,[Xn,Wm,UXTW]`, mask 0xFFE0FC00/0xB8604800) → inject raw BE via
  `thread_set_state` → resumed `REV` yields the correct guest value; **identical from a
  MAP_JIT page** (known-unknown resolved); zero entitlement/hardened-runtime friction.
  **Measured cost: ~5.8–9.6 µs/fault (typical ~8.5 µs) vs sub-ns mapped — ~10⁴×** — the
  2.6M iter/s idle poll would cost ~22 wall-s per guest-s through the fault path, hard-
  confirming backpatch-in-M1-scope. Decoder gotchas captured for M1: PAC-safe PC accessors,
  rt==31→WZR, zero-extended W writeback, width-keyed inject (LDRB has no REV; stores REV
  *before* STR so the faulting value is already raw BE).
- ✅ **S3 — stall-loop device probe** (`SPIKE-S3-STALL-DEVICE-PROBE.md`): **the identities
  were mislabeled again** (decision-5 caveat vindicated). The 9.2.1 post-splash stall is the
  ROM's factory **serial test monitor** ("STM 2.2/CTE 2.1") polling **SCC ch A** (RR0 bit 0
  at 0xF3012002, data +6) — but the monitor's designed escape is a **VIA 6522 T2 timeout**
  (IFR at 0xF3016000, 0x200 stride) + Cuda handshakes: r18=VIA, r19=SCC (SYSTEM-BOOT-GATES
  §5 had the labels backwards — corrected). An honest "no Rx char" SCC alone plausibly
  never terminates the monitor's blocking read — **the VIA timer is likely the real
  un-stick mechanism**. `check_work` (9.0.1) is **SCC-only, conclusively** (full Zilog WR
  init sequence decoded; HANDOFF §1.7.1 confirmed, commit e0e8640e refuted) — but its
  timeout loop needs a **ticking DEC** (M2 dependency reaching into M1 consumer (b)).
  **M1 device scope: SCC 8530 + VIA 6522 timer/IFR surface (Cuda as loud stub).** Open
  follow-up: the 9.2.1 entry path into the monitor (cheap SS_PROBE_PC probe).

| # | Milestone | Definition of done | Effort |
|---|---|---|---|
| **M0** | ✅ **Profile plumbing + machine description — DONE 2026-06-10** (machine pref + unit-tested profile module; SS_NW_* consolidated, SS_NW_TRAMPOLINE deprecated alias; skips/ignoresegv gated off on newworld; paravirtual byte-identical: test-jit 350/350 + e2e PASS; CORE99-MACHINE-DESCRIPTION.md + ROM-PATCH-AUDIT.md landed) | `machine` pref (`paravirtual` default / `newworld`); `SS_NW_*` env-gate sprawl consolidated under the profile; fidelity profile disables `ignoresegv` + legacy serial-skip hacks; paravirtual byte-identical, all gates green. **Plus the §2a machine-description artifact** (Core99 address map, interrupt tree, device-tree skeleton) reviewed against the 9.0.1 ROM's actual probes, **including a ROM-patch audit table** *(rev 3)*: every `PatchROM`/`patch_nanokernel`/`patch_68k` patch that neutralizes device init (`via_init*`, `scc_init`, `cuda_init`, GC interrupt-mask NOPs…) classified keep-on-fidelity / retire-at-Mx / replace-with-device-model — device models behind patched-out guest init are dead code, so each device milestone's DoD names the patches it retires and asserts the un-patched ROM init sequence completes. | M |
| **M1** | ✅ **MMIO bus + SCC 8530 + VIA timer/IFR surface — implementation complete + unit/e2e-validated 2026-06-10; live acceptance PARTIAL** *(scope set by spikes S2+S3; conformance audit: `docs/planning/machine/M1-DEVICE-CONFORMANCE.md`. Acceptance run 2026-06-10: bus activation, KDP wiring, scc\_init retirement, no aborts/regressions, paravirtual e2e PASS all verified; **consumer-(b) live acceptance blocked on the NK boot ceiling at pc=0x503123fc (ea=0xffffffff, page-descriptor build loop) — CEILING ROOT-CAUSED + FIXED (commit `d8932203`)**: un-seeded `KDP+0x6b4` clamped the page-descriptor loop to ~16k iterations, driving the stride-8 pointer walk at `KDP+0x80` into `0xFFFFFFFF` poison at `KDP+0x340` → faulting `stw r30,0(r8)` at `0x5031240c`; the pre-M0 paravirtual path had silently eaten these faults via `ignoresegv` for the entire page-init stage (prior "passed nanokernel init" claims under the old path were partially an ignoresegv illusion; the fidelity profile's abort-loudly design surfaced this within hours of landing). Fix: ROM instruction patch in `rom_patches.cpp` (gated `g_rom_904_lenient` + newworld): `lwz r8,0x6b4(r1)` at ROM 0x3123ac → `lis r8,1` (cap=65536 pages for 256 MB). Boot now advances one stage further then hits the **0x50326050–0x50326068 MMU/segment-handler wall**: `lwbrx` of hardcoded physical `0x200a0` (below RAMBase) inside the NK's segment-fault handler (`mtdbatl/mtdbatu`, `mtsrin`, `mtmsr` translation toggling, byte-reversed PTE accesses) — the genuine SR/BAT/supervisor-environment wall, M3/M5 territory, HANDOFF §2.8. Consumer-(b) live acceptance remains blocked, now by a root-caused, documented hard wall. Fault path + backpatch + thunk validated by unit tests (21-check machfault dispatch + THUNK-SELFTEST).)* | Three-path dispatch (§2b): AArch64 fault decoder + Mach writeback + endianness contract for the JIT path (S2-validated; S2's decoder gotchas are the unit-test checklist); software range-check in interpreter accessors (profile-gated, interpreter-mode bench proof); explicit `bus_read/write` host-accessor entry points; region kinds (trapped/aperture) in the API; **JIT backpatch for hot sites (in scope — S2 measured ~8.5 µs/fault, ~10⁴× a mapped access)**; per-region fault-rate logging + idle-detection hook; §2g locking rules implemented. Devices per S3: **SCC 8530** (legacy +2/+6 layout, WR-pointer state machine, RR0/RR1 bits) **+ VIA 6522 timer/IFR surface** (the serial monitor's T2-timeout escape is plausibly the real un-stick; Cuda = loud stub) **+ a minimal DEC tick** (pulled forward from M2: `check_work`'s timeout loop needs it — full clock/scheduler stays M2). Consumers: (a) ⚠️ **DEMOTED 2026-06-10 — likely DROP**: the post-splash stall's root cause is NOT missing SCC hardware but **A-line vector ($28) corruption from a Memory Manager heap free-list bug** (guest/ROM-vintage mismatch; interpreter-reproducible, not a JIT bug — see SYSTEM-BOOT-GATES §5 rewrite + commit 46e497d8). The serial monitor is merely where the corrupted vector lands. Combined with S1 (1.1 ROM structurally lacks the parcels/CFM fragments 9.2 audits), the 1.1-ROM path is closed; an SCC model cannot fix heap corruption. M1's acceptance consumer is (b) alone; keep (a) only if a cheap STM-monitor poke is wanted as a bus smoke test — run as a **named third config** (`paravirtual` + bus + devices − serial-skips), `SS_COMPAT_92X`'s SCC-neutralizing patches retired, DoD asserts **observed device register traffic** (no false pass); (b) nanokernel `check_work` polls a real SCC **at an unmapped F3 address** (not a RAM pointer) without a fault storm (backpatch + idle-detection proven). Device unit tests + QEMU conformance (§5). | **L** |
| **M2** | ✅ **Virtual clock — implementation complete + gates green 2026-06-10; acceptance: `9.0.1 diagnostic boot 2026-06-10: mtspr_dec=3 / mfspr_dec=0 / tb_writes=0 / dec_expiries=0; boot dies at the unchanged pre-existing 0x50326050–68 MMU/SR wall (ea=0x200a0), identical in baseline (SS_SYNTH_DEC=0) and acceptance runs — DEC is not load-bearing pre-wall on the current path; the S3 §2.4 check_work DEC consumer (0x50326520+) lies beyond the ceiling, so mfspr-DEC live coverage carries forward with it (unit-level: the check_work deadline-math simulation in test_virt_clock)`** *(virt_clock + event_sched modules: `SheepShaver/src/machine/virt_clock.cpp` + `event_sched.cpp`; DingusPPC `core/timermanager.{cpp,h}` port @ commit `92bb6d10549529f9f4031a85c2bc136149535bdc` (github.com/dingusdev/dingusppc), GPL-3.0; combined work GPLv3. Adaptations: rename, no singleton, no loguru, NS_PER_* inlined. `SS_SYNTH_DEC` absorbed as deprecated force-override (=0 escape hatch still works; readings no longer monotone free-run once the guest writes DEC). Cold-state bit-identical to M1 synth counter. DEC-expiry: CONDITION latch only — delivery is M3. `mdec_dat` gate live for 1.1-ROM newworld; 9.0.1 parcels ROM natively un-patched (pattern absent, byte-verified — the 9.0.1 boot exercises the M2 clock seams, not the gate). VIA timers re-clocked off VirtClockNowNS; N6/N7/N8 conformance notes resolved + T1CL-read ack added for symmetry. Integration: clock init after get_system_info (cpuclock pref honored), scheduler pump thread (10ms cap, kicked-predicate), DEC eager-expiry hook. Paravirtual profile inert (no thread, no output). Known accepted M2 risk (documented): VIA timer_arm allocates on a Mach-fault-reachable path (§2g); M3 hardening = pre-allocated slots. Gates: machine suite 8/8 (192+ checks), test-jit 350/350 batch+legacy, e2e-test 122/122, positive seam check (mfspr r3,DEC returned nonzero through the real interpreter seam).)* | Guest-visible TB/DEC honoring `mtspr`/`mfspr` (the `SS_SYNTH_DEC` hack and M1's minimal DEC tick retired/absorbed); host event scheduler for device timers; DEC-expiry raises the CPU decrementer exception *condition* (delivery lands in M3). | M |
| **M3a** | ✅ **Exception core + DEC delivery — DONE 2026-06-10** (exc_core pure module; sc/rfi real semantics; EE-edge re-raise; DEC delivery hook; [NW-INT] deleted; HandleInterrupt fenced; test-jit 353/353 + machine suite 9/9 + e2e PASS throughout. Acceptance: first real PPC exception delivered `[EXC] DEC delivered #1: restart=50429b40 srr1=0000f072 msr=00001040 -> entry=50412b1c`; both delivery directions verified (delivers when EE permits, defers when not); **end-to-end demo** SS_SCC_RX_INJECT CR at T+25s → 10 new compiled blocks; M1 Rx-path carry-forward closed. **Honest carry-forward:** the boot frontier is the NK Thud debug console whose designed wake is a serial character — M3a confirmed both interrupt-delivery directions live; waking past the console is M3b + M6 territory.) | M |
| **M3b** | **OpenPIC + Cuda/ADB + interrupt completion** ← *M3 second half* | OpenPIC model routing device inputs to the CPU exception path; Cuda minimal ADB stub (see M3-PIC-CUDA-DONOR-STUDY §7.2 for ADB scope decision); external-source wiring so device interrupts reach the delivery hook; `SDL_PumpEvents` relocated out of the interrupt path; tm_task/via_int/cuda_init/via_init patch retirements; RTC; deliverability harness vector (execute_depth>1 defers); completion of the nested-execute interrupt-path retirement (the `interrupt()` call path removed on newworld); boot-past-the-console question (couples to M6 PPC→68k handoff). | **L** |
| **M4** | **NVRAM + MacIO container + DBDMA stubs** | Full partitioned 8 KB NVRAM behind the bus at the KeyLargo-correct address; MacIO container address map live; **DBDMA channel stubs that abort loudly** (NewWorld serial/audio drivers probe DBDMA — rev-2 addition; real channel engine only when a milestone demands it). | S–M |
| **M5** | **Supervisor environment + boot framebuffer** | Rung 2 SR/BAT stored state; synthesized Trampoline handoff replaces `SS_NW_TRAMPOLINE` ad-hoc writes, publishing the M0 device tree — **including a boot-framebuffer aperture** *(rev 3)*: the ROM draws happy-Mac/splash to the OF display node's `address` long before any `.ndrv` loads, so M5 publishes a mapped-aperture bus region backed by real memory and blitted to SDL (also the first live test of the aperture region kind before Metal). Without it, M7 debugs a black screen. | M |
| **M6** | **PPC→68k handoff + shim triage** *(inherited Path A walls — HANDOFF §2.8 Phases 1–2, previously hidden inside "integration")* | DR Emulator cold-start ECB/dispatch-table completion (currently crashes at `rfi` to garbage SRR0); `patch_68k` shim triage for the 9.0.1 ROM (28 in-range / 31 relocated-unverified / 25 absent — incremental, boot-path-first per the obstacle map). | **L (1–2+ wks, incremental)** |
| **M7** | **Mac OS 9.2.2 boots on the fidelity profile** | End-to-end: **native 9.0.1 ROM — gate compatibility PROVEN by spike S1** (unpatched 9.2.1 boots to Finder under QEMU mac99 with this ROM; the gate-2 CFM-fragment audit is satisfied by the ROM's own parcels). Boot to Finder, E2E lifecycle PASS on the `newworld` profile. 4-byte bypass retained only for the residual $76-on-HD-copy case (QEMU couldn't exercise it). | L (integration) |
| **M8+** | **Platform features** (separate designs when reached) | PMU power management (builds on the M1 idle-detection hook); Metal-mapped video via the `.ndrv` seam + mapped-aperture bus regions; fidelity profile becomes default once it dominates paravirtual on the E2E + bench matrix. | — |

**Sequencing notes:**
- M1 is first after plumbing: smallest component that pays off on both fronts at once (the
  9.2-on-1.1 stall via the standalone SCC, and the bus+decoder foundation everything else
  uses) — the first "card slotted in."
- M2 before M3 (the PIC and VIA are meaningless without time); M4/M5 parallelizable after M3.
- M3 is the project's hardest milestone and is labeled accordingly. Its vector-base
  experiment runs *first* and is allowed to fail back to direct-entry.
- The forcing-function discipline survives: every general PPC-correctness bug surfaced en
  route (the SPRG/fctiw/SDR1/sc class) is fixed on **both** profiles immediately.

---

## 4. Sources, oracle, and provenance

- **DingusPPC = primary device-model donor.** GPL-3.0; combining is license-feasible (our
  GPLv2-or-later + their GPLv3 → combined work GPLv3 — resolved in DINGUSPPC-EVALUATION-PLAN).
  Follow backport hygiene: cite the DingusPPC source file/commit SHA in code comments and
  CHANGELOG. **Constraint: DingusPPC does not accept AI contributions into their repo — never
  send AI-generated PRs upstream to them.** Downstream GPL use in this (openly AI-assisted)
  repo is normal GPL practice with attribution. Porting caveat: their models depend on their
  event scheduler — M2 is the prerequisite.
- **QEMU mac99 = behavioral oracle** (and secondary code reference — its ESCC/Cuda/OpenPIC/
  MacIO/DBDMA models are GPL). Valid as oracle **for the Core99 target only** (decision 5).
- **Datasheets = ground truth** where emulators disagree: Zilog SCC 8530, VIA 6522, Apple
  Cuda/PMU protocol, OpenPIC spec.
- **M1 device-model conformance audit:** `docs/planning/machine/M1-DEVICE-CONFORMANCE.md` —
  QEMU + DingusPPC behavioral check for SCC 8530 + VIA 6522 against the M1 scope fence;
  zero model fixes warranted; 9 note-level deltas N1–N9 documented (2026-06-10).
- **Fork-ecosystem scan (M1, 2026-06-10):** no macemu fork (kanjitalk755, rcarmo, or any
  indexed derivative) carries device-model code; we are the first. `QEMU
  hw/misc/macio/macio.c` is the KeyLargo MacIO container reference DingusPPC lacks — relevant
  for M4 (MacIO address map + DBDMA stubs). Optional M2 input: siddhartha77
  clock-decoupling cherry-pick `7f367cf8eb62` (decouples the DingusPPC timer manager from its
  global event loop — a transitional shim for porting DingusPPC device models before M2's own
  scheduler exists).

## 5. Testing strategy

1. **Device unit tests (no boot):** each device model is a pure object — drive its register
   interface from a host-side test binary, assert state-machine transitions. Lives beside the
   existing offline suites (cheap, CI-able anywhere).
2. **QEMU conformance, not trace equality** *(rev 2 — `-d` doesn't log device registers, and
   OpenBIOS≠Apple-OF means access sequences will never match)*: (a) replay our device unit-test
   register scripts against QEMU's device model where isolable, asserting same end state;
   (b) milestone-level behavioral checkpoints ("after ROM serial init, SCC WR/RR state is X")
   captured from instrumented QEMU runs. Datasheets break ties.
3. **Existing gates, profile matrix:** `make test-jit` must stay 100 on both profiles (the
   identity-mapped data path is unchanged; the §2d slow-path seams are exercised by new
   exception-path vectors added to the harness as M3 lands); `make e2e` on paravirtual
   (non-regression contract); a new `make e2e-newworld` lane as soon as the fidelity profile
   boots anything.
4. **The boot-stage ladder as a regression suite:** each previously-conquered wall (nanokernel
   init, idle-loop wake, DR Emulator entry, 68k dispatch, splash, Finder) becomes a named,
   asserted checkpoint — so a device-model change can't silently regress an earlier stage.
5. **MMIO fault-rate telemetry as a gate:** per-region fault counters in the bus; an idle
   fidelity-profile boot must not exceed a budgeted fault rate (catches polling storms as a
   regression, not a discovery).
6. ***(rev 3)* Asset reality:** the 9.0.1 ROM and 9.2.x media are Apple-copyrighted and
   non-redistributable — the `e2e-newworld` lane is local-assets-only forever (no hosted CI,
   no fresh-clone reproducibility; same posture as the existing e2e assets). Interpreter-mode
   bench measurement added to the profile matrix (§2b range-check gating proof).

## 6. Error handling & risk

| Risk | Mitigation |
|---|---|
| AArch64 fault-decoder gaps (JIT emits an access form the decoder misses) | The JIT's memory-access emitters are the contract: decoder unit tests enumerate every emitted form; unmapped-but-unregistered or undecodable faults abort loudly with PC + address (no silent zero-reads, no blind `pc += 4`). |
| Cross-thread device-state races / Mach-handler deadlock | §2g locking model: per-device locks, no foreign locks on the exception-handler thread, atomic spcflags assertion, SIGUSR2 suspension-window documented until M3 retires it. |
| ROM patches and device models double-handling the same hardware | M0 ROM-patch audit table; each device milestone retires its patches and asserts the un-patched guest init completes. |
| M1 false pass via `SS_COMPAT_92X` (boot progresses but the device model never exercised) | Named third config; SS_COMPAT_92X SCC-neutralizing patches retired in that config; DoD requires observed device register traffic. |
| Wrong device identity behind the boot stall (SCC vs VIA — already flip-flopped twice) | Spike S3 disassembles the actual polling loop before M1's device scope is frozen. |
| Interpreter/nested-execute MMIO from handler contexts | First-class second dispatch path (§2b), not an afterthought; covered by device unit tests run through both engines. |
| MMIO polling storm (idle loop = SCC poll at MHz rates) | Idle-detection hook + fault-rate telemetry budget (§5.5); JIT backpatch held in reserve. |
| Side-effecting device reads double-executed by oracles | MMIO regions excluded from `SS_JIT_VERIFY` replay; bus logging never re-reads. |
| Device model fidelity rabbit holes (modeling more than the ROM probes) | YAGNI per device: implement registers the trace shows are touched; abort-loudly stubs for the rest (incl. DBDMA channels). QEMU conformance bounds "done." |
| Vector-page collision with 68k low memory (V=P) | Decided by a bounded experiment at M3 start; direct-entry fallback is the smaller delta from the proven `interrupt()` ABI. |
| Paravirtual regression while refactoring shared CPU files | Profile seam on slow paths only; new `powerpc_registers` fields appended last + clean recompile rule; existing gates run per PR. |
| Trampoline handoff contract is recovered-by-RE, not documented | Treated as a versioned, tested artifact (the M0 machine description); QEMU mac99 supervisor-state capture (UPGRADE-CARD Experiment 2) validates the recovered fields. |
| 9.0.1 ROM still rejects 9.2.x at some deeper gate | SYSTEM-BOOT-GATES methodology (DSAT parse + binary search) reusable on any System version; 4-byte bypass remains the fallback. |
| Effort balloons past appetite | Milestones individually shippable; stop-rule per wall (HANDOFF §2.7.1): re-evaluate when a wall needs multi-day RE with no general payoff. M6 is explicitly incremental (boot-path shims first). |

## 7. Roadmap integration

- **ROADMAP D3** points here as the primary approach (Upgrade Card demoted to tactical
  tool within M1/M7).
- Track B (perf) and the COMPATIBILITY-PAYOFF "make 8.6–9.0.4 usable" thrust continue on the
  paravirtual profile, unblocked and unaffected.
- Silicon Sheep (Track C) eventually surfaces the machine profile as a VM-library choice
  ("Power Mac 9500 (fast)" vs "Power Mac G4 (faithful)").

## 8. Review log (rev 2, 2026-06-10)

An adversarial code-verified review of rev 1 found 12 issues; all are incorporated above.
The structural ones, for the record:

1. **No AArch64 faulting-access decoder exists** (`sigsegv.cpp` aarch64 = `pc += 4`) — rev 1's
   keystone premise was wrong → §2b two-path dispatch; M1 re-scoped to L.
2. **Interrupt/exception redesign was scattered and mis-sequenced** (old M2 depended on old
   M4's vectoring; vector page collides with 68k low memory under V=P) → §2d, consolidated
   as M3, the big rock.
3. **No virtual clock existed in the plan or the code** (DEC reads 0, `mtspr DEC` dropped);
   DEC≠PIC paths conflated → §2c, milestone M2.
4. **Idle loop = MMIO poll storm** contradiction → §2b polling strategy + §5.5 telemetry gate.
5. **Hidden two-machine fork** (1.1 = Heathrow-class vs 9.0.1 = Core99/KeyLargo) → decision 5.
6. **DBDMA absent** → M4 stubs + §2a/§2f device-tree constraint.
7. **M5 (now M7) silently inherited Path A's walls** (DR Emulator handoff, 84-shim port) → M6.
8. **Trampoline "documented ABI" overstated** (it's recovered-by-RE) + device tree needed to
   be an M0 artifact → §2a, §2e.
9. **"CPU core unchanged" was false** (sc/DEC/MSR/SRR in shared files) → §2d CPU-core honesty
   + struct-offset rule.
10. **QEMU trace-diff methodology unworkable as written** → §5.2 conformance reframe.
11. **Side-effecting MMIO vs SS_JIT_VERIFY / legacy serial-skip hacks** → §2b exclusions,
    M0 disables.
12. **16 KB page granularity + trapped-vs-aperture region kinds** (Metal constraint now) →
    §2b.

**Rev 3 (second adversarial review, 2026-06-10)** — 9 further findings, none duplicating rev 2:

13. **No threading/concurrency model** — this build uses Mach exceptions on a dedicated
    handler thread (`sigsegv.cpp:727`); JIT-path MMIO runs on that thread with the CPU thread
    suspended; scheduler callbacks run on a third thread; SIGUSR2 interplay; Mach round-trip
    cost makes fault-path polling ~hours/sec at observed rates → new §2g; backpatch promoted
    into M1 scope; lldb exception-port caveat.
14. **0xF3016000 is the VIA per our own AddrMap** (`rom_patches.cpp:1903`), and the
    `[KDP-0x900]` identity has flip-flopped twice — the stall's device identity is unverified
    → decision 5 caveat + spike S3.
15. **M1 consumer (a) is a third, unnamed config on the frozen profile, confounded by the
    `SS_COMPAT_92X` SCC-neutralizing patches** (false-pass risk) → named config, patch
    disposition, register-traffic DoD.
16. **No ROM-patch retirement inventory** — device models behind patched-out guest init
    (`via_init*`, `scc_init`, `cuda_init`, GC mask NOPs) are dead code → M0 audit table,
    per-milestone patch retirement.
17. **Third memory-access path unhandled** — host C++ accessors (`ReadMacInt*`,
    EMUL_OP/HLE/`Execute68k`) and our own debug tools (`SS_PROBE_PC`, `SS_JIT_WATCH_ADDR`)
    would abort-loudly on device pages → §2b host-accessor contract.
18. **M7's premise testable for ~zero code now** (QEMU gate-check; 0x7E24 probe RE; newer
    family ROM fallback) → pre-M0 spikes S1; M7 fallback ladder.
19. **Real exception delivery vs kept nested-execute HLE unreconciled** (`rfi` into a host
    C++ continuation) → §2d deliverability rule + M3 test vector.
20. **No boot framebuffer** — the ROM draws the splash to the OF display node long before any
    `.ndrv`; M7 would debug a black screen → M5 aperture + blit.
21. **Honesty gaps:** interpreter range-check cost on the frozen profile (gating + bench
    proof) and the non-redistributable-assets posture for `e2e-newworld` → §2b, §5.6.

## 9. Holistic assessment — chances of success (2026-06-10, pre-implementation)

A calibration record, written before any code, to be re-scored as milestones land. Three
nested bets:

| Bet | Definition | Estimate | Why |
|---|---|---|---|
| **Platform** | SheepShaver gains a real machine layer (bus, clock, devices, exception model) as permanent, tested components | **~90%** | Every milestone independently shippable; paravirtual frozen as a floor; even M0–M2 alone leaves the codebase structurally better than the patch-pile. Downside bounded by design. |
| **Capability** | Mac OS 9.2.2 boots to Finder on the fidelity profile (M7) | **~60–65%** in ~2–4 months focused effort | Dragged down by M3 and M6 (below), not by the device models. |
| **Vision** | Full-stack platform: power management, Metal video (M8+) | unscoreable | Strictly downstream of the capability bet; prerequisites (aperture regions, idle hooks, device-tree video node) are baked into the architecture rather than blocked by it. |

### Where the risk actually lives (ranked by expected pain)

1. **M3 (interrupt/exception, XL) — the make-or-break.** MSR/SRR/`rfi`, the vector-page
   collision, replacing nested-execute, in shared CPU files. Could eat a month. The saving
   grace: the **direct-entry option is a designed, honest version of the mechanism that
   already works today** — M3's floor is not "fail" but "ship a cleaner version of the
   current architecture with real device sources." That floor is what holds the capability
   estimate at 60+ rather than lower.
2. **M6 (inherited Path A walls).** DR Emulator handoff + shim triage: a known grind with a
   known map. No architecture saves this — it's RE labor; motivation, not engineering, is the
   limiting reagent.
3. **The unknown 9.2 gate (spike S1).** What the 0x7E24 probe checks is genuinely unknown —
   but this is uncertainty about *which path* (native 9.0.1 / newer family ROM / 4-byte
   bypass), not *whether a path exists*.

The things that *look* scary — MMIO decoding, Mach exceptions, device models — are
well-understood engineering with spikes, contracts, and oracles attached.

### The meta-signal

**21 substantive findings before a line of code.** Read both ways: (a) the review process
works — two rounds of code-verified, plan-changing findings absorbed without the skeleton
breaking is evidence the skeleton is right; (b) this domain is **hostile** — reviews 3 and 4
will be delivered by reality, and the project's own history (the VIA/SCC identity
flip-flopping twice, rev 1's "sigsegv already decodes" assumption, the session-5 STUCK-PC
retraction) shows a high error rate on untested beliefs. The plan's main defense is that it
systematically converts beliefs into experiments (S1–S3, the M3 vector-base experiment, QEMU
conformance) before building on them.

### What success most depends on

1. **Run spikes S1–S3 before anything else** (~a week, de-risks the two biggest bets; S1 can
   reshape M7's endgame for zero code).
2. **Honor the stop-rule at M3.** If the vector-base experiment fails and direct-entry also
   bogs down: ship the M3 floor and reassess — don't tunnel.
3. **Resist the old failure mode.** The single most likely cause of failure is reverting to
   bug-chase mode mid-milestone (fixing whatever the boot hits next instead of finishing the
   component). The milestone DoDs (register-traffic assertions, patch-retirement lists,
   fault-rate budgets) are shaped specifically to resist that; trust them.

### Bottom line

Architecture sound; reviews made it honest; downside structurally capped (worst case:
today's working emulator unchanged, plus reusable components and a much better map); upside
is the project's stated reason to exist. The expected outcome is not binary — most futures
land on "machine layer partially built, every piece permanently useful, 9.2 reached by one
of three documented paths."

**Re-scoring triggers:** after spikes S1–S3 (adjust M1 scope + M7 path), after the M3
vector-base experiment (adjust the capability estimate), and at any stop-rule invocation.

### Re-score #1 — post-spikes (2026-06-10, same day; all three spikes complete)

| Bet | Was | Now | Why |
|---|---|---|---|
| Platform | ~90% | **~92%** | S2 removed the keystone unknown (fault-decode works end-to-end incl. MAP_JIT, zero platform friction); the bus's hardest mechanism is now demonstrated code, not a design. |
| Capability (M7) | ~60–65% | **~70–75%** | S1 removed the gate unknown *in the favorable direction* — unpatched 9.2.1 boots to Finder on the 9.0.1 ROM under QEMU, so M7 needs no System-file patching and no ROM-swap rung. The remaining drag is unchanged: M3 (exception architecture) and M6 (DR Emulator handoff + shims) — neither was touched by the spikes. |
| Vision | unscoreable | unscoreable | Unchanged; still downstream. |

Qualitative shifts:
- **The plan's epistemics validated on day one:** all three spikes changed the plan (S1
  re-framed M1 consumer (a) and settled M7; S2 quantified the fault cost and resolved the
  MAP_JIT unknown; S3 corrected the device identities a *third* time and pulled a DEC tick
  into M1). The "beliefs must become experiments" discipline is paying measurably.
- **Path B's post-mortem is now mechanistic:** the gate-2 probe audits CFM boot fragments
  that only parcels ROMs provide — identity patches never had a chance. The pivot was
  correct for reasons deeper than we knew when we made it.
- **New honest negative:** the 9.2-on-1.1 demo (M1 consumer (a)) is a testbed, not a
  product milestone — the visible "9.2 splash on the old ROM" win will not become a boot.
  The real 9.2 path runs entirely through the fidelity profile + 9.0.1 ROM (M3→M6).
