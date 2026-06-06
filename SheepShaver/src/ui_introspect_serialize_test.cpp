// Offline unit test for the MEMORY-WALKING serializers in ui_introspect.cpp — no emulator link.
//
// Why this exists: the serializers (serialize_snapshot / _window_controls / _dialog_items / _menu_bar)
// read guest memory via ReadMacInt*/Mac2HostAddr and were previously validated only by booting. This
// harness compiles the REAL ui_introspect.cpp against a flat big-endian mock RAM (stub sysdeps.h /
// cpu_emulation.h on the include path) and asserts the JSON for hand-built Toolbox structures — so each
// offset is a regression-tested fact. Closes the test-debt tracked in
// docs/planning/UI-INTROSPECTION-REVIEW-SYNTHESIS.md.
//
// Build/run: make -C SheepShaver ui-introspect-serialize-test
//   c++ -std=c++17 -I src/uitest -o /tmp/x src/ui_introspect_serialize_test.cpp && /tmp/x
#include "cpu_emulation.h"   // stub: types + ReadMacInt*/Mac2HostAddr + extern RAMBase/RAMSize/MOCK_RAM

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The three globals the cpu_emulation.h stub declares extern.
uint8 *MOCK_RAM = nullptr;
uint32 RAMBase  = 0;
uint32 RAMSize  = 0;

// Pull in the REAL serializers (they are file-static, so include the translation unit directly).
#include "ui_introspect.cpp"

// --- mock-RAM writers (big-endian, matching ReadMacInt*) ---
static void w8 (uint32 a, uint8  v) { MOCK_RAM[a] = v; }
static void w16(uint32 a, uint16 v) { MOCK_RAM[a] = (uint8)(v >> 8); MOCK_RAM[a+1] = (uint8)v; }
static void w32(uint32 a, uint32 v) {
    MOCK_RAM[a]   = (uint8)(v >> 24); MOCK_RAM[a+1] = (uint8)(v >> 16);
    MOCK_RAM[a+2] = (uint8)(v >> 8);  MOCK_RAM[a+3] = (uint8)v;
}
static void wpstr(uint32 a, const char *s) {            // Pascal string: len byte + chars
    size_t n = strlen(s); MOCK_RAM[a] = (uint8)n; memcpy(MOCK_RAM + a + 1, s, n);
}
static void wrect(uint32 a, int16 top, int16 left, int16 bottom, int16 right) {
    w16(a, (uint16)top); w16(a+2, (uint16)left); w16(a+4, (uint16)bottom); w16(a+6, (uint16)right);
}
// A Region = handle -> master ptr -> {size, bbox(top,left,bottom,right)}.
static void wregion(uint32 handle, uint32 master, int16 t, int16 l, int16 b, int16 r) {
    w32(handle, master);
    w16(master, 10);                 // rgnSize (unused by the reader)
    wrect(master + 2, t, l, b, r);   // rgnBBox
}

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); failures++; }
}
static bool has(const std::string &j, const char *sub) { return j.find(sub) != std::string::npos; }

int main() {
    RAMSize = 0x100000;                          // 1 MB mock RAM; valid ptrs in [0x100, RAMSize)
    std::vector<uint8> ram(RAMSize, 0);
    MOCK_RAM = ram.data();

    // --- low-mem globals ---
    w16(0x015A, 0x0904);                         // SysVersion -> "9.0.4"
    w32(0x016A, 12345);                          // Ticks
    wrect(0x0834, 0, 0, 600, 800);               // CrsrPin (screen Rect) -> 800x600
    w32(0x08A4, 0);                              // MainDevice nil -> depth 0 (exercises the nil-guard)
    w16(0x0BAA, 20);                             // MBarHeight
    w32(0x09D6, 0x2000);                         // WindowList -> window #1

    // === Menu bar (MenuList $0A1C): apple menu + File menu (New=N, Open=O) ===
    w32(0x0A1C, 0x5000);                         // MenuList handle
    w32(0x5000, 0x5010);                         //  -> MenuList ptr
    w16(0x5010, 12);                             // lastMenu = numMenus(2) * 6
    w32(0x5016, 0x5100);                         // menu[0] entry: MenuHandle (apple)
    w32(0x501C, 0x5200);                         // menu[1] entry: MenuHandle (File)
    // apple MenuInfo @0x5110: id 1, all-enabled, title = single 0x14 (apple-logo)
    w32(0x5100, 0x5110);
    w16(0x5110 + 0x00, 1);                       // menuID
    w32(0x5110 + 0x0A, 0xFFFFFFFF);              // enableFlags (menu + items)
    w8 (0x5110 + 0x0E, 1); w8(0x5110 + 0x0F, 0x14);    // title Str255 = {0x14} -> isApple
    wpstr(0x5120, "About");                      //   item 1 "About" (p = mi+0x0F+titleLen = 0x5120)
    w32(0x5126, 0);                              //   trailer (no cmd key)
    w8 (0x512A, 0);                              //   zero-length item = end of menu
    // File MenuInfo @0x5210: id 2, all-enabled, title "File"
    w32(0x5200, 0x5210);
    w16(0x5210 + 0x00, 2);
    w32(0x5210 + 0x0A, 0xFFFFFFFF);
    wpstr(0x5210 + 0x0E, "File");                // title (len 4) -> items start at 0x5223
    wpstr(0x5223, "New");  w8(0x5228, 'N');      //   item 1 "New", trailer cmdChar 'N'  (tr[1]@p+1+ilen+1)
    wpstr(0x522B, "Open"); w8(0x5231, 'O');      //   item 2 "Open", trailer cmdChar 'O'
    w8 (0x5234, 0);                              //   end of menu

    // === Window #1: NON-DIALOG (documentKind) with a controlList of 3 controls ===
    const uint32 W1 = 0x2000;
    w16(W1 + 0x6C, 8);                           // windowKind = 8 (documentKind, non-dialog)
    w8 (W1 + 0x6E, 1);                           // visible
    w8 (W1 + 0x6F, 1);                           // hilited (active)
    w32(W1 + 0x72, 0x2200);                      // strucRgn handle
    w32(W1 + 0x76, 0x2300);                      // contRgn handle (origin for globalizing controls)
    w32(W1 + 0x86, 0x2400);                      // titleHandle
    w32(W1 + 0x8C, 0x2F00);                      // controlList (ControlHandle, head of chain)
    w32(W1 + 0x90, 0x4000);                      // nextWindow -> window #2
    wregion(0x2200, 0x2210, 36, 6, 404, 604);    // struct bounds
    wregion(0x2300, 0x2310, 40, 10, 400, 600);   // content bounds -> ox=10, oy=40
    w32(0x2400, 0x2410); wpstr(0x2410, "Installer");   // title

    // control #1: "Continue" button, active. local rect (100,200,120,280) -> global (140,210,160,290)
    w32(0x2F00, 0x3000);                         // ControlHandle -> ControlRecord
    w32(0x3000 + 0x00, 0x2F10);                  // nextControl -> control #2
    wrect(0x3000 + 0x08, 100, 200, 120, 280);    // contrlRect (local)
    w8 (0x3000 + 0x11, 0);                       // contrlHilite = 0 (active)
    w16(0x3000 + 0x12, 0);                       // contrlValue
    wpstr(0x3000 + 0x28, "Continue");            // contrlTitle

    // control #2: DEGENERATE rect (zero size) -> must be SKIPPED. Distinctive title/value must NOT leak.
    w32(0x2F10, 0x3100);
    w32(0x3100 + 0x00, 0x2F20);                  // nextControl -> control #3
    wrect(0x3100 + 0x08, 0, 0, 0, 0);            // degenerate
    w8 (0x3100 + 0x11, 0);
    w16(0x3100 + 0x12, 999);
    wpstr(0x3100 + 0x28, "SKIPME");

    // control #3: dimmed, untitled. local (200,200,220,260) -> global (240,210,260,270); hilite 255.
    w32(0x2F20, 0x3200);
    w32(0x3200 + 0x00, 0);                       // nextControl = 0 (end of chain)
    wrect(0x3200 + 0x08, 200, 200, 220, 260);
    w8 (0x3200 + 0x11, 255);                     // contrlHilite = 255 (dimmed -> enabled:false)
    w16(0x3200 + 0x12, 7);                       // contrlValue
    w8 (0x3200 + 0x28, 0);                       // empty title -> type "control"

    // === Window #2: DIALOG (dialogKind) with a 1-item DITL (an "OK" button) ===
    const uint32 W2 = 0x4000;
    w16(W2 + 0x6C, 2);                           // windowKind = 2 (dialogKind)
    w8 (W2 + 0x6E, 1);                           // visible
    w8 (W2 + 0x6F, 0);                           // not active
    w8 (W2 + 0x7E, 1);                           // windowDefProc variant high byte = 1 -> modal
    w32(W2 + 0x72, 0x4200);                      // strucRgn
    w32(W2 + 0x76, 0x4300);                      // contRgn -> ox=20, oy=60
    w32(W2 + 0x86, 0x4400);                      // titleHandle
    w32(W2 + 0x98, 0x73706564);                  // refCon = 'sped'
    w16(W2 + 0xA8, 1);                           // aDefItem = 1
    w32(W2 + 0x9C, 0x4500);                      // dlgItems handle (DITL)
    w32(W2 + 0x90, 0);                           // nextWindow = 0 (end of list)
    wregion(0x4200, 0x4210, 56, 16, 240, 420);
    wregion(0x4300, 0x4310, 60, 20, 236, 416);   // content -> ox=20, oy=60
    w32(0x4400, 0x4410); wpstr(0x4410, "Save");  // dialog title

    // DITL: count word = N-1 = 0 (1 item). Item: 4-byte handle + Rect(8) + type byte + len byte + data.
    w32(0x4500, 0x4510);                         // DITL handle -> DITL data
    w16(0x4510, 0);                              // itemCount - 1 = 0
    uint32 it = 0x4512;
    w32(it + 0, 0);                              // item ControlHandle (nil -> no control-state fields)
    wrect(it + 4, 100, 30, 120, 110);            // item rect (local) -> global (160,50,180,130)
    w8 (it + 12, 4);                             // type = 4 (button), enable bit clear -> enabled
    w8 (it + 13, 2);                             // data length
    MOCK_RAM[it + 14] = 'O'; MOCK_RAM[it + 15] = 'K';   // "OK"

    // --- run the real serializer ---
    std::string j = serialize_snapshot("testnonce");
    printf("--- snapshot JSON ---\n%s\n---------------------\n", j.c_str());

    // header / structure
    check(has(j, "\"nonce\":\"testnonce\""), "nonce echoed");
    check(has(j, "\"sysVersion\":\"9.0.4\""), "sysVersion decoded");
    check(has(j, "\"screen\":{\"width\":800,\"height\":600"), "screen size from CrsrPin");

    // Window #1: non-dialog, controls surfaced as items (THE control-list branch)
    check(has(j, "\"title\":\"Installer\""), "window 1 title");
    check(has(j, "\"isDialog\":false"), "window 1 is non-dialog");
    // Continue button: type, globalized rect, active, titled
    check(has(j, "\"type\":\"button\""), "control-list emits a button");
    check(has(j, "\"text\":\"Continue\""), "control title 'Continue' surfaced");
    check(has(j, "\"rect\":{\"left\":210,\"top\":140,\"right\":290,\"bottom\":160}"),
          "control rect globalized via content origin");
    check(has(j, "\"enabled\":true,\"value\":0,\"hilite\":0,\"text\":\"Continue\""),
          "Continue active+enabled");
    // dimmed untitled control: type control, enabled:false, value/hilite preserved, no text
    check(has(j, "\"type\":\"control\""), "untitled control typed 'control'");
    check(has(j, "\"enabled\":false,\"value\":7,\"hilite\":255"), "dimmed control enabled:false");
    // degenerate control skipped: its distinctive payload must not appear
    check(!has(j, "SKIPME"), "degenerate-rect control skipped (title not leaked)");
    check(!has(j, "\"value\":999"), "degenerate-rect control skipped (value not leaked)");

    // Window #2: dialog, DITL items + dialog metadata
    check(has(j, "\"isDialog\":true"), "window 2 is dialog");
    check(has(j, "\"modality\":\"modal\""), "dialog modality from variant");
    check(has(j, "\"refCon\":1936745828"), "dialog refCon ('sped' = 0x73706564)");
    check(has(j, "\"defaultItem\":1"), "dialog defaultItem");
    check(has(j, "\"text\":\"OK\""), "DITL button text");
    check(has(j, "\"rect\":{\"left\":50,\"top\":160,\"right\":130,\"bottom\":180}"),
          "DITL item rect globalized");
    check(has(j, "\"default\":true"), "DITL default item flagged");

    // Menu bar: apple role + File menu with Command-key equivalents
    check(has(j, "\"role\":\"apple\""), "apple menu role");
    check(has(j, "\"title\":\"File\""), "File menu title");
    check(has(j, "\"text\":\"New\"") && has(j, "\"cmdKey\":\"N\""), "File ▸ New = ⌘N");
    check(has(j, "\"text\":\"Open\"") && has(j, "\"cmdKey\":\"O\""), "File ▸ Open = ⌘O");

    if (failures == 0) printf("ui_introspect_serialize: ALL OK\n");
    return failures ? 1 : 0;
}
