# Installing classic Mac software onto a disk image — host-side, forks intact

> **Why this exists:** Getting period-correct Mac apps (from `.hqx`/`.sit` abandonware archives) onto an
> HFS disk image **from the host** — without booting the emulator — so the E2E workload disks (see
> `docs/planning/` S4 / `SheepShaver/e2e/`) can be populated/scripted. The hard part is **resource forks
> + type/creator**: a classic Mac application is useless without its resource fork and an `APPL`
> type/`creator` signature, and the naive tools silently drop them. This is the verified, repeatable
> procedure (verified on **macOS arm64**; Linux notes at the end).

## TL;DR pipeline

```
download .hqx/.sit  →  unar (decode, keeps forks as xattrs)  →  repackage as MacBinary II
                    →  hformat the disk image  →  hcopy -m onto it  →  verify with hdir
```

The one rule that bites everyone: **`hcopy` without `-m` is data-fork-only** — it loses the resource
fork and stamps the file `????/UNIX`, so the app won't launch. Always go through **MacBinary** for apps.

---

## 0. Tools

- **`unar` / `lsar`** (The Unarchiver) — `brew install unar`. Decodes BinHex (`.hqx`), StuffIt
  (`.sit`/`.sitx`), Compact Pro, etc., and **preserves Mac forks**. This is the replacement for the old
  `macutils`/`hexbin`, which is **not in Homebrew**.
- **`hfsutils`** — `brew install hfsutils`. `hformat`, `hmount`/`humount`, `hcopy`, `hdir`, `hls`, `hcd`,
  `hdel`. Note: classic **HFS only** (not HFS+); fine for these small test volumes. Isolate its state
  with a throwaway `HOME` (it writes `~/.hcwd`): `export HOME=/tmp/hfs_scratch && mkdir -p "$HOME"`.

## 1. Download

The software is usually `.hqx` (BinHex 4.0, text) or `.sit` (StuffIt, binary), often **`.sit` wrapped in
BinHex** (`lsar` will say `StuffIt 5 in BinHex`). Grab it with `curl -L -A Mozilla/5.0 <url> -o file.hqx`.
A valid `.hqx` starts with `(This file must be converted with BinHex 4.0)`.

## 2. Decode with `unar` (forks land as xattrs)

```bash
lsar "AltiVec Fractal Carbon.hqx"          # inspect: shows the entries + (… , rsrc) fork sizes
unar -force-overwrite "AltiVec Fractal Carbon.hqx" -o /tmp/extract
```

On macOS the extracted file carries the forks as extended attributes:
- **data fork** = the file itself.
- **resource fork** = `com.apple.ResourceFork` xattr, readable at the path `"<file>/..namedfork/rsrc"`.
- **type/creator + Finder flags** = `com.apple.FinderInfo` xattr (32 bytes: `type[4] creator[4] flags[2]
  location[4] …`). Read it with `xattr -px com.apple.FinderInfo "<file>"`.

Confirm with `ls -la@ "<file>"` — you should see `com.apple.ResourceFork` and `com.apple.FinderInfo`.

## 3. Repackage as MacBinary II

MacBinary II is the OS-agnostic single-file container that carries **data fork + resource fork +
type/creator**, and `hcopy -m` knows how to unpack it onto an HFS volume. Build it from the three pieces:

```python
import os, struct, subprocess
src  = "/tmp/extract/AltiVec Fractal Carbon"
name = os.path.basename(src)
data = open(src, "rb").read()
try:    rsrc = open(src + "/..namedfork/rsrc", "rb").read()   # macOS resource-fork path
except FileNotFoundError: rsrc = b""
try:    finfo = bytes.fromhex("".join(
            subprocess.check_output(["xattr","-px","com.apple.FinderInfo",src], text=True).split()))
except Exception: finfo = b"\x00"*32
finfo = (finfo + b"\x00"*32)[:32]

h = bytearray(128)
nb = name.encode("mac_roman")[:63]
h[1] = len(nb); h[2:2+len(nb)] = nb
h[65:69] = finfo[0:4]            # type
h[69:73] = finfo[4:8]            # creator
h[73:74] = finfo[8:9]            # Finder flags (high byte)
h[75:79] = finfo[10:14]          # icon location v/h
h[79:81] = finfo[14:16]
struct.pack_into(">I", h, 83, len(data))    # data fork length
struct.pack_into(">I", h, 87, len(rsrc))    # resource fork length
mt = int(os.stat(src).st_mtime) + 2082844800   # Unix -> Mac (1904) epoch
struct.pack_into(">I", h, 91, mt); struct.pack_into(">I", h, 95, mt)
h[101:102] = finfo[9:10]         # Finder flags (low byte) — MacBinary II
h[122] = 129; h[123] = 129       # MacBinary II version / min-version-to-read
def crc16(b):                    # CRC-16-CCITT (XMODEM), poly 0x1021, init 0
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if (c & 0x8000) else (c << 1) & 0xFFFF
    return c
struct.pack_into(">H", h, 124, crc16(bytes(h[0:124])))
pad = lambda b: b + b"\x00" * ((128 - len(b) % 128) % 128)   # forks padded to 128-byte multiples
open("/tmp/app.bin", "wb").write(bytes(h) + pad(data) + pad(rsrc))
```

**Gotchas:**
- `os.getxattr` is **Linux-only** — on macOS Python use the `xattr` *command* (as above).
- The CRC at offset 124 matters; some readers reject a bad one. The loop above is the right CCITT/XMODEM CRC.
- Forks are each **zero-padded to a 128-byte boundary**; the header is exactly 128.

## 4. Format the disk image (raw HFS — SheepShaver mounts it)

```bash
mkfile -n 1g /Users/Shared/macemu/e2e-apps.dsk        # sparse blank; (Linux: truncate -s 1G …)
export HOME=/tmp/hfs_scratch && mkdir -p "$HOME"
hformat -l "E2E Apps" /Users/Shared/macemu/e2e-apps.dsk
```

`hformat` makes a **raw HFS volume** (no Apple Partition Map). **SheepShaver mounts a raw HFS image
directly** — verified: attaching it as a second `disk` line boots with **no "this disk is unreadable —
Initialize?" prompt** (`modal=False` in the introspection snapshot). So you do *not* need to initialize
it inside the emulator.

## 5. Copy the app onto the volume — **`-m` (MacBinary), always, for apps**

```bash
hmount /Users/Shared/macemu/e2e-apps.dsk
hcopy -m /tmp/app.bin ":AltiVec Fractal Carbon"     # -m = MacBinary -> restores forks + type/creator
hdir ; humount
```

`hdir` is the **verification** — a correctly-installed app shows its type/creator and a **non-zero
resource fork**:

```
f  APPL/ddPF      8126    169060   AltiVec Fractal Carbon     ← good: APPL type, 8126-byte rsrc fork
f  ????/UNIX         0    169060   AltiVec Fractal Carbon     ← BROKEN: plain hcopy, no rsrc, wrong type
```

If you see `????/UNIX` and `0` resource fork, you used plain `hcopy` — redo with `-m` on a MacBinary.

## 6. Launching it (no alias needed)

To run an app that lives on an attached data volume, you don't need a Startup-Items alias (aliases are
painful to forge host-side). Classic Finder supports **keyboard type-select**, so drive it over VNC:

```
boot a boot-to-Finder OS 9 disk → at the Finder, type the volume name (e.g. "E2E") → ⌘O  (open volume)
  → in its window, type the app name (e.g. "AltiVec") → ⌘O  (launch)
```

The guest-UI introspection (`SS_UI_DUMP_DIR`, `SheepShaver/docs/UI-INTROSPECTION.md`) confirms each
window/app appears, so the scenario can gate on facts rather than fixed coordinates.

---

## Linux host notes (upstream ARM64 — to verify)

- `unar`/`lsar` exist on Linux (`apt install unar`), but Linux has no native resource fork: `unar`
  typically writes the resource fork as an **AppleDouble** `._<name>` sidecar (or under `__MACOSX/`).
  Read the resource fork from the `._` file's AppleDouble payload, and FinderInfo from its header,
  instead of `/..namedfork/rsrc` + the `xattr` command. The MacBinary repackaging (§3) and
  `hformat`/`hcopy -m` (§4–5) are otherwise host-OS-agnostic.
- `mkfile` is macOS-only → use `truncate -s <size> file` on Linux.

## See also
- `SheepShaver/docs/UI-INTROSPECTION.md` — driving the launched app by its real menus/windows.
- `SheepShaver/e2e/README.md` / `assets/README.md` — the two-disk (boot + attached apps) harness model.
- `docs/MACOS9-STRESS-WORKLOADS.md` — the catalog of apps worth installing this way.
