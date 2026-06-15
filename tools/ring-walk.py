#!/usr/bin/env python3
"""ring-walk.py — walk SheepShaver trace-ring / R24RING dumps so agents don't read raw ring text.

Input (positional): one of
  * a JIT trace-ring dump file        (/tmp/ss_jit_ring.txt style)
  * a boot log                        (slot rundir boot.log; contains the [R24RING] section
                                       and the "dumped to /tmp/ss_jit_ring.txt" pointer)
  * a slot rundir                     (/tmp/ss-slots/slotN/runs/<stamp>/ — boot.log found inside)

FORMAT ASSUMPTIONS (pinned against ppc-cpu.cpp ppc_jit_dump_trace_ring()/r24ring_dump,
verified on 2026-06-11 slot rundirs):

  Trace-ring dump (one file, written to /tmp/ss_jit_ring.txt by the crash/atexit path):
    line 1:  "# records #A..#B (of T total; ring S records, oldest retained #O)"
             -> line K (1-based, after the header) is absolute record #(A + K - 1)
    lines:   "<type> <from_pc> <to_pc> op=X sp=X r24=X r27=X r29=X lr=X ctr=X cr=X
              a0=X .. a7=X d0=X .. d7=X"
    type:    J = JIT block executed (regs AFTER), I = interpreter block entered (regs BEFORE),
             C = inline interpreter call (regs BEFORE), E/R = EmulOp before/after
             (field reuse: r27=D0, r29=D1; lr/ctr/cr = IOParam captures for driver ops).
    sp       = PPC r1 = 68k A7 in the DR convention; r24 = 68k PC, r27 = opcode,
             r29 = handler; a0..a7 = r16..r23, d0..d7 = r8..r15.

  R24RING section (inline in the boot log, SS_DR_R24_RING=1):
    header:  "[R24RING] N transitions recorded (showing last M, oldest first;
              PC*N = N-1 dedup-suppressed repeats)"
    lines:   space-led rows of 8-hex-digit tokens, each "PC" or "PC*count"
             (count = how many times the value occurred consecutively, dedup-expanded here).
    The section ends at the first line that is not all hex tokens.
    "N transitions recorded" counts ring ENTRIES (post-dedup); expanding the
    *count suffixes yields MORE positions than that. R24RING positions printed
    by this tool ("@i") are therefore 0-based expanded-sequence positions
    (oldest-first), NOT comparable to trace-ring record numbers.
    NOTE: r24 sometimes holds non-PC scratch (ASCII bytes during NK printf,
    ffffffff sentinels while DR is inactive) — values < 0x1000 or >= 0x60000000
    are NON-PC: they reset the flow state and are excluded from classification.

r24-flow heuristic (DESYNC hunting): consecutive 68k PCs normally advance by
+2/+4/+6 (instruction word + extensions) or repeat (dedup'd loop). Anything else
is a control transfer: even->even jumps are marked BRANCH (normal: Bcc/JSR/RTS/
exception); a transition is marked DESYNC-CANDIDATE when the new PC is ODD or
the delta is odd (parity flip mid-stream) — the structurally-impossible-PC
desync signature. BRANCHes are not validated against real branch targets; a
desync that lands on a wrong-but-even PC will show as a BRANCH, so review the
BRANCH lines around any anomaly too.

Usage:
  ring-walk.py LOG_OR_DIR                       # summary of what was found
  ring-walk.py LOG_OR_DIR --window 3391300:3391350    # trace-ring records by absolute #
  ring-walk.py LOG_OR_DIR --r24-flow [--limit N]      # r24 sequence w/ transition marks
  ring-walk.py LOG_OR_DIR --find-pc 0x50313c24        # records touching a PC
  ring-walk.py LOG_OR_DIR --regs-at 3391300           # full register file at record #
  ring-walk.py LOG_OR_DIR --ring FILE                 # explicit trace-ring dump path
"""

import argparse
import os
import re
import signal
import sys

# Survive `| head` etc. without a BrokenPipeError traceback.
signal.signal(signal.SIGPIPE, signal.SIG_DFL)

# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

RING_HEADER_RE = re.compile(
    r"^# records #(\d+)\.\.#(\d+) \(of (\d+) total; ring (\d+) records, oldest retained #(\d+)\)")
RING_REC_RE = re.compile(
    r"^([A-Z]) ([0-9a-f]{8}) ([0-9a-f]{8}) ((?:[a-z0-9]+=[0-9a-f]{8} ?)+)$")
R24_HEADER_RE = re.compile(
    r"\[R24RING\] (\d+) transitions recorded \(showing last (\d+)")
R24_TOKEN_RE = re.compile(r"^([0-9a-f]{8})(?:\*(\d+))?$")
DUMPED_TO_RE = re.compile(
    r"JIT trace ring: \d+ records \(#\d+\.\.#\d+ of (\d+) total\) dumped to (\S+)")


class RingRecord:
    __slots__ = ("num", "type", "from_pc", "to_pc", "fields")

    def __init__(self, num, rtype, from_pc, to_pc, fields):
        self.num = num          # absolute record number
        self.type = rtype       # J/I/C/E/R
        self.from_pc = from_pc
        self.to_pc = to_pc
        self.fields = fields    # ordered dict name -> int (op, sp, r24, ..., d7)

    def line(self):
        f = " ".join("%s=%08x" % (k, v) for k, v in self.fields.items())
        return "#%-9d %s %08x %08x %s" % (self.num, self.type, self.from_pc, self.to_pc, f)


def parse_trace_ring(path):
    """Returns (records, header_info) or (None, None) if not a trace-ring dump."""
    records, header = [], None
    with open(path, errors="replace") as fh:
        first = fh.readline()
        m = RING_HEADER_RE.match(first)
        if not m:
            return None, None
        header = dict(zip(("first", "last", "total", "ring_size", "oldest"),
                          (int(g) for g in m.groups())))
        num = header["first"]
        for line in fh:
            rm = RING_REC_RE.match(line.rstrip("\n"))
            if not rm:
                continue  # tolerate trailing junk
            fields = {}
            for kv in rm.group(4).split():
                k, v = kv.split("=")
                fields[k] = int(v, 16)
            records.append(RingRecord(num, rm.group(1),
                                      int(rm.group(2), 16), int(rm.group(3), 16), fields))
            num += 1
    return records, header


def parse_r24_section(log_path):
    """Extract the [R24RING] section from a boot log.
    Returns (seq, header) where seq = list of (value, count) in oldest-first order
    (counts preserved, NOT pre-expanded — dumps can be millions of expanded entries)."""
    seq, header, in_section = [], None, False
    with open(log_path, errors="replace") as fh:
        for line in fh:
            if not in_section:
                m = R24_HEADER_RE.search(line)
                if m:
                    header = {"total": int(m.group(1)), "shown": int(m.group(2))}
                    in_section = True
                continue
            toks = line.split()
            if not toks:
                break
            parsed = []
            for t in toks:
                tm = R24_TOKEN_RE.match(t)
                if not tm:
                    parsed = None
                    break
                parsed.append((int(tm.group(1), 16), int(tm.group(2) or 1)))
            if parsed is None:
                break  # first non-token line ends the section
            seq.extend(parsed)
    return (seq, header) if header else (None, None)


def find_ring_pointer(log_path):
    """Find the 'dumped to <path>' pointer + its total count in a boot log."""
    ptr = None
    with open(log_path, errors="replace") as fh:
        for line in fh:
            m = DUMPED_TO_RE.search(line)
            if m:
                ptr = (m.group(2), int(m.group(1)))  # last one wins
    return ptr


# ---------------------------------------------------------------------------
# Source resolution: positional arg -> (trace records, r24 seq)
# ---------------------------------------------------------------------------

def resolve_sources(target, explicit_ring):
    """Returns dict with keys: records, header, r24_seq, r24_header, notes (list of str)."""
    out = {"records": None, "header": None, "r24_seq": None, "r24_header": None, "notes": []}

    log_path = None
    if os.path.isdir(target):
        cand = os.path.join(target, "boot.log")
        if not os.path.isfile(cand):
            sys.exit("error: %s is a directory with no boot.log" % target)
        log_path = cand
    elif os.path.isfile(target):
        recs, hdr = parse_trace_ring(target)
        if recs is not None:
            out["records"], out["header"] = recs, hdr
            out["notes"].append("trace ring: %s (%d records, #%d..#%d)" %
                                (target, len(recs), hdr["first"], hdr["last"]))
        else:
            log_path = target
    else:
        sys.exit("error: no such file or directory: %s" % target)

    if log_path:
        out["r24_seq"], out["r24_header"] = parse_r24_section(log_path)
        if out["r24_header"]:
            expanded = sum(c for _, c in out["r24_seq"])
            out["notes"].append("R24RING: %s (%d transitions recorded, %d expanded entries parsed)"
                                % (log_path, out["r24_header"]["total"], expanded))
        # Trace ring lives in a separate file; follow the pointer unless overridden.
        ring_path, log_total = None, None
        if explicit_ring:
            ring_path = explicit_ring
        else:
            ptr = find_ring_pointer(log_path)
            if ptr:
                ring_path, log_total = ptr
        if ring_path and os.path.isfile(ring_path):
            recs, hdr = parse_trace_ring(ring_path)
            if recs is not None:
                if log_total is not None and hdr["total"] not in (log_total, log_total + 1):
                    out["notes"].append(
                        "WARNING: %s total=%d but the log dumped total=%d — "
                        "/tmp/ss_jit_ring.txt is shared between runs and may be STALE; "
                        "pass --ring with the right file" % (ring_path, hdr["total"], log_total))
                out["records"], out["header"] = recs, hdr
                out["notes"].append("trace ring: %s (%d records, #%d..#%d)" %
                                    (ring_path, len(recs), hdr["first"], hdr["last"]))
        elif ring_path:
            out["notes"].append("note: trace-ring file %s not found (log pointed at it)" % ring_path)

    if out["records"] is None and out["r24_seq"] is None:
        sys.exit("error: no trace-ring dump or [R24RING] section found in %s" % target)
    return out


# ---------------------------------------------------------------------------
# Actions
# ---------------------------------------------------------------------------

def need_records(src):
    if src["records"] is None:
        sys.exit("error: this action needs a trace-ring dump (none found; try --ring FILE)")
    return src["records"], src["header"]


def do_window(src, spec):
    recs, hdr = need_records(src)
    try:
        lo, hi = (int(x, 0) for x in spec.split(":"))
    except ValueError:
        sys.exit("error: --window wants START:END (absolute record numbers, END inclusive)")
    hit = [r for r in recs if lo <= r.num <= hi]
    print("# window #%d..#%d: %d records (dump holds #%d..#%d)" %
          (lo, hi, len(hit), hdr["first"], hdr["last"]))
    if lo < hdr["first"] or hi > hdr["last"]:
        print("# WARNING: window partly outside the dump — evicted records are gone "
              "(re-capture with SS_RING_WINDOW / SS_RING_DUMP_FROM)")
    for r in hit:
        print(r.line())


NONPC_FLOOR = 0x1000          # below this, r24 is scratch (ASCII bytes), not a 68k PC
GUEST_CODE_CEIL = 0x60000000  # RAM + ROM + KDP; beyond (e.g. ffffffff) = scratch too


def is_pc_like(v):
    return NONPC_FLOOR <= v < GUEST_CODE_CEIL


def classify_step(prev, cur):
    """Classify a 68k PC transition. Returns (tag, is_candidate)."""
    if cur == prev:
        return "same", False
    if cur % 2 or (cur - prev) % 2:
        return "DESYNC-CANDIDATE", True
    d = cur - prev
    if d in (2, 4, 6):
        return "seq+%d" % d, False
    return "BRANCH %+d" % d, False


def r24_iter(src):
    """Yield (index, value, count) oldest-first from whichever r24 source exists.
    Prefers the R24RING section (index = 0-based expanded position); falls back
    to the trace ring's r24 column (index = absolute record number)."""
    if src["r24_seq"] is not None:
        i = 0
        for v, c in src["r24_seq"]:
            yield i, v, c
            i += c
    elif src["records"] is not None:
        last = None
        for r in src["records"]:
            if r.fields.get("r24") != last:
                last = r.fields.get("r24")
                yield r.num, last, 1
    else:
        sys.exit("error: no r24 source (need an R24RING section or a trace-ring dump)")


def do_r24_flow(src, limit, all_lines):
    prev = None
    shown = candidates = total = 0
    skipped = 0
    for idx, v, count in r24_iter(src):
        total += 1
        if not is_pc_like(v):
            prev = None      # scratch value: reset flow state, don't classify across it
            skipped += 1
            continue
        if prev is None:
            tag, cand = "(start)", False
        else:
            tag, cand = classify_step(prev, v)
            if cand:
                tag += " (from %08x, delta %+d)" % (prev, v - prev)
        prev = v
        if cand:
            candidates += 1
        if cand or all_lines or shown < limit:
            rep = "*%d" % count if count > 1 else ""
            marker = "  <<<" if cand else ""
            print("@%-10d %08x%-8s %s%s" % (idx, v, rep, tag, marker))
            shown += 1
    print("# r24-flow: %d ring entries walked, %d NON-PC skipped, "
          "%d DESYNC-CANDIDATE" % (total, skipped, candidates))


def do_find_pc(src, pc_str):
    pc = int(pc_str, 16) if not pc_str.startswith("0x") else int(pc_str, 0)
    hits = 0
    if src["records"] is not None:
        for r in src["records"]:
            where = [w for w, v in (("from", r.from_pc), ("to", r.to_pc),
                                    ("r24", r.fields.get("r24"))) if v == pc]
            if where:
                print("%s  [%s]" % (r.line(), ",".join(where)))
                hits += 1
    if src["r24_seq"] is not None:
        for idx, v, count in r24_iter(src):
            if v == pc:
                rep = "*%d" % count if count > 1 else ""
                print("R24RING @%-10d %08x%s" % (idx, v, rep))
                hits += 1
    print("# find-pc %08x: %d hits" % (pc, hits))


def do_regs_at(src, rec_str):
    recs, hdr = need_records(src)
    num = int(rec_str.lstrip("#"), 0)
    hit = [r for r in recs if r.num == num]
    if not hit:
        sys.exit("error: record #%d not in the dump (holds #%d..#%d)" %
                 (num, hdr["first"], hdr["last"]))
    r = hit[0]
    f = r.fields
    when = {"J": "AFTER block", "I": "BEFORE block", "C": "BEFORE insn",
            "E": "EmulOp inputs", "R": "EmulOp results"}.get(r.type, "?")
    print("record #%d  type=%s (regs %s)  from=%08x to=%08x op=%08x" %
          (r.num, r.type, when, r.from_pc, r.to_pc, f.get("op", 0)))
    print("  68k: pc(r24)=%08x opc(r27)=%08x handler(r29)=%08x sp(r1/A7')=%08x" %
          (f.get("r24", 0), f.get("r27", 0), f.get("r29", 0), f.get("sp", 0)))
    print("  ppc: lr=%08x ctr=%08x cr=%08x" % (f.get("lr", 0), f.get("ctr", 0), f.get("cr", 0)))
    for bank in ("a", "d"):
        print("  %s0-%s7: %s" % (bank, bank,
              " ".join("%08x" % f.get("%s%d" % (bank, i), 0) for i in range(8))))


def do_summary(src):
    for n in src["notes"]:
        print(n)
    if src["records"]:
        by_type = {}
        for r in src["records"]:
            by_type[r.type] = by_type.get(r.type, 0) + 1
        print("trace-ring record types: " +
              " ".join("%s=%d" % kv for kv in sorted(by_type.items())))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n")[0],
        epilog="No action flag = summary. Full format notes in the file header.")
    ap.add_argument("target", help="boot log, trace-ring dump file, or slot rundir")
    ap.add_argument("--ring", help="explicit trace-ring dump path (overrides the log's pointer)")
    ap.add_argument("--window", metavar="START:END",
                    help="print trace-ring records by absolute record number (END inclusive)")
    ap.add_argument("--r24-flow", action="store_true",
                    help="walk the r24 (68k PC) sequence, mark transitions, flag DESYNC-CANDIDATEs")
    ap.add_argument("--find-pc", metavar="0xPC", help="records touching a PC (from/to/r24 + R24RING)")
    ap.add_argument("--regs-at", metavar="REC", help="full register file at absolute record # ")
    ap.add_argument("--limit", type=int, default=64,
                    help="r24-flow: max non-candidate lines printed (default 64; candidates always print)")
    ap.add_argument("--all", action="store_true", help="r24-flow: print every line (no limit)")
    args = ap.parse_args()

    src = resolve_sources(args.target, args.ring)
    if args.window:
        do_window(src, args.window)
    elif args.r24_flow:
        do_r24_flow(src, args.limit, args.all)
    elif args.find_pc:
        do_find_pc(src, args.find_pc)
    elif args.regs_at:
        do_regs_at(src, args.regs_at)
    else:
        do_summary(src)


if __name__ == "__main__":
    main()
