# NewSheep — Trampoline RE (Task-0) Findings

**Status:** COMPLETE (2026-06-14) — **Route A decided**; Q0-A/B/C/D/E/F all CLOSED · spec `docs/superpowers/specs/2026-06-14-newsheep-trampoline-re-design.md`
**Method:** two instruments (static tbxi+capstone / dynamic QEMU gdbstub), gated on agreement; both on 9.2.

## Blocking-answer table
| # | Question | Static finding | Dynamic finding | Agree? | Status |
|---|----------|----------------|-----------------|--------|--------|
| Q0-D | Canonical 9.2.x ROM + internal version (the "9.0.1=9.2.2" reframe) | **CLOSED** — `2001-12-19 Mac OS ROM 9.0.1` (md5 `66210b4f…`, == active project ROM) chosen as canonical RE binary; NanoKernel **v02.27**, ROM-file ver only (no embedded OS ver) | n/a | n/a | ✅ |
| Q0-A | OF client-interface call set the Trampoline makes; bounded-and-stubbable vs open-ended | **BOUNDED** — 3 gateways: 21 direct services (177×), `call-method` (20×, ~14 fixed method names), `interpret` (4×, fixed Forth literals). Surface finite & enumerated; data = standard Core99 DT | **CONFIRMS** — runtime trace (2000 hits): same gateways, same `r2=0x1001e8`; services ⊆ static set (getprop/getproplen/nextprop/parent/peer/child/finddevice/claim/seek/read/open/canon/…); `call-method` translate/get-key-map/size; `interpret 'key?'` | ✅ mechanism | **CLOSED** |
| Q0-B | Interrupt-setup writes: constants/relocations vs computed-from-OF-tree | **computed(OF-input)** — Trampoline reads `interrupt-map`/`-mask`/`AAPL,interrupt-*` from OF; the routing data derives from the OF device tree, not ROM constants (explains M16's ROM-absent handler PC) | **CONFIRMS** — `getprop interrupt-map` ×6 + `interrupt-map-mask` ×6 observed at runtime; `claim`×11 + `call-method translate` = page-map build. Provenance = computed(OF-input) | ✅ mechanism | **CLOSED** |
| Q0-C | Can Route B be honest re-binding (relocation), not value-hardcoding? | Writes are `computed(OF-input)` → Route B honest only as a relocation/DT-adaptation, NEVER value-hardcode (see Q0-E table) | consistent (provenance computed → see Q0-E) | ✅ | **CLOSED** |
| Q0-F | **Who builds CGRP — the Trampoline directly, or the NanoKernel from the device tree the Trampoline produces?** | **CONFIRMED: Trampoline + NanoKernel.** `CGRP` absent from `MacOS.elf`; Trampoline only *reads* `interrupt-map` + *edits* DT via `setprop` + builds page map via `/mmu` claim/translate/map. NanoKernel builds CGRP from the DT downstream | **CONFIRMS** — no NK-struct writes seen from the Trampoline; it reads interrupt-map + claims memory. CGRP construction is downstream (NanoKernel parcel) | ✅ mechanism | **CLOSED** |
| Q0-E | Route decision (A run / B patch / C reproduce) + SS-integration sketch | **Route A — run the real Trampoline + NanoKernel** against a synthesized OF device tree + OF-CI callback (C≡A here since writes are computed(OF-input); B can't supply an OF env by patching). Passes the S3 feasibility check. | (same) | ✅ | **CLOSED** |

## Evidence

### Q0-D — ROM identity + version reframe (T0.0) [STATIC]

**Method:** `tbxi 0.13` (venv `/tmp/newsheep/venv`) `dump -o` of the two 9.2-era candidates.

- **Dump shape (both 9.0.1 and 8.4, identical top-level layout):** `Bootscript` (CHRP ASCII),
  `MacOS.elf` (the Trampoline — top-level ELF), `Parcels` / `Parcels.src/` (PEF device drivers +
  `MacROM.src/`). No parcels-layout break between 8.4 and 9.0.1 — both tbxi-dump cleanly with the
  same structure. (R4's "9.1+ different layout" concern does not bite the 9.0.1 binary we target.)
- **The ROM file has NO embedded Mac OS *system* version string.** The "9.0.1" in the filename is
  the **ROM-file version**; the Mac OS system version (9.2.x) is supplied by the System file on disk,
  not the ROM. So the reframe is confirmed by *date + components*, not an OS-version string:
  - `2001-12-19 Mac OS ROM 9.0.1` — md5 `66210b4f71df8a580eb175f52b9d0f88`, **byte-identical to our
    active project ROM**. NanoKernel **v02.27** (`Configfile-1: KernelCodeOffset=…=NanoKernel-v02.27`).
    Dec-2001 = the Mac OS 9.2.2 era. `BootstrapVersion='NewWorld v1.0   '`.
  - `2001-07-30 Mac OS ROM 8.4` — md5 `f97d4382…`, NanoKernel **v02.24**. Jul-2001 = 9.2/9.2.1 era.
- **Configfile-1 structural map (9.0.1)** — the ROM component table the NanoKernel/Trampoline use:
  `ROMImageSize=0x00400000`; `Mac68KROM` @BASE+0x0 (0x300000 B); `ExceptionTable` @BASE+0x300000;
  `HWInitCode` @BASE+0x320000; **`NanoKernel-v02.27` @BASE+0x310000 (KernelCode, 0x10000 B)**;
  `EmulatorCode` @BASE+0x360000; `OpcodeTable` @BASE+0x380000.
- **Two NanoKernel parcels present in 9.0.1 dump** (`MacROM.src/`): `NanoKernel-v02.27` (active per
  Configfile) + `NanoKernel-v02.24` (the alternate/fallback). The CGRP-builder disasm target (Q0-F)
  is **`NanoKernel-v02.27`**, 105280 B.

**Verdict:** canonical RE binary = the `66210b4f…` 9.0.1 ROM (== active project ROM; both instruments
analyze this one binary). The "9.0.1 file = 9.2.2-era ROM" reframe is **confirmed** (Dec-2001 date +
NanoKernel v02.27 + structural identity to the active ROM). Remaining 9.2 gap is system software, off
Task-0's path.

---

### Q0-A / Q0-B / Q0-F — static RE of the Trampoline (T0.1) [STATIC]

**Target:** `MacOS.elf` = the Trampoline. `ELF 32-bit MSB PowerPC, ET_EXEC`, entry **`0x20f078`**,
2 PT_LOAD segs: **exec `0x200000`** (filesz `0x10260`, r-x) + **data `0x100000`** (filesz `0x6bc0`,
memsz `0x19920`, rw). No section headers (stripped). Disassembled both segments with capstone PPC-BE
(15392 exec insns) → `/tmp/newsheep/tramp.asm`. Entry `0x20f078` is a PIC self-relocation stub
(`mflr/bl .+8`) at the segment tail.

**OF client-interface gateway (the single mechanism).** The OF entry pointer is stored at TOC slot
**`[r2-0x60]`** (`r2 = 0x1001e8`, the small-data-area base; pinned because the data word at `0x100000`
holds the OF-service string-pool base `0x100698`). The indirect-call glue is **`0x21024c`**
(`mtctr r12; bctrl`-equiv), reached only from **3 wrapper functions**, each building the CHRP
`{name, n_args, n_rets, args…}` array at scratch `[r2-0xc]`:

| Wrapper | Hardcoded OF service (word0) | Callers | Role |
|---|---|---|---|
| `0x20dbec` | *(per-call)* — direct OF service in `r3` | **177** | all the property/tree/IO/memory verbs |
| `0x20dcc0` | `call-method` (`0x106a58`) | **20** | invoke a method on an opened ihandle |
| `0x20ddb4` | `interpret` (`0x106a64`) | **4** | execute a Forth literal |

**(Q0-A) The complete OF call surface — enumerated, falsifiable (resolved 177/177 + 24/24, 0 unresolved):**

*Direct services via `0x20dbec` (21 unique):* `getprop`×46, `getproplen`×37, `finddevice`×21,
`close`×13, `setprop`×12, `open`×9, `seek`×6, `parent`×6, `exit`×4, `read`×4, `package-to-path`×3,
`instance-to-package`×3, `write`×3, `instance-to-path`×2, `peer`×2, `test`×1, `canon`×1,
`quiesce`×1, `nextprop`×1, `child`×1, `claim`×1.

*`call-method` targets via `0x20dcc0` (14 unique):* `read-blocks`×4, `dimensions`×2,
`set-hybernot-flag`×2, `claim`×2, `get-key-map`, `set-colors`, `fill-rectangle`, `draw-rectangle`,
`size`, `instantiate-rtas`, `block-size`, `write-blocks`, `translate`, `map`.

*`interpret` Forth literals via `0x20ddb4` (4 calls):* `key?`, `key`, `reset-all`, +1 dynamic.

**finddevice paths (11 unique):** `/chosen`×4, `/aliases`×3, `/`×3, `/rtas`×3, `/cpus/@0`,
`/cpus/@0/l2-cache`, `/cpus/@0/l2-cache/l2-cache`, `/options`, `/rom/macos`,
`/pci/mac-io/interrupt-controller`, +2 dynamic. **All standard Core99 OF nodes.**

**getprop/getproplen keys (37 unique):** the standard set — `reg`, `device_type`, `compatible`,
`interrupt-parent`, `name`, `ranges`, `screen`, `#address-cells`, `#interrupt-cells`,
**`interrupt-map`, `interrupt-map-mask`**, `assigned-addresses`, `slot-names`, `AAPL,address`,
`AAPL,toolbox-parcels`, `AAPL,reserved-memory-space`/`-io-space`, `cache-unified`, `model`,
`display-type`, `bootpath`, … (+8 dynamic). **All resolvable from CORE99-MACHINE-DESCRIPTION + the
QEMU device-tree oracle.**

**setprop keys — the Trampoline's DT edits (11 unique, 12 calls):** `AAPL,gray-page`×2,
`AAPL,reserved-memory-space`, `AAPL,reserved-io-space`, `AAPL,MacOSMachineName`, `AAPL,LANDiskInfo`,
and **5 netboot-only** (`AAPL,bootp-client-ip-address`/`-subnet-mask`/`-routers`/`-gateway-ip-address`/
`-domain-nameservers`). The non-netboot DT-edit surface is tiny.

**Judgment (Q0-A, both parts):**
1. **Call-surface BOUNDED — YES.** The verb set is finite and fully enumerated. The only
   open-ended-*shaped* primitive is `interpret` (arbitrary Forth), but it is used 4× with **fixed
   string literals** (`key?`/`key`/`reset-all`), not external/computed input — so the *surface* is
   bounded. Asterisk: Route A is **not** a pure property-query stub — it must also service
   `call-method` on instances (disk `read-blocks`, `/mmu` `claim`/`translate`/`map`, display draw) and
   a 3-word `interpret` shim.
2. **Stub-data TRACTABLE — YES for the DT-query surface.** Every finddevice path and getprop key is a
   standard Core99 device-tree node/property already in `CORE99-MACHINE-DESCRIPTION.md` + the QEMU
   oracle. The load-bearing data is `interrupt-map`/`interrupt-map-mask` on
   `/pci/mac-io/interrupt-controller` — exactly the interrupt-routing input the NanoKernel turns into
   CGRP. The real cost lives in the `call-method` instances (`read-blocks` = actual disk I/O;
   `/mmu` claim/translate/map = real page-table setup), not in the property queries.

**(Q0-B) Write provenance.** The Trampoline makes **no NK-interrupt-structure writes**. Its only
tree-affecting writes are the 12 `setprop` DT edits above (boot-screen + machine-name + reserved
ranges + netboot), plus the page map it builds via `/mmu` `claim`/`translate`/`map`. The
interrupt-routing data is **`computed(OF-input)`**: read from the OF `interrupt-map` property, not a
ROM constant. This is the mechanism behind M16's finding that the CGRP handler PC is ROM-absent — it
is derived at runtime from the OF device tree, downstream, by the NanoKernel.

**(Q0-F) CONFIRMED — producer = Trampoline + NanoKernel.** `CGRP` (`0x43475250`) is absent from
`MacOS.elf`. The Trampoline (a) reads the OF device tree incl. `interrupt-map`, (b) edits it with a
few `setprop`s, (c) builds the PPC page map via `/mmu`, then (d) hands off to `NanoKernelEntry`. The
**NanoKernel-v02.27 parcel** is the CGRP builder (separate disasm target; its `0x5031xxxx`/`0x503148e0`
code is in that parcel's address space, NOT in `MacOS.elf`). "Run/reproduce the producer" therefore
means the Trampoline's DT-build + page-map AND the NanoKernel's DT→CGRP step.

**Working artifacts:** `/tmp/newsheep/tramp.asm` (full disasm), `/tmp/newsheep/ofcalls.json` (per-site
service inventory). Resolver method: capstone PPC-BE + backward const-propagation anchored on
`r2=0x1001e8`; 177/177 + 24/24 OF call sites resolved with zero unresolved (the few `None` argument
slots are runtime-computed names — the dynamic instrument's job).

---

### Q0-A / Q0-B / Q0-F — dynamic RE: QEMU mac99 Trampoline tracer (T0.2) [QEMU-BEHAVIORAL]

**Rig:** QEMU `qemu-system-ppc -M mac99 -m 512` booting the rig's cached test ISO
(`/tmp/qemu-rig-cd_test_901.iso` = 9.2.1 installer + the **same** `66210b4f…` 9.0.1 ROM swapped in —
so static & dynamic analyze one binary), launched with **`-s -S`** (gdbstub on `:1234`, halted at
reset). **R1 resolved:** no PPC `gdb` on host → drove the stub with a hand-written Python gdb-remote
(RSP) client (`/tmp/newsheep/gdbcli.py`); breakpoint step-over = remove-bp → single-step → re-insert →
continue (the naive `c` re-fires the same bp).

**R1 made trivial by a load-bearing finding: OpenBIOS loads the Trampoline at its ELF vaddr.** A
breakpoint at the static ELF entry `0x20f078` hit at +7.4 s with **PC = `0x20f078`** and
**`r2 = 0x1001e8`** — identical to the static analysis. So the static addresses (the 3 OF wrappers
`0x20dbec`/`0x20dcc0`/`0x20ddb4`, the OF-entry TOC slot `[r2-0x60]`) are the runtime addresses; tracing
reduces to breakpointing those three wrappers and reading r3/args from live memory.

**Trace (2000 OF-wrapper hits in 2.5 s, then capped):**
- **Direct services** (`0x20dbec`): `getprop`×895, `getproplen`×455, `nextprop`×397, `parent`×97,
  `peer`×53, `child`×52, `finddevice`×14, `claim`×11, `seek`×5, `read`×5, `open`×3,
  `instance-to-package`×2, `instance-to-path`×2, `canon`×1, `write`×1, `package-to-path`×1.
- **call-method** (`0x20dcc0`): `translate`×3, `get-key-map`×1, `size`×1.
- **interpret** (`0x20ddb4`): `'key?'`×1.
- **finddevice paths:** `/chosen`, `/`, `/aliases`, `/options`, `/rtas`, `/cpus/@0`,
  `/cpus/@0/l2-cache`(+`/l2-cache`), `/rom/macos`, **`/pci/mac-io/interrupt-controller`**, and a
  runtime-resolved concrete boot path `/pci@f2000000/mac-io@c/ata-3@20000/cdrom@0` (one of static's
  `None` paths, now concrete).
- **getprop keys:** the standard set led by `name`/`device_type`/`compatible`/`#address-cells`/
  `interrupt-parent`/`reg`/`model`, **including `interrupt-map`×6 + `interrupt-map-mask`×6** and PCI
  config keys (`vendor-id`/`device-id`/`class-code`/…).

**Mechanism-level agreement (the gate):**
- ✅ Same OF gateway mechanism (3 wrappers, hardcoded `call-method`/`interpret`, OF entry at
  `[r2-0x60]`, `r2=0x1001e8`).
- ✅ Same service *set* (runtime services ⊆ the static-enumerated surface; every dynamic service was
  predicted statically).
- ✅ Same Q0-B provenance class: `interrupt-map`/`-mask` are **read** from OF at runtime → routing data
  is `computed(OF-input)`, corroborating the static classification and M16's ROM-absent handler PC.
- ✅ Same Q0-F: the Trampoline reads interrupt-map + claims memory; no NK-interrupt-struct writes —
  CGRP is built downstream by the NanoKernel.

**Expected divergences (logged, NOT blocking — OpenBIOS ≠ Apple OF, per the gate):** dynamic shows
**more** getprop keys (PCI-config probing: vendor-id/device-id/class-code/etc.) and **far more**
`nextprop` (full live tree-walk, 397× vs static's 1 resolved) and concrete device paths — all are
value/count/path differences from OpenBIOS's device tree, exactly the pre-declared expected-divergence
class. No mechanism-level divergence found.

**Working artifacts:** `/tmp/newsheep/gdbcli.py` (RSP client), `/tmp/newsheep/qemu.log`.

---

## Reconciliation — agreement gate (T0.3)

The two instruments analyze the **same ROM binary** (`66210b4f…`) but in different OF environments
(our offline disasm vs OpenBIOS at runtime). Applying the **mechanism-level** gate (services /
write-classes / provenance — never literal values/addresses/counts):

| Mechanism axis | Static | Dynamic | Verdict |
|---|---|---|---|
| OF gateway | 3 wrappers, OF entry `[r2-0x60]`, `r2=0x1001e8`, glue `0x21024c` | identical (`r2=0x1001e8`, Trampoline at ELF vaddr) | **agree** |
| Hardcoded services | `call-method`(`0x20dcc0`), `interpret`(`0x20ddb4`) | same wrappers fired with same hardcoded services | **agree** |
| Direct service set | 21 enumerated | runtime set ⊆ static set (all predicted) | **agree** |
| Q0-B provenance | interrupt routing = computed(OF-input) (reads `interrupt-map`) | `getprop interrupt-map`×6 + `-mask`×6 observed | **agree** |
| Q0-F producer | Trampoline reads DT + claims mem; NanoKernel builds CGRP | no NK-struct writes from Trampoline at runtime | **agree** |

**Expected divergences (pre-declared OpenBIOS≠AppleOF class — logged, NOT blocking):** dynamic shows
more getprop keys (PCI-config probing), 397× `nextprop` (full live tree-walk) vs static's 1 resolved,
and concrete device paths (`/pci@f2000000/mac-io@c/…/cdrom@0`). All are value/count/path differences,
not mechanism differences. **No mechanism-level divergence found → gate PASS; Q0-A/B/C/F CLOSED.**

## Route decision (T0.4 / Q0-E)

**Inputs:** Q0-A = **bounded** (call-surface finite + DT-query data tractable); Q0-B =
**computed(OF-input)**; Q0-F = **producer is Trampoline + NanoKernel**.

**Truth-table cell (bounded × computed(OF-input)) → Route A**, with **C ≡ A** here (reproducing the
writes requires reproducing the NanoKernel's CGRP computation *and* supplying its OF interrupt-map
input — i.e. you end up running/feeding the producer anyway). **Route B is excluded**: the Trampoline's
blocker is not relocation (OpenBIOS already runs it at its ELF vaddr) but the *absence of an OF
environment* — which `tbxi build` patching of the parcel cannot supply; a value-hardcode B would be the
banked forge in a tbxi hat (Q0-C). 

> **DECISION: Route A — run the real Trampoline (`MacOS.elf`) and let it hand off to the real
> NanoKernel-v02.27, against a SheepShaver-synthesized OpenFirmware client interface + Core99 device
> tree.** This is the highest-fidelity "solid foundations" rung: the frozen CGRP/IM structures get
> populated by *real guest code* computing *real values* from the device tree, clearing the whole
> frozen-struct class at once instead of forging one wall at a time.

**Honest cost (NOT cheap — the `SS_NW_TRAMPOLINE` problem one level up):** Route A's "stub OF" =
**synthesize the OF device tree the producer reads** + implement an OF client-interface callback that
services the bounded call set. Concretely, the next milestone must provide:
1. **An OF-CI callback** (single entry, handed to the Trampoline in r5 at launch; the Trampoline stashes
   it at `[r2-0x60]` and calls only through the 3 wrappers) dispatching the 21 direct services +
   `call-method` + a 3-word `interpret` shim (`key?`/`key`/`reset-all`).
2. **A Core99 OF device tree** answering finddevice/getprop/nextprop — populated from
   `CORE99-MACHINE-DESCRIPTION.md` + the QEMU device-tree oracle (`<rundir>/device-tree.txt`). The
   load-bearing properties are `interrupt-map` / `interrupt-map-mask` on
   `/pci/mac-io/interrupt-controller` (the routing data the NanoKernel turns into CGRP), plus
   `reg`/`ranges`/`assigned-addresses`/`AAPL,address` for the MMIO layout.
3. **`call-method` backends:** `read-blocks`/`write-blocks`/`block-size` → SheepShaver's boot disk;
   `/mmu` `claim`/`translate`/`map` + direct `claim` → SheepShaver's memory/page setup; display
   methods (`dimensions`/`set-colors`/`fill-rectangle`/`draw-rectangle`) → framebuffer or no-op;
   `instantiate-rtas` → a minimal RTAS stub.
4. **Launch + handoff:** load `MacOS.elf` into guest space (`0x100000`/`0x200000`; OpenBIOS proves the
   ELF vaddr is honorable), set the CHRP entry ABI (r5 = OF-CI callback), jump to `0x20f078`; the
   Trampoline builds the DT-derived page map and calls `NanoKernelEntry`; the NanoKernel builds CGRP.

**Mechanical-feasibility check (S3) — Route A passes (falsifiable assertion + evidence):** *"`MacOS.elf`
is relocatable into our guest space and its entry ABI is one we can supply."* Evidence: `ET_EXEC`, 2
`PT_LOAD` at fixed vaddrs `0x100000`/`0x200000` (total < `0x210260`, comfortably inside SheepShaver's
guest RAM aperture); a PIC self-reloc stub at entry; **its only external entry dependency is the single
OF-CI callback pointer** (everything else is the bounded, enumerated call set) — and OpenBIOS already
demonstrates a working launch at that vaddr. The dependency surface is one well-defined interface we
implement, not an open-ended firmware. ✅ GO.

## SS-integration sketch — what `SS_NW_TRAMPOLINE` becomes

**Today:** `SS_NW_TRAMPOLINE` (in `rom_patches.cpp` / `sheepshaver_glue.cpp`) is a *partial hand-built
substitute* — a register-fixup trampoline + entry-vector-slot synthesis + a cluster of staged globals
(`'Hnfo'@0x68ff4f00`, PIC-rail staging `[[KDP-0x20]+0xf18]`, `[KDP+0xf2c]` TimebaseSpeed, the Execute68k
emulator pair `[KDP+0x1074]=0x50480000`/`[KDP+0x1078]=0x50460000`). It forges the *outputs* the real
Trampoline+NanoKernel would produce.

**Route A milestone (proposed `SS_M18_TRAMPOLINE_LLE`, gated + `MachineProfileIsNewWorld()`,
paravirtual byte-identical):** replace the output-forge with a producer-run:

| Existing `SS_NW_TRAMPOLINE` write | Disposition under Route A |
|---|---|
| Entry-vector slot synthesis / register-fixup trampoline | **Replaced** — the real Trampoline + NanoKernel populate the NK dispatch state from the device tree. |
| `'Hnfo'@0x68ff4f00`, PIC-rail staging, `[KDP+0xf2c]` TimebaseSpeed | **Replaced/redundant** — produced by the real NanoKernel init (these are exactly its outputs). |
| Execute68k emulator pair `[KDP+0x1074]=0x50480000` / `[KDP+0x1078]=0x50460000` | **KEEP / re-inject post-handoff** — these point at SheepShaver's *own* JIT emulator (mirror base `0x50460000`, DR table `0x50480000`), not the ROM's; the real NanoKernel would point at the ROM emulator, so SS must re-bind this pair after the NanoKernel runs. The one genuinely SS-specific seam. |
| CGRP family (`*(KDP-0x338)`, `[CGRP+0x38/0x3c/0x40/0x44]`, service `0x503148e0`) | **Guest-populated** — built by the real NanoKernel from our device tree's `interrupt-map`; this is the win (M16's frozen, ROM-absent struct now computed by real code). |

**New code (next milestone):** (1) `of_ci_callback()` + device-tree model (new file, e.g.
`SheepShaver/src/openfirmware_ci.cpp`); (2) the `call-method` backends wired to SS disk/memory/fb;
(3) a Trampoline loader + CHRP entry in the newworld boot path; all behind `SS_M18_*` +
`MachineProfileIsNewWorld()`, with the paravirtual path untouched and `make test-jit`=100.

**Expected next wall (per charter §6, M14-FINDINGS):** the **Cuda device-model IFR/IER bug**
(`sr_int_pending` never reaches VIA IFR; the NK polls IER) — a *different* regime, which is the
**GOOD** outcome (producer approach worked; back on the machine-layer mainline with device models
already staged). Reaching Finder remains the north star, not this milestone's DoD.

## SS_M18 — gating risks to red-team BEFORE any code (carried forward from external review)

Task-0 answered *"is the Trampoline runnable?"* → **yes** (bounded OF surface, hostable at its ELF
vaddr). It did **not** fully answer *"is running the real Trampoline + NanoKernel tractable inside
**SheepShaver specifically**?"* Two architectural collisions are under-examined here and must be the
**gating Task-0 of SS_M18** (run its own binding recon + red-team first — the pattern that caught
M16/M17 cheaply). Neither invalidates Route A; "producer-run reveals a non-IM wall = the GOOD outcome"
applies. But they decide whether Route A is weeks or months.

1. **Emulator-host collision (bigger than "re-bind one pair").** SheepShaver *is* the 68k/PPC
   emulator (its JIT). The real **NanoKernel-v02.27 wants to OWN the 68k emulator** — it points the
   Execute68k pair at the ROM's `EmulatorCode @0x360000`. Beyond re-binding `[KDP+0x1074/0x1078]`,
   running the real NanoKernel may collide with the supervisor / exception / MixedMode / scheduler
   scaffolding SS built across M0–M13. **Gating question: how much of that existing scaffolding does
   the real NanoKernel REPLACE vs. FIGHT?** If it fights, Route A is months.
2. **MMU collision (foundational, not a backend).** The Trampoline builds a **real PPC page map**
   (`/mmu` `claim`/`translate`/`map`). SheepShaver runs a **V=P flat model**
   (DIRECT_ADDRESSING / NATMEM_OFFSET; the paged MMU was deliberately deferred in machine-layer M5).
   The sketch above treats `/mmu` as "a call-method backend to implement" — but it may instead force a
   **real MMU**, colliding head-on with flat addressing. **Gating question: can `/mmu` claim/translate/
   map be satisfied within V=P (identity/window mapping), or does the NanoKernel require a live paged
   MMU?**
3. **Trace the NanoKernel's CGRP construction directly (close the inferred gap).** The dynamic trace
   observed the *Trampoline's* OF calls but **inferred** (did not directly trace) the NanoKernel
   building CGRP from the device tree. SS_M18 Task-0 should breakpoint into the **NanoKernel-v02.27**
   region under QEMU and trace the device-tree→CGRP construction, to pin the **exact handoff state**
   (what the NanoKernel reads from the DT, what it writes, in what order) — the precise contract our
   synthesized device tree must satisfy.

> **Expected next wall after the IM-init class clears** (charter §6 / M14-FINDINGS): the **Cuda
> device-model IFR/IER bug**. Risks 1–2 above are *earlier* gates — they decide SS_M18's tractability
> before that wall is even reached.

## Scope-guard verification
`git log --stat newsheep-baseline..HEAD` shows **docs-only** (no source files) — RE-only milestone
respected. The SS integration is the NEXT, code-writing milestone (sketched above, not implemented here).
