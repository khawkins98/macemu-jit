# Operation NewSheep — Research Log

> Append-only, dated. Every probe / RE finding / decision lands here as a one-block entry, newest
> last within each date. Mechanism-level facts get an evidence tag ([RAW-ROM]/[PATCH]/[STATIC]/
> [PROBE✓]/[QEMU-BEHAVIORAL]). This is the effort-wide record; per-milestone FINDINGS docs may
> still live under `docs/planning/` and are linked from here.

## 2026-06-14 — effort opened

- **Operation NewSheep created.** Baseline tag `newsheep-baseline` on `cc6fc422` (forge-era-end:
  M8→M17 closed, all NO-GO). Charter: `README.md`. Reframe = "run/reproduce the producer (the
  Trampoline), not forge its outputs."
- **Root cause carried in from M17 red-team [STATIC/PROBE✓]:** the EXT-edge regime is MODE_68K
  (run-mode `[0x2810]=0`), not MODE_EMUL_OP; the CGRP handler PC `0x5000ec50` is ROM-absent (0
  word-refs in 4 MB ROM); `"CGRP"` tag absent from ROM → both built at runtime by the Trampoline /
  IM-init that never runs in our emulation.
- **External tooling identified (not yet installed):** `tbxi` (dump/build the Mac OS ROM file,
  exposes the Trampoline ELF parcel — operates on the System-Folder ROM file, NOT a hardware dump),
  `tbxi-patches`, `newworld-rom` (rebuilds from a 4 MB hardware ROM + PEFs — NOT a no-extract path).
- **Open: first milestone = Trampoline RE Task-0** (tbxi dump the 9.0.1/9.0.4 ROMs we already hold;
  disassemble; pin the OF-tree reads + nanokernel interrupt-setup writes; pin the gap vs our
  synthesized OF + `SS_NW_TRAMPOLINE`). Zero-cost, offline. Awaiting the milestone-machine pass.

- **Round-1 charter feedback folded (2026-06-14).** External agent review: framing sound
  ("not just a different forge"; route-ladder-with-shared-cheap-Task-0 is right). Load-bearing
  points, now captured in `DECISIONS.md` (Q0-A…E) + charter §6/§9:
  - **Q0-A is THE feasibility gate:** does Route A mean implementing OpenFirmware? (the Trampoline is
    an OF client). Enumerate its OF calls FIRST; bounded→A viable, open-ended→ladder collapses to B/C.
  - **Route C ≠ automatically distinct from the buried forge** (M16: handler PC is computed-at-runtime);
    C viable only if writes are constants/relocations, not computed-from-OF-tree (Q0-B).
  - **Route B must be honest re-binding, not value-hardcoding** (red-team enforces in DoD — Q0-C).
  - **QEMU as a Trampoline TRACER** (Task-0 step, not backstop): trace OF calls + write values under
    mac99 to answer Q0-A/B behaviorally before committing a route.
  - **9.0.x findings may not transfer to 9.2** (parcels-layout change); source the 9.2 "Mac OS ROM"
    file early, separate from the ISO (Q0-D).
  - **Win condition = clear the IM-init class; expected next wall = M14 `[ALARM]` boot-gate, NOT
    Finder.** R3 (non-IM wall) is the GOOD outcome (back on machine-layer mainline).

- **Task-0 brainstorm decisions (2026-06-14):** (1) **both instruments gated on agreement** (static
  `tbxi`-RE + QEMU trace; divergence = own investigation); (2) **both on 9.2** (version-matched);
  (3) **QEMU tracer required, open budget** (no static-only fallback). See `DECISIONS.md` log.
- **NewWorld ROM collection found + banked (2026-06-14).** User pointed to
  `~/Downloads/New_World_Mac_Roms`; copied 19 ROM files (1998→2003 progression) to
  `/Users/Shared/macemu/newworld-roms/` + `MANIFEST.txt` (md5s). Key facts: (a) filename version =
  ROM-*file* version, NOT the OS version; (b) our active "9.0.1" ROM (`66210b4f…`) is byte-identical
  to the folder's `2001-12-19 Mac OS ROM 9.0.1` — dated **9.2.2 era**; (c) "source 9.2 ROM first" is
  effectively SOLVED (8.4≈9.2/9.2.1, 9.0.1≈9.2.2 in hand); (d) **reframe to verify via `tbxi`:** the
  M-series likely ran against a 9.2-era ROM all along. Remaining 9.2 gap = system software (not on
  the Task-0 RE path). Updates `ASSETS-AND-TOOLING.md` + `DECISIONS.md` Q0-D (🟢 mostly closed).

- **Task-0 spec+plan red-team folded → rev 2 (2026-06-14).** 3 reviewers, all GO-WITH-FIXES; one
  installed `tbxi 0.13` + dumped our ROM (empirical). Key:
  - **Producer reframe (new fork Q0-F):** Trampoline = top-level `MacOS.elf` (ELF PPC BE, entry
    0x20f078), NOT a parcel; `Parcels/` are PEF drivers; NanoKernel = `NanoKernel-vNN` parcel. `CGRP`
    absent from `MacOS.elf` — Trampoline builds the OF device tree + page map; **NanoKernel builds CGRP
    from it** → producer likely Trampoline+NanoKernel. "Find CGRP writes in the Trampoline" retired.
  - **Agreement gate → mechanism-level** (services/write-classes/provenance, never literal
    values/addresses/counts). Static & dynamic run the *same* ROM binary (md5 `66210b4f…` verified) but
    dynamic runs it against **OpenBIOS ≠ Apple OF** → value/address/path divergence expected, not blocking;
    Q0-B agreement is on provenance, not values.
  - **Route A "bounded" ≠ "cheap":** stubbing OF = synthesizing the device tree the producer reads (the
    `SS_NW_TRAMPOLINE` problem one level up). Q0-A split into call-surface-bounded + stub-data-tractable;
    full 4-cell route table; per-route mechanical-feasibility check; DoD-negative is a costed first-class outcome.
  - **Tracer:** no PPC `gdb` on this host → Python gdb-remote client; QEMU needs `-S` (halt at reset) or
    the one-shot Trampoline is missed. tbxi: `tbxi dump -o <dir> <rom>`.
  All folded into plan/spec rev-2 + charter §1 + DECISIONS (Q0-F).

<!-- next entry below -->
