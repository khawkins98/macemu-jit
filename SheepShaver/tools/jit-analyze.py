#!/usr/bin/env python3
"""
jit-analyze.py — SheepShaver JIT log analysis tool

Usage:
  python3 jit-analyze.py diag [LOG]       Analyze jit_diag.log
  python3 jit-analyze.py ring [LOG] [PC]  Analyze ss_jit_ring.txt, optionally filter around PC
  python3 jit-analyze.py hot [LOG] [N]    Top-N hottest PCs from interrupt-delivered lines

Defaults: LOG=/tmp/jit_diag.log or /tmp/ss_jit_ring.txt depending on subcommand.
"""
import sys, re, collections, struct

ROM_RANGE = (0x50000000, 0x50500000)
RAM_RANGE = (0x10000000, 0x11000000)

def classify(pc):
    if ROM_RANGE[0] <= pc < ROM_RANGE[1]:
        return 'ROM'
    if RAM_RANGE[0] <= pc < RAM_RANGE[1]:
        return 'RAM'
    return 'OTH'

# ── diag subcommand ──────────────────────────────────────────────────────────

def cmd_diag(path):
    path = path or '/tmp/jit_diag.log'
    heartbeats = []   # (t, blocks, pc)
    interrupts = []   # (t, pc)
    stuck = []        # (t, pc, secs)
    regdumps = []     # raw lines

    with open(path) as f:
        for line in f:
            line = line.rstrip()
            m = re.match(r'\[JIT ([\d.]+)s\] blocks=(\d+) pc=([0-9a-f]+)', line)
            if m:
                heartbeats.append((float(m[1]), int(m[2]), int(m[3], 16)))
                continue
            m = re.match(r'\[JIT ([\d.]+)s\] interrupt delivered, pc=([0-9a-f]+)', line)
            if m:
                interrupts.append((float(m[1]), int(m[2], 16)))
                continue
            m = re.match(r'\[JIT ([\d.]+)s\] STUCK at pc=([0-9a-f]+) for ~(\d+)s', line)
            if m:
                stuck.append((float(m[1]), int(m[2], 16), int(m[3])))
                continue
            if re.match(r'\s+r1=', line) or re.match(r'\s+Ticks', line):
                regdumps.append(line)

    print(f"=== jit_diag.log analysis: {path} ===")
    print(f"  Heartbeats:  {len(heartbeats)}")
    print(f"  Interrupts:  {len(interrupts)}")
    print(f"  STUCK events: {len(stuck)}")
    if not heartbeats:
        print("  (no heartbeats — log may be empty or from a very short run)")
        return

    t0, b0, _ = heartbeats[0]
    tN, bN, _ = heartbeats[-1]
    elapsed = tN - t0
    total_blocks = bN - b0
    rate = total_blocks / elapsed if elapsed > 0 else 0
    print(f"\nTime range: {t0:.1f}s – {tN:.1f}s  ({elapsed:.1f}s covered)")
    print(f"Block throughput: {rate/1e6:.1f}M blocks/sec (avg over log window)")

    if len(heartbeats) > 1:
        print("\nHeartbeat PC history (last 20):")
        for t, b, pc in heartbeats[-20:]:
            print(f"  [{t:7.1f}s] pc={pc:08x} ({classify(pc)})  blocks={b:,}")

    if stuck:
        print(f"\nSTUCK events:")
        for t, pc, secs in stuck:
            print(f"  [{t:7.1f}s] pc={pc:08x} ({classify(pc)}) for ~{secs}s")
        for line in regdumps:
            print(" ", line)

    if interrupts:
        print(f"\nHot interrupt-delivery PCs (top 20 of {len(interrupts)} samples):")
        ctr = collections.Counter(pc for _, pc in interrupts)
        for pc, n in ctr.most_common(20):
            pct = 100 * n / len(interrupts)
            print(f"  {pc:08x} ({classify(pc)})  {n:6d}×  {pct:5.1f}%")

        # Time-sliced view: group by 10s windows, show top PC per window
        print(f"\nPC activity by 10s window:")
        windows = collections.defaultdict(list)
        for t, pc in interrupts:
            windows[int(t // 10) * 10].append(pc)
        for w_start in sorted(windows):
            pcs = windows[w_start]
            top_pc, top_n = collections.Counter(pcs).most_common(1)[0]
            print(f"  {w_start:4d}-{w_start+10}s: {top_pc:08x} ({classify(top_pc)}) dominates "
                  f"({top_n}/{len(pcs)} = {100*top_n/len(pcs):.0f}%)")


# ── ring subcommand ──────────────────────────────────────────────────────────

def cmd_ring(path, filter_pc_str):
    path = path or '/tmp/ss_jit_ring.txt'
    filter_pc = int(filter_pc_str, 16) if filter_pc_str else None

    records = []
    with open(path) as f:
        for line in f:
            line = line.rstrip()
            # J from to [fields...]
            m = re.match(r'^([JICE]) ([0-9a-f]+) ([0-9a-f]+)', line)
            if m:
                typ, from_pc, to_pc = m[1], int(m[2], 16), int(m[3], 16)
                records.append((typ, from_pc, to_pc, line))

    if not records:
        print(f"No records found in {path}")
        return

    if filter_pc is not None:
        # Find last occurrence of filter_pc as from_pc and show context
        last_idx = None
        for i, (typ, from_pc, to_pc, _) in enumerate(records):
            if from_pc == filter_pc:
                last_idx = i
        if last_idx is None:
            print(f"PC {filter_pc:08x} not found as from_pc in ring")
            return
        context_start = max(0, last_idx - 50)
        context_end = min(len(records), last_idx + 10)
        print(f"=== Ring context around last visit to pc={filter_pc:08x} (records {context_start}–{context_end}) ===")
        for i in range(context_start, context_end):
            typ, from_pc, to_pc, raw = records[i]
            marker = " <<<" if from_pc == filter_pc else ""
            print(f"  [{i:6d}] {typ} {from_pc:08x}→{to_pc:08x} ({classify(from_pc)}){marker}")
    else:
        print(f"=== ss_jit_ring.txt: {len(records)} records ===")
        print("Last 80 records:")
        for i, (typ, from_pc, to_pc, _) in enumerate(records[-80:]):
            print(f"  [{len(records)-80+i:6d}] {typ} {from_pc:08x}→{to_pc:08x} ({classify(from_pc)})")

        # Count transitions to each PC
        print("\nMost common from_pc (top 20):")
        ctr = collections.Counter(from_pc for typ, from_pc, to_pc, _ in records)
        for pc, n in ctr.most_common(20):
            print(f"  {pc:08x} ({classify(pc)})  {n:6d}×")


# ── hot subcommand ───────────────────────────────────────────────────────────

def cmd_hot(path, n_str):
    path = path or '/tmp/jit_diag.log'
    n = int(n_str) if n_str else 30

    pcs = []
    with open(path) as f:
        for line in f:
            m = re.match(r'\[JIT ([\d.]+)s\] interrupt delivered, pc=([0-9a-f]+)', line)
            if m:
                pcs.append((float(m[1]), int(m[2], 16)))

    if not pcs:
        print("No interrupt-delivered records found")
        return

    print(f"Top {n} interrupt-delivery PCs from {len(pcs)} samples in {path}:")
    ctr = collections.Counter(pc for _, pc in pcs)
    for pc, count in ctr.most_common(n):
        pct = 100 * count / len(pcs)
        print(f"  {pc:08x}  {classify(pc)}  {count:7d}×  {pct:5.1f}%")


# ── main ─────────────────────────────────────────────────────────────────────

if __name__ == '__main__':
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(0)
    sub = args[0]
    a1 = args[1] if len(args) > 1 else None
    a2 = args[2] if len(args) > 2 else None
    if sub == 'diag':
        cmd_diag(a1)
    elif sub == 'ring':
        cmd_ring(a1, a2)
    elif sub == 'hot':
        cmd_hot(a1, a2)
    else:
        print(__doc__)
        sys.exit(1)
