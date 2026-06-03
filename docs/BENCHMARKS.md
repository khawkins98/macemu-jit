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

## Speedometer 4.02 Results

### Configuration
- **Host**: macOS arm64 (Apple Silicon)
- **ROM**: OldWorld 1998-07-21 Mac OS ROM 1.1 (ROM Version $077D, 3072K)
- **Guest OS**: Mac OS 8.6 (fresh HD install, 4GB disk)
- **RAM**: 256MB
- **JIT**: Full native (ROM=0x500000, chaining=1, no skip list)
- **Reported CPU**: Power Macintosh, MC68020 (native/nominal), PowerPC Math FPU

### JIT Results (PR: 40.424, relative to Quadra 605 = 1.0)

| Category | Score | Rating |
|----------|-------|--------|
| **CPU** | 62.732 | 1 |
| **Graphics** | 42.120 | 1 |
| **Disk** | 17.811 | 1 |
| **Math** | 10310.327 | 1 |
| **PR (overall)** | **40.424** | |

### Benchmark Mix (vs Quadra 605 = 1.0)

| Test | Absolute | Rating |
|------|----------|--------|
| KWhetstones/sec | 1,340,482.573 | 4558.131 |
| Dhrystones/sec | 1,310,650.344 | 75.884 |
| Towers | 0.006s | 107.187 |
| Quick Sort | 0.007s | 106.627 |
| Bubble Sort | 0.007s | 101.390 |
| Queens | 0.006s | 65.613 |
| Puzzle | 0.008s | 139.761 |
| Permutations | 0.007s | 123.411 |
| Int. Matrix | 0.003s | 240.032 |
| Sieve | 0.020s | 66.974 |
| **Mix Average** | | **558.501** |

### Color Benchmarks (vs Quadra 605 = 1.0)

| Test | Time | Rating |
|------|------|--------|
| Monochrome | 0.181s | 38.095 |
| Two Bit | 0.203s | 37.622 |
| Four Bit | 0.215s | 40.529 |
| Eight Bit | 0.221s | 48.026 |
| Sixteen Bit | 0.184s | 73.852 |
| **Average** | | **47.625** |

### FP Benchmarks (vs Quadra 650 = 1.0)

| Test | Absolute | Rating |
|------|----------|--------|
| KWhetstones/sec | 1,290,322.580 | 247.807 |
| Matrix Mult. | 0.004s | 177.216 |
| Fast Fourier | 0.001s | 249.974 |
| **Average** | | **224.999** |

### Interpreter Results (PR: 21.485)

| Category | JIT | Interpreter | JIT/Interp Ratio |
|----------|-----|-------------|------------------|
| **CPU** | 62.732 | 35.651 | **1.76x** |
| **Graphics** | 42.120 | 21.441 | **1.96x** |
| **Disk** | 17.811 | 9.380 | **1.90x** |
| **Math** | 10310.327 | 7974.433 | **1.29x** |
| **PR (overall)** | **40.424** | **21.485** | **1.88x** |

### Benchmark Mix Comparison (vs Quadra 605 = 1.0)

| Test | JIT | Interpreter | Speedup |
|------|-----|-------------|---------|
| KWhetstones/sec | 4558.131 | 2009.672 | 2.27x |
| Dhrystones/sec | 75.884 | 33.423 | 2.27x |
| Towers | 107.187 | 57.658 | 1.86x |
| Quick Sort | 106.627 | 69.467 | 1.53x |
| Bubble Sort | 101.390 | 44.822 | 2.26x |
| Queens | 65.613 | 36.445 | 1.80x |
| Puzzle | 139.761 | 59.336 | 2.36x |
| Permutations | 123.411 | 57.327 | 2.15x |
| Int. Matrix | 240.032 | 114.296 | 2.10x |
| Sieve | 66.974 | 40.414 | 1.66x |
| **Mix Average** | **558.501** | **252.286** | **2.21x** |

### Color Benchmarks Comparison

| Test | JIT Rating | Interp Rating | Speedup |
|------|-----------|---------------|---------|
| Monochrome | 38.095 | 21.333 | 1.79x |
| Two Bit | 37.622 | 21.209 | 1.77x |
| Four Bit | 40.529 | 22.753 | 1.78x |
| Eight Bit | 48.026 | 28.911 | 1.66x |
| Sixteen Bit | 73.852 | 47.867 | 1.54x |
| **Average** | **47.625** | **28.415** | **1.68x** |

### FP Benchmarks Comparison (vs Quadra 650 = 1.0)

| Test | JIT Rating | Interp Rating | Speedup |
|------|-----------|---------------|---------|
| KWhetstones/sec | 247.807 | 114.588 | 2.16x |
| Matrix Mult. | 177.216 | 105.436 | 1.68x |
| Fast Fourier | 249.974 | 83.721 | 2.99x |
| **Average** | **224.999** | **101.248** | **2.22x** |

### Summary

The PPC→ARM64 JIT delivers a **1.88x overall speedup** (PR score) over the interpreter.

| Category | Speedup | Notes |
|----------|---------|-------|
| Integer (Whetstone/Dhrystone) | 2.27x | Pure compute, biggest JIT win |
| Sorting/algorithms | 1.5-2.4x | Varies by memory access pattern |
| Graphics | 1.7-2.0x | Limited by SDL rendering overhead |
| Disk | 1.90x | Surprising — File Manager PPC overhead |
| FP | 2.2x avg, up to 3.0x | Fast Fourier shows best FP speedup |
| **Overall PR** | **1.88x** | |

Screenshots: [JIT](speedometer-jit.jpg) | [Interpreter](speedometer-interpreter.jpg)

