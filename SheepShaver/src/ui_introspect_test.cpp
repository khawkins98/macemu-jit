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
    // A raw control byte (0x01) becomes the JSON escape backslash-u-0001.
    { std::string in("a\x01"); check(json_escape(in) == "a\\u0001", "escape control -> \\u00xx"); }

    if (failures == 0) printf("ui_introspect_text: ALL OK\n");
    return failures ? 1 : 0;
}
