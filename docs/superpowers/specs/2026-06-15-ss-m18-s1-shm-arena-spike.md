# SPEC — Dolphin SHM-arena foundation feasibility spike (S3-Task-0 item-0)

> **Status:** build-ready spec. NK-INDEPENDENT, READ-ONLY-of-the-emulator, gated-off.
> **Origin:** pulled forward from S3-Task-0 item-0 per
> `docs/planning/newsheep/ADVISORY-2026-06-15-s1-mmu-dolphin-review.md` §3 + Disposition #3.
> **Owner constraint:** this is a standalone PASS/FALSIFY probe in the model of
> `SheepShaver/src/machine/test_paged_mmu.cpp` — no SS wiring, no NATMEM edit, no NK, no kpx_cpu link.

## 1. Purpose (the single load-bearing unknown)

Prove **or falsify**, on **macOS arm64 (16 KB host page)**, the Dolphin "separate SHM-backed
arena with aliased views" primitive that the deferred window-MMU plan rejected on a
**topology-mismatch** argument
(`docs/superpowers/plans/2026-06-15-ss-m18-s1-taskB-window-build.md` rev-2). The rejection was
correct *for macemu's current single-`vm_allocate` NATMEM* (store == window, so in-place
`vm_remap VM_FLAGS_OVERWRITE` holes the store). Dolphin does NOT do that: it builds a SEPARATE
physical backing and aliases it into multiple VAs, so a remap touches a VIEW and never holes the
store (`docs/planning/newsheep/DONOR-NOTES.md` Donor 1 §Reservation, ~L45–51 and §Apple-Silicon
~L96–99).

This spike decides — **before S3 commits to the window approach** — whether that primitive is
viable on our actual platform. PASS ⇒ S3 inherits a proven arena. FALSIFY ⇒ S3 knows the softmmu
path is mandatory before construction starts (MILESTONE-WORKFLOW §3, de-fuse named surprises).

## 2. Non-goals / Stop-rule (BINDING)

- **NO** SheepShaver wiring. **NO** edit to NATMEM / `main_unix.cpp` / `vm_alloc.cpp`. **NO** NK,
  no boot, no kpx_cpu link, no `RMEMBASE` site touched.
- **NO** BAT/SR/SDR1 decode, no `paged_mmu` link — the *translation* core is already proven
  (Task A). This spike proves only the **host-VM aliasing/remap primitive**.
- The new file MUST be **inert**: gated out of every emulator build, linked into nothing but its own
  standalone test target (exactly like `test_paged_mmu` / `test_openfirmware_ci`). Paravirtual stays
  byte-identical because the file is structurally unreachable from the binary.
- Synthetic mappings ONLY. No reliance on guest RAM, no `Mac2HostAddr`, no real BAT tables.

## 3. File location + build target (pinned)

- New file: `SheepShaver/src/machine/test_shm_arena_spike.cpp` (self-contained `main()`, the probe
  AND its assertions in one TU, matching `test_paged_mmu.cpp`).
- Makefile: `SheepShaver/src/machine/Makefile` — add `test_shm_arena_spike` to `TESTS`, to the
  `test:` run list, and a build rule:
  ```
  # SS_M18 S3-Task-0 item-0: Dolphin SHM-arena platform-feasibility spike.
  # Standalone (no kpx_cpu, no paged_mmu link); proves the mach VM aliasing/remap
  # primitive on macOS arm64. Inert: linked into no emulator binary.
  test_shm_arena_spike: test_shm_arena_spike.cpp
  	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ test_shm_arena_spike.cpp
  ```
  (No extra .cpp deps — the probe calls mach APIs directly, like `vm_alloc.cpp` does.)
- Uses `<mach/mach.h>`, `<mach/mach_vm.h>`, `<pthread.h>`, `<unistd.h>`. macOS-arm64-only: guard the
  body with `#if defined(__APPLE__) && defined(__MACH__) && defined(__aarch64__)`; on any other host
  `main()` prints `SKIP (non-macOS-arm64)` and returns 0 (so the cross-platform `make test` is not
  broken, but the probe only *attests* on the target).

## 4. Citation ritual (BINDING, at the porting site)

Top-of-file comment must carry the backport-hygiene triple (DONOR-NOTES.md preamble L5–7):
- Dolphin repo + `Source/Core/Common/MemArena.cpp` (the macOS `#ifdef`:
  `mach_make_memory_entry_64` / `vm_map`) + `Source/Core/Core/HW/Memmap.cpp`
  (`InitFastmemArena`, `MapInMemoryRegion`).
- **Full pinned SHA `144d19433aa734c19c34e5978a1b817d2aa12663`** (GPLv2).
- Mark **"needs validation"** in the comment AND in the CHANGELOG entry. Note this is a
  *reimplemented-to-spec synthetic probe*, not vendored Dolphin code.

## 5. The probe — five sub-tests, each with PASS predicate + FAIL/UNPROVABLE disposition

Constants: `N = 256 MiB` realistic guest-RAM size (a 32 MiB proxy is acceptable if 256 MiB
allocation is flaky in CI — record which). `HOST_PAGE = sysconf(_SC_PAGESIZE)` (assert == 16384 on
target). All `mach_vm_*` calls check `kern_return_t`; any unexpected `KERN_*` prints
`mach_error_string` + the dead-end disposition below and the whole probe returns non-zero (FALSIFY).

### S0 — Page-size attestation (precondition)
- **Do:** read `_SC_PAGESIZE`.
- **PASS:** `== 16384`. (If `== 4096` the host is not arm64-16K; print `SKIP` — the spike is not
  attesting the real platform and must not claim PASS.)

### S1 — SHM-backed physical store
- **Do:** `mach_make_memory_entry_64(mach_task_self(), &size, 0, VM_PROT_READ|VM_PROT_WRITE, &mem_entry, MACH_PORT_NULL)`
  with `size = N`, producing ONE named memory entry (the physical backing).
- **PASS:** `KERN_SUCCESS` AND the returned `size >= N` (rounded up to 16 KB).
- **FAIL disposition:** `KERN_INVALID_ARGUMENT`/`KERN_FAILURE` ⇒ **DEAD-END-A**: macOS will not vend
  a named entry of this size/prot ⇒ Dolphin SHM model unavailable ⇒ S3 softmmu-mandatory.

### S2 — Aliased RW views (the alias proof)
- **Do:** `mach_vm_map` the SAME `mem_entry` at TWO distinct, kernel-chosen VAs
  (`VM_FLAGS_ANYWHERE`): `view_phys` and `view_window`, both `cur_prot = max_prot = RW`,
  `offset = 0`, `copy = FALSE`.
- **PASS predicate (load-bearing):** write a sentinel `0xA5A5A5A5` at `view_phys[k]` (k a 16 KB-aligned
  offset and a few unaligned ones) and read the SAME value back through `view_window[k]`; then write a
  *different* sentinel through `view_window` and read it through `view_phys`. **Both directions must
  observe the other's write** (same physical page aliased into two VAs). `view_phys != view_window`.
- **FAIL disposition:** writes don't alias (reads return stale/zero) ⇒ **DEAD-END-B**: `vm_map` of a
  named entry produced a COPY, not an alias ⇒ the whole separate-arena trick is impossible ⇒
  softmmu-mandatory. `KERN_NO_SPACE` ⇒ retry with smaller N / record.

### S3 — Safe re-alias of a sub-range (the window operation — THE crux)
- **Do:** pick a 16 KB-aligned sub-range `[off, off+span)` of `view_window` (`span` a few host pages).
  Re-map THAT sub-range of the WINDOW to a DIFFERENT offset `off2` of the SAME `mem_entry`, using
  `mach_vm_map(... address=view_window+off, size=span, mask=0, flags=VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE,
  object=mem_entry, offset=off2, copy=FALSE, RW, RW, VM_INHERIT_NONE)`.
  (If `VM_FLAGS_OVERWRITE` on `vm_map` is rejected, fall back to the documented two-step
  `vm_remap(...VM_FLAGS_OVERWRITE...)` form from a freshly-mapped alias of `mem_entry@off2` — record
  which form succeeded; this IS the BAT-context-switch analogue.)
- **PASS predicate (three assertions, ALL required):**
  1. **New page lands:** a bare read of `view_window[off]` now returns the bytes physically stored at
     `mem_entry` offset `off2` (pre-seed `off2`'s page through `view_phys` with a distinct sentinel
     before the remap; the bare window read must observe it).
  2. **Store NOT holed:** `view_phys[off2]` still reads the sentinel — the physical store is intact;
     the remap moved a VIEW, not the backing. Additionally `view_phys[off]` (the OLD physical page the
     window used to show) is unchanged and still readable.
  3. **No collateral:** the window pages OUTSIDE `[off, off+span)` still alias their original physical
     offsets (spot-check a page on each side).
- **FAIL disposition:** `KERN_PROTECTION_FAILURE` ⇒ **DEAD-END-C** (overwrite-remap forbidden on this
  mapping class — note: DONOR-NOTES §96–99 predicts this should NOT happen for RW *data*, only for
  `MAP_JIT` code; a failure here would contradict the donor study and is a hard stop). Bare window read
  returns OLD page ⇒ remap silently no-op'd ⇒ **DEAD-END-C2** (overwrite not honored). Either ⇒
  softmmu-mandatory.

### S4 — 16 KB-host-page granularity (the 4×4KB-guest fork)
- **Do:** repeat S3's remap at the **minimum legal granularity = one 16 KB host page** (`span =
  16384`), and separately attempt a remap at a **4 KB-aligned-but-not-16KB-aligned** `off` and a
  **4 KB span**.
- **PASS predicate:** the 16 KB-aligned/16 KB-span remap PASSES all of S3's three assertions. The
  sub-16-KB attempt **must return a `KERN_*` error or be detected as rejected** — i.e. the probe
  proves the primitive's granule is exactly 16 KB (you canNOT independently remap one of the four 4 KB
  guest pages inside a host page). This empirically pins the Discriminator-A "coarse vs fine" gate
  (`CanCreateHostMappingForGuestPages`, DONOR-NOTES ~L77–93): coarse (≥16 KB-aligned/contiguous) =
  mappable; fine (per-4KB) = not.
- **FAIL/UNPROVABLE disposition:** if the kernel SILENTLY accepts a 4 KB remap and only the targeted
  4 KB changes, that would be a surprise (record it — it would actually *relax* Discriminator-A). If
  16 KB-granular remap fails ⇒ **DEAD-END-D**: even coarse mappings can't be remapped ⇒
  softmmu-mandatory. Report the observed granule explicitly either way.

### S5 — Overwrite-under-concurrent-reader (Stop-rule #14, the genuine live-safety question)
- **Do:** spawn ONE reader pthread that bare-reads a fixed window VA inside the swap range in a tight
  loop (`volatile` load, no locks), classifying each read into one of the two valid sentinel values
  (the "before-swap" and "after-swap" page contents) — anything else is a **torn/garbage read**. The
  main thread re-aliases that sub-range back and forth between `off` and `off2` for **≥10,000 swaps**
  (target the thousands the advisory cites). The two physical pages are pre-seeded with two distinct,
  whole-page-uniform sentinels so any value other than the two known sentinels = corruption.
- **PASS predicate (ALL):**
  1. **No crash:** no `SIGSEGV`/`SIGBUS` over the full swap count (a transient unmapped window during
     `OVERWRITE` would fault the reader — its ABSENCE is the core safety claim).
  2. **No torn read:** every observed value is exactly one of the two known page sentinels — never a
     mix, never zero, never stale-other-page garbage.
  3. **No hole:** after the loop, `view_phys` still reads both physical pages intact (store never
     holed under churn).
  4. Reader observed BOTH sentinels at least once (proves the swaps were actually visible to the
     concurrent reader, not optimized away).
- **FAIL disposition:** any crash ⇒ **DEAD-END-E** (the `OVERWRITE` remap is NOT atomic against a
  concurrent bare reader — there is a window where the VA is unmapped ⇒ live use unsafe ⇒ the window
  approach is falsified for live operation, softmmu-mandatory). Any torn/garbage value ⇒ **DEAD-END-E2**
  (non-atomic page substitution). Either is the single most important negative result this spike can
  produce — report it loudly.
- **Note (faithfulness caveat, for the adversary):** a single-page back-and-forth swap is a *proxy*
  for the real BAT context switch, which unmaps a *set* of old entries and maps a *set* of new ones
  (`UpdateDBATMappings`, DONOR-NOTES ~L63–72). S5 proves per-remap atomicity, not multi-entry
  transactional atomicity. State this limit in the result; it is the honest residual the spike does
  NOT close (mirrors test_paged_mmu's "external cross-check OWED" honesty note).

## 6. Output / verdict contract

`main()` prints per-sub-test `[S0..S5] PASS/FAIL/SKIP` lines and a final verdict line:
- `SHM-ARENA SPIKE: PROVEN (macOS arm64 16K)` — all of S1–S5 PASS on a 16 KB host ⇒ S3 may adopt the
  Dolphin separate-arena window.
- `SHM-ARENA SPIKE: FALSIFIED @ <DEAD-END-x>` — first failing gate + its disposition ⇒ S3 softmmu-mandatory.
- `SHM-ARENA SPIKE: SKIP (non-target host)` — not arm64-16K; no attestation.
Return code: 0 on PROVEN or SKIP, non-zero on FALSIFIED (so `make -C SheepShaver/src/machine test`
goes red if the platform primitive regresses).

## 7. Cleanup
`mach_vm_deallocate` both views; `mach_port_deallocate` the memory entry; `pthread_join` the reader
before teardown (set a stop flag). No files written, no env touched.

---

## Appendix — implementer must-answer list

1. **Which mach call vends the backing?** Confirm `mach_make_memory_entry_64` with `parent=0, offset=0`
   and a positive `size` returns a usable named entry on macOS arm64 (vs needing a parent `vm_allocate`
   region first). Record the exact signature used.
2. **Does `mach_vm_map` of the named entry alias or copy?** Must pass `copy=FALSE`; confirm S2 sees
   cross-view writes. If it copies, record the flag combination tried.
3. **Which remap form honors `VM_FLAGS_OVERWRITE` on a sub-range — `mach_vm_map(FIXED|OVERWRITE)` or
   `vm_remap(...OVERWRITE)`?** Both in scope; record which succeeded + the exact flags.
4. **Observed remap granule?** 16384 only, or does the kernel accept sub-page (4 KB) remaps? Pins
   Discriminator-A empirically — report the number.
5. **Atomic vs concurrent reader?** Did S5 ever crash or tear across ≥10,000 swaps? If yes, capture the
   failing value/sentinel and signal.
6. **256 MiB or proxy size?** Did the full 256 MiB backing allocate/map cleanly, or was a proxy needed?
7. **`max_protection` ceiling:** did views come back `max_prot = RW` so OVERWRITE is permitted, or was
   explicit max-prot needed on the original `mach_make_memory_entry_64`?
8. **Inert?** Confirm it is in `TESTS`/`test:` only, links no other .cpp, touches no NATMEM/RMEMBASE,
   and the SHA `144d1943…` citation + "needs validation" is in the header AND CHANGELOG.
9. **Non-target host:** confirm `SKIP` + return 0 on non-arm64/non-macOS so cross-platform `make test`
   stays green.

## Appendix — faithfulness caveat (adversary vote at spec time)

**Substantially faithful on the platform-primitive question, with one disclosed gap.** S1–S4 exercise
the actual mach calls Dolphin's `MemArena.cpp`/`InitFastmemArena` make — named-entry backing, true
aliasing (not copy), sub-range `OVERWRITE` remap that moves a view without holing the store, the 16 KB
granule. A PASS is real evidence; a `KERN_*` FAIL is a real platform showstopper. **The gap:** S5 proves
*single-remap* atomicity against one tight reader; the real BAT context switch is a *multi-entry
transaction* (unmap a set / map a set per `mtspr` storm) with the CPU thread bare-accessing the whole
arena. S5 does NOT prove multi-region transactional atomicity or real `mtspr`-churn behavior — that
residual is OWED to S3's live window milestone. The FALSIFIED dispositions + the S5 residual must be
reported as loudly as a PASS so S3 does not over-read a green probe as "the live window is fully proven."

---

## Appendix — POST-BUILD faithfulness review (added 2026-06-15, S3 Task-0 rev-2 fold)

**Verdict: FAITHFUL-WITH-DISCLOSED-GAPS.** The built `test_shm_arena_spike` (commit `acd89dce`) is a
faithful probe of the Dolphin SHM-arena platform primitive — it **PROVEN-de-risks CONSTRUCTION** (named
SHM backing, true aliasing, sub-range OVERWRITE remap hole-free, the 16 KB granule), **NOT LIVE
OPERATION**.

**Form-mismatch + design directive (binding for the S3-impl window port):** the spike proved the
**single-remap** primitive. The live BAT context switch is a **multi-entry transaction** (a `mtspr`-storm
maps/unmaps a SET of regions) with the CPU thread bare-accessing the whole arena. **Design directive: port
the window using the ATOMIC one-step `mach_vm_map(VM_FLAGS_FIXED|VM_FLAGS_OVERWRITE)` per-region form
(spike-proven hole-free), NOT Dolphin's unmap-then-map sequence** (which exposes a transient-unmapped fault
window to the concurrent reader).

**OWED list (carried to S3's live window milestone — the residuals this spike does NOT close):**
- **O1** — per-region atomicity vs a *concurrent bare-arena reader*; OR prove the inline-single-thread
  invariant (that no concurrent reader exists during a BAT swap) so O1 is moot.
- **O2** — guard-page layout + the ~14 GiB VA scale + `KERN_NO_SPACE` behavior at full guest-RAM + guard
  reservation.
- **O3** — measure remap latency under live `mtspr` churn (the swap is on the hot supervisor path).
- **O4** — the **mixed-perm / non-contiguous 4×4KB fork** = the real Discriminator-A decision (coarse vs
  fine mappability); the single most load-bearing carried unknown.
- **O5** — `MAP_JIT` code-cache coexistence (the JIT arena + the SHM arena in one address space).
- **O6** — a multi-thread torn+fault detector, REQUIRED iff O1 finds a genuine concurrent reader.

**Process note:** a CHANGELOG entry + an UPSTREAM-LINEAGE-SYNC entry are OWED for the Dolphin port (SHA
`144d19433aa734c19c34e5978a1b817d2aa12663`, GPLv2, "needs validation" / reimplemented-to-spec synthetic
probe — per the §4 citation ritual).
