# Development & Debugging Tools

## screenshot.sh — capture the guest display

Captures what the emulated Mac is showing, **without** macOS Screen Recording
permission, by dumping the guest framebuffer from the running SheepShaver
process and converting it to PNG.

```bash
./tools/screenshot.sh                    # → /tmp/sheepshaver_screen.png
./tools/screenshot.sh /path/to/out.png   # → custom path
```

How it works: SheepShaver maps the guest framebuffer at a fixed host address
(`NATMEM_OFFSET + mac_frame_base` = `0x400050590000` for the standard
800×600×32bpp mode). The script attaches lldb to the running process, dumps
the raw pixels, detaches (the emulator pauses ~100 ms), and writes a PNG.

Overrides via environment: `SS_FB_WIDTH`, `SS_FB_HEIGHT`, `SS_FB_ADDR`.

Note: SheepShaver also has a built-in VNC server (`vnc_server.cpp`), but it's
compiled as a no-op stub unless libvncserver is installed at build time.

## JIT debugging environment variables

| Variable | Effect |
|---|---|
| `SS_USE_JIT=0` | Force interpreter-only execution |
| `SS_JIT_TRACE=/path` | Log every block dispatch (`I <pc>`) and JIT execution (`J <from> <to> ...regs`) — flushed per line so traces survive crashes |
| `SS_JIT_DEBUG_PC=<hex>` | Print every compile decision for one PC (cache hit/miss, fetch failure, failing instruction) |
| `SS_JIT_NO_OE=1` | Bisect switch: fall back to interpreter for OE=1 arithmetic (addco/subfco/...) |
| `SS_JIT_NO_ROM=1` | Bisect switch: keep Mac ROM interpreter-only (no ROM block compilation) |

## Differential trace debugging recipe

The most effective way to find JIT correctness bugs (used to find every bug so far):

1. Run a **reference** boot (correct behavior): `SS_USE_JIT=1 SS_JIT_NO_OE=1 SS_JIT_TRACE=/tmp/ref.txt ./SheepShaver`
2. Run the **suspect** boot: `SS_USE_JIT=1 SS_JIT_TRACE=/tmp/bug.txt ./SheepShaver`
3. Diff the traces by guest-PC position (not line number — block granularity differs).
   The first divergence in control flow or register state localizes the bug to one block.

The Mac ROM's built-in 68k emulator uses r24=68k PC, r27=68k opcode, r29=handler
address — the trace logs these, so 68k-level execution can be followed directly.
