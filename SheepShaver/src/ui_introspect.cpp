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
    if (!ptr || !guest_ptr_ok(ptr) || !guest_ptr_ok(ptr + 10)) return r;
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
    if (len > 255 || !guest_ptr_ok(ptr + 1 + len)) return "";
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
        if (!guest_ptr_ok(win + 0xA0)) break;   // ensure the whole WindowRecord is in RAM before reading its fields
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
static bool write_atomic(const std::string &dir, const char *name, const std::string &content) {
    std::string tmp = dir + "/." + name + ".tmp";
    std::string dst = dir + "/" + name;
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    return rename(tmp.c_str(), dst.c_str()) == 0;
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
    if (nonce.size() > 64) nonce.resize(64);

    // Backend A only in Plan 1 (ignore "backends"/"screenshot" until Plans 2-3).
    if (!write_atomic(dir, "ss_ui.A.json", serialize_snapshot(nonce)))
        return;                              // data write failed -> no sentinel (consumer times out)
    write_atomic(dir, "ss_ui.done", std::string("{\"nonce\":\"") + json_escape(nonce) + "\"}");
}
