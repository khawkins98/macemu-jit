# NewSheep — pre-S1 bug-hunt over the session's gated bringup code

**Status:** COMPLETE (2026-06-15) · READ-ONLY review (one fan-out agent) → 5 fixes landed
(`8cccd78c`), remainder accepted-as-noted. All reviewed paths are gated default-OFF; the
paravirtual (default) flow is results-identical; `test-jit`=100, machine unit suite green.

**Scope reviewed:** the 7 new machine-layer TUs + headers (`trampoline_loader`,
`openfirmware_ci`, `trampoline_ofci_backends`, `trampoline_ofci_shim`, `exc_inject`,
`nk_mmu_trace`, `dolphin_bat_arena`) and the gated seams in `sheepshaver_glue.cpp`,
`ppc-execute.cpp`, `rom_patches.cpp`. Gating discipline assessed **solid** — every new path is
reachable only behind `SS_M18_TRAMPOLINE` / `SS_M18_NK_SUPERVISOR` / `SS_M18_NK_TRACE`, all
default-OFF.

## Fixed (`8cccd78c`) — silent-failure → fail-loud
- **M1** `trampoline_ofci_backends.cpp` /mmu `map` table overflow (32 entries) was
  accept-and-drop → a later `translate` of the dropped range silently returns V=P identity
  instead of the recorded **non-identity** phys (the exact fidelity S1's live MMU is validated
  against). Now `[S2B-MMU-OVERFLOW]` + stop-hook. **The single most important pre-S1 fix.**
- **M2** `openfirmware_ci.cpp` direct IEEE-1275 `claim` fixed path had no shim-page overlap
  check (only the `/mmu` call-method `claim` did). A fixed claim over
  `[TRAMP_SHIM_ENTRY,+0x1000)` would silently hand over the launch shim page. Now emits the
  `[S2B-SHIM-COLLIDE]` tripwire (Stop-rule #13).
- **M3** `openfirmware_ci.cpp` `setprop` with `len>0` but NULL buffer stored `data=NULL,len>0`
  → later `getprop` memcpy's from NULL. Force `len=0` when no buffer copied.
- **m1** `trampoline_loader.cpp` staged compressed `prcl` >4 MB was silently truncated (would
  decompress to garbage KernelCode — the footgun faithful-staging closes). Now refuses loudly
  (`[S2B-PARCEL-OVERSIZE]`).
- **m2** `nk_mmu_trace.cpp` clamp `snprintf` would-have-written return to `buflen-1` so
  `fwrite(buf,1,n)` can't read past the line buffer on truncation.

## Accepted-as-noted (not currently reachable / diagnostic-only; revisit if touched)
- **m3** direct OF services read `args[0..3]` without an `n_args` check — the marshaller zeroes
  `host[]` and caps to `OF_CI_MAX_CELLS`, so this is logic-on-zeros, not host-OOB. Guard per
  service if a short-arity producer call ever appears.
- **m4** un-asserted layout coupling: staged ROM top (`0xC00000`+4 MB = `0x1000000`) abuts the
  claim arena floor (`0x01000000`). Half-open ⇒ no overlap today; add a static/runtime disjoint
  assert before bumping `TRAMP_PARCEL_SIZE`/`rom_virt`/claim base.
- **m5** `slice_compressed_prcl` Forth-constant parse is positionally fragile but **fails safe**
  (the `prcl` magic check at the parsed offset catches a bad parse loudly).
- **m6** `SS_M18_CLAIM_BASE` override moves `base` not `limit` (diagnostic env only; the bump
  allocator doesn't enforce `claim_limit` anyway).
- **m7** resource hygiene (all process-lifetime singletons, single PPC thread): `g_trace_file`
  never `fclose`d, `g_s2b_ofci_ctx` never destroyed, unchecked `malloc`s consistent with file
  style. Low-risk; no concurrency exposure.

## Verified-correct (audited, no bug — banked so S1 doesn't re-tread)
- **exc_inject.cpp** local-table technique provably bypasses `g_exc_entry_table`; all
  `ExcTransition` fields initialized; sentinel guard (`0`/`0xDEADBEEF`/`0xFFFFFFFF`) consistent
  across all three resolvers; EXT cache-drop on `EXC_PC_UNRESOLVED` correct.
- **trampoline_ofci_shim.cpp** the `3 + n_args + n_rets > OF_CI_MAX_CELLS` cap bounds every
  `host[]` index; scalar zero-extension has no sign bug; call-method nested-arg ordering is
  order-correct.
- **backends.cpp out[3]=phys reconcile** dual-consumer routing internally consistent with the
  documented watchpoint evidence; miss-path writes `out[3]=0` (false) correctly.
- **BE encode/decode** (`be16/be32`, `rd/wr_be32`, `cfg_be`, EXEC_NATIVE round-trip) and ELF
  parse/placement bounds all correct.
- **ROM-identity guard** correctly refuses the 1.1 ROM; FNV-1a/size double-check fails loud.
- **Forge gating** (`rom_patches.cpp`) `!TrampolineLoaderGateEnabled() && !NkSupervisorEnabled()`
  is the right predicate (uses the *gate*, not `…Ran()`); default-OFF runs the forge
  byte-identically.

**Carry into S1 Task-0:** M1's loud overflow is the fidelity backstop the live MMU validates
against; keep MMU_MAP_MAX honest (raise/evict if the real Trampoline issues >32 maps). M2's
tripwire is what catches a shim-page collision once S1 starts relocating — trust it, don't
re-stub it.
