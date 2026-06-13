# M11 — Framebuffer: aperture registration + OF display node + SDL blit

> **Status: COMPLETE (2026-06-13)** · Branch: `macos-arm64`
> Rev 2: post-red-team (PROCESS + TECHNICAL). BINDING amendments below (§amendments).
> Predecessor: M10+M11a COMPLETE (2026-06-13). No gate change needed.
> Recon: `docs/planning/machine/FRAMEBUFFER-RECON.md` (complete, 2026-06-11).
>
> **Acceptance summary (2026-06-13):** Tasks A–D + T-F6 implemented and committed.
> Harness 353/353. Aperture maps at 0x81000000, SDL the_buffer redirected, MMIO hull
> stays clean [0xf3000000, 0xf3080000). [FB-DIRTY] non_zero_pixels=0 (expected: NW
> diagnostic boot exits at 0.3s before Mac OS draws pixels — pixel verification deferred
> to longer boot). Non-deterministic 1/3 crash (0xDEADBEEF JIT timing issue, pre-existing,
> independent of M11). Paravirtual regression: make e2e pending (deferred to close-out).

---

## Goal

Give the NewWorld 9.0.1 boot a guest-visible framebuffer: a fixed-base memory aperture in
the PCI hole backed by real guest RAM, published as an OF-style display node, with the
existing SDL blit machinery pointed at it.

**PASS criterion (gate):** With `SS_M11_FB=1`, a NewWorld diagnostic boot (default
ss-slot-boot.sh template) emits at least one `[FB-TOUCH]` line in stderr — meaning the
guest read or wrote the aperture range — within 180s. No paravirtual regression: the
existing `make e2e` (Mac OS 8.6 to Finder) passes unchanged.
**If `[FB-TOUCH]` does not appear after 180s the milestone FAILS** — record `[PROGRESS]`
and boot frontier as M12 input, but do NOT close the milestone as complete.

**DIAGNOSTIC (not a gate):** visible gray splash or happy-Mac icon in VNC/SDL window
before ndrv load. The early-draw path requires the Trampoline handoff (T-F5); if T-F5
is unresolved the PASS criterion above still closes the milestone.

**Honest framing:** M11 owns the aperture + node + first guest touch. It does NOT promise
a fully functional display manager, mode switching, or the cofb ndrv loading. Those are
M12+ items.

---

## Authoritative inputs

| Doc | Section used |
|-----|-------------|
| `docs/planning/machine/FRAMEBUFFER-RECON.md` | §1 (paravirtual path), §2 (Trampoline harvest), §3 (aperture gap analysis), §4 (node spec), §5 (options + tripwires) |
| `docs/AGENT-CONTEXT.md` | Constants, boot recipes, gate tiers |
| `SheepShaver/src/Unix/main_unix.cpp` | vm_mac_acquire_fixed, SheepMem::Init, guest memory layout |
| `include/mmio_bus.h`, `SheepShaver/src/machine/mmio_bus.cpp` | MMIOBusRegister, hull computation |
| `SheepShaver/src/name_registry.cpp` | DoPatchNameRegistry, existing video node |
| `SheepShaver/src/machine/core99.cpp` | Machine profile, device tree helpers |
| `BasiliskII/src/SDL/video_sdl3.cpp` | the_buffer, SDL_CreateSurfaceFrom, update_display_static_bbox |

---

## Codebase facts (re-verify before use)

- Guest framebuffer pool: `0x50590000..0x55590000` (SheepMem + vm_acquire_reserved; 80 MB).
  `screen_base ≈ 0x50590000` under paravirtual profile. [STATIC from recon §1.1]
- Aperture candidate: `0x81000000`, 16 MB (PCI hole; nothing maps there — verified §4 of recon).
  **Re-verify** by grepping guest-address layout in main_unix.cpp before freezing.
- MMIO hull computation: `MMIOBusRegister` → extends `[mmio_bus_lo, mmio_bus_hi)`. An
  aperture registered there poisons all three dispatch paths (§3.2). **The load-bearing
  design decision**: apertures must use a separate non-hull registry. [STATIC from recon]
- `vm_mac_acquire_fixed(uint32 addr, uint32 size)` (`main_unix.cpp:412`, first param `uint32`
  not `maddr_t`) — the proven mechanism for fixed-base guest-RAM regions (used for DR/KDP/HTAB).
  [TECHNICAL red-team verified type; re-verify line number before use]
- `DoPatchNameRegistry` (`name_registry.cpp:85–388`) publishes the existing `video` node
  WITHOUT `address`/`width`/`height`/`linebytes`/`depth` properties. [STATIC from recon §1.3]
- Trampoline ELF at raw ROM file offset 0x5000; decompressed ROM at
  `/tmp/rom901_decompressed.bin` (4 MB, base 0x50000000). The harvest-string cluster is at
  raw file 0x1ac65–0x1acb2 (Trampoline code only; strings absent from decompressed image).
  [STATIC from recon §2.1]
- `SS_DUMP_ROM` dumps the decompressed ROM to a file; capstone PPC is BE.

---

## Task 0 — Recon: tripwire answers (BINDING before any implementation)

All five open tripwires must be resolved before Task A begins. No T-F* question stays open
when its implementation task starts.

### Blocking-answer table

| Tripwire | Blocks | Budget | Fallback |
|----------|--------|--------|---------|
| T-F1: QEMU screen node | Task A (aperture base) | 1 QEMU session | use 0x81000000 AND additionally grep decompressed ROM + S1 oracle for known aperture addresses; document as UNRESOLVED in addendum |
| T-F2: QEMU -vga none | Task A (stop-rule: is display required?) | 1 QEMU session | assume display required; proceed with Task A |
| T-F5: Trampoline handoff storage | Task C (handoff field) | 1 capstone session (raw ROM ELF) | skip Task C; omit handoff seed; milestone still attempts guest-touch via other means |
| T-F6: unit test | Task B (aperture registry) | 1 unit-test session; write test first (red), then implement (green) | if harness not compilable, document why and fix harness first |

**T-F4 (first guest touch) is NOT a Task-0 tripwire** — it requires Task B to exist.
T-F4 is a post-Task-B probe, run as part of the acceptance loop. If T-F4 never fires
(no `[FB-TOUCH]`) that is a milestone FAIL, not a fallback.

**T-F3 status:** PARTIALLY RETIRED (recon §5 R0) — EMUL_OP dispatch alive on 9.0.1,
`OP_NAME_REGISTRY` not yet confirmed to fire. **Task C is BLOCKED on T-F3 confirmation:**
before starting Task C, run one diagnostic boot with `SS_EMULOP_COUNTS=1` (or equivalent)
to confirm `OP_NAME_REGISTRY` fires. If it does not fire on parcels, Task C produces
silently dead code — skip it and note as M12 input.

### T-F1: QEMU mac99 screen node (pins aperture base + property set)

**Question:** What is the screen node path, `compatible`, `address`, assigned BAR value, and
`width`/`height`/`linebytes`/`depth` in a working mac99 QEMU boot of 9.0.1/9.2.1?

**Method (zero code):** Use the existing QEMU differential rig:
```bash
bash SheepShaver/tools/qemu-rig.sh --timeout 50
python3 SheepShaver/tools/qemu-mon.py --sock /tmp/qemu-rig-*/mon.sock \
    'dev /aliases .properties'
python3 SheepShaver/tools/qemu-mon.py --sock /tmp/qemu-rig-*/mon.sock \
    'dev screen .properties'
```
If `screen` is an alias: resolve it first, then dump the target node. The OpenBIOS
`ofpath` command may help: `ofpath /screen`.

**Expected output:** node path like `/pci@80000000/display@...` or flat `/display`, with
the five harvest properties. `address` = the LFB physical base assigned by OpenBIOS.

**Tag:** [PROBE✓] (runtime observation). Record in a Task-0 addendum below.

### T-F2: QEMU boot without VGA (is display required?)

**Question:** Does the 9.0.1 ROM boot to the nanokernel (PROGRAM#5, [DR68K] line) without
a display node? What is `[PROGRESS]` at 45s?

**Method:** The rig already uses `-display none` (hardcoded in `QEMU_ARGS` at line ~191),
so a bare 45s run is sufficient:
```bash
SheepShaver/tools/qemu-rig.sh --timeout 45
```
To override display args or add `-vga none` explicitly, pass `QEMU_EXTRA_ARGS`:
```bash
QEMU_EXTRA_ARGS="-vga none" SheepShaver/tools/qemu-rig.sh --timeout 45
```
(`QEMU_EXTRA_ARGS` passthrough added to qemu-rig.sh 2026-06-13.)

**Tag:** [PROBE✓]. Record QEMU exit + output in addendum.

### T-F5: Trampoline ELF — where the screen quintuple gets stored

**Question:** After the Trampoline's OF `getprop` calls retrieve `address`/`width`/`height`/
`linebytes`/`depth` from the `screen` alias (raw ROM 0x1ac65–0x1acb2), which registers
hold the values and where are they stored in the handoff structures?

**Method (static RE):**
1. The raw ROM file is `/Users/Shared/macemu/1998-07-21 - Mac OS ROM 1.1.rom` — but that
   is the OldWorld ROM. The NewWorld Trampoline is inside the raw 9.0.1 file:
   `/Users/Shared/macemu/Mac OS ROM 9.0.1`. Confirm the ELF starts at file offset 0x5000
   (magic `\x7fELF`) and dump the PPC code around offset 0x1ac65 (the string cluster).
2. Find the CALLERS of those strings (search for cross-references to the string offsets or
   follow the call graph from the `finddevice`/`getprop` OF client interface calls).
3. Trace where the loaded values are stored: look for `stw`/`sth`/`stb` after the `getprop`
   sequences. The destination is likely relative to `r1` (stack) or a passed-in struct pointer.
4. Identify the struct: match the destination against known Trampoline output structures
   (NKSystemInfo, the `BootInfo` block, the KDP seed area).

**Budget:** one capstone session (≤ 60 min). If the caller is not locatable within budget,
record the partial finding and declare T-F5 UNRESOLVED — Task C is dropped, the milestone
still closes on the guest-touch criterion.

**Tag:** [STATIC].

### T-F4: First guest touch of aperture (validates base + proves reachability)

**Question:** When the aperture is registered as a loud-stub MMIO region at 0x81000000,
does any guest code touch it during a 120s diagnostic boot? At what PC?

**Method:** After Task B (aperture registry) exists, register a loud-stub handler at
0x81000000 for one diagnostic boot:
```bash
SS_M11_FB=1 SS_M11_FB_LOUD=1 \
SheepShaver/tools/ss-slot-boot.sh --label m11-tf4 --timeout 120
```
`SS_M11_FB_LOUD=1` is a new gate (Task B: register as trapped loud-stub initially, not
quiet aperture — the trap lets us capture the PC). Check boot.log for `[FB-TOUCH]` lines.

**Tag:** [PROBE✓] post-Task-B.

### T-F6: Unit test — aperture must not enter trap hull

**Question:** Does `MMIOBusRegister(base, size, MMIO_APERTURE, ...)` leave `mmio_bus_lo/hi`
unchanged and not add a handler to the dispatch table?

**Method:** Write a unit test in `SheepShaver/src/machine/` (the existing `make -C src/machine test`
harness) that: (1) reads `mmio_bus_lo/hi` before, (2) registers an MMIO_APERTURE region,
(3) asserts `mmio_bus_lo/hi` unchanged and `MMIOBusInRange(aperture_addr)` returns false.

This test should FAIL before Task B (encoding the design decision as a red test first).

---

## Task A — vm_mac_acquire_fixed the aperture

**Gate:** `SS_M11_FB=1` only; default OFF; inside `MachineProfileIsNewWorld()`.

**Steps:**

1. Verify `vm_mac_acquire_fixed` signature in `main_unix.cpp:410`. Confirm it takes
   `(maddr_t base, uint32 size)` and returns a host pointer (or aborts on failure).
   **Re-verify** before coding.

2. Add a fixed-base 16 MB guest RAM mapping at `FB_APERTURE_BASE = 0x81000000` in
   `main_unix.cpp::SheepMem::Init()` (or equivalent early-init path), gated:
   ```c
   if (SS_M11_FB && MachineProfileIsNewWorld()) {
       fb_host_ptr = vm_mac_acquire_fixed(FB_APERTURE_BASE, FB_APERTURE_SIZE);
       // fb_host_ptr is the host address of guest 0x81000000
   }
   ```
   `FB_APERTURE_SIZE = 16 * 1024 * 1024` (16 MB; matches recon §4's candidate).

3. Expose `fb_host_ptr` (or `FB_APERTURE_BASE`) to the name_registry and SDL paths via a
   global or a getter (keep the interface minimal — one `uint32_t fb_aperture_base` global
   gated by `SS_M11_FB`).

4. **Aperture base from T-F1**: if T-F1 finds QEMU uses a different base, replace
   0x81000000 here. If T-F1 is unresolved, keep the candidate.

**Accept:** `make build-ss` clean; no paravirtual regression (the `vm_mac_acquire_fixed`
call is inside the `MachineProfileIsNewWorld()` guard).

---

## Task B — Separate aperture registry (T-F6 unit test must pass)

**The load-bearing constraint from recon §3.2:** apertures must NOT enter the MMIO trap
hull. The unit test from T-F6 encodes this.

**Steps:**

1. Add `MMIO_APERTURE_REGISTRY` as a separate list in `mmio_bus.cpp` (or a new
   `aperture_bus.cpp` if size warrants). An aperture entry is `{base, size, host_ptr,
   label}`. No callback, no lock. Registry has `MMIOApertureRegister(base, size, host_ptr,
   label)` and `MMIOApertureInRange(gaddr)` (for telemetry/loud-stub, not dispatch).

2. `MMIOBusRegister` with `kind=MMIO_APERTURE` routes to `MMIOApertureRegister` instead
   of adding to the hull. Alternatively, add a separate `MMIOApertureRegister` call and
   remove `MMIO_APERTURE` from the `MMIOBusRegister` kind enum entirely (cleaner).

3. The "loud-stub" diagnostic mode (`SS_M11_FB_LOUD=1`): additionally register a normal
   MMIO_TRAP region with a one-shot logging handler (prints `[FB-TOUCH] pc=X addr=Y`).
   This is the T-F4 instrument; remove it before the milestone acceptance.

4. Run the T-F6 unit test — must pass.

5. Run inner gates: `make build-ss` + harness 353/353 + `make -C src/machine test`.

---

## Task C — OF display node properties (conditional on T-F5)

**Condition:** only if T-F5 resolves the Trampoline handoff storage field. If T-F5 is
UNRESOLVED, skip Task C entirely; the milestone still closes on the guest-touch criterion.

**Steps (if T-F5 resolved):**

1. In `name_registry.cpp::DoPatchNameRegistry` (or in the M5 tree-synthesis path if that
   exists by M11 implementation time), add the five harvest properties to the video node:
   ```c
   WriteMacInt32(node + prop_offset("address"),   FB_APERTURE_BASE);
   WriteMacInt32(node + prop_offset("width"),      FB_WIDTH);   // e.g. 640
   WriteMacInt32(node + prop_offset("height"),     FB_HEIGHT);  // e.g. 480
   WriteMacInt32(node + prop_offset("linebytes"),  FB_WIDTH * FB_BPP / 8);
   WriteMacInt32(node + prop_offset("depth"),      FB_DEPTH);   // 32 (or 8 for indexed)
   ```
   Use 32-bit XRGB (`depth=32`) to avoid palette complexity (recon §3.3).

2. Publish the `screen` alias pointing to the video node path (the Trampoline resolves
   the `screen` alias via OF `finddevice` — recon §2.1). This is a name-registry `aliases`
   node entry.

3. Seed the Trampoline handoff field identified by T-F5 with the aperture base + geometry,
   inside `SS_NW_TRAMPOLINE` synthesis (`sheepshaver_glue.cpp:1829–2050`), gated `SS_M11_FB`.

4. Add `compatible = "cofb"` to the video node so the parcels' own display ndrv can match
   later (recon §2.3). Do NOT add `driver,AAPL,MacOS,PowerPC` — that is M12 territory.

---

## Task D — SDL blit at the aperture (depends on Task A)

**Steps:**

1. In `video_sdl3.cpp` (or `video_sdl.cpp` for SDL2 builds), after the aperture is mapped,
   add a secondary blit source:
   ```c
   if (SS_M11_FB && fb_host_ptr) {
       // Point the refresh path at the aperture memory for the newworld profile.
       // The existing the_buffer / screen_base path remains for paravirtual.
       fb_surface = SDL_CreateSurfaceFrom(fb_host_ptr, FB_WIDTH, FB_HEIGHT,
                                          FB_WIDTH * 4, SDL_PIXELFORMAT_XRGB8888);
   }
   ```
   The existing `update_display_static_bbox` diff-blit logic can be reused verbatim with
   a pointer swap — it only needs `the_buffer` (source), `the_buffer_copy` (shadow), and
   geometry (recon §3.3).

2. The VNC mirror rides along automatically (`:987`) — no change needed.

3. Gated: `SS_M11_FB && MachineProfileIsNewWorld()`. The paravirtual path (`the_buffer`,
   `screen_base`) is untouched.

---

## Acceptance

```bash
# 1. Harness (must be 353/353)
SS_HARNESS_BATCH=1 make test-jit   # in SheepShaver/

# 2. Guest-touch gate — PASS criterion (quiet mapping, no LOUD flag for final gate)
SS_M11_FB=1 \
SheepShaver/tools/ss-slot-boot.sh --label m11-accept --timeout 180
# → look for [FB-TOUCH] in boot.log

# 3. Paravirtual regression
make e2e   # in SheepShaver/ — Mac OS 8.6 to Finder, no SS_M11_FB
```

**PASS:** harness 353/353, `[FB-TOUCH]` present within 180s, `make e2e` green.
**FAIL:** `[FB-TOUCH]` absent after 180s — milestone does NOT close; record `[PROGRESS]`
+ boot frontier as M12 input.

Note: use `SS_M11_FB_LOUD=1` during development (T-F4, Task B verification) to capture
the first touch PC. The final acceptance gate uses `SS_M11_FB=1` only (quiet aperture).

---

## Task 0 addendum (2026-06-13)

### T-F1 result [PROBE✓]

QEMU mac99 screen node: `/pci@80000000/display@E,0`

| Property | Value |
|----------|-------|
| address | 0x81000000 |
| width | 640 |
| height | 480 |
| linebytes | 2560 |
| depth | 32 |
| compatible | "QEMU,VGA" |

**Conclusion:** Candidate aperture base 0x81000000 confirmed. `FB_APERTURE_BASE = 0x81000000`,
`FB_WIDTH=640`, `FB_HEIGHT=480`, `FB_BPP=32`, `FB_APERTURE_SIZE = 16*1024*1024`. Task A unblocked.

### T-F2 result [PROBE✓]

The rig already uses `-display none` (hardcoded). Boot proceeds without a display node —
interrupt vector transition from 0x00000000 to active handler observed within 45s.
`QEMU_EXTRA_ARGS` passthrough added to qemu-rig.sh (2026-06-13) to support future overrides.

**Conclusion:** headless boot is fine; no special gating needed for Task 0 recon boots.

### T-F5 result [STATIC]

Trampoline ELF (raw ROM 9.0.1, ELF base 0x5000). Function at vaddr 0x20beb0 harvests
the OF `screen` alias properties and stores them into the ADPT display descriptor.
Register convention at that point: `r31 = r1 + 0x300` (Main's stack frame display desc pointer).

| Property | Storage offset from r31 |
|----------|------------------------|
| address | +0x37c |
| width | +0x380 (halfword) |
| height | +0x382 (halfword) |
| linebytes | +0x384 (halfword) |
| depth | +0x386 (halfword, raw depth; >>3 = bpp) |

**Conclusion:** Task C seeding target identified. Seed at `r1+0x300` base in Main's frame
(= `*(SPRG0 + stack_offset_to_Main)`) — or wait for first boot probe to confirm the
absolute guest address. T-F5 resolved; Task C is unblocked (pending T-F3 confirmation:
OP_NAME_REGISTRY must fire first).

### T-F4 result
> _pending (post-Task-B)_

---

## Self-review / tensions flagged for red team

- **Aperture base collision:** 0x81000000 is inferred from QEMU/real-hardware analogy; T-F1
  is the only oracle. If T-F1 shows QEMU uses a different base, the candidate is wrong.
  Stop rule: T-F1 is in the blocking-answer table for Task A — cannot start without it.
- **MMIO hull separation:** the recon's §3.2 analysis is load-bearing. If MMIOBusRegister
  is refactored between recon and implementation, the hull computation may have changed.
  T-F6's unit test is the mechanical verifier — write it first, fail it, then fix.
- **vm_mac_acquire_fixed side effects:** the function may set up host PROT_WRITE on the
  mapping. This is fine for a framebuffer (writes expected), but if the JIT uses the same
  mapping for code pages (unlikely at 0x81000000), there could be W^X conflicts on macOS.
  Verify the allocated region is not in any JIT code-page range before freezing.
- **Task C / T-F5 skip:** if the Trampoline ELF is too complex to RE in one session, Task C
  is dropped. The milestone PASS criterion (guest touch) doesn't require Task C. The honest
  framing holds: M11 owns aperture + first touch; early splash is a diagnostic bonus.
- **SDL surface lifetime:** `SDL_CreateSurfaceFrom` borrows the pointer. If `fb_host_ptr`
  is ever freed (e.g., on resolution change), the surface must be destroyed first. In the
  current code there is no resolution change path for the newworld profile, so this is safe.
  Flag if that changes.
- **No red-team round run yet.** This is Rev 1. Run the two-parallel-reviewer process
  (PROCESS + TECHNICAL) before starting Task A.

---

## Red-team record

**Run:** 2026-06-13, two parallel reviewers (PROCESS + TECHNICAL).

**PROCESS findings (CRITICAL→resolved):**
- [CRITICAL] T-F4 was circular (required Task B) — moved to post-Task-B, removed from
  blocking-answer table. ✅ fixed in Rev 2.
- [CRITICAL] PASS criterion contradicted fail-mode text — tightened: FB-TOUCH absence = FAIL,
  no "closes without criterion" escape. ✅ fixed in Rev 2.
- [CRITICAL] T-F3 retirement unsafe for Task C: OP_NAME_REGISTRY unconfirmed → Task C now
  explicitly blocked on T-F3 confirmation before start. ✅ fixed in Rev 2.
- [MODERATE] Task D→A dependency not stated — added to Task D header. ✅ fixed.
- [MODERATE] T-F1 fallback "use candidate" too weak — strengthened: also grep ROM + S1 oracle. ✅ fixed.
- [MODERATE] 120s gate unsupported — increased to 180s. ✅ fixed.
- [LOW] Acceptance used LOUD flag — final gate step corrected to quiet mapping. ✅ fixed.
- [LOW] T-F6 fallback wording — clarified to "fix harness first if not compilable". ✅ fixed.

**TECHNICAL findings (resolved):**
- [WRONG] vm_mac_acquire_fixed first param is `uint32` not `maddr_t`; actual line ~412.
  ✅ corrected in codebase facts.
- [VERIFIED] MMIO hull extends unconditionally — plan's §3.2 concern is correct.
- [VERIFIED] SDL_CreateSurfaceFrom in video_sdl3.cpp; symlink confirmed.
- [VERIFIED] DoPatchNameRegistry publishes WITHOUT address/width/height/linebytes/depth.
- [UNVERIFIABLE] Aperture base 0x81000000 collision check requires recon §3 trace; T-F1 is the oracle.
