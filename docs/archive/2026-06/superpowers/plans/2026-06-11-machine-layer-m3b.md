> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Machine Layer M3b — Cuda + ADB stub, OpenPIC, external-source wiring (the big rock, second half)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete MACHINE-LAYER-PLAN M3 on the newworld profile. **Wave 1 (the live consumer):** a Cuda protocol model behind the M1 VIA's shift-register surface — SR state machine, the boot-critical command set (ADB autopoll/scan via the **minimal ADB stub** per the documented decision, RTC get/set, PRAM read/write, power/restart acks) — so the 68k boot's TREQ poll (`ORB=18338` against the loud stub, the current frontier) completes the handshake and the boot proceeds. **Wave 2:** OpenPIC (reimplement-vs-spec, QEMU oracle), external-source wiring (VIA/SCC → PIC → `EXC_EXTERNAL` through the M3a delivery hook), `tm_task`/`via_int`/`cuda_init`/`adb_init`/`gc_mask` patch dispositions, SDL_PumpEvents decision, the deliverability harness vector, nested-execute retirement completion. Paravirtual byte-identical throughout.

**Architecture:** Wave 1 needs NO interrupt delivery — SPIKE-S3 §1.5 shows the 68k Cuda transfer is poll-driven (ORB TREQ/TIP/byteack handshake + IFR bit-2 SR-complete polls), so the Cuda model is a pure module (`dev_cuda.cpp`) driven by VIA SR/ORB traffic via a narrow attachment seam on the M1 VIA (replacing the loud stub), with timing one-shots on the M2 scheduler (~88 µs/byte per the donor study §3.2). The ADB stub lives behind its own narrow interface (`adb_stub`) per the documented deferral (M3-PIC-CUDA-DONOR-STUDY §7.2: Talk-R3 responses for keyboard+mouse at default addresses; full host-input-over-ADB deferred). Wave 2's OpenPIC is a pure module registered at `0xF3040000+0x40000` (the donor study's resolved Q6), sources VIA=0x19/ESCCB=0x24/ESCCA=0x25 (QEMU-oracle values, first-IACK logged per Q8), output wired as `EXC_EXTERNAL` into the M3a delivery hook (a second pending source beside the DEC latch, same depth/EE gating).

---

## Authoritative inputs

| Doc | What it fixes |
|---|---|
| `docs/planning/machine/M3-PIC-CUDA-DONOR-STUDY.md` | THE Wave-1/2 spec: Cuda Option-B extract-protocol (§3.5), the command tables (§3.2/§4), SR handshake states (§3.2 end), the ADB decision (§7.2), OpenPIC reimplement verdict + register surface (§5), region size Q6, input numbers Q8 |
| `docs/planning/spikes/SPIKE-S3-STALL-DEVICE-PROBE.md` §1.5 | The 68k-side VIA/Cuda access catalog (ORB bits 3/4 TIP/byteack, ORB bit 3 TREQ query, SR at +0x1400, IFR bit 2 poll) — the conformance vectors for the Cuda unit test |
| `docs/planning/machine/M6A-WAVE2-SHIM-RECON.md` (night-run results) + the boot-F histogram | The live consumer: ORB-poll frontier; the post-Cuda non-device spin is the first acceptance unknown |
| `docs/planning/machine/M3A-ENTRY-TABLE.md` + M3a plan | The delivery hook contract Wave 2 extends; EE/depth gating; telemetry idiom |
| `docs/planning/machine/ROM-PATCH-AUDIT.md` | cuda_init_dat (RETIRE@M3 — retire in Wave 1 WITH the model, audit's loud-stub tension note resolves), adb_init_dat, via_int cluster, tm_task_dat, gc_mask (OldWorld-only) dispositions |
| QEMU `hw/misc/macio/cuda.c` + DingusPPC `viacuda.cpp` @ pinned SHAs (donor study §1) | Behavioral oracles — extract-protocol, do NOT port wholesale (§3.5 Option B rationale) |

## Codebase facts (carried; implementers re-verify sites)

- **The VIA model's Cuda surface today**: SR (+0x1400) stored byte + `cuda_touch` loud-stub latch; ORB bits 3/4 transitions counted. The attachment seam: a `CudaDevice*` (or fn-table) bound to the VIA like `VIABindScheduler` — SR writes/reads and ORB bit-3/4 transitions forward to the Cuda model UNDER THE SAME REGION LOCK (§2g rule 1; the Cuda model is lock-free, the VIA's bus lock covers it — same pattern as the timers).
- **Timing**: Cuda byte-shift one-shots (~88 µs) on `g_event_sched` via the VIA's existing `locked_call` plumbing (gen-guarded, the established idiom). The boot path also has a pure-poll fallback (the 68k loops on IFR bit 2) — model must work even if events lag (lazy completion on SR read, like the M2 timer backstop).
- **PRAM**: 256 bytes, host-file-backed? NO for Wave 1 — in-memory zero-init scratch (prefs persistence is M4's NVRAM scope; document). RTC: derive from host time via the M2 clock (`GetTicks_usec` epoch + Mac 1904 offset — check how the paravirtual XPRAM/TimerDateTime does it and reuse the constant).
- **cuda_init_dat retirement** (audit :2320–2333) lands WITH the model in Wave 1 (the audit's loud-stub tension note anticipated exactly this); **adb_init_dat** retires with it (the wait now gets real TREQ responses).
- **The post-Cuda spin** (boot F: comp=841, no MMIO traffic) is UNDIAGNOSED — Wave 1's acceptance must re-diagnose after Cuda lands (the spin may simply be the boot waiting on the Cuda transfer it kept failing).
- **Wave 2 delivery extension**: the M3a hook consumes ONLY the DEC latch today (documented discrimination). Adding `EXC_EXTERNAL`: a PIC-pending check alongside, same `execute_depth==1` + `ExcDeliverable` gating, IACK protocol (the PIC hands the vector; for the NK direct-entry model the vector # is informational — the NK reads the PIC itself), EOI on the guest's PIC writes. Priority: DEC before EXTERNAL (OEA order).
- **All M2/M3a standing rules** (batch inner loop/legacy authoritative; clean recompile on kpx header changes; grep aarch64 ppc-jit before promoting stubs; no powerpc_registers fields; ask-first boots — night authorization stands; per-task commits; §2g locking).

## Wave 1 tasks

> **✅ Wave 1 COMPLETE (2026-06-11).** Tasks 1–5 all landed: dev_cuda (`143f66e7`, 3924 checks),
> adb_stub (`e7120368`+`bb09269f`, 90 checks), Task-3 integration + retirements
> (`b7a3445b`/`912f27cb`/`94c4a0f0`/`8c7f6796`), acceptance (`d3e60d88` CV-10 deferred SR-int
> delivery — the unlocking fix; + `4e544aff` I2C 0x22/0x25), Wave-1 docs (this commit group).
> The 18338-read sync frontier is CROSSED; boot runs past the Cuda walls but cycles its probe
> sequence — the loop-ender recon (Ticks-starvation hypothesis) gates Wave 2's shape per the
> stop-rule. Full acceptance record: `docs/planning/machine/M6A-WAVE2-SHIM-RECON.md`
> "M3b Wave 1 acceptance". All gates green throughout (machine 11/11, test-jit 353/353
> batch+legacy, e2e-test 122, paravirtual e2e PASS + byte-identical).

### Task 1: `dev_cuda` pure module (standalone-tested)
`src/include/dev_cuda.h` + `src/machine/dev_cuda.cpp` + `test_dev_cuda.cpp`. The SR-handshake state machine, the command dispatcher (boot-critical set per donor §4.3: AUTOPOLL on/off + rate, GET/SET_TIME, READ/WRITE_PRAM + MCU_MEM, FILE_SERVER_FLAG/POWER_MESSAGES acks, RESET/POWERDOWN as loud logs, DEVICE_LIST/bitmap), ADB packets → an **injected handler callback** with the pinned `ADBStubCommand` shape (`CudaBindADB(dev, fn, opaque)`; no compile-time dependency on adb_stub.h — Tasks 1+2 build/test independently, Task 3 binds the real stub). Oracles cited per backport hygiene (behavioral extraction, not code port — cite both SHAs in the header).

**Protocol red-team corrections (rev 2 — BINDING for this task):**
- **(C3) Bit assignments — donor study §3.2 PROSE IS WRONG.** The real ORB bits, ROM-disassembly-verified: **TREQ = bit 3 (Cuda→host input), TACK/BYTEACK = bit 4, TIP = bit 5, all active-LOW.** The ROM contains TWO handshake engines of opposite polarity: Cuda-polarity at 0x8f94/0x9068/0x918a/0x9168 and Egret-polarity at 0x9bfa/0x9c28. The live engine is pinned by the C3 polarity probe (`SS_PROBE_PC='0x50009068;0x50009c28'`) BEFORE this task freezes the state machine — result recorded in the red-team record below.
- **(C1) TREQ derivation on EVERY ORB read.** ORB reads must re-derive bit 3 from Cuda state (idle ⇒ bit 3 = 1, no-treq; pending response ⇒ 0). The ROM does RMW (`bset`/`bclr`/`eori`) on ORB, so a stored-byte echo is wrong — reads always recompute bit 3, host-written bits 4/5 are stored. This alone is what releases the ORB=18338 frontier (CV-1).
- **(C2) Synchronous response at packet commit.** When the host completes a command packet (`ori.b #$30,(a1)` raising TIP+TACK), the ROM checks TREQ ~immediately (0x90f2). Command processing is synchronous at commit (QEMU processes inline); the response must be queued and TREQ asserted before the next ORB read. Lazy settle runs on ORB reads AND IFR reads (the two poll surfaces).
- **(M4) SR access semantics:** SR read or write clears IFR.2; byte capture into SR is edge-triggered on TACK/TIP transitions, not level.
- **(M6) Lock contract, two paths:** calls arriving via the VIA (already under the region lock) raise IFR.2 by direct `v->ifr_latched |= 0x04` — the region lock is NON-recursive, `locked_call` would deadlock. Only pump-thread (scheduler) entry uses `locked_call`.
- **(m10) Timing constants** (donor-verified): 71µs sync-ack, 88µs/byte shift, 61µs inter-byte, 13µs TREQ settle — used only if eager one-shots are armed; the lazy backstop is primary (Task 3).
- **(m11) Note for Wave 2:** OEA priority puts External ABOVE Decrementer.

**Unit tests = the must-pass conformance vectors (verbatim contract):**
- **CV-0 (the live frontier, boot-observed 2026-06-11):** DDRB=0x30 (bits 4/5 output, bit 3 input); host writes ORB 0x38 (idle, all negated) then 0x28 (TACK asserted low, TIP still negated) and polls ORB bit 3. This is the Cuda **sync/attention sequence** — the model must respond per the QEMU `cuda_update()` sync semantics (Cuda asserts TREQ low in answer to TACK-with-TIP-high) so the poll terminates. The exact post-sync choreography comes from the oracle, not this plan.
- **CV-1:** fresh VIA+Cuda, host reads ORB → bit 3 == 1 (idle, no TREQ pending).
- **CV-2:** full GET_TIME choreography — scripted TIP/TACK/SR exchange per the live engine's polarity, multi-byte response `[0x01, 0x00, 0x03, T>>24, T>>16, T>>8, T]` with TREQ asserted across response bytes and negated after the last.
- **CV-3:** ADB scan including **Listen R3 address-move** (device responds at the new address afterward; old address goes silent).
- **CV-4:** autopoll enabled + no input events ⇒ Cuda stays silent (no spurious TREQ).
Plus per-command table tests and the S3 §1.5 IFR-bit-2 poll sequence.

### Task 2: `adb_stub` module (the documented deferral seam)
`src/include/adb_stub.h` + impl + test: Talk R3 for keyboard (addr 2) + mouse (addr 3) default handlers, Talk R0 = empty (no input data — the deferral), the autopoll register plumbing (the donor §7.2 pickup requirements verbatim — the full implementation replaces this behind the same interface). Document the deferred-work pointer in the header.

**Protocol red-team corrections (rev 2 M5 — BINDING):**
- **Listen R3 is NOT ignore-able**: the boot's ADB scan MOVES devices to resolve address conflicts. The stub must implement the R3 address-move (update the device's address from the Listen payload; subsequent Talks answer at the new address). CV-3 tests this.
- **Pinned reply encodings** (QEMU-oracle): Talk R3, device present → `[devaddr, handler_id]` data; Talk to ABSENT address → Cuda packet `[ADB_PACKET, 0x02, cmd]` (timeout status, no data); Talk R0 with no input data → `[ADB_PACKET, 0x00, cmd]` (success, zero data bytes).
- Autopoll with no events = silent (verified against the oracle) — CV-4 covers it from the Cuda side.

**Pinned interface (Tasks 1+2 run in parallel against THIS contract; house style = SCC/VIA plain-struct + extern fns, §2g no-stdio/no-malloc on bus-lock paths):**
```c
#define ADB_STUB_MAX_REPLY 8
struct ADBStub {
	uint8_t kbd_addr, kbd_handler;     // defaults 2 / per-oracle
	uint8_t mouse_addr, mouse_handler; // defaults 3 / per-oracle
	bool    autopoll_on; uint8_t autopoll_rate;
	uint64_t talks, listens, absent_talks, resets;   // telemetry
};
extern void ADBStubReset(ADBStub *a);
// One ADB command (cmd byte = [addr:4][cmd:2][reg:2]) + optional Listen payload.
// Fills reply[] with DATA bytes only (no Cuda framing). Returns:
//   >=0  data length (0 = success, no data — e.g. Talk R0 with no input)
//   -1   no device at address (Cuda frames as timeout status 0x02)
extern int ADBStubCommand(ADBStub *a, uint8_t cmd,
                          const uint8_t *listen_data, int listen_len,
                          uint8_t *reply, int reply_max);
```

### Task 3: VIA↔Cuda attachment + retirement + bring-up
The VIA seam (`VIABindCuda(...)` — SR/ORB forwarding under the region lock, loud-stub
removed), main_unix bring-up (Cuda+ADB instantiation, scheduler binding),
`cuda_init_dat` + `adb_init_dat` retirements (established idiom + audit rows).
**(rev 2 m7 — retirement framing corrected):** on the 9.0.1 parcels ROM BOTH patterns
already MISS their search windows (cuda_init @0x9be2, adb_init @0x2b780 — outside) — the
inits already run unpatched there, which is exactly why the boot reached the TREQ poll.
Retirement is therefore a 1.1/OldWorld-ROM-only behavior change and MUST stay
profile-gated (newworld-only), with the audit rows annotated accordingly; on 9.0.1 the
"retirement" is a no-op by construction.
**(rev 2 m8) RTC convention:** GET_TIME returns Mac epoch seconds in the
`TimeToMacTime` LOCAL-time convention (reuse the paravirtual helper/constant — do not
re-derive UTC). Response encoding per CV-2.
**(rev 2 m9) PRAM:** zero-init in-memory is acceptable, but READ_PRAM must return
WELL-FORMED full-length responses — the ROM caches all 256 bytes at init and malformed
short replies wedge the read loop.
**(rev 2 M4 — the §2g allocation question, decided in writing):** the VIA header's own
deferral rationale ("T1CH/T2CH writes are config-time, not hot fault paths") does NOT
hold for Cuda byte timing (~one scheduler arm per SR byte, first-touch reachable from
the Mach handler thread). Wave 1 therefore relies on the **lazy-completion-on-SR-read
backstop as the PRIMARY mechanism** and arms scheduler one-shots ONLY from non-fault
contexts (the implementer verifies the arm sites; if any arm is fault-path-reachable,
either pre-allocate the event slot or drop the eager arm for that path — the poll-driven
boot protocol works lazy-only). Document the decision at the seam.
**Gates (rev 2 m5, enumerated):** build-ss; batch + legacy test-jit 353/353; machine
suite ALL PASS (now 11 binaries: +test_dev_cuda, +test_adb_stub); test-opcodes inert;
e2e-test 122; paravirtual `make e2e` PASS; inertness greps (no [CUDA] lines on
paravirtual). Checkbox tracking per task during execution.
**(rev 2 m2):** unknown Cuda commands = absent-device-consistent loud-log + per-unknown
counter (house style), surfaced in Task 4; ONE_SECOND_MODE deliberately absent from
Wave 1 (DingusPPC-only; add if the boot demands it — the counter will say).
**(rev 2 m4):** gc_mask is OldWorld-only — no action on 9.0.1; audit note only.
**(rev 2 M5, Wave-2 dependency named):** the via_int cluster is REPLACE@M3, and its
replacement requires interrupts delivered INTO the 68k world (NK→68k forwarding through
the DR emulator — neither the PIC nor the M3a PPC hook; couples to M6's
boot-past-console work). Paravirtual keeps all patches forever (gate idiom).

### Task 4: Wave-1 acceptance (boots authorized) — rev 2 M1: honest gating
PASS/FAIL gates (in the plan's power): (a) the ORB-poll give-up signature is GONE — TREQ
handshake completes, ≥1 full multi-byte transfer observed, per-command counters nonzero
for the ADB scan (+GET_TIME if issued); (b) the un-patched `cuda_init`/`adb_init` inits
execute and reach the model (the audit's observed-traffic DoD rule); (c) paravirtual
byte-identical. **Boot advancement past the post-Cuda spin is a DIAGNOSTIC OUTCOME, not
a gate** — a concrete competing hypothesis exists (rev 2 M1): on newworld the tick
thread never fires (XLM_IRQ_NEST=0xFFFFFFFF) so `Ticks`/0x16a-class time state never
advances; a zero-MMIO spin polling it cannot be fixed by Cuda and lands in Wave 2/M6.
On non-advancement the deliverable is a root-caused frontier: spin-PC probes + capstone
of the loop + an explicit Ticks-polling check, recorded in the entry-table idiom.

### Task 5: Wave-1 docs (rev 2 M2 — the missing process spine)
DIAGNOSTICS.md ([CUDA] counters + any knobs); CHANGELOG (acceptance numbers + BOTH
oracle SHAs); ROM-PATCH-AUDIT rows flipped with evidence (cuda_init/adb_init);
MACHINE-LAYER-PLAN M3b row; **the donor study §7.2 item-4 obligation: track the
deferred full-ADB item in ROADMAP** (documented obligation, was dropped); cross-tracker
grep; the dual-PRAM inconsistency window documented explicitly (rev 2 m3: Cuda
READ/WRITE_PRAM serves in-memory zeros while the nvram* EMUL_OP HLE stays applied until
M4 — two divergent PRAM sources, deliberate).

## Stop-rule (rev 2 M3, per MACHINE-LAYER-PLAN §9)
If Wave-1 acceptance shows the post-Cuda frontier is NOT interrupt-shaped (e.g.
M6-territory 68k handoff), Wave 2's OpenPIC ships as model + unit tests with the wiring
deferred — no tunneling on a delivery path with no live consumer.

## Wave 2 tasks (scoped after Wave-1 acceptance; summary)

**Task W2.0 (rev 2 C1 — REQUIRED recon before any EXC_EXTERNAL wiring):** disassemble
the staged NK handler at 0x50412b1c, map the r7-flag branch tree, locate (or rule out)
the PIC IACK read + EOI write on the KDP-shim entry path, and resolve the `[KDP+0x660]`
flag encoding for "external pending" (the M3A-ENTRY-TABLE finding-4 watch item). DEC and
EXT share one entry + one shim today — how the handler discriminates sources is the
load-bearing unknown. Also (rev 2 C1 latch semantics): the DEC latch is
one-shot-clear-on-delivery; PIC pending is LEVEL-HELD until guest EOI/mask — the hook
must not clear PIC pending DEC-style. (rev 2 m1: OEA priority is External ABOVE
Decrementer — if DEC-first is kept, justify locally: the DEC latch clears on delivery
while level-held PIC pending safely waits one poll.)
OpenPIC pure module (donor §5.2 surface: FRR/GCR, per-source IVPR/IDR, IACK/EOI/TPR; timers+IPI abort-loudly... now absent-device-consistent: loud-log) at 0xF3040000 (carve the macio-stub overlap); `EXC_EXTERNAL` into the delivery hook (PIC-pending beside the DEC latch, DEC priority); VIA IFR→PIC input 0x19 + SCC→0x24/0x25 assertion edges; first-IACK-per-source logging (Q8 tripwire); tm_task_dat + via_int cluster retirement (real delivery replaces host 60Hz injection — THE deliverability harness vector lands here); SDL_PumpEvents decision (recon option (c): keep + decouple — re-evaluate against what the 68k world needs); nested-execute interrupt-path retirement completion; acceptance = a device-sourced external interrupt delivered through PIC→ExcEnter→NK→rfi on the live boot.

**Wave-2 status (2026-06-11): OpenPIC model + unit tests LANDED per the stop-rule
disposition** — `dev_openpic.{h,cpp}` + `test_dev_openpic.cpp` (206 checks, machine suite
12/12 green), a pure module with NO live consumer: the EXC_EXTERNAL wiring (bus registration
at 0xF3040000, VIA/SCC assertion edges, the M3a delivery-hook extension) remains GATED ON
W2.0 — the NK-handler recon must resolve the IACK/EOI discrimination and the level-held
latch semantics before anything binds `OpenPICBindOutput`. Surface per donor §5.2
(FRR/GCR/SPVE, per-source IVPR/IDR, IACK/EOI/CTPR, raise/lower inputs, edge/level sense);
timers+IPI+multi-CPU are absent-device-consistent loud-latch (rev: not abort — and
higher-fidelity than QEMU's mapped-but-dead tmr bank, KEYLARGO_MAX_TMR=0). Q8 first-IACK
tripwire recorded per source (`OpenPICFormatFirstIACKs`). Oracle: QEMU openpic.c/openpic.h
@ de5d8bfd6105d3dd3ae668df9762df244a6d1506 (reimplementation, not a port); KeyLargo is
mapped little-endian in QEMU — the byte-lane decision is wiring-task scope, documented in
the header. Mutation probes run: 5/5 killed after two added vectors (raise-time CTPR
boundary, edge-pending consumed on IACK).

## Self-review record
Wave-1-before-OpenPIC is sound: the boot's Cuda path is poll-driven (S3 §1.5 — IFR bit 2 + ORB handshake; no interrupt required); the PIC has no live consumer until the 68k world runs further. ADB scope honors the documented decision verbatim. PRAM-in-memory vs M4 NVRAM boundary stated. The post-Cuda spin is an honest unknown gating Wave 2's final shape.

## Red-team record

**Round 1 (pre-implementation, two reviewers, 2026-06-11):**

*Process reviewer* — folded as rev 2 markers M1–M5/m1–m5 in-line: honest acceptance
gating (post-Cuda spin = diagnostic outcome; Ticks-starvation competing hypothesis
named), the missing docs task (Task 5), the stop-rule, Task W2.0 recon gate before any
EXC_EXTERNAL wiring, the §2g fault-path allocation decision in Task 3, enumerated gates.

*Protocol reviewer* (vs QEMU cuda.c + DingusPPC viacuda.cpp + ROM disassembly) — folded
as the BINDING blocks in Tasks 1–3:
- **C1** ORB reads re-derive TREQ (bit 3) from Cuda state every read (ROM does RMW on ORB).
- **C2** synchronous command processing at packet commit; lazy settle on ORB AND IFR reads.
- **C3** donor study §3.2 prose bit table is WRONG — TREQ=3, TACK=4, TIP=5, active-LOW;
  two opposite-polarity ROM engines (Cuda 0x8f94/0x9068 vs Egret 0x9bfa/0x9c28); live
  engine pinned by probe before Task 1 (result below).
- **M4** SR access clears IFR.2; edge-triggered SR capture. **M5** Listen-R3 address-move
  + pinned reply encodings. **M6** non-recursive region lock → direct IFR raise on the
  VIA path. **m7** cuda_init/adb_init already skip on 9.0.1 (outside windows). **m8** RTC
  local-time convention. **m9** well-formed full-length PRAM replies. **m10** timing
  constants. **m11** OEA External-above-DEC (Wave 2). Conformance vectors CV-1..CV-4
  adopted verbatim as Task 1's test contract.

**C3 polarity-probe result (2026-06-11, empirical):** `SS_PROBE_PC` on the engine PCs was
blind as feared (68k code under the DR emulator — zero PPC block-entry hits), so the pin
came from new VIA ORB write-value telemetry (`VIAFormatOrbTrace`, +`SS_TERM_DUMP=1` so
timeout-killed boots run the atexit dumps). 45s diagnostic boot:
`[VIA] orb: ddrb=30 writes=3 trace=38,28` with reads ORB=18338 SR=1.
**DDRB=0x30 ⇒ bits 4/5 outputs, bit 3 INPUT — the Cuda-polarity engine is live**
(TREQ=3 input / TACK=4 / TIP=5, active-LOW, exactly the C3-corrected assignments).
The 18338-read loop is the sync sequence: host parked at ORB=0x28 (TACK asserted, TIP
negated) polling for TREQ-low. Adopted as CV-0.
