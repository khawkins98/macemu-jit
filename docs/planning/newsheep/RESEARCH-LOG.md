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

<!-- next entry below -->
