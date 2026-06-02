# ppc-testresults.dat.bz2 — Provenance

Golden results file for `test-powerpc.cpp` (the original maintainer's PowerPC Emulator
Tester), recorded by Gwenolé Beauchesne on **real PowerPC hardware** (PowerBook G4,
PPC 7410) with AltiVec tests enabled.

## Recovery and verification (2026-06-02)

- Recovered from the Internet Archive Wayback Machine (sole surviving snapshot,
  crawled 2006-12-12):
  `https://web.archive.org/web/20061212220526id_/http://gwenole.beauchesne.info:80/projects/ppctester/files/ppc-testresults.dat.bz2`
- **Verified bit-for-bit**: the decompressed `ppc-testresults.dat` has md5
  `3e29432abb6e21e625a2eef8cf2f0840`, exactly matching:
  - the md5 documented in `test-powerpc.cpp:21` in this tree
  - the md5 documented on the maintainer's wiki (`../../../../doc/PowerPC-Testsuite.txt`)
- Decompressed size: 37,768,080 bytes. `bzip2 -t` integrity: OK.

## Why this matters

This is **real-silicon ground truth** — including the results real hardware produces for
architecturally *unspecified* cases. It is the strongest possible oracle for validating
both the interpreter and the AArch64 JIT (see
`../../../../docs/COMPATIBILITY-TESTING-PLAN.md`, Tier 1.4).

## Usage

```bash
bunzip2 -k ppc-testresults.dat.bz2     # produces ppc-testresults.dat (36 MB)
# then run test-powerpc against it (build wiring TBD — see compatibility plan)
```

License: the results file is GPL like the tester that produced it.
