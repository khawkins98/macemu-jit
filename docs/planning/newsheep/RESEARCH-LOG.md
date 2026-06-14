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

<!-- next entry below -->
