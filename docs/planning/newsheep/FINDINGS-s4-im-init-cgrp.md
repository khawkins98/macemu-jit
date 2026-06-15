# NewSheep — S4 recon: the REAL disk-loaded IM-init → CGRP builder

**Status:** COMPLETE (2026-06-15) · READ-ONLY recon (parallel fan-out). Substantive correction/sharpening
of the M16/Q3 model. No src/**, no commits, no boots.

**Method.** 9.2.1 ISO (`~/Downloads/Apple Mac OS 9.2.1/macos_921_ppc.iso`, md5 `3f129e03…`) and 9.2.2 ISO
(`~/Downloads/macos-922-uni/macos-922-uni.iso`, md5 `2cfb856b…`). Modern macOS dropped HFS-standard mount
support, so `hdiutil attach` fails ("no mountable file systems"); instead parsed the Apple Partition Map
(512-byte addressing units despite the 2048 DDM block size) and read the `Apple_HFS` partition with
`machfs`+`macresources` in a venv. 9.2.1 HFS partition starts at file byte 495616; 9.2.2 at 168448.

## HEADLINE — CGRP is built by the disk-resident "replacement multitasking NanoKernel" (`krnl 0` of the System file); "CGRP" = "motherboard Coherence GRouP"
This corrects the M-series framing of CGRP as an opaque "interrupt-group descriptor built by a CFM driver
IM-init." The artifact:
- `System Folder/System` (`zsys/MACS`, 7.19 MB data / 6.98 MB rsrc on 9.2.1) carries resource **`krnl` id 0**
  = 101,440 bytes (9.2.1) / 104,768 bytes (9.2.2) of **raw position-dependent PPC** (starts `48 00 00 0c` =
  `b +0xc`, NOT a `Joy!peff` PEF). Embedded strings: *"Hello from the replacement multitasking NanoKernel.
  Version:"*, *"Old KDP:"*, *"new KDP:"*, *"new irp:"*, *"Kernel code base at 0x"*, *"Physical RAM size
  0x"*, **"Created motherboard coherence group. ID "**, *"Created system address space. ID "*, *"BATs"*,
  *"System context at 0x"*, *"Vector save area at 0x"*, *"SDR1 0x"*.
- This is the disk code the gating Task-0 Q3 saw running from low RAM `0x0045xxxx–0x0046xxxx`: the
  replacement NK relocates itself + the KDP off the ROM builtin NK ("Old KDP → new KDP") — exactly the
  "computed at runtime, ROM-absent" property M16 hit.

## The CGRP build algorithm (disassembled, 9.2.1 `krnl 0` file offset ≈ 0x1960–0x1a0c)
The literal tag `'CGRP'`=`0x43475250` is constructed inline at offset 0x19e0 (`lis r17,0x4347; ori
r17,r17,0x5250`) — the only place in the file the tag appears (BUILT, never stored as a string, matching
M16's "`CGRP` tag 0× in ROM"). The routine:
1. `li r8,0x20; bl 0x11a3c` — allocate a 0x20-byte object (allocator at 0x11a3c uses `mfspr r18,0x110` =
   SPRG0/KDP base).
2. `bl 0x142f0` (r9=1) — register/assign an ID via the kernel object registry (table at `r1-0xa78`, indexed
   by object type; returns `(type<<16)|index` form — explains M16's live `[CGRP+0x00]=0x00010001`).
3. Build a **`'PROC'`** object (`0x50524f43` at +4, field +0x10=2) — the processor object.
4. Build a **`'GRPS'`** group-list head (`0x47525053`) on a stack temp, self-linked (`stw r30,8(r30); stw
   r30,0xc(r30)` → empty circular doubly-linked list).
5. `li r8,0x58; bl 0x11a3c` — allocate the **0x58-byte (88-byte) CGRP object** (r31).
6. Splice it into the GRPS doubly-linked list (node at CGRP+0x10).
7. `lis r17,0x4347; ori r17,r17,0x5250; stw r17,4(r29)` — write tag `'CGRP'` at **+0x04**; self-link the
   node (`stw r29,8(r29); stw r29,0xc(r29)` at +0x08/+0x0c).
8. `bl 0x142f0` (r9=0xa) — assign the CGRP's own ID; `stw r8,0(r31)` writes **+0x00 = ID**.
9. `bl` the kernel debug-printer → "Created motherboard coherence group. ID N".

**Inputs:** SPRG0/KDP base (`mfspr 0x110`); the kernel object registry (`*(r1-0xa78)`); the bump allocator
(0x11a3c). At this skeleton stage the object sources only kernel-internal state — it does NOT yet read the
OF device-tree `interrupt-map`.

## Critical distinction (the key S4 refinement) — TWO-PHASE producer
This routine builds only the **CGRP skeleton**: +0x00 ID, +0x04 tag, +0x08/+0x0c list node, +0x10 group
link. The **interrupt handler arrays M16 pinned — `[+0x38]` guard, `[+0x3c]` handler-descriptor base,
`[+0x40]` stack base, `[+0x44]` count(=18 in QEMU)** — are left ZERO by the skeleton and populated
**downstream**, when interrupt sources register (driver/Interrupt-Manager IM-init walking the DT
`interrupt-map` → `RegisterInterrupt`-class calls). All in the same 88-byte object (`+0x38..+0x44 < 0x58`).
This precisely explains M16: our forge boot showed the skeleton fields plausible but the handler arrays
frozen-zero, while QEMU's real boot shows +0x44=18. So the producer is **two-phase**: (a) replacement-NK
creates the empty coherence group, (b) interrupt-source registration fills the handler table.

## Falsifiable claims for S4 to satisfy (G4.a)
- **C1.** Under S1+S2+S3, the `krnl 0` blob from the 9.2.x System file is loaded + executed; a
  watchpoint/probe on `[CGRP+0x04]` fires from a writer PC inside the relocated replacement-NK code (the
  `krnl 0` image in low RAM), NOT from any SS-side forge PC. **Refuted if** the tag write originates from
  ROM/parcel code or never fires.
- **C2.** CGRP `+0x00` is a registry ID of form `(type<<16)|index` (expect `0x00010001`), `+0x04`=
  `0x43475250`, `+0x08`/`+0x0c` self-referential at creation. **Refuted if** the live object's low fields
  differ in shape.
- **C3.** The handler arrays `+0x38/+0x3c/+0x40/+0x44` are written by a SECOND, later phase
  (interrupt-source registration), from a DIFFERENT writer PC than the skeleton, and `+0x44` reaches a
  nonzero count (QEMU oracle: 18). **Refuted if** a single routine writes both, or the count is a ROM
  constant.
- **C4.** The handler-array population reads the OF device-tree `interrupt-map` (S2a DT) as input.
  **Refuted if** the count/handlers are independent of the DT.

## Pinned vs unknown
**PINNED:** CGRP identity ("motherboard coherence group"), the builder artifact (`krnl 0` raw-PPC
replacement NanoKernel), the skeleton-build algorithm (disassembled), the 88-byte size, the
+0x00/+0x04/+0x08/+0x0c field origins, the two-phase split. **UNKNOWN (deferred to S4's live trace, per the
program plan):** the exact downstream routine + writer PC that fills +0x38..+0x44, the precise DT-read
order, and how the replacement NK is loaded/entered from the ROM builtin NK (the `Old KDP→new KDP`
handoff). These are S4-Task-0 (Q-S4.1 writer-PC trace, Q-S4.2 DT read-order) — now *materially de-risked*
because the builder is a static, extractable, disassemblable file rather than an unknown.

**Connects to:** B2's CD-3 (the NK EXT handler reads an interrupt-source block) + B3's Cuda IFR/IER work —
the interrupt-source registration (phase b) is the same seam.

**Reference scratch (kept in /tmp, not committed):** `/tmp/krnl0.bin` (extracted 9.2.1 replacement NK),
`/tmp/System.data`/`/tmp/System.rsrc`, `/tmp/list921.txt`. Re-extract via the APM-parse + `machfs` recipe.
