# The Machine Layer — a designed NewWorld fidelity profile

> **Status:** 🟢 Approved architecture (rev 3) — implementation not started · **Created:** 2026-06-10
> · **Updated:** 2026-06-10 (rev 2 + rev 3: two rounds of adversarial code-review findings — §8;
> plus §9 holistic success assessment with re-scoring triggers)
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
5. **(rev 2) The fidelity profile models exactly ONE machine: Core99-class (Power Mac G3
   B&W/G4 era — KeyLargo MacIO + OpenPIC), the machine QEMU `mac99` and DingusPPC both model
   well and the 9.0.1 ROM targets.** The 1.1 ROM expects Heathrow/Paddington-class hardware —
   a *different* machine; supporting both would mean two address maps, two PICs, two device
   trees. The 1.1-ROM/9.2.1 tactical path (Path B's frontier) gets **only standalone device
   model(s) at the addresses the stall loop actually polls** — not the full profile.
   *(rev 3 correction: the device identity behind the stall is NOT settled. Our own AddrMap
   patch — `rom_patches.cpp:1903` — assigns 0xF3016000 to the **VIA** and 0xF3012000 to the
   SCC, and the project has flip-flopped on the `[KDP-0x900]` identity twice. A pre-M1 probe
   pass must disassemble the actual stall loop and pin which device(s)/offsets it polls —
   it may need the VIA (+ timer) as well as or instead of the SCC, which would change M1's
   scope. See §3 spikes.)*

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

**Pre-M0 spikes** *(rev 3 — cheap experiments that de-risk the plan's two biggest bets,
run BEFORE committing to the milestone sequence)*:
- **S1 — QEMU gate-check (days, ~zero code):** boot Mac OS 9.2.x under QEMU `mac99` with the
  same 9.0.1 "Mac OS ROM" file. Directly answers M7's untested premise ("9.0.1 may satisfy
  9.2.x's gates natively") — what 9.2's gate-2 subroutine (~0x7E24) actually checks is still
  unknown. Statically RE-ing that probe is the second cheap angle. If 9.0.1 fails, the
  near-free fallback is a **newer family ROM** (9.6.1/9.8.1 differ from 9.0.1 by 0.04% in the
  nanokernel — HANDOFF §1.5), before falling back to the 4-byte bypass.
- **S2 — Mach fault-decode spike (~1 day):** trap one unmapped page, decode one JIT-emitted
  `LDR`, inject a value via `thread_set_state`, resume, observe the `REV`'d result in the
  guest. Proves M1's keystone end-to-end (incl. measuring the real Mach round-trip cost)
  before the bus is designed around it.
- **S3 — stall-loop device probe (~half day):** disassemble the 9.2.1 post-splash polling
  loop and the nanokernel `check_work` consumer; pin **which device(s)** (SCC vs VIA vs both)
  and which register offsets they poll (decision 5 rev-3 caveat). Determines M1's actual
  device scope.

| # | Milestone | Definition of done | Effort |
|---|---|---|---|
| **M0** | **Profile plumbing + machine description** | `machine` pref (`paravirtual` default / `newworld`); `SS_NW_*` env-gate sprawl consolidated under the profile; fidelity profile disables `ignoresegv` + legacy serial-skip hacks; paravirtual byte-identical, all gates green. **Plus the §2a machine-description artifact** (Core99 address map, interrupt tree, device-tree skeleton) reviewed against the 9.0.1 ROM's actual probes, **including a ROM-patch audit table** *(rev 3)*: every `PatchROM`/`patch_nanokernel`/`patch_68k` patch that neutralizes device init (`via_init*`, `scc_init`, `cuda_init`, GC interrupt-mask NOPs…) classified keep-on-fidelity / retire-at-Mx / replace-with-device-model — device models behind patched-out guest init are dead code, so each device milestone's DoD names the patches it retires and asserts the un-patched ROM init sequence completes. | M |
| **M1** | **MMIO bus + boot-stall device model(s)** | Three-path dispatch (§2b): AArch64 fault decoder + Mach writeback + endianness contract for the JIT path (validated by spike S2); software range-check in interpreter accessors (profile-gated, interpreter-mode bench proof); explicit `bus_read/write` host-accessor entry points; region kinds (trapped/aperture) in the API; **JIT backpatch for hot sites (in scope, not reserve)**; per-region fault-rate logging + idle-detection hook; §2g locking rules implemented. Device model(s) per spike S3 (SCC 8530, possibly VIA timer) answer the ROM's polling. Consumers: (a) 9.2.1-on-1.1-ROM boot progresses past the post-splash stall — run as a **named third config** (`paravirtual` + bus + device − serial-skips), with `SS_COMPAT_92X`'s SCC-neutralizing patches retired so the DoD asserts **observed device register traffic**, not just boot progress (no false pass); (b) nanokernel `check_work` polls a real device **at an unmapped F3 address** (not a RAM pointer) without a fault storm (backpatch + idle-detection proven). Device unit tests + QEMU conformance (§5). | **L** |
| **M2** | **Virtual clock** | Guest-visible TB/DEC honoring `mtspr`/`mfspr` (the `SS_SYNTH_DEC` hack retired); host event scheduler for device timers; DEC-expiry raises the CPU decrementer exception *condition* (delivery lands in M3). | M |
| **M3** | **Interrupt & exception architecture + PIC + VIA/Cuda** ← *the big rock* | §2d in full: MSR(EE)/SRR0-1/`rfi` model; vector-base experiment decided (direct-entry vs single mapping); OpenPIC model routing device inputs; VIA/Cuda (timers via M2 scheduler, ADB, RTC); the `[NW-INT]` host-injection hack and nested-execute interrupt path **deleted on the fidelity profile**; nanokernel idle loop wakes via real delivery on the 9.0.1 diagnostic boot; `SDL_PumpEvents` relocated. | **XL** |
| **M4** | **NVRAM + MacIO container + DBDMA stubs** | Full partitioned 8 KB NVRAM behind the bus at the KeyLargo-correct address; MacIO container address map live; **DBDMA channel stubs that abort loudly** (NewWorld serial/audio drivers probe DBDMA — rev-2 addition; real channel engine only when a milestone demands it). | S–M |
| **M5** | **Supervisor environment + boot framebuffer** | Rung 2 SR/BAT stored state; synthesized Trampoline handoff replaces `SS_NW_TRAMPOLINE` ad-hoc writes, publishing the M0 device tree — **including a boot-framebuffer aperture** *(rev 3)*: the ROM draws happy-Mac/splash to the OF display node's `address` long before any `.ndrv` loads, so M5 publishes a mapped-aperture bus region backed by real memory and blitted to SDL (also the first live test of the aperture region kind before Metal). Without it, M7 debugs a black screen. | M |
| **M6** | **PPC→68k handoff + shim triage** *(inherited Path A walls — HANDOFF §2.8 Phases 1–2, previously hidden inside "integration")* | DR Emulator cold-start ECB/dispatch-table completion (currently crashes at `rfi` to garbage SRR0); `patch_68k` shim triage for the 9.0.1 ROM (28 in-range / 31 relocated-unverified / 25 absent — incremental, boot-path-first per the obstacle map). | **L (1–2+ wks, incremental)** |
| **M7** | **Mac OS 9.2.2 boots on the fidelity profile** | End-to-end: 9.0.1 ROM (gate compatibility answered up-front by spike S1, not discovered here), boot to Finder, E2E lifecycle PASS on the `newworld` profile. Fallback ladder *(rev 3)*: newer family ROM (9.6.1/9.8.1) → 4-byte gate bypass. | L (integration) |
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
