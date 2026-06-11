# Framebuffer Recon — what does the M5 boot framebuffer aperture actually require?

> **Status:** recon complete (2026-06-11) · Stream C, M7 risk-retirement (sibling to
> `DISK-PATH-RECON.md`, commit 585305ca — same method, overlapping evidence base)
> **Question:** the ROM draws happy-Mac/splash to the OF display node's `address` long
> before any `.ndrv` loads (MACHINE-LAYER-PLAN M5 row, rev 3). M5 plans a mapped-APERTURE
> bus region backed by real memory, blitted to SDL — the first live consumer of the M1
> bus's `MMIO_APERTURE` region kind. Without it, M7 debugs a black screen. What does that
> actually take?
> **Method:** static + docs only (no boots, no source edits). ROM evidence from
> `/Users/Shared/macemu/2001-12-19 - Mac OS ROM 9.0.1.rom` (raw file: Trampoline ELF +
> parcels) and `/tmp/rom901_decompressed.bin` (SS_DUMP_ROM image).
> **Headline:** three findings change the M5 milestone's shape. (1) **Paravirtual video is
> already an aperture in all but registration** — the guest draws straight into vm-mapped
> guest memory and a host thread diff-blits it at 60 Hz; M5 is a *relocation + publication*
> job, not a new video path. (2) The early-draw consumer is **the Trampoline we skip**: it
> harvests the OF screen alias's `address`/`width`/`height`/`linebytes`/`depth`
> (byte-verified, §2.1) and we have never replicated that handoff — where the values land
> in the structure passed to the ROM is the recon's biggest unknown (T-F5). (3) The
> `MMIO_APERTURE` kind is **an enum value only**; registering a region with it today would
> poison all three M1 dispatch paths (§3.2) — keeping apertures OUT of the trap hull is the
> load-bearing design decision.

---

## §1. The paravirtual video path, as built

### 1.1 Where the guest-visible VRAM is (address, size, allocator)

There is no trapped VRAM anywhere — the "framebuffer" is ordinary mapped guest memory:

- `the_buffer` is allocated by `vm_acquire_framebuffer()`
  (`BasiliskII/src/SDL/video_sdl3.cpp:248` — `SheepShaver/src/SDL` is a symlink to
  `BasiliskII/src/SDL`), which on macOS arm64 returns `vm_acquire_reserved()`
  (`BasiliskII/src/CrossPlatform/vm_alloc.cpp:265`) — a **pre-reserved 80 MB pool**
  (`RESERVED_SIZE = 80 MB` "for 6K Retina", `vm_alloc.cpp:241`).
- The pool is pinned **inside guest address space** at init: `SheepMem::Init()`
  (`SheepShaver/src/Unix/main_unix.cpp:3187–3198`) calls
  `vm_init_reserved(adr + size)` where `adr` = guest `ROM_BASE + ROM_AREA_SIZE +
  SIG_STACK_SIZE` = `0x50000000 + 0x500000 + 0x10000` = `0x50510000` and SheepMem
  `size = 0x80000` (`include/thunks.h:122`). So the pool occupies guest
  **`0x50590000 .. 0x55590000`** on every profile (SheepMem::Init is unconditional).
- `screen_base` (the guest framebuffer base, `video.cpp:45`, `include/video.h:106`) is set
  to `Host2MacAddr(the_buffer)` at mode set (`video_sdl3.cpp:558`,
  `set_mac_frame_base` → `screen_base`, `:361`) — i.e. `screen_base ≈ 0x50590000`.

The guest's QuickDraw then writes **directly** to `screen_base` with plain loads/stores —
JIT and interpreter alike hit host memory natively, zero faults, zero per-access cost.
This is exactly the access model M5's aperture promises; it already runs in production.

### 1.2 How the guest learns the address (the ndrv stub chain)

1. `DoPatchNameRegistry()` (`name_registry.cpp:85–388`) creates the `video` node and
   plants the `driver,AAPL,MacOS,PowerPC` property = `video_driver[]`
   (`name_registry.cpp:364–373`; bytes from `VideoDriverStub.i` — a hand-rolled PEF
   `Joy!peffpwpc` whose DoDriverIO TVector is a 9-instruction PPC stub that loads a
   function pointer + selector from its TOC and `bctr`s; PEF-internal name
   `Display_Video_Apple_Sheep`, visible in the .i hex).
2. The stub's target is the NativeOp seam: `NATIVE_VIDEO_DO_DRIVER_IO`
   (`thunks.cpp:302–304`, `include/thunks.h:32–34`) → host `VideoDoDriverIO()`
   (`video.cpp:1030`).
3. `VideoDoDriverIO` dispatches Driver-Gestalt-era commands (`kOpenCommand` →
   `VideoOpen`, `video.cpp:1106–1130`); `VideoOpen` records
   `csSave->saveBaseAddr = screen_base` (`video.cpp:165`), and the Status/Control calls
   return it to the guest via `csBaseAddr` (`video.cpp:669,725,752`). VSL interrupt
   services + `IOCommandIsComplete` are imported from the guest's own
   DriverServicesLib/VideoServicesLib (`video.cpp:74–105`) — the **real** NDRV framework
   runs; only the bottom is host code (same posture as the disk recon's option (c)).
4. Host→SDL: a 60 Hz redraw thread calls `video_refresh()` →
   `update_display_static_bbox` (`video_sdl3.cpp:2847–2861, 2700`): diff `the_buffer`
   against `the_buffer_copy` (software dirty-rect, no page tricks), blit dirty rects via
   `guest_surface` (`SDL_CreateSurfaceFrom(the_buffer, …)`, `:855–867`) → `host_surface`
   → texture → present, and mirror to VNC (`VNCServerUpdate`, `:987`).

So the host blits **from `the_buffer` directly**; there is no copy the guest ever waits on.

### 1.3 Why none of this gives the fidelity profile an early splash

Two distinct gaps:

- **No address properties.** The published `video` node has `AAPL,connector`,
  `device_type=display`, the ndrv property, `model` — and **no `address`/`width`/
  `height`/`linebytes`/`depth`, no `reg`** (`name_registry.cpp:364–373`;
  CORE99-MACHINE-DESCRIPTION §3 row `/video`). A pre-ndrv consumer that reads the display
  node's `address` finds nothing. First pixels appear only at `kOpenCommand` time.
- **The injection chain is the same statically-broken chain as the disk path.** The
  registry nodes are created by `DoPatchNameRegistry`, triggered by the 68k EMUL_OP
  `OP_NAME_REGISTRY` (`emul_op.cpp:779`, planted by `patch_68k()` at
  `rom_patches.cpp:3224–3231`). On the 9.0.1 image the search window `[0x300,0x380)`
  misses — the pattern `70 ff ab eb` sits at **0x2fa** (probed 2026-06-11 against
  `/tmp/rom901_decompressed.bin`) — so it is applied only by the lenient whole-image
  fallback (`find_rom_data`, `rom_patches.cpp:128–138`). It then still requires (i) the
  unproven 68k EMUL_OP dispatch table on parcels (DISK-PATH-RECON §1.4 residue 2 /
  tripwire T2), (ii) `FindLibSymbol("NameRegistryLib")` + `ExecuteNative`
  (`name_registry.cpp:391–403`), and (iii) the boot reaching that stage at all. No 9.0.1
  boot has ever demonstrably run it.

---

## §2. The ROM's early-draw path — who reads what, and where we cut it

### 2.1 The Trampoline harvests the OF screen properties (byte evidence)

The raw ROM file is `<CHRP-BOOT>` header + **ELF Trampoline at file offset 0x5000** +
parcels blob at 0x1bfc0 (magic `prcl`; probed 2026-06-11). Inside the Trampoline's string
pool — i.e. **bootloader code we never run** — sits the screen-harvest call cluster
(file offsets):

```
0x1ac65 finddevice   0x1ac70 /aliases    0x1ac79 getproplen
0x1ac84 screen       0x1ac8b getprop
0x1ac93 address  0x1ac9b width  0x1aca1 height  0x1aca8 linebytes  0x1acb2 depth
```

i.e. OF client-interface calls: resolve the `screen` alias, then `getprop` exactly the
five classic OF display properties. A second display cluster at 0x1a5f2–0x1a64f shows the
Trampoline's display-node handling: `display`, `display-type`, `CRT`/`LCD`, `setprop`,
**`AAPL,gray-page`** (the "screen already grayed" marker), `dimensions` — the gray
boot screen is Trampoline-era, pre-nanokernel. None of these strings exist in the
decompressed 4 MB image (`grep` of `/tmp/rom901_decompressed.bin`: `linebytes`,
`height`, `boot-display` — zero hits), confirming the *ROM image* does not talk to OF
itself: **it receives the display parameters from the Trampoline's handoff**.

`SS_NW_TRAMPOLINE` (the synthesized handoff, `sheepshaver_glue.cpp:1829–2050`) seeds
SPRG0/KDP/HTAB/IRP/NKSystemInfo/Hnfo — and **nothing display-shaped**. The NKSystemInfo
block we build (P6, `:1930–1940`) carries only PhysicalMemorySize + Bank0; whether the
real Trampoline stores the screen quintuple into NKSystemInfo, another KDP field, or the
grafted device tree is **unverified** (residue R1; tripwire T-F5). The M5 row's claim
("the ROM draws … to the OF display node's `address`") is consistent with this evidence
but the exact ROM-side *reader* has not been located.

### 2.2 Does the current 9.0.1 boot path reach a splash stage? No — and nothing ahead is display-shaped yet

The frontier (M6A-WAVE2-SHIM-RECON "Frontier update", NK-syscall-surface closeout) is the
**FE1F service surface**: the boot parks at 0x5000f248 after an F-line NK/DR service trap
selector 0x31. The bounded capstone look there names the loop as **CFM/MixedMode
accelerator slot-fill** (`_GetToolTrapAddress` on $AA7F + an ExpandMem-anchored pointer
array) — the e3e0-family routine is CFM-prep, **not video-adjacent**. No probe, ring, or
MMIO log on any 9.0.1 boot has ever shown a touch of a display-shaped address; the splash
stage is an unknown number of walls past FE1F. (Same schedule caveat as the disk recon:
this de-risks the plan, not the timing.)

### 2.3 The parcels ROM carries Apple's own generic display ndrv (`cofb`)

The prop-parcel TOC includes a `prop` parcel keyed **`cofb` / `display`** (raw file
0x2137dc; match keys at +0x1c/+0x3c of the entry) whose payload is an LZSS-compressed
`driver,AAPL,MacOS,PowerPC` PEF (`Joy!peff` at 0x213859). This is Apple's generic
linear-framebuffer ndrv: on real hardware the Trampoline grafts it onto a display node
whose name/compatible matches `cofb`. SheepShaver discards all prop parcels today
(`rom_decode.hpp:86–100`; DISK-PATH-RECON §2.1). **Implication:** if M5's display node
declares `compatible = "cofb"` and honest linear-FB properties, the eventual
parcels-grafting work (already M5 territory, CORE99 §3 "Parcels/override handshake")
hands the 9.x Display Manager *Apple's own driver* for our aperture — no custom video
ndrv needed on the fidelity path. QEMU's mac99 does the same move with its own blob
(`qemu_vga.ndrv` via fw_cfg, `/tmp/qemu_mac_newworld.c:85,513–520` @ de5d8bfd), and S1
proved 9.2.1 Finder accepts that synthetic node + driver end-to-end.

---

## §3. The `MMIO_APERTURE` gap analysis

### 3.1 What exists

One enum value: `MMIO_APERTURE = 1 // real memory, direct access (future Metal
framebuffer); M1: registry-only` (`include/mmio_bus.h:17–20`). Nothing registers one;
no code path inspects `kind` after registration (`mmio_bus.cpp` stores it and never reads
it). The plan's contract is right — "mapped-aperture regions (real memory, direct access,
dirty-tracked …). Trap-per-write framebuffers are [forbidden]" (MACHINE-LAYER-PLAN §2b,
line ~172) — but it is contract only.

### 3.2 The trap: registering an aperture TODAY would poison all three dispatch paths

`MMIOBusRegister` folds every region into the single hull `[mmio_bus_lo, mmio_bus_hi)`
regardless of kind (`mmio_bus.cpp:44–48`). The hull is what all three fast-path checks
test:

1. **Interpreter:** `vm_is_mmio()` (`kpx_cpu/src/cpu/vm.hpp:43–47`) would route every
   framebuffer load/store through `MMIOBusRead/Write` — a mutex + callback per pixel
   access (`mmio_bus.cpp:86–110`), exactly the forbidden trap-per-write model, and
   `lookup_or_die` aborts unless the aperture also supplies device callbacks.
2. **Host accessors:** `Mac2HostAddr()` **aborts** on any in-hull address
   (`include/cpu_emulation.h:72–83` → `mmio_mac2host_abort`) — but the SDL blit path is
   built on host pointers into the framebuffer (`SDL_CreateSurfaceFrom(the_buffer,…)`).
3. **JIT backpatch thunk:** backpatched sites are shared code; the thunk dispatches any
   in-hull EA to the bus (`ppc-jit.cpp:4886–4930`, `MMIOBusInRange(gaddr)` →
   `MMIOBusRead/Write`) — a hot site that ever touched both SCC and framebuffer would
   serialize framebuffer traffic through the bus mutex.

Also note: a wide aperture (e.g. 16 MB at 0x80000000 vs MacIO at 0xF3000000) would stretch
the hull across ~2 GB of address space, degrading the hull check from "rare device range"
to "half the map", with `MMIO_MAX_REGIONS`-scan lookups on everything inside it.

**Conclusion (the load-bearing design decision):** apertures must NOT join the trap hull.
Either (a) keep a *separate* aperture registry (list + occupancy/telemetry, no effect on
`mmio_bus_lo/hi`), or (b) make hull computation and `MMIOBusInRange` kind-aware. (a) is
simpler and keeps every existing hot path byte-identical. The JIT then needs **no new
mechanism at all**: aperture memory is host-mapped at the guest address
(`vm_mac_acquire_fixed`, `main_unix.cpp:410`), so JIT loads/stores hit it natively under
DIRECT_ADDRESSING — no faults, no backpatch interaction, full speed. That is precisely how
`screen_base` works today (§1.1).

### 3.3 What "backed by real memory + blitted to SDL" still needs

| Piece | Status | Note |
|---|---|---|
| Guest mapping at a fixed base | exists — `vm_mac_acquire_fixed()` (`main_unix.cpp:410`), proven for DR/KDP/RAM regions | 16 KB host-page alignment required (MACHINE-LAYER-PLAN lesson #12); any PCI-hole base of 64 KB alignment satisfies it |
| Aperture registration (occupancy, no-trap) | **missing** — §3.2's separate-registry decision + a unit test asserting an `MMIO_APERTURE` region never enters the trap hull | S |
| Dirty tracking | exists — software dirty-rect diff (`update_display_static_bbox`, `video_sdl3.cpp:2700`) needs only a base pointer + geometry; no page-protection tricks to port | reuse |
| Refresh timer | exists — the 60 Hz redraw thread (`do_video_refresh`, `video_sdl3.cpp:2890+`); no M2-scheduler work needed (an event_sched-driven blit is an option, not a requirement) | reuse |
| SDL surface plumbing | exists — `SDL_CreateSurfaceFrom(ptr,…)` takes any pointer; pointing it (or a second `driver_base`) at `Mac2HostAddr(aperture_base)` is the whole change. VNC mirroring rides along (`:987`) | S |
| Boot-time pixel format | decide once: 8-bit indexed needs the palette path; 32-bit XRGB avoids it. The Trampoline-era gray screen is a `memset` either way | trivial |
| Trampoline-handoff storage of the screen quintuple | **missing + unlocated** (§2.1, residue R1) | the real unknown |

---

## §4. The device-tree node spec (sketch)

Per the binding no-phantom-capabilities rule (MACHINE-LAYER-PLAN §2a/§2f; CORE99 §3
"Display-node constraint"): publish exactly what we back, nothing more.

```
/chosen or /aliases:  screen -> <node path>          (the Trampoline resolves the
                                                      "screen" ALIAS, §2.1 — publishing
                                                      the alias is part of the contract)
<node> (suggest a PCI-shaped path once /pci exists; flat "/video" until then):
  device_type   "display"
  name          e.g. "video" (or "ATY,…"-free neutral name; no phantom ATI caps)
  compatible    "cofb"        ← lets the parcels' own display ndrv match later (§2.3)
  reg / address 0x... aperture base   (one cell each, OF-style; "address" is the
  width         e.g. 640 (or 800)      property the Trampoline getprops — §2.1)
  height        e.g. 480 (or 600)
  linebytes     width * bytes-per-pixel
  depth         8 or 32 (per §3.3 format decision)
  AAPL,gray-page / display-type     optional — the Trampoline SETS these; if we
                                     synthesize its handoff we emulate the effect
                                     (gray fill), not necessarily the props
  driver,AAPL,MacOS,PowerPC          ONLY when the chosen ndrv route lands (stub or
                                     cofb parcel graft) — not needed for the boot draw
  -- forbidden: any DBDMA/dma-channels property (binding constraint, CORE99 §3)
```

**Aperture base candidate:** inside the uni-north **PCI hole at `0x80000000`**
(`/tmp/qemu_mac_newworld.c:334–336` @ de5d8bfd — mac99 maps the PCI memory window there;
real Core99 AGP video BARs live in this window, and QEMU/OpenBIOS assigns the VGA LFB
within it). Suggest `0x81000000`, 16 MB (clear of plausible low BAR assignments, 16 KB
aligned). Guest-space occupancy check (nothing collides): RAM `0/0x10000000 + RAMSize`
(`main_unix.cpp:1724,1766`), ROM+SheepMem+reserved-pool `0x50000000–0x55590000` (§1.1),
DR emulator `0x68070000`, KDP/HTAB/DR-cache `0x68fb0000–0x69010000`
(`cpu_emulation.h:32–37`; sheepshaver_glue P4/P5), MacIO `0xF3000000`, uni-north ctrl
`0xF8000000`, `0xFFC00000` (CORE99 §1). This stays a **candidate**: CORE99 §5 Q9
deliberately defers the base to M5, and tripwire T-F1 pins it from the S1 oracle before
freezing.

**What 9.2's Display Manager will later demand of the SAME node** (the disk recon's
option-(c) precedent applied to video): the S1 QEMU boot is the existence proof that a
synthetic display node + non-Apple ndrv satisfies 9.2.1 through Finder (qemu_vga.ndrv,
§2.3). Our existing `VideoDriverStub` carries the full Driver-Gestalt surface the 8.6 DM
exercises (`video.cpp` csc* handlers), but reaching it on newworld inherits the
three-ways-broken injection risk (§1.3) — whereas the M5 device-tree synthesis can plant
the same PEF property *tree-side* (no EMUL_OP, no NameRegistryLib patch), or graft the
parcels' cofb ndrv. Either way the node above is the one contract both consumers share.

---

## §5. Options, costed + recommendation

| | (a) M5 plan: fixed-base aperture + OF props + host blit | (b) stub-only (no early splash) |
|---|---|---|
| **What it is** | `vm_mac_acquire_fixed` a 16 MB region in the PCI hole; register kind=APERTURE in a non-hull registry; publish §4's node + screen alias in the M5 synthesized handoff; seed the Trampoline-handoff field (once T-F5 locates it); point the existing refresh/blit machinery at it | Do nothing for early boot; rely on the paravirtual ndrv stub appearing whenever the guest's NDRV framework loads it |
| **What exists** | everything in §3.3's "exists" rows; the S1 oracle for node shape; the cofb parcel for the eventual driver | the stub + video.cpp backend, proven for 8.6/OldWorld |
| **What's missing** | separate aperture registry (S); node publication (M5 work anyway); the handoff-field RE (T-F5 — the real cost); pixel-format decision | nothing — but see risk |
| **What breaks / risk** | low — every piece has a running precedent in-tree | **M7 debugs a black screen** (the plan's own words): no visual signal between now and full NDRV-framework liveness; the injection chain is unproven on parcels (§1.3); if any ROM-side early-draw or Display-Manager gate *requires* the screen properties (unknown — T-F2), the boot wedges invisibly |
| **Size class** | M (matches the plan's M5 estimate) | 0 now, unbounded debugging cost later |
| **Fidelity** | the aperture + `cofb` node is the same contract real hardware offers; upgrade path to the parcels ndrv and the M8+ Metal seam is the same region kind | bypasses the boot-display contract entirely |

### Recommended default: **(a)**, shaped by the three findings

1. Build the aperture as a **non-hull** registration + `vm_mac_acquire_fixed` (§3.2) —
   encode it first as a unit test (an APERTURE region must leave `mmio_bus_lo/hi` and all
   three dispatch paths untouched).
2. **Reuse, don't rebuild, the blit**: the SDL machinery only needs a pointer + geometry
   (§3.3). The boot aperture can even share the reserved pool's machinery later, but a
   fixed PCI-hole base is what the device tree must advertise.
3. Publish §4's node with `compatible "cofb"` so the parcels' own display ndrv becomes
   reachable when prop-grafting lands — the highest-fidelity endgame at zero extra driver
   cost (§2.3).
4. Treat the Trampoline-handoff storage (T-F5) as the milestone's Task-0 RE item — it is
   the only genuinely unknown piece.

### Tripwires (the eventual milestone's Task 0 — zero/near-zero code)

| # | Tripwire | What it decides | Cost |
|---|---|---|---|
| T-F1 | Dump the screen node from the S1 QEMU mac99 setup (OF prompt `dev screen .properties`, or a Linux guest's offb dmesg): node path, `compatible`, `address`, assigned BAR | Pins §4's base + property set against the oracle that already boots 9.2.1; resolves CORE99 §5 Q9 | zero |
| T-F2 | S1 negative experiment: same QEMU boot with `-vga none -nographic` | Whether 9.2.x boots *at all* without a display node — bounds option (b)'s viability as a stopgap and tells us if any boot gate hard-requires the screen props | zero |
| T-F3 | `SS_ROM_PATCH_TRACE=1` + the disk recon's T2 (`SS_EMULOP_COUNTS`) on the next sanctioned 9.0.1 boot: does `name_reg` hit (relocated, expect @0x2fa) and does `OP_NAME_REGISTRY` ever fire? | Whether the stub-injection chain is even alive on parcels — gates how much of §1.2 survives onto newworld | zero (exists) |
| T-F4 | Register the *candidate* aperture range as a TRAPPED loud-stub region for one diagnostic boot (M1 bus + per-region fault stats, exists) | First guest touch = the early-draw consumer found, with PC — converts §2.1's "who reads the address" from inference to observation; also validates the base choice | S |
| T-F5 | Bounded capstone-PPC session on the Trampoline ELF (raw ROM 0x5000+, around the 0x1ac65 string cluster's referencing code): where do the five getprop results get STORED in the handoff structures? | The biggest unknown — the field M5's synthesized handoff must seed | S (one RE session) |
| T-F6 | Unit test: `MMIOBusRegister(kind=MMIO_APERTURE)` must not extend the trap hull / not dispatch / not abort `Mac2HostAddr` | Encodes §3.2's design decision before any consumer exists; fails today by design | S |

### Honest residues

0. **T-F3's dispatch half is RETIRED (2026-06-11):** EMUL_OP dispatch is **ALIVE** on the
   9.0.1 parcels boot (OP_XPRAM1 executed via the mirror table slot; `name_reg` patch hit
   relocated @0x2fa as predicted, though `OP_NAME_REGISTRY` itself has not yet fired —
   frontier, not machinery) — see DISK-PATH-RECON.md §6 for the evidence chain.
1. **R1 (biggest unknown):** where the Trampoline stores the harvested screen quintuple
   (NKSystemInfo field? KDP? the grafted tree?) and which ROM-side code reads it for the
   early draw — both unlocated. §2.1's evidence proves the harvest exists; the storage
   and the consumer are inference. T-F4 + T-F5 are the closers.
2. **Happy-Mac authorship is unresolved:** the gray screen is Trampoline-era
   (`AAPL,gray-page`), but whether the happy-Mac icon itself is drawn by the Trampoline
   or by the ROM image post-handoff was not determined. Affects only how much of the
   "splash" M5 must host-synthesize vs merely enable.
3. **No live verification of anything in §1.3/§2.2** — same caveat class as the disk
   recon: static analysis of dumps + code, zero boots run for this recon.
4. **The aperture base is a candidate, not a decision** (CORE99 §5 Q9 stays open until
   T-F1); likewise the pixel-format choice (§3.3) and whether the M5 node should be
   PCI-pathed (needs a /pci skeleton that doesn't exist yet) or flat.
5. **The cofb ndrv's actual requirements** (what properties/registers Apple's generic
   driver probes beyond the five harvest props) were not reverse-engineered; if it
   demands more than a linear FB, the VideoDriverStub-via-tree fallback (§4) covers it.
6. **9.2-specific Display Manager behavior** (mode lists, Display Manager 2.x gestalt,
   multi-monitor expectations) not re-derived; S1's Finder boot is the only oracle used.
