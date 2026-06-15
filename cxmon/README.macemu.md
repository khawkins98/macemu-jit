# Why `cxmon/` is vendored in this repo

> **Status:** 📖 Reference · **Created:** 2026-06-05 · **Updated:** 2026-06-05
> **Why this doc exists:** `cxmon/` is upstream third-party source, not part of the JIT work.
> This note explains what it is, why it lives at the repo root, and that it's currently
> compiled **off** — so nobody mistakes it for active code or "tidies" it into a subdirectory.

## What it is

`cxmon` is **cxmon 3.2**, Christian Bauer & Marc Hellwig's command-line file/memory monitor
and disassembler (GPL; bundles GNU binutils disassemblers). See [`README`](README) for the
upstream tool documentation. It is a long-standing companion to macemu: when enabled, it
provides BasiliskII/SheepShaver's interactive `mon` debugger (memory inspection, disassembly,
ROM breakpoints).

## Why it lives at the repo root (don't move it)

It is **vendored** source (a checked-in copy, **not** a git submodule). Both emulators'
`configure.ac` hard-code the path to it relative to `src/Unix`:

```
# SheepShaver/src/Unix/configure.ac and BasiliskII/src/Unix/configure.ac
mon_srcdir=../../../cxmon/src
```

From `<emulator>/src/Unix`, `../../../cxmon/src` resolves to **`<repo>/cxmon/src`**. Moving
`cxmon/` elsewhere would silently break `--with-mon` (default `yes`) in *both* emulators.
Root is the build-mandated location.

## It is currently compiled OFF

The macOS arm64 build does **not** include the monitor — `config.h` carries
`/* #undef ENABLE_MON */`. The `--with-mon` probe (`grep mon_init $mon_srcdir/mon.h`) only
wires `mon` in when it finds the source *and* the host toolchain accepts the binutils
disassembler C; on this port it ends up disabled, so the emulator binaries do not link `mon`.
Nothing in the AArch64 JIT depends on it.

## Upstream / maintenance

Effectively frozen. The vendored tree carries ~14 commits, the last a 2017 cosmetic fix
(`e4298d3a`, "remove stray non-ascii chars"); upstream cxmon itself has long been dormant.
There are no pending upstream changes worth pulling in. Treat this directory as a stable
third-party dependency — leave it as-is unless `--with-mon` is intentionally revived on macOS.
