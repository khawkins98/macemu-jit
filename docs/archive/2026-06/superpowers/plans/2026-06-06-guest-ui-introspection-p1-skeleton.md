> **ARCHIVED 2026-06-12** — Moved to archive during the Reku pass.
> May contain outdated assumptions, resolved questions, or superseded plans.
> Current state: `docs/HANDOFF.md` · `docs/planning/ROADMAP.md`
> **Reason:** milestone complete
>

# Guest UI Introspection — Plan 1 (Walking Skeleton) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship an end-to-end, env-gated "what windows are on screen" dump — Backend A walks the guest `WindowList` at the idle safe point, writes a nonce-stamped JSON snapshot of the front→back window list (title, bounds, kind, active/visible) to a file, and a Python `uidump` layer requests + parses + queries it.

**Architecture:** A new read-only C++ module (`ui_introspect.{h,cpp}`) is called from the existing idle hook (`emul_op.cpp` `OP_IDLE_TIME`). When `SS_UI_DUMP_DIR` is set, it polls for a request file, walks `WindowList` reading documented `WindowRecord` offsets, serializes JSON by hand, and writes artifacts atomically with a `.done` sentinel carrying the request nonce. A Python module (`sse2e/uidump.py`) drives the file handshake and provides query helpers. Pure text transforms (MacRoman→UTF-8, JSON-escape) live in a dependency-free header with a standalone C++ unit test; the Python layer and an integration smoke cover the rest.

**Tech Stack:** C++ (SheepShaver emulator, autoconf build), Python 3 + pytest (the existing `sse2e` e2e package).

**Scope note — this is Plan 1 of 3** (per the spec `docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md`). Plan 1 = walking skeleton (window list + transport + Python layer). **Plan 2** = rich fields (dialog items/DITL, control state, window-part hot-zones, ParamText, menu bar, full WDEF window class, screen depth). **Plan 3** = Backend B (Toolbox-trap oracle) + `compare()` + `overlay()`. Plan 1 deliberately emits only the window-list subset of the schema; later plans add fields without breaking the v1 contract.

**Reference offsets (classic `WindowRecord`, GrafPort = 108/0x6C bytes; verified against the two offsets already used in `emul_op.cpp` — `windowKind`@+0x6C, `titleHandle`@+0x86):**

| Field | Offset | Type | Notes |
|-------|--------|------|-------|
| `windowKind` | +0x6C | int16 | 2 = `dialogKind` |
| `visible` | +0x6E | byte | 0/1 |
| `hilited` | +0x6F | byte | 0/1 — the real "active" bit |
| `strucRgn` | +0x72 | RgnHandle | structure region (global) |
| `contRgn` | +0x76 | RgnHandle | content region (global) |
| `titleHandle` | +0x86 | StringHandle | already used in `emul_op.cpp` |
| `nextWindow` | +0x90 | WindowPeek | z-order chain (front→back) |
| `refCon` | +0x98 | int32 | window refCon |

Low-mem globals: `WindowList`@`$09D6` (front WindowPeek), `CrsrPin`@`$0834` (Rect = screen bounds), `Ticks`@`$016A`, `SysVersion`@`$015A`. A `Region` is `{int16 rgnSize; Rect rgnBBox;…}` — its bounding box is `Rect` at masterPtr+2 = `{top@+2, left@+4, bottom@+6, right@+8}` (all int16, big-endian). `Rect`/coords are global = VNC framebuffer pixels.

---

## File Structure

- **Create `SheepShaver/src/ui_introspect_text.h`** — dependency-free pure transforms: `macroman_to_utf8()`, `json_escape()`. Header-only so the standalone unit test can include it without linking the emulator.
- **Create `SheepShaver/src/ui_introspect.h`** — public API: `void ui_introspect_service(void)` (called from the idle hook). Also declares the shared guest-read helpers used by both `emul_op.cpp` and the module.
- **Create `SheepShaver/src/ui_introspect.cpp`** — request-file poll, `WindowList` walk (Backend A), JSON serialization, atomic write + `.done` sentinel.
- **Create `SheepShaver/src/ui_introspect_test.cpp`** — standalone `main()` unit test for `ui_introspect_text.h` (no emulator link).
- **Modify `SheepShaver/src/emul_op.cpp`** — call `ui_introspect_service()` from `OP_IDLE_TIME`/`OP_IDLE_TIME_2`; move the shared `e2e_guest_ptr_ok` deref-guard into `ui_introspect.h` and reuse it (DRY).
- **Modify `SheepShaver/src/Unix/Makefile.in`** — add `../ui_introspect.cpp` to `SRCS`.
- **Modify `SheepShaver/Makefile`** — add a platform-guarded `ui-introspect-test` target that builds + runs `ui_introspect_test.cpp`.
- **Create `SheepShaver/e2e/sse2e/uidump.py`** — `Snapshot` dataclass, `snapshot()` handshake, query helpers (`windows`, `find`, `front_window`, `clickable`).
- **Create `SheepShaver/e2e/tests/test_uidump.py`** — offline unit tests (fixture JSON, no boot).
- **Create `SheepShaver/e2e/tests/fixtures/ui_two_windows.json`** — a sample snapshot fixture.

---

## Task 1: Pure text transforms (MacRoman→UTF-8, JSON-escape) + standalone unit test

**Files:**
- Create: `SheepShaver/src/ui_introspect_text.h`
- Create: `SheepShaver/src/ui_introspect_test.cpp`
- Modify: `SheepShaver/Makefile` (add `ui-introspect-test` target)

- [ ] **Step 1: Write the failing test**

Create `SheepShaver/src/ui_introspect_test.cpp`:

```cpp
// Standalone unit test for ui_introspect_text.h — no emulator link.
// Build/run: make -C SheepShaver ui-introspect-test
#include "ui_introspect_text.h"
#include <cstdio>
#include <string>
#include <cassert>

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}

int main() {
    // ASCII passes through unchanged.
    check(macroman_to_utf8("Macintosh HD") == "Macintosh HD", "ascii passthrough");
    // 0x8E = MacRoman 'é' -> U+00E9 (UTF-8 C3 A9).
    { std::string in("caf\x8e"); check(macroman_to_utf8(in) == "caf\xc3\xa9", "macroman e-acute"); }
    // 0xD2 = MacRoman left double quote -> U+201C (UTF-8 E2 80 9C).
    { std::string in("\xd2hi\xd3"); check(macroman_to_utf8(in) == "\xe2\x80\x9chi\xe2\x80\x9d", "macroman curly quotes"); }
    // 0xF0 = Apple logo -> U+F8FF (UTF-8 EF A3 BF).
    { std::string in("\xf0"); check(macroman_to_utf8(in) == "\xef\xa3\xbf", "apple logo -> U+F8FF"); }

    // JSON escaping: quote, backslash, and control chars.
    check(json_escape("a\"b\\c") == "a\\\"b\\\\c", "escape quote+backslash");
    check(json_escape("x\ny") == "x\\ny", "escape newline");
    check(json_escape("tab\there") == "tab\\there", "escape tab");
    // A raw control byte (0x01) becomes .
    { std::string in("a\x01"); check(json_escape(in) == "a\\u0001", "escape control -> \\u00xx"); }

    if (failures == 0) printf("ui_introspect_text: ALL OK\n");
    return failures ? 1 : 0;
}
```

- [ ] **Step 2: Add the build target and run it to verify it fails to compile**

Add to `SheepShaver/Makefile` (place near the other platform-guarded test targets such as `test-opcodes`; keep the same `UNAME`/`Darwin` guard style already in that file):

```make
.PHONY: ui-introspect-test
ui-introspect-test:
	c++ -std=c++17 -Wall -o /tmp/ui_introspect_test src/ui_introspect_test.cpp
	/tmp/ui_introspect_test
```

Run: `make -C SheepShaver ui-introspect-test`
Expected: FAIL — `ui_introspect_text.h: No such file or directory`.

- [ ] **Step 3: Write minimal implementation**

Create `SheepShaver/src/ui_introspect_text.h`:

```cpp
// Pure, dependency-free text transforms for UI introspection JSON output.
// Header-only so the standalone unit test (ui_introspect_test.cpp) needs no emulator link.
#ifndef UI_INTROSPECT_TEXT_H
#define UI_INTROSPECT_TEXT_H

#include <string>
#include <cstdint>
#include <cstdio>

// MacRoman code points 0x80–0xFF -> Unicode. Standard Apple "Mac OS Roman" table,
// EXCEPT 0xF0 which is the Apple-logo glyph -> U+F8FF (Apple's own private-use codepoint).
static const uint16_t kMacRomanToUnicode[128] = {
    0x00C4,0x00C5,0x00C7,0x00C9,0x00D1,0x00D6,0x00DC,0x00E1, // 80
    0x00E0,0x00E2,0x00E4,0x00E3,0x00E5,0x00E7,0x00E9,0x00E8, // 88
    0x00EA,0x00EB,0x00ED,0x00EC,0x00EE,0x00EF,0x00F1,0x00F3, // 90
    0x00F2,0x00F4,0x00F6,0x00F5,0x00FA,0x00F9,0x00FB,0x00FC, // 98
    0x2020,0x00B0,0x00A2,0x00A3,0x00A7,0x2022,0x00B6,0x00DF, // A0
    0x00AE,0x00A9,0x2122,0x00B4,0x00A8,0x2260,0x00C6,0x00D8, // A8
    0x221E,0x00B1,0x2264,0x2265,0x00A5,0x00B5,0x2202,0x2211, // B0
    0x220F,0x03C0,0x222B,0x00AA,0x00BA,0x03A9,0x00E6,0x00F8, // B8
    0x00BF,0x00A1,0x00AC,0x221A,0x0192,0x2248,0x2206,0x00AB, // C0
    0x00BB,0x2026,0x00A0,0x00C0,0x00C3,0x00D5,0x0152,0x0153, // C8
    0x2013,0x2014,0x201C,0x201D,0x2018,0x2019,0x00F7,0x25CA, // D0
    0x00FF,0x0178,0x2044,0x20AC,0x2039,0x203A,0xFB01,0xFB02, // D8
    0x2021,0x00B7,0x201A,0x201E,0x2030,0x00C2,0x00CA,0x00C1, // E0
    0x00CB,0x00C8,0x00CD,0x00CE,0x00CF,0x00CC,0x00D3,0x00D4, // E8
    0xF8FF,0x00D2,0x00DA,0x00DB,0x00D9,0x0131,0x02C6,0x02DC, // F0  (0xF0 = Apple logo -> U+F8FF)
    0x00AF,0x02D8,0x02D9,0x02DA,0x00B8,0x02DD,0x02DB,0x02C7  // F8
};

static inline void append_utf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// Decode a MacRoman byte string to UTF-8.
static inline std::string macroman_to_utf8(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c < 0x80) out.push_back((char)c);
        else append_utf8(out, kMacRomanToUnicode[c - 0x80]);
    }
    return out;
}

// Escape a UTF-8 string for embedding in a JSON string literal.
static inline std::string json_escape(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out.push_back((char)c);
        }
    }
    return out;
}

#endif // UI_INTROSPECT_TEXT_H
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `make -C SheepShaver ui-introspect-test`
Expected: PASS — prints `ui_introspect_text: ALL OK`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/src/ui_introspect_text.h SheepShaver/src/ui_introspect_test.cpp SheepShaver/Makefile
git commit -F- <<'EOF'
feat(ss-ui): MacRoman->UTF-8 + JSON-escape transforms + unit test

Dependency-free header for UI introspection text output. Apple-logo
glyph 0xF0 -> U+F8FF. Standalone test via `make ui-introspect-test`.
EOF
```

---

## Task 2: Shared guest-read helper (DRY the deref guard)

Move the safe-pointer guard out of `emul_op.cpp` into the new module header so Backend A reuses it instead of duplicating it.

**Files:**
- Create: `SheepShaver/src/ui_introspect.h`
- Modify: `SheepShaver/src/emul_op.cpp:81-84` (remove the local `e2e_guest_ptr_ok`, include the header)

- [ ] **Step 1: Create the module header with the shared guard**

Create `SheepShaver/src/ui_introspect.h`:

```cpp
// Guest UI introspection (Backend A) — read-only host-side walk of the classic Mac OS
// Toolbox structures. Serviced from the idle hook. See
// docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md
#ifndef UI_INTROSPECT_H
#define UI_INTROSPECT_H

#include "sysdeps.h"
#include "cpu_emulation.h"   // ReadMacInt*/Mac2HostAddr, RAMBase/RAMSize

// Is `a` a guest pointer we can safely dereference? Valid Mac pointers/handles are even-aligned
// and live in mapped Mac RAM; a wild handle outside RAM would SIGSEGV under DIRECT_ADDRESSING.
// (Shared with emul_op.cpp's idle-signal reads.)
static inline bool guest_ptr_ok(uint32 a)
{
    return (a & 1) == 0 && a >= 0x100 && a < RAMBase + RAMSize;
}

// Called from the idle hook (OP_IDLE_TIME). No-op unless SS_UI_DUMP_DIR is set and a request
// file is present; otherwise polls/services one UI-snapshot request.
extern void ui_introspect_service(void);

#endif // UI_INTROSPECT_H
```

- [ ] **Step 2: Point `emul_op.cpp` at the shared guard**

In `SheepShaver/src/emul_op.cpp`, delete the local definition (currently at lines ~81-84):

```cpp
static inline bool e2e_guest_ptr_ok(uint32 a)
{
	return (a & 1) == 0 && a >= 0x100 && a < RAMBase + RAMSize;
}
```

Add near the top includes of `emul_op.cpp` (after the existing `#include`s):

```cpp
#include "ui_introspect.h"
```

Then replace the two call sites in `emul_op.cpp` that use `e2e_guest_ptr_ok(` with `guest_ptr_ok(` (in `e2e_front_window_title`, lines ~108-118). Use a global replace of `e2e_guest_ptr_ok` → `guest_ptr_ok` within `emul_op.cpp`.

- [ ] **Step 3: Build to verify it still compiles + links**

Run: `cd SheepShaver && make build-ss`
Expected: builds clean (the binary still links; `guest_ptr_ok` now comes from the header). If the build tree isn't configured, run the one-time configure first (see `SheepShaver/e2e/README.md` "Building the emulator").

- [ ] **Step 4: Sanity-run the offline harness (no regression)**

Run: `cd SheepShaver && make test-jit`
Expected: score=100 (unchanged — this task is a pure refactor + new unused header).

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/src/ui_introspect.h SheepShaver/src/emul_op.cpp
git commit -F- <<'EOF'
refactor(ss-ui): share guest-ptr deref guard via ui_introspect.h

Hoist e2e_guest_ptr_ok out of emul_op.cpp as guest_ptr_ok so Backend A
reuses it. Declares ui_introspect_service() (impl follows). No behavior
change; make test-jit score=100.
EOF
```

---

## Task 3: Backend A window-list walk + JSON serialization + transport

Implement the actual snapshot: poll the request file, walk `WindowList`, serialize JSON, write artifacts atomically with a nonce-stamped `.done`.

**Files:**
- Create: `SheepShaver/src/ui_introspect.cpp`
- Modify: `SheepShaver/src/Unix/Makefile.in:68` (add `../ui_introspect.cpp` to `SRCS`)
- Modify: `SheepShaver/src/emul_op.cpp` (call `ui_introspect_service()` from the idle handlers)

- [ ] **Step 1: Create `ui_introspect.cpp`**

```cpp
#include "ui_introspect.h"
#include "ui_introspect_text.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

// --- low-mem globals + WindowRecord offsets (see plan's offset table) ---
enum {
    kWindowList = 0x09D6,
    kCrsrPin    = 0x0834,   // Rect = screen bounds
    kTicks      = 0x016A,
    kSysVersion = 0x015A,
};
enum {                       // offsets within a WindowRecord (GrafPort = 0x6C)
    kWinKind   = 0x6C,       // int16; 2 = dialogKind
    kWinVisible= 0x6E,       // byte
    kWinHilited= 0x6F,       // byte (the real "active" bit)
    kWinStruc  = 0x72,       // RgnHandle
    kWinCont   = 0x76,       // RgnHandle
    kWinTitleH = 0x86,       // StringHandle
    kWinNext   = 0x90,       // WindowPeek
    kWinRefCon = 0x98,       // int32
};

struct Rect16 { int16 top, left, bottom, right; bool ok; };

// Read a Region handle's bounding box (global coords). ok=false if any deref is wild.
static Rect16 region_bbox(uint32 rgnHandle) {
    Rect16 r = {0,0,0,0,false};
    if (!rgnHandle || !guest_ptr_ok(rgnHandle)) return r;
    uint32 ptr = ReadMacInt32(rgnHandle);            // master pointer
    if (!ptr || !guest_ptr_ok(ptr)) return r;
    r.top    = (int16)ReadMacInt16(ptr + 2);
    r.left   = (int16)ReadMacInt16(ptr + 4);
    r.bottom = (int16)ReadMacInt16(ptr + 6);
    r.right  = (int16)ReadMacInt16(ptr + 8);
    r.ok = true;
    return r;
}

// Read a window's title (StringHandle -> Str255), MacRoman-decoded. "" if untitled/wild.
static std::string window_title(uint32 win) {
    uint32 hdl = ReadMacInt32(win + kWinTitleH);
    if (!hdl || !guest_ptr_ok(hdl)) return "";
    uint32 ptr = ReadMacInt32(hdl);
    if (!ptr || !guest_ptr_ok(ptr)) return "";
    uint8 *s = Mac2HostAddr(ptr);
    int len = s[0];
    if (len > 255) return "";
    return macroman_to_utf8(std::string((const char *)s + 1, len));
}

static void append_rect(std::string &j, const char *key, const Rect16 &r) {
    char b[160];
    snprintf(b, sizeof(b),
        "\"%s\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d}",
        key, r.left, r.top, r.right, r.bottom);
    j += b;
}

// Serialize the window list (Backend A) to JSON. nonce is the request nonce echoed into output.
static std::string serialize_snapshot(const std::string &nonce) {
    std::string j = "{";
    char hdr[256];

    // header
    uint32 ticks = ReadMacInt32(kTicks);
    uint16 sv = ReadMacInt16(kSysVersion);
    int sv_maj = (sv >> 8) & 0xf, sv_min = (sv >> 4) & 0xf, sv_bug = sv & 0xf;
    snprintf(hdr, sizeof(hdr),
        "\"schemaVersion\":1,\"backend\":\"A\",\"nonce\":\"%s\",\"ticks\":%u,"
        "\"sysVersion\":\"%d.%d.%d\",\"coords\":\"global\",\"text\":\"utf-8\",",
        json_escape(nonce).c_str(), ticks, sv_maj, sv_min, sv_bug);
    j += hdr;

    // screen from CrsrPin (Rect). depth filled in Plan 2.
    {
        int16 t = (int16)ReadMacInt16(kCrsrPin + 0), l = (int16)ReadMacInt16(kCrsrPin + 2);
        int16 b = (int16)ReadMacInt16(kCrsrPin + 4), rt = (int16)ReadMacInt16(kCrsrPin + 6);
        char sc[96];
        snprintf(sc, sizeof(sc), "\"screen\":{\"width\":%d,\"height\":%d,\"depth\":0},", rt - l, b - t);
        j += sc;
    }

    // modalActive = front window is a dialog (windowKind==2)
    uint32 front = ReadMacInt32(kWindowList);
    bool modal = front && guest_ptr_ok(front) && ((int16)ReadMacInt16(front + kWinKind) == 2);
    char ma[64];
    snprintf(ma, sizeof(ma), "\"modalActive\":%s,\"windows\":[", modal ? "true" : "false");
    j += ma;

    int idx = 0, front_index = -1;
    for (uint32 win = front; win && guest_ptr_ok(win); win = ReadMacInt32(win + kWinNext)) {
        int16 kind = (int16)ReadMacInt16(win + kWinKind);
        bool isDialog = (kind == 2);
        bool visible  = Mac2HostAddr(win)[kWinVisible] != 0;   // byte read
        bool active   = Mac2HostAddr(win)[kWinHilited] != 0;
        if (idx == 0) front_index = 0;
        Rect16 sb = region_bbox(ReadMacInt32(win + kWinStruc));
        Rect16 cb = region_bbox(ReadMacInt32(win + kWinCont));
        bool suspect = !sb.ok || !cb.ok;

        if (idx) j += ",";
        char w[256];
        snprintf(w, sizeof(w),
            "{\"index\":%d,\"ptr\":\"0x%08x\",\"title\":\"", idx, win);
        j += w;
        j += json_escape(window_title(win));
        snprintf(w, sizeof(w),
            "\",\"windowClass\":\"%s\",\"modality\":\"%s\",\"isDialog\":%s,"
            "\"active\":%s,\"visible\":%s,\"collapsed\":false,",
            isDialog ? "dialog" : "document", isDialog ? "modal" : "none",
            isDialog ? "true" : "false", active ? "true" : "false", visible ? "true" : "false");
        j += w;
        append_rect(j, "contentBounds", cb.ok ? cb : sb);
        j += ",";
        append_rect(j, "structBounds", sb.ok ? sb : cb);
        if (suspect) j += ",\"suspect\":true";
        j += "}";
        idx++;
        if (idx > 256) break;     // runaway guard
    }
    char tail[64];
    snprintf(tail, sizeof(tail), "],\"frontWindowIndex\":%d}", front_index);
    j += tail;
    return j;
}

// --- transport: poll request file, write artifacts atomically, sentinel last ---

static std::string read_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string s; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// Write `content` to `dir/name` atomically (temp in same dir + rename).
static void write_atomic(const std::string &dir, const char *name, const std::string &content) {
    std::string tmp = dir + "/." + name + ".tmp";
    std::string dst = dir + "/" + name;
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    rename(tmp.c_str(), dst.c_str());
}

// Extract a flat JSON string value: "key":"value" (no nesting/escapes in our request file).
static std::string json_str_field(const std::string &j, const char *key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return "";
    p = j.find('"', p + k.size());          // opening quote of value
    if (p == std::string::npos) return "";
    size_t e = j.find('"', p + 1);
    if (e == std::string::npos) return "";
    return j.substr(p + 1, e - p - 1);
}

void ui_introspect_service(void) {
    const char *dir_c = getenv("SS_UI_DUMP_DIR");
    if (!dir_c || !*dir_c) return;          // feature off -> zero cost
    std::string dir = dir_c;
    std::string req = dir + "/ss_ui.req";

    if (access(req.c_str(), F_OK) != 0) return;   // no pending request

    // Consume the request so we service it exactly once.
    std::string consumed = dir + "/.ss_ui.req.consumed";
    rename(req.c_str(), consumed.c_str());
    std::string body = read_file(consumed);
    std::string nonce = json_str_field(body, "nonce");

    // Backend A only in Plan 1 (ignore "backends"/"screenshot" until Plans 2-3).
    write_atomic(dir, "ss_ui.A.json", serialize_snapshot(nonce));
    write_atomic(dir, "ss_ui.done", std::string("{\"nonce\":\"") + json_escape(nonce) + "\"}");
}
```

- [ ] **Step 2: Wire the service call into the idle hook**

In `SheepShaver/src/emul_op.cpp`, in the `OP_IDLE_TIME` and `OP_IDLE_TIME_2` cases (lines ~743-754), add the service call right after the existing calls:

```cpp
		case OP_IDLE_TIME:
			e2e_emit_idle_signals();
			e2e_check_host_shutdown();
			ui_introspect_service();        // <-- add
			…existing body…
			break;
		case OP_IDLE_TIME_2:
			e2e_emit_idle_signals();
			e2e_check_host_shutdown();
			ui_introspect_service();        // <-- add
			…existing body…
			break;
```

(Place each `ui_introspect_service();` adjacent to the existing `e2e_check_host_shutdown();` in the same case.)

- [ ] **Step 3: Add the source to the build**

In `SheepShaver/src/Unix/Makefile.in`, line 68, add `../ui_introspect.cpp` to `SRCS` next to `../emul_op.cpp`:

```make
    ../rom_patches.cpp ../rsrc_patches.cpp ../emul_op.cpp ../ui_introspect.cpp ../name_registry.cpp \
```

- [ ] **Step 4: Build, then verify the transforms test still passes**

Run: `cd SheepShaver && make build-ss && make ui-introspect-test`
Expected: emulator builds + links with the new module; `ui_introspect_text: ALL OK`.
Also run: `make test-jit` → score=100 (the new code path is dormant unless `SS_UI_DUMP_DIR` is set).

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/src/ui_introspect.cpp SheepShaver/src/Unix/Makefile.in SheepShaver/src/emul_op.cpp
git commit -F- <<'EOF'
feat(ss-ui): Backend A window-list snapshot + file transport

Idle hook polls SS_UI_DUMP_DIR/ss_ui.req, walks WindowList (z-order,
title, struct/content bounds, active/visible/dialog), serializes JSON,
writes ss_ui.A.json + nonce-stamped ss_ui.done atomically. Dormant
unless SS_UI_DUMP_DIR is set; make test-jit score=100.
EOF
```

---

## Task 4: Python `uidump` — snapshot handshake + parse

**Files:**
- Create: `SheepShaver/e2e/sse2e/uidump.py`
- Create: `SheepShaver/e2e/tests/fixtures/ui_two_windows.json`
- Create: `SheepShaver/e2e/tests/test_uidump.py`

- [ ] **Step 1: Write the fixture + failing parse test**

Create `SheepShaver/e2e/tests/fixtures/ui_two_windows.json`:

```json
{
  "schemaVersion": 1, "backend": "A", "nonce": "test1", "ticks": 1000,
  "sysVersion": "8.6.0", "coords": "global", "text": "utf-8",
  "screen": { "width": 1024, "height": 768, "depth": 0 },
  "modalActive": true, "frontWindowIndex": 0,
  "windows": [
    { "index": 0, "ptr": "0x00abe340", "title": "", "windowClass": "dialog",
      "modality": "modal", "isDialog": true, "active": true, "visible": true,
      "collapsed": false,
      "contentBounds": { "left": 312, "top": 300, "right": 712, "bottom": 460 },
      "structBounds":  { "left": 312, "top": 280, "right": 712, "bottom": 460 } },
    { "index": 1, "ptr": "0x00abc120", "title": "Macintosh HD", "windowClass": "document",
      "modality": "none", "isDialog": false, "active": false, "visible": true,
      "collapsed": false,
      "contentBounds": { "left": 100, "top": 100, "right": 500, "bottom": 400 },
      "structBounds":  { "left": 100, "top": 80, "right": 500, "bottom": 400 } }
  ]
}
```

Create `SheepShaver/e2e/tests/test_uidump.py`:

```python
"""Offline tests for the uidump consumer (no emulator/boot)."""
import json
from pathlib import Path

from sse2e import uidump

FIX = Path(__file__).parent / "fixtures" / "ui_two_windows.json"


def _load():
    return uidump.Snapshot(json.loads(FIX.read_text()))


def test_parse_windows_and_front():
    snap = _load()
    assert len(snap.windows) == 2
    assert snap.modal_active is True
    assert snap.front_window().title == ""           # the modal dialog is front
    assert snap.front_window().is_dialog is True


def test_find_by_title():
    snap = _load()
    hd = snap.find(title="Macintosh HD")
    assert len(hd) == 1
    assert hd[0].window_class == "document"
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd SheepShaver/e2e && make e2e-test` (or `.venv/bin/python -m pytest -q tests/test_uidump.py`)
Expected: FAIL — `ModuleNotFoundError: No module named 'sse2e.uidump'`.

- [ ] **Step 3: Implement the parse + query core**

Create `SheepShaver/e2e/sse2e/uidump.py`:

```python
"""Consumer for the guest UI introspection dump (Backend A, Plan 1).

Drives the file handshake (request -> idle service -> nonce-stamped artifacts) and parses the
window-list JSON into a queryable Snapshot. See
docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md.
"""
from __future__ import annotations

import json
import os
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


@dataclass
class Rect:
    left: int
    top: int
    right: int
    bottom: int

    @classmethod
    def from_json(cls, d: dict) -> "Rect":
        return cls(d["left"], d["top"], d["right"], d["bottom"])

    @property
    def center(self) -> tuple[int, int]:
        return ((self.left + self.right) // 2, (self.top + self.bottom) // 2)

    def intersects(self, o: "Rect") -> bool:
        return not (o.left >= self.right or o.right <= self.left
                    or o.top >= self.bottom or o.bottom <= self.top)


class Window:
    def __init__(self, d: dict):
        self._d = d
        self.index: int = d["index"]
        self.title: str = d.get("title", "")
        self.window_class: str = d.get("windowClass", "")
        self.is_dialog: bool = d.get("isDialog", False)
        self.active: bool = d.get("active", False)
        self.visible: bool = d.get("visible", False)
        self.collapsed: bool = d.get("collapsed", False)
        self.content_bounds = Rect.from_json(d["contentBounds"])
        self.struct_bounds = Rect.from_json(d["structBounds"])
        self.suspect: bool = d.get("suspect", False)


class Snapshot:
    def __init__(self, data: dict):
        self.raw = data
        self.backend: str = data.get("backend", "")
        self.nonce: str = data.get("nonce", "")
        self.modal_active: bool = data.get("modalActive", False)
        self.front_index: int = data.get("frontWindowIndex", -1)
        self.windows: list[Window] = [Window(w) for w in data.get("windows", [])]

    def front_window(self) -> Optional[Window]:
        if 0 <= self.front_index < len(self.windows):
            return self.windows[self.front_index]
        return None

    def find(self, *, title=None, window_class=None, visible=None) -> list[Window]:
        out = []
        for w in self.windows:
            if title is not None and w.title != title:
                continue
            if window_class is not None and w.window_class != window_class:
                continue
            if visible is not None and w.visible != visible:
                continue
            out.append(w)
        return out

    def clickable(self, win: Window) -> bool:
        """True if `win` can actually receive a click right now: visible, not collapsed, and not
        sitting behind a modal dialog (windows[] is front->back z-order, so anything after the
        front modal is deactivated)."""
        if not win.visible or win.collapsed:
            return False
        if self.modal_active and not win.active:
            return False
        return True


def snapshot(dump_dir, *, timeout: float = 5.0, poll: float = 0.05) -> Snapshot:
    """Request a fresh snapshot and parse it. Writes a nonce'd request, then polls for ss_ui.done
    carrying that nonce. The emulator must be running with SS_UI_DUMP_DIR=<dump_dir>."""
    d = Path(dump_dir)
    d.mkdir(parents=True, exist_ok=True)
    nonce = uuid.uuid4().hex[:8]

    # Clear any stale sentinel so we never read a prior run's result.
    done = d / "ss_ui.done"
    try:
        done.unlink()
    except FileNotFoundError:
        pass

    # Write the request atomically (temp + rename), then wait.
    req = {"nonce": nonce, "backends": ["A"], "screenshot": False}
    tmp = d / ".ss_ui.req.tmp"
    tmp.write_text(json.dumps(req))
    os.replace(tmp, d / "ss_ui.req")

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if done.exists():
            try:
                if json.loads(done.read_text()).get("nonce") == nonce:
                    data = json.loads((d / "ss_ui.A.json").read_text())
                    return Snapshot(data)
            except (json.JSONDecodeError, FileNotFoundError):
                pass        # mid-write; keep polling
        time.sleep(poll)
    raise TimeoutError(f"no UI snapshot with nonce {nonce} within {timeout}s "
                       f"(is the emulator running with SS_UI_DUMP_DIR={dump_dir}?)")
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd SheepShaver/e2e && .venv/bin/python -m pytest -q tests/test_uidump.py`
Expected: PASS (2 passed).

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/e2e/sse2e/uidump.py SheepShaver/e2e/tests/test_uidump.py SheepShaver/e2e/tests/fixtures/ui_two_windows.json
git commit -F- <<'EOF'
feat(e2e): uidump consumer — snapshot handshake + window query

Snapshot/Window/Rect parse, front_window/find/clickable queries, and the
nonce file handshake (request -> poll ss_ui.done). Offline tests on a
two-window fixture.
EOF
```

---

## Task 5: `clickable()` occlusion tests + a `front_dialog` helper

Lock down the occlusion/z-order logic that makes the dump *trustworthy* for "what can I click."

**Files:**
- Modify: `SheepShaver/e2e/sse2e/uidump.py` (add `front_dialog`)
- Modify: `SheepShaver/e2e/tests/test_uidump.py` (occlusion + dialog tests)

- [ ] **Step 1: Write the failing tests**

Append to `SheepShaver/e2e/tests/test_uidump.py`:

```python
def test_clickable_respects_modal():
    snap = _load()
    front = snap.windows[0]          # modal dialog, active
    behind = snap.windows[1]         # document behind it, inactive
    assert snap.clickable(front) is True
    assert snap.clickable(behind) is False   # behind a modal -> not clickable


def test_clickable_invisible_or_collapsed():
    snap = _load()
    w = snap.windows[1]
    w.visible = False
    assert snap.clickable(w) is False
    w.visible = True
    w.collapsed = True
    assert snap.clickable(w) is False


def test_front_dialog():
    snap = _load()
    dlg = snap.front_dialog()
    assert dlg is not None and dlg.is_dialog
    assert dlg.index == 0
```

- [ ] **Step 2: Run to verify the dialog test fails**

Run: `cd SheepShaver/e2e && .venv/bin/python -m pytest -q tests/test_uidump.py`
Expected: FAIL — `AttributeError: 'Snapshot' object has no attribute 'front_dialog'` (the two `clickable` tests already pass from Task 4).

- [ ] **Step 3: Add `front_dialog`**

In `SheepShaver/e2e/sse2e/uidump.py`, add to `Snapshot`:

```python
    def front_dialog(self) -> Optional[Window]:
        """The front window if it is a dialog, else None (the dialog accepting input right now)."""
        fw = self.front_window()
        return fw if (fw is not None and fw.is_dialog) else None
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cd SheepShaver/e2e && .venv/bin/python -m pytest -q tests/test_uidump.py`
Expected: PASS (5 passed).

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/e2e/sse2e/uidump.py SheepShaver/e2e/tests/test_uidump.py
git commit -F- <<'EOF'
feat(e2e): uidump front_dialog + occlusion/z-order tests

Lock down clickable() (modal-occlusion, invisible, collapsed) and add
front_dialog() — the window actually accepting input.
EOF
```

---

## Task 6: Integration smoke — capture a real window list under the harness

The walking-skeleton end-to-end proof. This one needs a boot, so it follows the E2E harness rules (isolated config; the sanctioned agent-launch exception). It is a *manual/opt-in* check, not part of `make e2e-test` (which is offline).

**Files:**
- Create: `SheepShaver/e2e/run_uidump_smoke.py`

- [ ] **Step 1: Write the smoke driver**

Create `SheepShaver/e2e/run_uidump_smoke.py`:

```python
#!/usr/bin/env python3
"""Manual smoke: boot, wait for the desktop, request a UI snapshot, print the window list.

Proves Backend A end-to-end. Uses the isolated-config harness primitives (own prefs + pristine
disk copy). Run from SheepShaver/e2e/:  .venv/bin/python run_uidump_smoke.py
"""
import sys
import tempfile
from pathlib import Path

from sse2e import harness, runner, scenario, uidump


def main() -> int:
    assets = harness.check_preconditions(need_disk=True)
    if assets is None:
        return 1
    dump_dir = tempfile.mkdtemp(prefix="ss_ui_")
    # Launch with SS_UI_DUMP_DIR set; reuse the harness boot path (own prefs + disk copy).
    with scenario.booted_session(assets, extra_env={"SS_UI_DUMP_DIR": dump_dir}) as sess:
        sess.wait_ready(timeout=120)               # [READY] desktop settled
        snap = uidump.snapshot(dump_dir, timeout=10)
        print(f"backend={snap.backend} windows={len(snap.windows)} "
              f"modal={snap.modal_active} front={snap.front_index}")
        for w in snap.windows:
            cb = w.content_bounds
            print(f"  [{w.index}] '{w.title}' class={w.window_class} "
                  f"active={w.active} vis={w.visible} "
                  f"bounds=({cb.left},{cb.top},{cb.right},{cb.bottom}) clickable={snap.clickable(w)}")
        ok = len(snap.windows) >= 1 and snap.backend == "A"
        print("PASS" if ok else "FAIL: no windows / wrong backend")
        return 0 if ok else 1


if __name__ == "__main__":
    harness.hard_exit(main())
```

> **Integration note for the implementer:** `scenario.booted_session(...)` / `sess.wait_ready(...)` are the names this smoke expects. Before implementing, open `SheepShaver/e2e/sse2e/scenario.py` and `run_smoke.py` and adapt these calls to the **actual** boot-context API there (the existing smoke shows the real pattern for launching the isolated config and waiting for `[READY]`). If the existing API doesn't expose an `extra_env` hook, add one to the launch primitive (the runner builds the child env) — that env passthrough is the only emulator-launch change this task needs. Do **not** invent a parallel boot path.

- [ ] **Step 2: Run the offline suite to confirm no regression**

Run: `cd SheepShaver/e2e && make e2e-test`
Expected: all offline tests pass (now includes `test_uidump.py`; the smoke driver isn't collected by pytest).

- [ ] **Step 3: Run the smoke once (boot required — ask the user first per CLAUDE.md)**

Run: `cd SheepShaver/e2e && .venv/bin/python run_uidump_smoke.py`
Expected: boots, prints `backend=A windows=N …` with at least the Finder/desktop window and sane global bounds, then `PASS`.

> Per the repo rule, an agent must **ask the user before launching an emulator instance**; the E2E isolated config is the sanctioned exception, but confirm before running this step. If the host is degraded from prior launches, hand this step to the user and read the output.

- [ ] **Step 4: Eyeball the bounds against reality**

Confirm the printed `bounds` for a known window look right (e.g. a Finder window's global rect lands on-screen, width/height positive, front dialog — if any — has `active=true`). This is the manual calibration that Plan 3's `compare()`/`overlay()` will automate.

- [ ] **Step 5: Commit**

```bash
git add SheepShaver/e2e/run_uidump_smoke.py
git commit -F- <<'EOF'
feat(e2e): uidump integration smoke (boot -> window list)

Manual end-to-end proof of Backend A: boots the isolated config with
SS_UI_DUMP_DIR, requests a snapshot, prints the window list + clickability.
EOF
```

---

## Task 7: Docs — env var + key-doc pointers

**Files:**
- Modify: `SheepShaver/e2e/README.md` (env-var table: add `SS_UI_DUMP_DIR`)
- Modify: `CHANGELOG.md` (component-tagged entry)

- [ ] **Step 1: Document the env var**

In `SheepShaver/e2e/README.md`, add a row to the env-var table:

```
| `SS_UI_DUMP_DIR` | unset | Dir for on-demand UI introspection snapshots. When set, the idle hook services `ss_ui.req` and writes `ss_ui.A.json` + `ss_ui.done` (Backend A window list). See `uidump.py` / the introspection spec. |
```

- [ ] **Step 2: Add a CHANGELOG entry**

In `CHANGELOG.md`, under the current date, add:

```
- **[SheepShaver]** Guest UI introspection (Plan 1 / walking skeleton): a read-only,
  env-gated (`SS_UI_DUMP_DIR`) host-side dump of the guest `WindowList` — front→back window
  list with global (VNC-clickable) bounds, title, dialog/active/visible — serviced at the idle
  safe point, plus a Python `sse2e.uidump` consumer (snapshot handshake + query/occlusion).
  Spec: `docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md`.
```

- [ ] **Step 3: Run the offline suite once more**

Run: `cd SheepShaver/e2e && make e2e-test`
Expected: all pass.

- [ ] **Step 4: Commit**

```bash
git add SheepShaver/e2e/README.md CHANGELOG.md
git commit -F- <<'EOF'
docs(ss-ui): document SS_UI_DUMP_DIR + changelog (introspection P1)
EOF
```

---

## Self-Review (completed by plan author)

**1. Spec coverage (Plan 1 subset):** ✅ env-gated `SS_UI_DUMP_DIR` + signal-free file-poll trigger (Task 3); ✅ nonce handshake + atomic write + `.done`-last (Tasks 3-4); ✅ Backend A `WindowList` walk in true z-order with global coords (Task 3); ✅ MacRoman→UTF-8 + Apple-glyph U+F8FF + JSON-escape (Task 1); ✅ `coords:"global"`/`text:"utf-8"`/`backend:"A"` contract fields (Task 3); ✅ Python query + `clickable()` occlusion (Tasks 4-5); ✅ integration smoke (Task 6). **Deliberately deferred to Plans 2-3** (spec §7): dialog items/DITL, control state, window-part rects, ParamText, menu bar, full WDEF class, screen depth (Plan 2); Backend B + `compare()` + `overlay()` (Plan 3). `screen.depth` is emitted as `0` (placeholder) in Plan 1.

**2. Placeholder scan:** No "TBD/TODO/handle edge cases" left. Task 6's `booted_session`/`wait_ready` are flagged explicitly as *adapt-to-actual-API* with a concrete instruction to read `scenario.py`/`run_smoke.py` first and add only an `extra_env` passthrough — this is the one place the exact existing API must be matched at implementation time (it couldn't be pinned from the plan without the file open).

**3. Type/name consistency:** `Snapshot`/`Window`/`Rect`, `snapshot()`, `front_window()`/`front_dialog()`/`find()`/`clickable()`, `content_bounds`/`struct_bounds`, `ui_introspect_service()`, `guest_ptr_ok()`, `serialize_snapshot()`, `SS_UI_DUMP_DIR`, artifact names `ss_ui.req`/`ss_ui.A.json`/`ss_ui.done` — used identically across C++ and Python tasks. The C++ writes `frontWindowIndex`/`modalActive`/`windowClass`/`isDialog` and the Python reads exactly those keys (fixture matches).

**Known follow-ups for Plan 2/3 (not gaps):** full WDEF-derived `windowClass` + `modality` (Plan 1 approximates dialog↔document from `windowKind==2`); `active` uses the `hilited` byte (correct) but `clickable()`'s modal rule assumes front-modal deactivation (holds for standard modals); `screen.depth`.
