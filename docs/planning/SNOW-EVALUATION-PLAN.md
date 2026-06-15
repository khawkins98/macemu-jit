# Plan: Snow comparative evaluation (classic-mac emulation + debugger tooling)

> **Status:** ⏸ Dormant — S1 crosswalk done, S2 panels specified but not executed · **Created:** 2026-06-06 · **Updated:** 2026-06-10
> **Why this doc exists:** Track a disciplined comparison against Snow so we can selectively adopt useful emulation and debugging ideas without derailing SheepShaver priorities.
> _Markers: ✅ done · 🟡 in progress · ⏸ blocked/deferred · ☐ todo. Finished an item? Flip its marker, bump **Updated**, and add a `CHANGELOG.md` entry (see [CONTRIBUTING](../../CONTRIBUTING.md) → "Documentation Lifecycle")._

---

## TL;DR

We treat [twvd/snow](https://github.com/twvd/snow) as a **reference emulator** with strong
hardware-fidelity and debugging UX ideas, not as a port target. **Inspiration, not code lifting** —
Snow is Rust + egui with an embedded 68K emulator; we're Tauri + web with a sidecar PPC emulator.
What transfers is the *UX pattern* of per-panel debug widgets, not the implementation.

> **Validated 2026-06-06 (web).** Snow is a **68K-only** Macintosh emulator (Mac 128K/512K/Plus/SE/
> Classic/Macintosh II era; 68000/68020/68030 + FPU/PMMU), **Rust + egui**, **MIT-licensed**, and
> explicitly **hardware-level** ("emulate at the hardware level… as opposed to emulators that patch
> the ROM or intercept system calls" — the deliberate *opposite* of SheepShaver/BasiliskII's
> paravirtualization). Consequences for us:
> - **Snow's *emulation* lessons apply to BasiliskII (68K) only — never to SheepShaver's PowerPC
>   path.** Don't let "hardware fidelity" leak into SheepShaver design discussions.
> - **Only Snow's *debugger/observability UX* transfers to SheepShaver** — and it's the highest-value
>   thing here.

---

## Snow's 10 debug panels → SiliconSheep mapping

Snow (`frontend_egui/src/widgets/`): one module per panel, floating egui windows overlaid on the
emulator framebuffer. State access via message-passing to the emulator thread — cached snapshots,
never direct memory access from the UI.

| # | Snow panel | What it shows | SiliconSheep feasibility | Data source |
|---|-----------|---------------|------------------------|-------------|
| 1 | **Registers** | CPU data/addr/SR/FPU, change highlighting | **Tier 2** — needs new emulator endpoint | New `dump_regs` signal handler or UDS command (~50 LOC) |
| 2 | **Disassembly** | Live instruction disassembly at PC | **Tier 3** — needs memory read + disassembler | Guest RAM read + capstone WASM |
| 3 | **Memory** | Hex viewer with navigable address | **Tier 2** — needs UDS memory read | `Mac2HostAddr()` via UDS command |
| 4 | **Breakpoints** | Execution breakpoints | **Tier 3** — needs debug protocol | Significant new emulator work |
| 5 | **Watchpoints** | Memory write/read watchpoints | **Already have** `SS_JIT_WATCH_ADDR` | Env var, log-based; UI in Debug tab |
| 6 | **Instruction history** | Trace of recent instructions | **Already have** `SS_JIT_TRACE_RING` | Env var enables; ring dump on crash |
| 7 | **Trap history** | A-trap call log | **Tier 1** — parse existing stderr | `[BOOT]`, `[SYSV]`, `[APP]` signals |
| 8 | **Peripherals** | VIA/SCC/SCSI register state | **Tier 3** — needs emulator introspection | Not exposed; low value for PPC |
| 9 | **Framebuffer** | Raw video memory | **Already have** VNC screenshots | `vnc_capture.py` every ~10s |
| 10 | **Terminal** | Serial port output | ⏸ Not applicable | SheepShaver serial is rarely used |

---

## Adopt / defer / avoid (revised 2026-06-06)

### Adopt now — Tier 1 (zero emulator changes, pure log/stats parsing)

These can be built in SiliconSheep's Debug tab today:

- [ ] **JIT Stats dashboard** — block count, cache usage/capacity, pool utilization, compile rate.
  Source: `ppc_jit_aarch64_get_stats()` (already exists, already in SDL title bar). Render as
  gauges or sparklines in the Debug tab. Parse from heartbeat `[HB ...]` lines in stderr.
- [ ] **Diagnostic log viewer** — live-tail of the heartbeat/diagnostic log with category filtering
  (heartbeat, HOT-PC, j2i, warnings). Source: `jit_diag.log` or stderr. Replace the current
  `alert()` log viewer with a proper scrollable, filterable panel.
- [ ] **Signal/event history** — structured timeline of `[BOOT]`, `[SYSV]`, `[APP]`, `[READY]`
  signals with timestamps. Source: stderr parsing (already captured to `last_run.log`).
- [ ] **Explicit cost model** — Snow-style note: "Debug panels impact performance while active."
  Show a warning when debug env vars are set.

### Adopt next — Tier 2 (minor emulator additions, ~50 lines each)

These need small, bounded C++ additions to the emulator:

- [ ] **Register inspector** — GPR (r0-r31), SPR (LR, CTR, XER, CR), PC snapshot on demand.
  Needs: a signal handler (e.g. `SIGUSR2`) or UDS endpoint that dumps `powerpc_registers`
  as JSON to a file or socket. SiliconSheep reads and displays with change highlighting
  (Snow-style: yellow on changed values between snapshots).
- [ ] **Memory hex viewer** — read N bytes of guest RAM at an arbitrary address. Needs: UDS
  command that calls `Mac2HostAddr(guest_addr)`, reads N bytes, returns hex. SiliconSheep
  renders a classic hex+ASCII grid. Navigable address input.
- [ ] **Guest state sidebar** — CurApName, WindowList, Ticks, MBarHeight, SysVersion — the
  low-memory globals from `HOST-GUEST-CHANNELS.md`. Already readable; just needs a periodic
  dump via the idle hook or UDS.

### Defer — Tier 3 (significant new emulator work)

- [ ] **Live disassembly** — fetch + disassemble guest code at PC. Needs memory read (Tier 2)
  plus a PPC disassembler (capstone). Could use a WASM capstone build in the web UI, or
  a server-side disassembly endpoint.
- [ ] **Execution breakpoints** — pause/resume/single-step. This is a *debugger*, not an
  inspector. Needs a full debug command protocol. Large scope, deferred until the differential
  verification (SS_JIT_VERIFY) workflow is mature enough to know what's needed.
- [ ] **Peripheral state** — VIA/SCC/SCSI registers. Low value for PPC (SheepShaver
  paravirtualizes most hardware). Only relevant for deep nanokernel debugging.

### Avoid

- Porting Snow code (wrong architecture, wrong CPU, wrong UI toolkit).
- Building a step-debugger before the observability inspector proves its value.
- Expanding to support Snow's hardware targets (68K-only, not PPC).

---

## Scope and non-goals

**In scope**
- Debugger and instrumentation ergonomics applied to SheepShaver triage workflows.
- Snow's per-panel widget architecture as a *design reference* for SiliconSheep's Inspector.
- Cross-pollination with DingusPPC debugger/profiler concepts.

**Out of scope**
- Porting Snow. Replacing SheepShaver architecture. Supporting Snow's target machines.

---

## Work plan

### S0. Baseline capture — ✅ done
- Snow feature set reviewed. 10 debug panels catalogued with SiliconSheep mapping.
- Overlap: debugger ergonomics + traceability tooling.

### S1. Debug-tooling crosswalk — ✅ done (revised 2026-06-06)

| Capability | SheepShaver today | Snow | Gap / upgrade |
|-----------|------------------|------|---------------|
| JIT/CPU stats | `get_stats()` API, SDL title bar, `[HB]` heartbeat | N/A (interpreted) | **Present** SiliconSheep Inspector gauges |
| Register view | None live; `SS_JIT_VERIFY` dumps on divergence | Live egui panel, change highlighting | **Tier 2** add `dump_regs` endpoint |
| Memory view | `lldb` manual attach (disrupts VBL timer!) | Live hex viewer with address nav | **Tier 2** add UDS memory read |
| Disassembly | Offline via capstone scripts | Live at PC | **Tier 3** UDS + capstone WASM |
| Breakpoints | `SS_JIT_WATCH_ADDR` (address watchpoints, log-based) | Full execution/bus/trap/exception BPs | **Already have** watchpoints; execution BPs = Tier 3 |
| Trace history | `SS_JIT_TRACE_RING` (ring buffer, dump on stall) | Live instruction history panel | **Already have** trace; improve presentation |
| Trap history | `[BOOT]`/`[SYSV]`/`[APP]` signals in stderr | System trap history panel | **Tier 1** parse + timeline view |
| Framebuffer | VNC screenshots every ~10s | Raw framebuffer overlay | **Already have** via VNC |
| Perf profiling | `make bench` (offline), `[HB]` rates | N/A | **B1 profiler** is the next data source |
| Guest state | `emul_op.cpp` reads CurApName/WindowList/SysVersion | Peripheral registers | **Tier 2** expose via UDS or periodic dump |

### S2. Bounded first moves — 🟡 specified

**Move 1: JIT Stats dashboard in SiliconSheep Debug tab**
- Parse `[HB ...]` heartbeat lines from stderr (already captured to `last_run.log`)
- Display: block count, cache usage %, compile rate, j2i/i2j ratio
- Success: live-updating gauges visible during a running VM
- Rollback: remove the panel; no emulator changes needed
- Owner: Track C (SiliconSheep)

**Move 2: Signal/event timeline**
- Parse `[BOOT]`, `[SYSV]`, `[APP]`, `[READY]` from stderr
- Display: chronological list with timestamps and payload
- Success: visible boot-progress timeline for a running or recently-run VM
- Rollback: remove the panel
- Owner: Track C

**Move 3: Replace alert() log viewer with scrollable panel**
- The Debug tab's "View Logs" button currently shows an `alert()` dialog
- Replace with an inline scrollable, filterable log viewer
- Success: readable log output within the app
- Rollback: revert to alert()
- Owner: Track C

### S3. D1 tie-in decision — ☐ todo
- If BasiliskII track D1 is reactivated, decide which Snow findings become concrete 68K tasks.
- Snow's hardware-fidelity model is the natural reference for a from-scratch 68K emulator;
  SheepShaver's paravirtualized PPC is the contrast case.

---

## Build/debug presentation — observability inspector, not step-debugger

**Direction: yes — but build an *observability inspector*, not a Snow-style step-debugger, and host
it in SiliconSheep, not the emulator core.**

- **We already have the data, not the presentation.** The heartbeat, trace ring, HOT-PC, watchpoints,
  `jit-analyze.py`, and the SDL title bar stats all exist. The gap is presentation.
- **A full Snow-style debugger is the wrong target.** Our debugging is *differential* (SS_JIT_VERIFY,
  the harness), not single-step. What pays off is **live observability**.
- **Host it in SiliconSheep (Tauri), not the emulator.** Web panel is cheaper than native egui, keeps
  debugger churn out of the emulator core.
- **Profiling-first.** B1 (execution-weighted profiler) produces the data; the Inspector is its
  natural front-end.

**Sequence:** B1 profiler (data) → Tier 1 panels (stats + log + timeline) → Tier 2 panels (registers
+ memory + guest state) → Tier 3 only if earned. No emulator-core debugger work until Tier 2 proves
its value.

## References

- Snow source: `https://github.com/twvd/snow` (MIT)
- Snow debug widgets: `frontend_egui/src/widgets/` (one module per panel)
- Snow architecture: message-passing to emulator thread, cached `EmulatorState` snapshots
- SiliconSheep host-guest channels: `docs/planning/HOST-GUEST-CHANNELS.md`
- SiliconSheep desktop plan: `docs/planning/DESKTOP_INTEGRATION_PLAN.md`
- SheepShaver diagnostics: `SheepShaver/docs/DIAGNOSTICS.md`
