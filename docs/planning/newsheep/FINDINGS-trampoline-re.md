# NewSheep — Trampoline RE (Task-0) Findings

**Status:** IN PROGRESS · spec `docs/superpowers/specs/2026-06-14-newsheep-trampoline-re-design.md`
**Method:** two instruments (static tbxi+capstone / dynamic QEMU gdbstub), gated on agreement; both on 9.2.

## Blocking-answer table
| # | Question | Static finding | Dynamic finding | Agree? | Status |
|---|----------|----------------|-----------------|--------|--------|
| Q0-D | Canonical 9.2.x ROM + internal version (the "9.0.1=9.2.2" reframe) | **CLOSED** — `2001-12-19 Mac OS ROM 9.0.1` (md5 `66210b4f…`, == active project ROM) chosen as canonical RE binary; NanoKernel **v02.27**, ROM-file ver only (no embedded OS ver) | n/a | n/a | ✅ |
| Q0-A | OF client-interface call set the Trampoline makes; bounded-and-stubbable vs open-ended | **BOUNDED** — 3 gateways: 21 direct services (177×), `call-method` (20×, ~14 fixed method names), `interpret` (4×, fixed Forth literals). Surface finite & enumerated; data = standard Core99 DT | _pending_ | _pending_ | static done |
| Q0-B | Interrupt-setup writes: constants/relocations vs computed-from-OF-tree | **computed(OF-input)** — Trampoline reads `interrupt-map`/`-mask`/`AAPL,interrupt-*` from OF; the routing data derives from the OF device tree, not ROM constants (explains M16's ROM-absent handler PC) | _pending_ | _pending_ | static done |
| Q0-C | Can Route B be honest re-binding (relocation), not value-hardcoding? | Writes are `computed(OF-input)` → Route B honest only as a relocation/DT-adaptation, NEVER value-hardcode (see Q0-E table) | _pending_ | _pending_ | static done |
| Q0-F | **Who builds CGRP — the Trampoline directly, or the NanoKernel from the device tree the Trampoline produces?** | **CONFIRMED: Trampoline + NanoKernel.** `CGRP` absent from `MacOS.elf`; Trampoline only *reads* `interrupt-map` + *edits* DT via `setprop` + builds page map via `/mmu` claim/translate/map. NanoKernel builds CGRP from the DT downstream | _pending_ | _pending_ | static done |
| Q0-E | Route decision (A run / B patch / C reproduce) + SS-integration sketch | _pending_ | _pending_ | — | — |

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
