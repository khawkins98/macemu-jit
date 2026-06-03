# SheepShaver ARM64 JIT Benchmarks

Benchmark data for the macOS ARM64 (Apple Silicon) JIT port.
All tests on Mac OS 8.6 Internal Edition ISO, OldWorld ROM, 256MB RAM.

## Test Configuration

- **Host**: macOS arm64 (Apple Silicon)
- **ROM**: 1998-07-21 Mac OS ROM 1.1 (OldWorld)
- **Boot media**: Mac OS 8.6 Internal Edition ISO (CD boot, `bootdriver -62`)
- **RAM**: 256 MB
- **Display**: 800x600 windowed
- **JIT config**: ROM=0x500000 (full DR emulator), chaining=1

## Boot Benchmark (ISO cold boot to Finder desktop)

| Metric | JIT (full native) | Interpreter | Ratio |
|--------|-------------------|-------------|-------|
| Boot to desktop | ~10s | ~10s | ~1.0x |
| Blocks at t=5s | 622M | 525M | 1.18x |
| Blocks at t=90s (idle) | 641M | 607M | 1.06x |
| CPU at idle desktop | 4-5% | ~5% | similar |
| Blocks compiled (unique) | 83K-85K | n/a | — |
| j2i transitions (total) | 6,136 | n/a | negligible |

### Block rate progression (blocks/5s at idle desktop)

| Time | JIT | Interpreter |
|------|-----|-------------|
| t=5 | 622M (boot burst) | 525M (boot burst) |
| t=10 | 1.2M | 55M |
| t=15 | 1.2M | 1.7M |
| t=20 | 1.2M | 1.7M |
| t=30 | 1.2M | 1.7M |

JIT boots faster (622M blocks in first 5s vs 525M) then settles to a lower idle
rate. Both reach ~4-5% CPU at idle.

### Region breakdown at desktop (t=90s)

| Region | JIT | Interpreter |
|--------|-----|-------------|
| NK (nanokernel/toolbox) | 9.7M | 10.7M |
| DR (68k emulator) | 117M (JIT-compiled) | 224M (interpreted) |
| RAM (Mac OS code) | 514M | 372M |

The JIT compiles the DR emulator natively (117M jDR blocks, 0 iDR), eliminating
the 2.4M/s JIT↔interpreter transitions that dominated the old ROM=0x460000 config.

## Comparison: ROM=0x460000 (DR interpreted) vs ROM=0x500000 (DR JIT-compiled)

| Metric | ROM=0x460000 | ROM=0x500000 | Improvement |
|--------|-------------|--------------|-------------|
| j2i transitions/s | ~2,400,000 | ~70 | 34,000x fewer |
| DR blocks JIT-compiled | 0 | 117M | — |
| Boot to desktop (HD) | ~18s | ~10s | 1.8x faster |

## Idle Desktop Characteristics

Both JIT and interpreter settle to similar CPU usage at the Finder desktop.
The JIT's advantage is primarily during boot (loading extensions) and during
active use (launching apps, scrolling, disk access) — not at idle.

## JIT Compilation Statistics

| Metric | Value |
|--------|-------|
| Unique blocks compiled | ~83,000 |
| Code cache size | 64 MB |
| Cache flushes during boot | 1-2 (at ~12s, ~28s) |
| Opcode coverage | 100.0% (all encountered opcodes compile) |
| Fallback opcodes | icbi, isync, lwarx, stwcx., mftb (by design) |
| Harness vectors | 235/235, score=100 |

## Methodology

Boot tests use the standardized procedure from `docs/BOOT-TEST-METHOD.md`:
1. Kill any running instance
2. Start with `SS_JIT_DIAG_LOG=/tmp/jit_bench.log`
3. VNC screenshot at t=20s and t=90s
4. Heartbeat data from diagnostic log
5. Kill at 90s

Desktop confirmed via VNC screenshot (menu bar + Finder window visible).

## History

| Date | Config | Boot (HD) | Boot (ISO) | Notes |
|------|--------|-----------|------------|-------|
| Pre-session 7 | ROM=0x460000, chaining=0 | ~18s | ~5min | DR emulator interpreted |
| Session 7a | ROM=0x500000, chaining=1 | ~10s* | n/a | *Disk First Aid, not true desktop |
| Session 7 final | ROM=0x500000, chaining=1 | TBD | ~10s | Full native JIT, 8 bugs fixed |

## Next Steps

- [ ] MacBench 5.0 benchmark (requires HD install)
- [ ] Speedometer 4.0
- [ ] Application launch timing (SimpleText, TeachText)
- [ ] Compare with upstream Linux ARM64 (rcarmo/macemu-jit)
- [ ] HD boot timing (after clean install)
