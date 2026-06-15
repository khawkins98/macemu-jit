# SS_M18 Stage 2b — Trampoline loader + CHRP launch + OF-CI wiring: S2b-IMPL — IMPLEMENTATION milestone

> **Status:** rev-1 DRAFT (2026-06-15) — pending red-team. **This is the S2b-impl build plan.** It executes
> the BINDING contracts from `FINDINGS-s2b-loader-handoff.md` (RESIDUE-PASS — the *expected* case): stage the
> md5-verified 9.0.1 `MacOS.elf`, place its 2 PT_LOAD at ELF vaddrs under the gate, build the guest-callable
> r5 marshalling shim, wire the committed-inert `of_ci_callback`, and launch the real Trampoline from the
> glue seam — all behind `SS_M18_TRAMPOLINE ∧ MachineProfileIsNewWorld()` (default OFF), paravirtual
> PROVABLY byte-identical.
>
> **★★ MILESTONE RE-SCOPE (rev-1 headline — inherited from the Task-0 RESIDUE-PASS):** this milestone
> delivers **BUILD-RESIDUE** — T1–T5 land gated-OFF (paravirtual byte-identical, branch-revert-safe), the
> loader/launch/wiring seam structurally in place, validated by `make test-jit`=100 + real `make e2e` A/B at
> every task. **GREEN-PASS (the gated newworld boot reaching `NanoKernelEntry` `0x50310000` with
> `of_ci_unresolved_count()==0`) is NOT reachable this milestone**: it inherits S1 through the `/mmu`
> recording stub (the Trampoline does REAL `claim`/`translate`/`map` and *dereferences* the results — a
> recording stub MAY let it progress OR MAY fault on a `translate` before the handoff; unknowable
> statically). The live handoff-predicate sample is owed to the **post-S1 real-`/mmu` gated boot**. The
> post-handoff NK fault is **EXPECTED** (severed EXT path + no live MMU pre-S1/S3 — `FINDINGS-s3` §★) and is
> NOT a falsification.
>
> **★ The contracts are PINNED, not relitigated.** Per `FINDINGS-s2b-loader-handoff.md`: source = staged
> 9.0.1 asset (Q-S2b.1 GREEN; `decode_parcels` does NOT surface `MacOS.elf`); placement at ELF vaddrs
> honorable + PIC stub self-relocates; r5 = GUEST address of the marshalling shim (NOT the host
> `of_ci_callback` pointer); r2=`0x1001e8`, entry `0x20f078`; r3/r4 = carried first-boot provisional;
> `NanoKernelEntry=0x50310000` pinned from Configfile-1 (no disasm). This plan sequences the build; it does
> NOT re-pick the source or re-pin the addresses.
>
> **Scope law (carried, NOT relitigated):** Route A SETTLED; program shape (S1→S3→S4 critical path; S2a
> boot-disjoint DONE; **S2b serialized — lands FIRST on the shared forge/glue surface, ahead of S3-impl
> T3/T4**) SETTLED; S2a/S2b split SETTLED; CHRP entry ABI (r5/r2/entry) SETTLED. Asset-staging-vs-in-tree
> extraction is SETTLED to asset-staging (Task-0 rev-1 amendment 1) and NOT reopened.
>
> **★ S2b-impl START preconditions (BINDING — SPLIT by outcome):**
> - **The BUILD-RESIDUE (T1–T5, gated-OFF, THIS milestone)** needs only: Task-0 RESIDUE-PASS (DONE — Q-S2b.1/.2
>   GREEN, no blocking residue) + the committed-inert `openfirmware_ci.{cpp,h}` (S2a-impl `14c9384e`+`6c892840`,
>   DONE) + the staged 9.0.1 `MacOS.elf` asset (md5 `1300a95e…`) + **S3-impl T1+T2 already committed (the
>   `SS_M18_NK_SUPERVISOR` master gate + EXT-injection shim) — S2b REBASES under them** (they do not touch the
>   forge body S2b gates). T1–T5 write the seam inert at default-OFF, validated without the live NK.
> - **GREEN-PASS** ADDITIONALLY needs **S1 landed (the live paged `/mmu` backend the Trampoline dereferences)**
>   + the post-S1 gated boot that samples `of_ci_unresolved_count()==0` at the `0x50310000` handoff PC. ★ This
>   precondition is currently UNMET — S1 is unbuilt. So GREEN-PASS is DEFERRED behind S1; the live-boot
>   acceptance + the `/mmu` real backend wait on S1. **S1 is the parallel critical-path track.**

## Goal (PASS-vs-DIAGNOSTIC)

**PASS (falsifiable).** SheepShaver STOPS forging the NanoKernel's entry-vector outputs and RUNS the real,
md5-verified Trampoline `MacOS.elf` as the in-tree path that loads, launches, resolves its bounded OF
surface against the synthesized Core99 DT, and reaches `NanoKernelEntry` (`0x50310000`) with zero
unresolved OF calls — behind `SS_M18_TRAMPOLINE ∧ MachineProfileIsNewWorld()` (default OFF), paravirtual
PROVABLY byte-identical. PASSES when all of:
1. **T1–T5 land GREEN under the gate** — the staged-asset ELF loader places the 2 PT_LOAD at ELF vaddrs +
   zero-fills BSS; the guest-callable r5 marshalling shim built (single-arg PPC entry at a loader-owned guest
   address, ctx-bound, BE-32↔host-cell, pointer-translated both directions); the CHRP launch seam sets
   r5/r2/r3/r4 + enters PPC at `0x20f078`; `of_ci_callback` wired + `openfirmware_ci` linked behind the gate
   (DT/display/RTAS/interpret resolve now, `/mmu`=recording stub NON-ACCEPTANCE, disk=S4 stub);
   `PatchROM_NW_trampoline` becomes loader-gated (OFF=byte-identical, bypassed when the loader runs) — each
   CLEAN PPC recompile, `make test-jit`=100, paravirtual byte-identical via REAL `make e2e`.
2. **The handoff predicate samples clean (DEFERRED behind S1)** — `of_ci_unresolved_count(ctx)==0` sampled at
   the `0x50310000` handoff PC at the post-S1 real-`/mmu` gated boot.
3. **The gate flips LAST** — only after T1–T5 are merged-green at default-OFF; **revert-on-red = BRANCH
   revert** (the loader/launch restructure merges regardless of the flag's eventual default).

**The G2b gates (falsifiable-in-advance):**
- **G2b.a — loader vaddr placement + PIC self-reloc stub runs.** Under the gate, the 2 PT_LOAD land at vaddr
  data `0x100000` (filesz `0x6bc0`, memsz `0x19920` rw — BSS tail `0x19920-0x6bc0` zero-filled) / exec
  `0x200000` (filesz `0x10260` r-x); entry `0x20f078` executes its `mflr/bl .+8` self-reloc; the launch
  reaches the first OF-CI call through a wrapper (`0x20dbec`/`0x20dcc0`/`0x20ddb4` via indirect glue
  `0x21024c`). **Identity/forge-state FAILS by construction** (the real Trampoline bytes must be present and
  running). *Mechanism observable at T3 launch; live-confirmed at the post-S1 boot.*
- **G2b.shim — the r5 marshalling shim round-trips.** A PLANTED-ARRAY micro-test (see T2 G-gate) proves a
  guest BE-32 CHRP cell array is marshalled to a host `of_cell[]` (ntohl, array[0]=translated host
  `const char*`, pointer args translated, scalars zero-extended 32→64), `of_ci_callback` invoked, rets[]
  written back (htonl + handle/pointer translation), int returned as r3. Non-vacuous without the live boot.
- **G2b.d — `/mmu` routed to S1's paged MMU.** The `/mmu` `claim`/`translate`/`map` call-method is serviced
  by S1's live paged MMU. **Pre-S1: a recording stub (logs the requests, returns identity/plausible claims)
  — build/unit ONLY, marked NON-ACCEPTANCE.** G2b.d PASS requires the real S1 backend; "build-complete on
  the stub" is a false-clean (Stop-rule #7). *DEFERRED behind S1.*
- **G2b.handoff — reaches `NanoKernelEntry` with 0 unresolved OF calls.** `of_ci_unresolved_count(ctx)==0`
  sampled at the `0x50310000` handoff PC at the post-S1 gated boot. The post-handoff NK fault is EXPECTED
  (not a falsification). *DEFERRED behind S1.*
- **G2b.e — paravirtual byte-identical.** `make test-jit`=100 (353-vector harness) AND — because S2b edits
  SHARED runtime-gated paths (`rom_patches.cpp` + `sheepshaver_glue.cpp` + the Makefile link, NOT
  genuinely-new files — the §6 structural-inertness substitute is NOT available) — **real `make e2e` A/B +
  multi-run soak** (`SS_E2E_RUNS=N` median±CV%) with the gate OFF, byte-identical to today's forge path.
  **Checked at EVERY task gate T1–T5. revert-on-red = BRANCH revert.**

**PASS taxonomy (disambiguated; "BUILD-RESIDUE" here MERGES — the opposite of a Task-0 recon residue, which BLOCKS):**
- **GREEN-PASS** = T1–T5 green AND the post-S1 gated boot reaches `0x50310000` with
  `of_ci_unresolved_count()==0`, paravirtual G2b.e byte-identical → S2b complete, hands the reached NK to
  S3. **NOT reachable this milestone** (needs S1 first — see the rev-1 re-scope header).
- **BUILD-RESIDUE** (THIS milestone's target outcome) = the loader/launch/wiring (T1–T5) lands + paravirtual
  byte-identical, but the live handoff predicate is unsampled (S1 unbuilt) → a finding, not a green light:
  the restructure MERGES (branch-revert keeps paravirtual safe), the gate stays OFF, the live boot is a named
  residue, coordinator re-bands the boot-bringup behind S1. **Carried OPEN residues: (a) the live `/mmu` real
  backend (G2b.d, → S1, recording stub stands in — NON-ACCEPTANCE); (b) the `of_ci_unresolved_count()==0`
  handoff sample (G2b.handoff, → post-S1 boot); (c) r3/r4 confirmation at CHRP entry (→ first gated boot,
  documented provisional, never a hidden ABI).**

**DIAGNOSTIC (never a gate):** which OF services resolve-now vs stub/defer (21 direct DT + display + RTAS + 3
interpret resolve; `/mmu`→S1; disk→S4); how far a gated probe boot gets toward `0x50310000`; the
`nw-northstar` observe line (reaching the NK is the win, NOT Finder); whether the active 1.1 ROM carries a
compatible `MacOS.elf` parcel (UNTESTED — the asset-staging default sidesteps it). `0xDEADBEEF` where a real
guest address/handle is expected is ESCALATED.

## Authoritative inputs

| Doc | Role |
|---|---|
| `FINDINGS-s2b-loader-handoff.md` (RESIDUE-PASS) | The four pinned contracts: Q-S2b.1 source=staged asset + ELF-vaddr placement; Q-S2b.2 r5/r2/entry + glue seam `:2747` + r3/r4 residue; Q-S2b.3 the marshalling-shim I/O contract + backend ownership map; Q-S2b.4 `NanoKernelEntry=0x50310000` + the `of_ci_unresolved_count()==0` predicate + the EXPECTED NK fault. **The contracts this plan builds.** |
| `2026-06-15-ss-m18-s2b-loader-handoff-task0.md` rev-1 | Gates G2b.a/.d/.e, env-gate, file-ownership LAW + the S3-impl collision, Stop-rule, budgets, blocking-answer table. |
| `FINDINGS-trampoline-re.md` (Q0-A/B/E + gating Task-0) | ELF facts (ET_EXEC, 2 PT_LOAD, entry `0x20f078` PIC stub), `[r2-0x60]`/r5/`r2=0x1001e8`/glue `0x21024c`, the OF inventory, the NK entry sequence (`rfi`@`0x5031003c`). |
| `FINDINGS-s2a-ofci-dt.md` (RESIDUE-PASS) | The committed-inert `openfirmware_ci.{cpp,h}` to WIRE; the Core99 DT read-set + ADV-1 finddevice; the residues to S2b (r3/r4; interrupt-map/-mask tuple shape; +8/+2 dynamic names). |
| `FINDINGS-s3-two-supervisor.md` (GREEN-PASS, REPLACE) §★ | What "reaching `NanoKernelEntry`" hands to S3; the file-ownership collision (`rom_patches.cpp`/`glue` are S3's RETIRE surface — S2b lands FIRST); the post-handoff NK fault is EXPECTED. |
| `2026-06-15-ss-m18-s3-impl-replace.md` | The house-template EXEMPLAR (structure copied) + the S3-impl T1/T2 already-committed gate/shim S2b rebases under. **External doc.** |
| `MILESTONE-WORKFLOW.md` §2/§4/§6 | The machine; falsifiable-in-advance gates; env-on-first/flip-LAST/revert-on-red; file ownership; gate tiers; QEMU-as-oracle LAW. |

## Codebase facts (carried — RE-VERIFY at impl; drift ±1–5)

- **The gate to REUSE:** `SS_M18_TRAMPOLINE` (default OFF) ∧ `MachineProfileIsNewWorld()`. Boot-time
  mode-select ONLY; MUST NOT become a per-block/per-access branch. Precedents:
  `sheepshaver_glue.cpp:1082/1554/1614/1645/1984/2747` `MachineProfileIsNewWorld()` sites + the S3-T1
  `NkSupervisorEnabled()` boot-latch in `kpx_cpu/src/cpu/ppc/ppc-cpu.cpp`.
- **The staged asset:** `MacOS.elf`, 94144 B, md5 `1300a95e1a243c582c6c7d619075075a`, from
  `/tmp/newsheep/dump-9.0.1/` (the `66210b4f…` 9.0.1 parcel set). ET_EXEC, 2 PT_LOAD: **data vaddr
  `0x100000`** filesz `0x6bc0` memsz `0x19920` rw; **exec vaddr `0x200000`** filesz `0x10260` r-x. Top =
  `0x210260`, inside the guest RAM aperture. Entry `0x20f078` = `mflr/bl .+8` PIC self-reloc stub at the
  exec-segment tail (no loader fixup beyond correct vaddr placement). **Re-extract via `tbxi` (venv
  `/tmp/newsheep/venv`) if /tmp evaporated; re-verify md5 before staging.**
- **`decode_parcels` does NOT surface `MacOS.elf`** (`rom_decode.hpp:86-100`, `:93` processes only
  `FOURCC('r','o','m',' ')` into flat `ROMBaseHost`). The loader reads the staged asset, NOT the ROM image.
  In-tree extraction is a FUTURE optimization behind a parcel-identity guard — OUT OF SCOPE.
- **The forge to loader-gate (verified):** `rom_patches.cpp` `PatchROM_NW_trampoline` (`:729`+,
  `SS_NW_TRAMPOLINE`) — register-fixup trampoline + entry-vector synthesis at mirror `0x50429b40`. S2b makes
  it loader-gated: OFF=byte-identical to today; bypassed when the real loader runs. **This is the S3-impl
  collision surface** (S3-impl T4 RETIREs the same reg-fixup forge `:716`).
- **The launch seam (verified):** `kpx_cpu/sheepshaver_glue.cpp` `MachineProfileIsNewWorld()` init block
  (`:2747`+) — the SAME block that today stages `[KDP+0x1074]=0x50480000`/`[KDP+0x1078]=0x50460000` at
  `:3140-3141` and the sub-KDP map at `:2762`. The gated CHRP launch (load `MacOS.elf`, set r5/r2/r3/r4, jump
  `0x20f078`) is sited here. **This is the S3-impl collision surface** (S3-impl T4 RETIREs the Execute68k
  pair `:3140-3141` + forge `:2806/2804/2918`).
- **The committed-inert OF-CI artifact (verified):** `SheepShaver/src/machine/openfirmware_ci.cpp` (24811 B)
  + `include/openfirmware_ci.h` (5586 B), commits `14c9384e`+`6c892840`; compiled ONLY by
  `machine/Makefile:87-88` `test_openfirmware_ci` target, **NOT linked into the SheepShaver binary**. S2b
  links it behind the gate (Makefile edit). Callback signature (`openfirmware_ci.h:101`):
  `int of_ci_callback(of_ci_context *ctx, of_cell *array)` — **2-arg, `of_cell=uint64_t` HOST width**
  (`:44`); impl reads `array[0]` as a **host** `const char*` (`openfirmware_ci.cpp:463` `cell_str`,
  `:491` `service`), `array[1/2]`=n_args/n_rets, `args=&array[3]`, `rets=&array[3+n_args]` (`:492-495`).
  `OF_CI_MAX_CELLS=32` (`:58`). Context factory `of_ci_create_core99()`; `of_ci_unresolved_count()`
  (`:105`); `of_call_method_fn` injected-backend hook (`:74`).
- **`NanoKernelEntry` (pinned, re-verify — DO NOT disasm the parcel to over-pin):** `ROMBase+0x310000 =
  0x50310000`, derived by the Trampoline from the Configfile-1 component table (NK-v02.27 @ BASE+0x310000,
  KernelCode, 0x10000 B). Entry sequence `mfmsr`→test MSR[DR]→`mtspr SRR0/SRR1`→`rfi`@`0x5031003c`;
  cross-check parcel offset `0x4880` == `0x50314880` EXT dispatcher. **NOT a guessed address.**
- **Parcel provenance (gate-item-0):** ROM md5 `66210b4f71df8a580eb175f52b9d0f88`; NK-v02.27 md5
  `61c176e90b6365e84e5c660d703e56af` (105280 B); `MacOS.elf` 94144 B md5 `1300a95e…`. **Active project boot
  ROM is the 1.1 ROM `e0fc03faa589ee066c411b4603e0ac89` — NOT the RE'd 9.0.1** (the wrong-bytes risk that
  drives asset-staging).

## The marshalling-shim contract the r5 entry resolves (read before T2)

`FINDINGS-s2b-loader-handoff.md` Q-S2b.3 §★ pinned the apparent incompatibility: `of_ci_callback(ctx,
of_cell*)` is **2-arg, host-pointer/64-bit-cell**; the real CHRP `r5` callback the Trampoline invokes is a
**single-arg, guest-callable PPC entry** receiving **one guest pointer (in r3 at the PPC call) to a
big-endian 32-bit cell array** where string/buffer cells are **guest** addresses. The two are width-,
endianness-, arity-, and address-space-incompatible. **The shim is the spine of T2 — load-bearing NEW S2b
code, a hidden G2b.a prerequisite.** Its pinned I/O contract:
1. **Entry shape:** single-arg PPC-callable trampoline at a loader-owned guest address; its guest address IS
   the value handed in r5. On invocation the guest passes the CHRP array pointer in r3 (PPC ABI arg0).
2. **ctx binding:** captured host-side (static/closure keyed by the single live context) from
   `of_ci_create_core99()` (created once at gated boot) — not passable through the single guest arg.
3. **Inbound (guest→host):** read the guest BE-32 cell array via `Mac2HostAddr`+`ntohl`; build host
   `of_cell[]` (≤32): `array[0]`=host `const char*` from translating the guest service-name string ptr
   (`Mac2HostAddr(guest_ptr)`); `array[1/2]`=n_args/n_rets; pointer arg cells translated guest→host; scalar
   args zero-extended 32→64.
4. **Outbound (host→guest):** after `of_ci_callback` writes `rets[]`, write each return cell back BE-32 via
   `htonl`+`Mac2HostAddr`; returned pointers/handles that are guest-visible translated host→guest
   (phandles/ihandles/lengths are scalars — truncated 32-bit). Return the callback's int as r3.
5. **Pointer-direction discipline:** every semantically-pointer cell crosses with explicit translation; the
   `call-method` backends receive HOST-decoded args from `of_ci_callback`'s `of_call_method_fn` — the shim
   does NOT re-translate those (the backend owner does).

**Where the shim's guest entry + scratch live is a RED-TEAM tension (see Self-review #1).** Provisional: a
loader-owned guest region carved inside the staged-ELF aperture footprint or a reserved scratch page the
launch seam allocates and records — NOT an arbitrary guess into live guest memory.

## Task breakdown (SERIALIZED, gated, LOWEST-RISK-FIRST; one task in flight per file set)

> **File-ownership LAW (sole-in-flight + S3 collision).** Owned set: `rom_patches.cpp`,
> `kpx_cpu/sheepshaver_glue.cpp`, the `openfirmware_ci` link (`machine/Makefile` or the SS build Makefile),
> + a NEW loader/shim translation unit if added (must NOT be `machine/openfirmware_ci.*` which stays as-is).
> T1→T2→T3→T4→T5 SERIALIZED, no two concurrently edit an owned file. **★ S2b MUST land FIRST and MUST NOT be
> in flight on `rom_patches.cpp`/`sheepshaver_glue.cpp` concurrently with S3-impl T3/T4 (which RETIRE the
> same forge/glue surface) — Stop-rule #7 file-collision; coordinator-enforced.** S2b rebases under S3-impl
> T1+T2 (already committed; they do not touch the forge body). Each task env-gated default OFF, falsifiable
> gate, structurally inert when gated off (paravirtual byte-identical via REAL `make e2e`). env-on-first,
> flip-LAST, revert-on-red = BRANCH revert. **S2b owns no memory-translation surface (S1's `/mmu`) — it
> routes `/mmu` to S1 via the injected `of_call_method_fn` backend, never by touching `paged_mmu.cpp`/NATMEM.**

### T1 — the staged-asset ELF loader (LOWEST RISK; inert when off)
- [ ] Re-verify the staged `MacOS.elf` md5 `1300a95e…`/94144 B + the 2 PT_LOAD ELF headers (data vaddr
      `0x100000` filesz `0x6bc0` memsz `0x19920`; exec vaddr `0x200000` filesz `0x10260`). Add a manifest
      entry pinning the asset (size + md5).
- [ ] Under the gate, parse the ELF program headers and copy each PT_LOAD's `filesz` bytes from the asset to
      guest `Mac2HostAddr(p_vaddr)`; zero-fill `memsz − filesz` (the data BSS tail `0x19920 − 0x6bc0`).
      **NO relocation off vaddrs** (ET_EXEC fixed; the PIC stub `0x20f078` self-relocates — Stop-rule #6).
- [ ] Assert each placed range is inside the guest RAM aperture (top `0x210260` < aperture) and disjoint from
      ROM/reserved regions; refuse + STOP on overlap (NOT a relocation hack).
- [ ] Gate-OFF: loader code unreachable; `PatchROM_NW_trampoline` runs byte-identical. CLEAN PPC recompile.
- **G(T1):** `make test-jit`=100; real `make e2e` A/B + soak confirms no paravirtual divergence (gate OFF).
  A gated-ON build-only/unit assertion: the 2 PT_LOAD land at vaddr `0x100000`/`0x200000` and the BSS tail is
  zeroed (assert against guest memory post-load via a probe/unit harness, NOT a live boot). Satisfies the
  placement half of G2b.a; the PIC-stub-runs half is owed to T3 launch + the post-S1 boot.

### T2 — the guest-callable r5 marshalling shim (LOAD-BEARING)
- [ ] Re-verify the `of_ci_callback` signature + `of_cell` width + `array[0..2]`/args/rets layout
      (`openfirmware_ci.h:44/58/101/105`, `.cpp:463/491-495`).
- [ ] Allocate/record the loader-owned guest entry+scratch region for the shim (its guest address becomes
      r5). **Resolve the placement tension (Self-review #1) before coding** — carve from the staged-ELF
      footprint or a reserved page recorded by the launch seam; NO arbitrary guest-memory guess.
- [ ] Build the single-arg PPC-callable guest trampoline → host bridge implementing the §"marshalling-shim
      contract" I/O: ctx captured host-side; inbound BE-32→host `of_cell[]` (ntohl, array[0]=translated host
      `const char*`, pointer args translated, scalars zero-ext 32→64); invoke `of_ci_callback`; outbound
      rets[] host→guest (htonl + pointer/handle translation); return int as r3. ≤`OF_CI_MAX_CELLS`=32; refuse
      over-cap. CLEAN PPC recompile.
- **G(T2):** `make test-jit`=100; paravirtual byte-identical (gate OFF) via real `make e2e` A/B + soak.
  **★ G2b.shim is a PLANTED-ARRAY micro-test, NOT the live boot (which needs S1):** plant a synthetic guest
  BE-32 CHRP array (a `getprop`-shaped call with a guest string ptr + a guest buffer ptr + scalar lengths) at
  a known guest address, invoke the shim, and assert (a) array[0] reaches `of_ci_callback` as a valid host
  `const char*`, (b) each pointer cell was translated guest→host, (c) scalars zero-extended, (d) rets[]
  written back BE-32 with handle/pointer translation, (e) the int return lands in r3. This makes G2b.shim
  non-vacuous without the live install. The LIVE confirmation (the Trampoline's real r5 calls) is **carried
  as OPEN residue to GREEN-PASS behind S1**.

### T3 — the CHRP launch seam (DEPENDS ON T1+T2)
- [ ] Re-verify the glue `MachineProfileIsNewWorld()` block `:2747`+ and the Execute68k pair `:3140-3141`.
- [ ] Under the gate, in that block: after T1 loads `MacOS.elf`, set **r5 = the T2 shim's guest address**,
      **r2 = `0x1001e8`**, **r3/r4 = documented provisional (default 0), LOGGED — never a hidden ABI**
      (carried S2a residue; confirm at the first post-S1 gated boot). Enter PPC execution at **`0x20f078`**
      instead of running the forge path.
- [ ] Emit a `[S2B-LAUNCH]` log line recording the r3/r4 provisional values + r5/r2/entry, so the first-boot
      confirmation is auditable (Stop-rule #3 — no guessed ABI claimed as fact).
- [ ] Gate-OFF: the seam is unreachable; the forge stages `:3140-3141`/`:2762` exactly as today. Audit
      `MachineProfileIsNewWorld()==false` AND `SS_M18_TRAMPOLINE` OFF at the gate SITE. CLEAN PPC recompile.
- **G(T3):** `make test-jit`=100; paravirtual byte-identical (gate OFF) via real `make e2e` A/B + soak.
  **★ The gated-ON "enters at `0x20f078` + PIC stub self-relocates + reaches the first OF-CI wrapper"
  observable inherits S1** (the launch runs but the Trampoline needs the wired backend of T4 + the live
  `/mmu` to progress) — annotate it **DEFERRED-TO-post-S1-boot as a LIMIT**, not a checkable T3 gate now.
  What IS checkable at T3: the seam compiles inert, paravirtual byte-identical, and a gated-ON static
  assertion that the forge path is bypassed + r5/r2/entry are set to the pinned values. Satisfies the
  launch-seam half of G2b.a structurally.

### T4 — wire `of_ci_callback` + link `openfirmware_ci` behind the gate (DEPENDS ON T3)
- [ ] Makefile edit: link `openfirmware_ci.cpp` (+ any DT model TUs) into the SheepShaver binary **behind the
      gate** (compile always for build coverage; the gated boot path is the only consumer). Confirm no
      paravirtual symbol/link regression (gate OFF = same binary behavior).
- [ ] At gated boot, create the context once via `of_ci_create_core99()`; bind it into the T2 shim host-side;
      install the `of_call_method_fn` backend dispatcher.
- [ ] **Backend ownership map (pinned):**
      - `/mmu` `claim`/`translate`/`map` (+ direct `claim`) → **RECORDING STUB** — logs each request, returns
        identity/plausible claims (build/unit ONLY). **NON-ACCEPTANCE (Stop-rule #7) — owed to S1.** Emit
        `[S2B-MMU-STUB]` per request.
      - disk `read-blocks`/`write-blocks`/`block-size` → **S4 stub** (deferred; the NK is reached before disk
        IM-init matters).
      - DT queries (getprop/getproplen/finddevice/nextprop/parent/peer/child/canon/…) → **resolve now** via
        `of_ci_create_core99()` DT.
      - display (`dimensions`/`set-colors`/`fill-rectangle`/`draw-rectangle`) → **no-op resolve**.
      - `instantiate-rtas` → **minimal RTAS stub node/handle**. `interpret` `key?`/`key`/`reset-all` →
        **3-word literal shim**.
      The S2a residues (interrupt-map/-mask tuple shape + §5-Q8 PIC input numbers) are answered by the DT
      RETURNING the provisional (query resolves, `getproplen` NON-FINAL); VALUE confirmation owed to the first
      integration boot — NOT closed here, NOT fabricated. CLEAN PPC recompile.
- **G(T4):** `make test-jit`=100; paravirtual byte-identical (gate OFF) via real `make e2e` A/B + soak.
  Gated-ON unit/build assertion: the backend dispatcher routes each call-method to the pinned owner and the
  `/mmu` recording stub logs (NOT a live boot). **★ G2b.d (real `/mmu`) + G2b.handoff
  (`of_ci_unresolved_count()==0` at `0x50310000`) are DEFERRED behind S1** — recorded as the owed GREEN-PASS
  probes, NOT T4-now gates. Satisfies the OF-CI wiring contract structurally; `/mmu` stub = NON-ACCEPTANCE.

### T5 — loader-gate `PatchROM_NW_trampoline` (DEPENDS ON T4; the S3 collision surface)
- [ ] Re-verify `rom_patches.cpp:729` `PatchROM_NW_trampoline` / `SS_NW_TRAMPOLINE` body + the mirror
      `0x50429b40` entry-vector synthesis.
- [ ] Make the forge **loader-gated**: when `SS_M18_TRAMPOLINE` OFF → run byte-identical to today (the forge
      stages the entry vectors). When the gate ON AND the loader ran → **bypass** the forge (the real
      Trampoline owns the entry-vector synthesis). The gate is read once at boot (reuse the precedent), not
      per-patch-site.
- [ ] **★ Sequencing LAW:** this edit collides with S3-impl T4's forge-RETIRE on the SAME function. **S2b
      lands FIRST** (gates the forge behind `SS_M18_TRAMPOLINE`); S3-impl later RETIRES it behind
      `SS_M18_NK_SUPERVISOR`, rebasing on S2b's gate structure. **S2b and S3-impl T3/T4 MUST NOT be in flight
      on `rom_patches.cpp`/`glue` concurrently** (Stop-rule #7). CLEAN PPC recompile.
- **G(T5):** `make test-jit`=100; paravirtual byte-identical via REAL `make e2e` A/B + `SS_E2E_RUNS=N` soak
  (gate OFF — the forge runs exactly as today). Gated-ON assertion: the forge is bypassed when the loader
  ran. **★ The full live boot (loader→launch→wiring→handoff reaching `0x50310000` with
  `of_ci_unresolved_count()==0`) is the post-S1 gated boot — DEFERRED behind S1.** All together at the post-S1
  boot → GREEN-PASS; this milestone delivers BUILD-RESIDUE (restructure merges, gate OFF, live boot owed).

## Acceptance discipline (env-on-first, flip-LAST, revert-on-red = BRANCH revert)

env-on-first (every task default-OFF, structurally inert, merges that way); flip-LAST
(`SS_M18_TRAMPOLINE=ON` only after T1–T5 merged-green at default-OFF AND S1 landed); **revert-on-red = BRANCH
revert** (revert the flag flip, not the merged loader/launch restructure — what makes the dangerous flip
safe); real `make e2e` paravirtual A/B + soak at EVERY task gate (GUI boots+builds AUTHORIZED, slot boots
only — the standard slot protocol; never global pkill); the planted-array micro-test (T2) + the placement
unit assertion (T1) stand in for the live boot pre-S1; the `0xDEADBEEF`/sentinel-where-a-real-guest-address-is-expected
tripwire ⇒ STOP.

## Env-gate

`SS_M18_TRAMPOLINE` (default OFF) ∧ `MachineProfileIsNewWorld()`; selected ONCE at boot (reuse a
profile-gating precedent — `sheepshaver_glue.cpp:1082/1554/1614/1645/1984/2747` or the S3-T1
`NkSupervisorEnabled()` boot-latch; never a per-access branch). When OFF, `PatchROM_NW_trampoline` runs
byte-identical to today and the loader/launch/wiring are unreachable. **Proof obligation:** "paravirtual
provably unreachable" = `MachineProfileIsNewWorld()==false` ∨ `SS_M18_TRAMPOLINE==OFF` audited at every gate
SITE + G2b.e real-`make e2e` byte-identity at every task gate.

## File ownership (LAW, sole-in-flight + S3 collision)

`rom_patches.cpp` (`PatchROM_NW_trampoline` → loader-gated, T5); `kpx_cpu/sheepshaver_glue.cpp` (the
`:2747`+ CHRP launch seam, T3); the `openfirmware_ci` link (Makefile, T4) + any NEW loader/shim TU (T1/T2);
**S2b owns NO memory-translation surface (S1's `/mmu`)** — `/mmu` routes to S1 via the injected
`of_call_method_fn` backend. One task in flight per file set; T1→T2→T3→T4→T5 SERIALIZED. **★ S2b lands FIRST
on `rom_patches.cpp`+`glue`, ahead of S3-impl T3/T4; concurrent in-flight on these two files = STOP,
coordinator re-serializes (Stop-rule #7).** Depends on Task-0 RESIDUE-PASS (DONE) + S2a-impl committed (DONE)
+ S3-impl T1+T2 committed (rebase under) + the staged 9.0.1 `MacOS.elf` asset. S2b PASS (full) additionally
depends on S1 landed.

## Stop-rule (named in advance; one-iteration mechanics)

1. **#1 (program) — Forge the Trampoline's output instead of running it.** STOP (M17 tripwire). S2b RUNS the
   real `MacOS.elf`; it does NOT re-synthesize the entry-vector forge as the answer. T5 loader-gates the
   forge so the real loader owns it.
2. **#7 (program) — S2b PASS on the stub `/mmu`.** "Build-complete on the recording stub" ≠ S2b PASS. The
   `/mmu` backend is owed to S1 (not faked); full PASS requires the post-S1 real-`/mmu` handoff (G2b.d
   non-stub). Shipping S2b GREEN on the stub is a false-clean.
3. **#3 (program) — Claim a handoff boundary the recon proved fictional / guess `NanoKernelEntry`.**
   `NanoKernelEntry=0x50310000` is pinned from Configfile-1; if the live boot disproves it, that is a
   residue, NEVER a re-guessed address.
4. **#6 (program) — QEMU/hardcode-as-address-oracle.** No QEMU MMIO address is a reference value; the rig is
   a behavioral oracle only (and is known to stall pre-NK-install).
5. **#5 (program) — Unbounded disasm.** No whole-parcel/whole-ROM disasm. Past the handoff window ⇒ STOP,
   conservative residue.
6. **Relocate `MacOS.elf` off its ELF vaddrs to "make it fit".** ET_EXEC at fixed vaddrs; the PIC stub
   self-relocates. Aperture overlap = a Q-S2b.1 finding + STOP, not a relocation hack.
7. **Edit `rom_patches.cpp` / `sheepshaver_glue.cpp` concurrently with S3-impl T3/T4.** Serialize (S2b
   first); concurrent in-flight on these files = STOP, coordinator re-serializes.
8. **Guess r3/r4 into a contract.** Inject a documented, LOGGED provisional (default 0); confirm at the first
   post-S1 gated boot; never a hidden ABI.
9. **`0xDEADBEEF`/sentinel where a real guest address/handle is expected ⇒ STOP** (a translation failed).
10. **Flip the gate before T1–T5 are merged-green at default-OFF AND S1 has landed** — flip-LAST is BINDING.
11. **Claim a fictional boot.** Pre-S1, the gated boot is not run for acceptance; do NOT fabricate a handoff
    sample. The post-handoff NK fault (once S1 lands) is EXPECTED, recorded honestly, NOT a falsification.

**One-iteration rule.** Falsified pin → dated entry in the close-out → ONE bounded re-pin → resume. SECOND
falsification of the same answer ⇒ STOP, re-plan.

## Self-review record — tensions FLAGGED FOR THE RED TEAM

1. **Where does the loader-owned guest scratch/entry region for the r5 shim live without colliding live guest
   memory?** The shim's guest entry address IS r5, and it needs a scratch buffer for marshalling. Carving it
   from the staged-ELF footprint (the BSS tail? a reserved page above `0x210260`?) vs a launch-seam-allocated
   reserved page vs reusing a known-dead low-RAM region — each has a collision risk against what the
   Trampoline itself claims via `/mmu`. **Is there a provably-free guest region under the gate, or is this
   owed a first-boot memory-map probe?** (The sharpest correctness risk in T2.)
2. **Does the recording `/mmu` stub returning identity claims let the Trampoline PROGRESS to the
   `0x50310000` jump, or must it be smarter?** Q-S2b.4 flagged this as unknowable statically — the Trampoline
   does REAL `translate` and *dereferences* the result (per-context SR reload + MMIO segment-swap). An
   identity stub MAY suffice to reach the handoff OR MAY fault on a bogus `translate` mid-build. **If the stub
   is insufficient to even build/exercise the launch path meaningfully, is T4 itself partially blocked behind
   S1 (not just G2b.d)?** I argue the stub reaches "launch runs + first OF calls resolve" for the build, with
   the real translation owed to S1 — red team to confirm the stub floor is enough for BUILD-RESIDUE.
3. **Is staging the 9.0.1 `MacOS.elf` asset the right call vs forcing a 9.0.1-ROM boot, given the ACTIVE ROM
   is the 1.1 ROM (`e0fc03faa…`)?** Asset-staging guarantees the RE'd bytes but couples the loader to an
   out-of-tree artifact (manifest entry; CI/clone reproducibility; /tmp evaporation). A forced 9.0.1-ROM boot
   would make the loader self-consistent with the running ROM but reopens the in-tree-extraction question
   (new code + parcel-identity guard) the Task-0 demoted. **Is the asset the right default for the milestone,
   or should the gate additionally REQUIRE a 9.0.1 ROM (refuse to launch on the 1.1 ROM) to avoid a
   bytes/ROM mismatch the loader can't detect?**
4. **Is the S2b-FIRST serialization with S3-impl T3/T4 robust, or does it thrash the gate structure?** S2b
   gates the forge behind `SS_M18_TRAMPOLINE`; S3-impl T4 then RETIRES it behind `SS_M18_NK_SUPERVISOR`,
   rebasing on S2b's gate. Two gate predicates over one forge function — is that a clean rebase, or should
   the coordinator co-land the forge disposition in one window? (The S2b Task-0 adversary endorsed S2b-first
   to keep each milestone independently revert-on-red; re-confirm against the S3-impl rev-2 re-scope that now
   names S2b as ITS critical-path blocker — the two milestones reference each other.)
5. **Can BUILD-RESIDUE genuinely MERGE with the live boot entirely unexercised pre-S1?** Every gated-ON
   observable for T3/T4/T5 is deferred behind S1; the only non-deferred evidence is the T1 placement
   assertion + the T2 planted-array micro-test + G2b.e paravirtual byte-identity. **Is that enough
   gated-ON-path evidence to merge the restructure, or is there a false-green risk that the launch seam is
   wired wrong in a way no pre-S1 test catches?**

No residual self-review tension beyond these five.

## Red-team record

*(empty — to be filled by the three-reviewer pre-implementation red-team: PROCESS + TECHNICAL + ADVERSARY. 8-item voting list submitted alongside.)*
