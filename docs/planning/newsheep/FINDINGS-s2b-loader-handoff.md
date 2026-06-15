# SS_M18 Stage 2b — Trampoline loader + OF-CI handoff + /mmu: Task-0 RECON findings

**Status:** COMPLETE (2026-06-15) — **RESIDUE-PASS** (the *expected* case). Q-S2b.1 and Q-S2b.2
close affirmatively (no blocking residue). Q-S2b.3 closes with the `/mmu`-stub NON-ACCEPTANCE
residue (owed to S1 — expected, does NOT block the loader/launch build) **plus** the newly-pinned
guest-callable r5 marshalling-shim contract. Q-S2b.4 **caps at RESIDUE-PASS pre-S1** by plan rule
(amendment 3): the handoff address/mechanism is pinned statically, but the `of_ci_unresolved_count()==0`
sampling and the dereference of real `/mmu` results are owed to the post-S1 boot.
**Authority:** `docs/superpowers/plans/2026-06-15-ss-m18-s2b-loader-handoff-task0.md` (rev-1, 5 amendments folded).
**Method:** static source/doc reads + grep + md5. **ZERO boots, ZERO full-parcel disasm** (the
NanoKernelEntry address is derived from the already-RE'd Configfile-1 component table; no new disasm needed).
**Inputs cross-walked:** `FINDINGS-trampoline-re.md` (ELF/CHRP/handoff facts + OF inventory + Configfile-1
map) × `FINDINGS-s2a-ofci-dt.md` (committed inert artifact + residues-to-S2b) × `FINDINGS-s3-two-supervisor.md`
§5 + §6 + ★BINDING-AMENDMENT (handoff target + file-ownership collision + EXT-sever) × live
`rom_decode.hpp` / `rom_patches.cpp` / `sheepshaver_glue.cpp` / `openfirmware_ci.{cpp,h}`.

**Evidence-tag legend:** `[SRC]` = verified against live source this run (file:line); `[RE]` = the
static/dynamic Trampoline RE in `FINDINGS-trampoline-re.md`; `[S2a]` = `FINDINGS-s2a-ofci-dt.md`;
`[S3]` = `FINDINGS-s3-two-supervisor.md`.

---

## Gate-item-0 — parcel provenance (re-verified this run)

| Artifact | Expected md5 | Verified md5 | Source |
|---|---|---|---|
| Canonical 9.0.1 ROM | `66210b4f71df8a580eb175f52b9d0f88` | `66210b4f71df8a580eb175f52b9d0f88` ✓ | `/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom` |
| NanoKernel-v02.27 parcel (105280 B) | `61c176e90b6365e84e5c660d703e56af` | `61c176e90b6365e84e5c660d703e56af` ✓ | `/tmp/newsheep/dump-9.0.1/Parcels.src/MacROM.src/NanoKernel-v02.27` |
| `MacOS.elf` (Trampoline, 94144 B) | — (identity recorded) | **`1300a95e1a243c582c6c7d619075075a`** ✓ present | `/tmp/newsheep/dump-9.0.1/MacOS.elf` |

ROM + NK parcel md5s match `[S2a]`. The `MacOS.elf` source artifact is **recorded by identity** (the plan
gives no expected md5 for it, only for the ROM/NK parcel of the same `66210b4f…` parcel set): 94144 B,
md5 `1300a95e1a243c582c6c7d619075075a`, derived from the `66210b4f…` ROM dump. **NOTE for S2b-impl:** this
md5 becomes the staged-asset manifest pin (Q-S2b.1 default). **The active project boot ROM is the 1.1 ROM
`e0fc03faa589ee066c411b4603e0ac89`, NOT this 9.0.1 — the wrong-bytes risk that drives the asset-staging default.**

**Committed inert OF-CI artifact re-verified `[SRC]`:** `SheepShaver/src/machine/openfirmware_ci.cpp`
(24811 B) + `SheepShaver/src/include/openfirmware_ci.h` (5586 B) exist (commits `14c9384e` + `6c892840`);
compiled ONLY by `machine/Makefile:87-88` (`test_openfirmware_ci` target, `TESTS` line 6) — **NOT linked
into the SheepShaver binary** (confirmed: no reference to `of_ci_callback` / `openfirmware_ci` outside
`machine/`). S2b folds this as the artifact-to-WIRE.

---

## Q-S2b.1 — loader source + placement — **GREEN (asset-staging default; in-tree extraction demoted)**

**WINDOW:** `rom_decode.hpp` parcels-decode path `[SRC]` + the `[RE]` ELF facts + in-tree ROM identity.

**Source decision — STAGE the md5-verified 9.0.1 `MacOS.elf` as an asset (plan amendment 1, CONFIRMED `[SRC]`).**
`decode_parcels()` (`rom_decode.hpp:86-100`) walks the `prcl` chain but **processes ONLY parcels of type
`FOURCC('r','o','m',' ')`** (`:93`), LZSS-decompressing each into a single flat `dest` (`= ROMBaseHost`,
via `decode_rom_image:168`). **Re-verified line-by-line this run:** the loop tests `parcel_type ==
FOURCC('r','o','m',' ')` and silently skips every other type; there is no return of a per-parcel extent.
`MacOS.elf` is the **top-level Trampoline ELF — a sibling of the `Parcels` container** in the tbxi dump
(`/tmp/newsheep/dump-9.0.1/` directory listing confirms `Bootscript` / `MacOS.elf` / `Parcels` as
top-level siblings), **never a `'rom '` parcel** — so the existing path NEVER writes it into `ROMBaseHost`
and surfaces no locator. **"Asset-free extract via decode_parcels" is NEW extraction code resting on a
nonexistent path** — demoted to a later optimization gated on a parcel-identity guard. Wrong-bytes risk
reinforces: extracting from the *active* 1.1 ROM (`e0fc03faa…`) would yield an un-characterized Trampoline.

→ **S2b-impl default: a staged-asset path.** Add a manifest entry for `MacOS.elf` (94144 B, md5
`1300a95e…`), loaded under the gate; in-tree ROM extraction is a future optimization behind a
parcel-identity check (md5/`AAPL`-type guard) that confirms the loaded ROM is the RE'd 9.0.1.

**Placement — HONORABLE inside the guest aperture `[RE]`.** `MacOS.elf` = `ET_EXEC`, 2 `PT_LOAD`:
**data `0x100000`** (filesz `0x6bc0`, memsz `0x19920`, rw) + **exec `0x200000`** (filesz `0x10260`, r-x).
Total top = `0x200000 + 0x10260 = 0x210260`, comfortably inside SheepShaver's guest RAM aperture (RAM base 0,
multi-hundred-MB NewWorld profile). The loader copies each `PT_LOAD`'s filesz from the asset to guest
`Mac2HostAddr(p_vaddr)` and zero-fills `memsz - filesz` (the data BSS tail `0x19920 - 0x6bc0`). **HARD STOP
honored:** segments are placed at their ELF vaddrs verbatim — NOT relocated to "make it fit" (`ET_EXEC` at
fixed vaddrs; OpenBIOS honors them, `[RE]` T0.2). **PIC self-reloc stub:** entry `0x20f078` is a
`mflr/bl .+8` self-relocation stub at the exec-segment tail `[RE]`; it runs from the placed bytes with no
loader fixup beyond correct vaddr placement (it computes its own runtime base).

**Q-S2b.1 verdict:** **GREEN.** Source = staged asset (default); placement honorable at ELF vaddrs; PIC
stub self-relocates. No blocking residue. Residue (non-blocking, → optimization): in-tree extraction needs
new code + a parcel-identity guard. **LOADER-INFEASIBILITY-VERDICT NOT emitted** (positive evidence: the
ELF is placeable and the asset exists).

---

## Q-S2b.2 — CHRP entry ABI + launch seam — **GREEN on r5/r2/entry; r3/r4 = carried S2a residue (first-boot owner)**

**WINDOW:** the `[r2-0x60]`/`r5`/`0x20f078`/`r2=0x1001e8` block `[RE]` + the glue boot path `[SRC]`.

**ABI (carried from `[RE]` / `[S2a]`, re-verified against the findings):**
- **`r5` = OF-CI callback pointer — CONFIRMED.** Handed at launch; the Trampoline stashes it at `[r2-0x60]`
  (`r2 = 0x1001e8`, small-data base) and calls it ONLY through the 3 wrappers (`0x20dbec` / `0x20dcc0` /
  `0x20ddb4`) via indirect glue `0x21024c`. **The r5 value is the GUEST address of the marshalling shim
  (Q-S2b.3), not the host `of_ci_callback` pointer.**
- **`r2 = 0x1001e8` after entry; entry = `0x20f078`** (PIC self-reloc stub). Glue indirect = `0x21024c`.
- **`r3` / `r4` — NOT pinned in the RE inventory `[S2a]` Q-S2a.2.** Carried as the named S2a residue,
  resolved HERE only as a disposition: **confirm at the first gated boot; inject a documented default
  (`r3`/`r4 = 0`) ONLY as a logged provisional, NEVER a hidden ABI.** Not guessed into a contract. CHRP
  convention makes `r3`=OF-CI callback / `r4`=0 plausible on some launchers but the RE did not establish it,
  so it stays a first-boot confirmation item.

**Launch seam — concrete site `[SRC]`.** The newworld boot machinery lives in
`SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp` inside the `MachineProfileIsNewWorld()` init block
(`:2747`+, the same block that today stages `[KDP+0x1074]=0x50480000` / `[KDP+0x1078]=0x50460000` at
`:3140-3141` and the sub-KDP map at `:2762`). **This block is the natural home for the gated CHRP launch
seam:** under `SS_M18_TRAMPOLINE ∧ MachineProfileIsNewWorld()`, load `MacOS.elf` (Q-S2b.1), set
`r5`=shim-guest-addr / `r2`=`0x1001e8` / `r3`/`r4`=provisional, and enter PPC execution at `0x20f078`
instead of running the `PatchROM_NW_trampoline` forge path. Env-gate precedents to reuse (boot-latched,
NOT per-access): `MachineProfileIsNewWorld()` sites at `glue:1082/1554/1614/1645/1984/2747` `[SRC]` + the
S3-T1 `NkSupervisorEnabled()` boot-latch pattern (`ppc-cpu.cpp`).

**Q-S2b.2 verdict:** **GREEN** on r5/r2/entry/seam-location. Residue (non-blocking for loader placement;
blocks only the launch *claim*): **"r3/r4-UNKNOWN → first-gated-boot owner; documented provisional default,
never a hidden ABI."**

---

## Q-S2b.3 — wire `of_ci_callback` + the guest-callable r5 marshalling shim + backend ownership — **RESIDUE (shim contract pinned; /mmu stub = NON-ACCEPTANCE, → S1)**

**WINDOW:** `openfirmware_ci.h` callback signature `[SRC]` + the program backend map.

### The marshalling shim (plan amendment 2 — pinned HERE as load-bearing S2b code + hidden G2b.a prerequisite)

**`of_ci_callback` is NOT directly r5-wireable — CONFIRMED `[SRC]`.** Signature
(`openfirmware_ci.h:101`): `int of_ci_callback(of_ci_context *ctx, of_cell *array)` — **two** args.
`of_cell = uint64_t` **HOST width** (`:44`). The impl reads `array[0]` as a **host** `const char*`
(`openfirmware_ci.cpp:463` `cell_str` = `(const char*)(uintptr_t)c`; `:491` `service = cell_str(array[0])`),
`array[1/2]` = n_args/n_rets, `args=&array[3]`, `rets=&array[3+n_args]` (`:492-495`). So `of_ci_callback`
expects a **host-side** array of 64-bit cells with **host** pointers.

The real CHRP `r5` callback the Trampoline invokes is a **single-arg, guest-callable PPC entry** receiving
**one guest pointer (in r3 at the PPC call) to a big-endian 32-bit cell array**, where string/buffer cells
are **guest** addresses. The two are width-, endianness-, arity-, and address-space-incompatible.

**Pinned I/O contract for the S2b marshalling shim (`MacOS.elf` → host `of_ci_callback`):**
1. **Entry shape:** a single-arg PPC-callable trampoline at a **guest address** placed under the gate
   (in a guest scratch/code region the loader owns; its guest address is the value handed in `r5`). On
   invocation the guest passes the CHRP array pointer in `r3` (PPC ABI arg0).
2. **`ctx` binding:** the shim binds the host `of_ci_context*` (from `of_ci_create_core99()`, created once
   at gated boot) — NOT passable through the single guest arg, so it is captured host-side (static/closure)
   keyed by the single live context.
3. **Inbound marshal (guest→host):** read the guest BE-32 cell array via `Mac2HostAddr` + `ntohl` per cell;
   build a host `of_cell[]` (≤ `OF_CI_MAX_CELLS`=32, `openfirmware_ci.h:58`): `array[0]` = a **host**
   `const char*` obtained by translating the guest service-name string pointer (`Mac2HostAddr(guest_ptr)`,
   then used directly as `cell_str` expects a host ptr); `array[1/2]` = n_args/n_rets (plain integers);
   each input arg cell that is a **pointer** (property name, buffer, path string) translated guest→host;
   plain scalar args copied as-is (zero-extended 32→64).
4. **Outbound marshal (host→guest):** after `of_ci_callback` writes `rets[]` host-side, write each return
   cell back to the guest BE-32 array via `htonl` + `Mac2HostAddr`; any returned **pointer/handle** that is
   a guest-visible address translated host→guest (phandles/ihandles/lengths are scalars — copy truncated
   32-bit). Return the callback's int result as the PPC return (r3).
5. **Pointer-direction discipline:** every cell that is semantically a pointer (service name, prop name,
   path, data buffer for `getprop`/`read`/`write`) crosses the boundary with explicit translation; the
   `call-method` backends (below) receive **host**-side decoded args from `of_ci_callback`'s
   `of_call_method_fn` (`openfirmware_ci.h:74`) — the shim does not re-translate those (the backend owner does).

**This shim is a hidden G2b.a prerequisite:** without it, `of_ci_callback` cannot be the r5 target.
Blocking-table entry "Guest-callable r5 marshalling shim" is REAL and load-bearing S2b code.

### call-method backend ownership map (plan backend map, pinned)

| call-method (via `of_call_method_fn` injected backend) | Owner | Disposition at S2b |
|---|---|---|
| `/mmu` `claim` / `translate` / `map` (+ direct `claim`) | **S1/S3** | **Recording stub pre-S1** (logs requests; build/unit ONLY) — **NON-ACCEPTANCE (Stop-rule #7).** Real backend owed to S1's live paged MMU. |
| disk `read-blocks` / `write-blocks` / `block-size` | **S4** | stub/deferred (S2b reaches the NK before disk IM-init matters; S4 owns). |
| DT queries (getprop/getproplen/finddevice/nextprop/parent/peer/child/canon/…) | **S2b (now)** | resolve now via `of_ci_create_core99()` DT (committed `[S2a]`). |
| display (`dimensions`/`set-colors`/`fill-rectangle`/`draw-rectangle`) | **S2b (now)** | no-op resolve. |
| `instantiate-rtas` | **S2b (now)** | minimal RTAS stub node/handle. |
| `interpret` `key?` / `key` / `reset-all` | **S2b (now)** | 3-word shim resolves. |

The S2a residues (interrupt-map/-mask tuple shape `[S2a]` Q-S2a.3 + §5-Q8 PIC input numbers) are answered
by the DT **returning the provisional** (query resolves, `getproplen` NON-FINAL); their VALUE confirmation
is owed to the first integration boot — NOT closed here, NOT fabricated.

**Q-S2b.3 verdict:** **RESIDUE-PASS (expected).** The wiring contract is pinned (link `openfirmware_ci`
into the binary behind the gate + the marshalling-shim I/O contract above + the backend ownership map). The
`/mmu` recording stub is **NON-ACCEPTANCE** — owed to S1 — and does NOT block S2b's loader/launch build but
DOES block S2b PASS. Residue: **"`/mmu` backend-ownership → S1 (recording stub = NON-ACCEPTANCE); disk → S4."**

---

## Q-S2b.4 — the handoff — **RESIDUE-PASS (capped pre-S1 by plan amendment 3); address pinned statically**

**WINDOW:** the Trampoline tail (post-`/mmu`-build → `NanoKernelEntry` jump) + the NK entry + the S3
hand-off contract. **No full-parcel disasm** (Stop-rule #5 honored).

**What `NanoKernelEntry` is — PINNED statically from the Configfile-1 component table (`[RE]` Q0-D), no new disasm.**
The Trampoline computes `NanoKernelEntry` from the ROM component table it reads via `/rom/macos`
`AAPL,toolbox-parcels` / the Configfile-1 structural map. `[RE]` Q0-D pins Configfile-1:
**`NanoKernel-v02.27` @ BASE+0x310000 (KernelCode, 0x10000 B)** → guest **`0x50310000`** (ROMBase
`0x50000000` + `0x310000`). `[RE]` Q1 (SS_M18 gating Task-0) independently pins the NK run base at
**`0x50310000`** with the entry sequence `mfmsr` → test MSR[DR] → `mtspr SRR0/SRR1` → **`rfi`@`0x5031003c`**.
So **`NanoKernelEntry = ROMBase + 0x310000 = 0x50310000`**, derived by the Trampoline from the ROM
component table (NOT a guessed address — Stop-rule #3 honored). The cross-check (`[RE]` gating Task-0):
parcel offset `0x4880` == the `0x50314880` EXT dispatcher confirms the `0x50310000` base.

**Falsifiable predicate (bound to an instrument, plan amendment 3):** "reaches `NanoKernelEntry` with 0
unresolved OF calls" = **`of_ci_unresolved_count(ctx)` (`openfirmware_ci.h:105`) reads `0` sampled at the
handoff PC** (the Trampoline's branch to `0x50310000`), NOT asserted narratively. The shim/loader
instruments the sample at the gated boot.

**Cap at RESIDUE-PASS pre-S1 (plan amendment 3 — BINDING).** Q-S2b.4 inherits S1 through the `/mmu` stub:
the Trampoline does REAL `claim`/`translate`/`map` and **dereferences the results** (`[RE]` Q2: translation
is *consumed*, not merely installed — per-context SR reload + MMIO segment-swap). A recording stub
returning identity/plausible claims **MAY** let the Trampoline progress to the `0x50310000` jump, **OR** it
**MAY** fault on a `translate` before the handoff — **unknowable statically.** GREEN-closure (the predicate
sampled `==0` at the handoff PC) **waits on the post-S1 real-`/mmu` boot.**

**The post-handoff NK fault is EXPECTED pre-S1/S3 — NOT a Q-S2b.4 falsification (plan amendment 4 + `[S3]` ★BINDING-AMENDMENT).**
Per `[S3]`: REPLACE's RETIRE list severs the device-IRQ→NK-EXT path (`deliver_pending_dec_exception()` +
`g_exc_entry_table` killed; the EXT-injection shim is S3-T2), and the NK is a permanently-resident paged
supervisor needing the live MMU SS lacks pre-S1 (`[RE]` Q1/Q2). So **"reached `NanoKernelEntry`" is
immediately followed by an NK fault by construction** — record it as EXPECTED; do NOT read the first gated
boot's NK fault as a regression (the same honest-limit move `[S3]` §5 made).

**Probe-boot exposure named (Stop-rule #6 honored — behavioral oracle only).** The QEMU rig stalls in
OF→OS handoff **PRE-NK-install** (`FINDINGS-discriminator-a.md` Evidence B / `[S3]` §5: hw bps at
`0x50310604`/`0x503104b4`/`0x50310000` never fired; SDR1 stayed at OpenBIOS's `0x1f80003f`). **S2b IS the
in-tree path that gets past that stall to the NK** — positive corroboration is owed to S2b-impl's own gated
boot, NOT gettable from the rig. **No QEMU boots were spent here** (the address is statically pinned from
the Configfile component table; a boot would only re-observe the known OF-handoff stall, buying no signal —
the same reasoning Discriminator-A and `[S3]` §5 used to NOT spend boots).

**Seam to S3.** S2b's success predicate = "Trampoline reaches `NanoKernelEntry` (`0x50310000`) with
`of_ci_unresolved_count()==0`"; S3-impl's start predicate = "the NK install runs under
`SS_M18_NK_SUPERVISOR`." The handoff IS the milestone seam; S2b reaches, S3 runs (+ the EXT-injection shim
`[S3]` ★).

**Q-S2b.4 verdict:** **RESIDUE-PASS (capped pre-S1).** Address + mechanism + predicate-instrument pinned;
the `==0`-at-handoff-PC sampling and the dereference-of-real-`/mmu`-results are owed to the post-S1 boot.
Residue: **"handoff sampled-predicate UNKNOWN pre-S1 → owed to post-S1 gated boot; NK fault post-handoff is
EXPECTED."** NOT "handoff-UNKNOWN" — the address is pinned; only the live-sampling is S1-gated.

---

## PASS-taxonomy verdict — **RESIDUE-PASS** (Q-S2b.4 caps at RESIDUE-PASS pre-S1 by rule)

| Question | Closure | Blocks S2b-impl? |
|---|---|---|
| Q-S2b.1 (loader source + placement) | **GREEN** — staged-asset default (`decode_parcels` confirmed does NOT surface `MacOS.elf`); ELF-vaddr placement honorable; PIC stub self-relocates | **No** |
| Q-S2b.2 (CHRP entry ABI + launch seam) | **GREEN** on r5/r2/entry + seam at `glue` `MachineProfileIsNewWorld()` block (`:2747`+); r3/r4 = carried first-boot residue (provisional, never hidden ABI) | **No** (blocks only the launch *claim* about r3/r4) |
| Q-S2b.3 (OF-CI wiring + marshalling shim + backend map) | **RESIDUE** — marshalling-shim I/O contract pinned (guest-callable, ctx-bound, BE-32↔host-cell, pointer-translated); `/mmu` recording stub = NON-ACCEPTANCE (→ S1); disk → S4; rest resolve now | **No** for loader/launch build; **YES** for S2b PASS (the `/mmu` stub) |
| Q-S2b.4 (handoff) | **RESIDUE-PASS (capped pre-S1)** — `NanoKernelEntry=0x50310000` pinned from Configfile-1 (no disasm); predicate = `of_ci_unresolved_count()==0` at handoff PC; live sampling owed to post-S1 boot; post-handoff NK fault EXPECTED | live sampling blocks S2b PASS (→ S1); address pinned does not block impl START |
| Gate-item-0 provenance | ROM + NK parcel md5s ✓; `MacOS.elf` identity recorded (94144 B, `1300a95e…`) | — |

**Overall: RESIDUE-PASS** — the expected case. **No BLOCKING residue lands on Q-S2b.1 or Q-S2b.2** (the two
that would block S2b-impl START), so **S2b-impl's loader + launch + OF-CI-wiring BUILD/UNIT work is cleared
to start** (serialized behind S3-impl file-ownership per below; coordinated with S1 for `/mmu` acceptance).
**S2b PASS (full)** remains gated on the post-S1 real-`/mmu` handoff (Stop-rule #7) and the
post-S1 gated-boot predicate sample (Q-S2b.4). **LOADER-INFEASIBILITY-VERDICT NOT emitted** (positive
placement evidence).

---

## What blocks S2b-impl start (the actionable summary)

1. **NOT blocked on Task-0 answers** — Q-S2b.1/.2 are GREEN; the loader/launch/wiring build can begin.
2. **Blocked on file-ownership serialization with S3-impl (LAW, `[S3]` §"File ownership").** Both
   `rom_patches.cpp` (`PatchROM_NW_trampoline:729` `[SRC]`) and `sheepshaver_glue.cpp` (the
   `MachineProfileIsNewWorld()` block + Execute68k pair `:3140-3141` `[SRC]`) are **also S3-impl's RETIRE
   surface.** Rule: **S2b lands FIRST** (gates the forge behind `SS_M18_TRAMPOLINE`); S3 later retires it
   behind `SS_M18_NK_SUPERVISOR`, rebasing on S2b's gate. **S2b and S3-impl MUST NOT be in flight on these
   two files concurrently** (Stop-rule #7 file-collision).
3. **S2b PASS (not START) gated on S1** — the `/mmu` real backend (Q-S2b.3 NON-ACCEPTANCE stub) and the
   Q-S2b.4 live handoff-predicate sample both require S1's live paged MMU.

## Headline residues handed forward

1. **Q-S2b.2 → first gated boot:** `r3`/`r4` at CHRP entry (inject documented provisional, confirm at boot,
   never a hidden ABI).
2. **Q-S2b.3 → S1:** `/mmu` `claim`/`translate`/`map` real backend (recording stub = NON-ACCEPTANCE). **→ S4:**
   disk `read-blocks`/`write-blocks`/`block-size`.
3. **Q-S2b.3 marshalling shim → S2b-impl:** the guest-callable r5 trampoline is load-bearing NEW code (the
   I/O contract is pinned above; it is a hidden G2b.a prerequisite).
4. **Q-S2b.4 → post-S1 gated boot:** sample `of_ci_unresolved_count()==0` at the `0x50310000` handoff PC;
   the post-handoff NK fault is EXPECTED (severed EXT path + no live MMU pre-S1/S3).
5. **Carried from `[S2a]`:** interrupt-map/-mask tuple cell-shape + §5-Q8 PIC input numbers (DT returns the
   NON-FINAL provisional; VALUE owed to first integration boot).

---

## DIAGNOSTIC (recorded, NEVER a gate)

- **OF calls resolved-now vs stubbed/deferred:** of the bounded surface (21 direct + 14 call-method + 3
  interpret, `[RE]`/`[S2a]`), S2b resolves NOW: all 21 direct DT-query services + display no-op + RTAS stub
  + 3 interpret literals. Deferred: `/mmu` claim/translate/map → S1 (stub); disk read/write/block-size → S4.
- **Which backends S2b stubs vs S1/S4 own:** S2b stubs `/mmu` (recording, NON-ACCEPTANCE) + disk (deferred);
  resolves DT/display/RTAS/interpret. S1 owns real `/mmu`; S4 owns real disk.
- **How far a gated probe boot gets:** unknown pre-impl; the QEMU rig stalls in OF→OS handoff
  pre-NK-install (`FINDINGS-discriminator-a.md` Evidence B) — S2b is the in-tree path past it; corroboration
  owed to S2b-impl's own boot.
- **Active-ROM `MacOS.elf` compatibility:** the active project ROM is the 1.1 ROM
  `e0fc03faa589ee066c411b4603e0ac89`; whether it carries a `MacOS.elf` parcel compatible with the RE'd 9.0.1
  one is UNTESTED — the reason the asset-staging default (Q-S2b.1) sidesteps the question. In-tree extraction
  would need a parcel-identity guard before trusting the loaded ROM's bytes.
- **`nw-northstar`:** reaching the NK (`0x50310000`) is the S2b win, NOT Finder.

---

## Scope-guard
Docs-only — **no `SheepShaver/src/**` written**, no builds, no boots, no full-parcel disasm. All line cites
re-verified against live source this run (`rom_decode.hpp:86-100/168`, `openfirmware_ci.{h:44/74/101/105,
cpp:463/487-495}`, `rom_patches.cpp:46/729`, `sheepshaver_glue.cpp:1082/2747/3140-3141`). QEMU boots spent: 0.
Falsifications this Task-0: 0 (re-pins: 0). Route A + program shape + S2a/S2b split NOT relitigated.
