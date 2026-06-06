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
    kWinDefProc= 0x7E,       // WDEF handle; high byte = GetWVariant code
    kWinTitleH = 0x86,       // StringHandle
    kWinNext   = 0x90,       // WindowPeek
    kWinRefCon = 0x98,       // int32
};
enum { kDlgItems = 0x9C, kDlgDefItem = 0xA8 };   // DialogRecord fields, past the 0x9C WindowRecord

struct Rect16 { int16 top, left, bottom, right; bool ok; };

// Is the guest BYTE at `a` inside mapped Mac RAM? Range-only (no even-alignment requirement):
// unlike guest_ptr_ok, this is for the END of an in-RAM extent (a title/item string's last byte),
// which legitimately lands on an ODD address. Those bytes are read via Mac2HostAddr, never deref'd
// as a pointer, so alignment is irrelevant — and requiring it wrongly rejected odd extents.
static inline bool guest_range_ok(uint32 a)
{
    return a >= 0x100 && a < RAMBase + RAMSize;
}

// Read the main screen's pixel depth from the main GDevice via low-mem MainDevice ($08A4).
// Returns a sane depth (one of 1,2,4,8,16,32) or 0 if any deref fails or the value is garbage.
// Offsets: GDHandle@0x08A4 -> GDevice; gdPMap (PixMapHandle) @ GDevice+0x16;
// PixMap -> pixelSize (int16) @ PixMap+0x20. These are standard Mac toolbox structs.
static int read_screen_depth() {
    uint32 gdH = ReadMacInt32(0x08A4);                 // MainDevice (GDHandle)
    if (!gdH || !guest_ptr_ok(gdH)) return 0;
    uint32 gd = ReadMacInt32(gdH);                     // -> GDevice
    if (!gd || !guest_ptr_ok(gd) || !guest_ptr_ok(gd + 0x16)) return 0;
    uint32 pmH = ReadMacInt32(gd + 0x16);              // gdPMap (PixMapHandle)
    if (!pmH || !guest_ptr_ok(pmH)) return 0;
    uint32 pm = ReadMacInt32(pmH);                     // -> PixMap
    if (!pm || !guest_ptr_ok(pm + 0x22)) return 0;
    int d = (int16)ReadMacInt16(pm + 0x20);            // pixelSize
    if (d==1||d==2||d==4||d==8||d==16||d==32) return d;
    return 0;                                          // not a sane depth -> placeholder
}

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

// Returns the MacRoman-decoded title; sets *valid=false on a wild/garbage read (the caller marks
// the window suspect). An untitled window is a VALID empty title (*valid stays true).
static std::string window_title(uint32 win, bool *valid) {
    *valid = true;
    uint32 hdl = ReadMacInt32(win + kWinTitleH);
    if (!hdl) return "";
    if (!guest_ptr_ok(hdl)) { *valid = false; return ""; }
    uint32 ptr = ReadMacInt32(hdl);
    if (!ptr) return "";
    if (!guest_ptr_ok(ptr)) { *valid = false; return ""; }
    uint8 *s = Mac2HostAddr(ptr);
    int len = s[0];
    if (len > 63 || !guest_range_ok(ptr + 1 + len)) { *valid = false; return ""; }
    std::string raw;
    for (int i = 0; i < len; i++) {
        uint8 c = s[1 + i];
        if (c < 32) { *valid = false; return ""; }   // control byte in a title = junk read
        raw.push_back((char)c);
    }
    return macroman_to_utf8(raw);
}

static void append_rect(std::string &j, const char *key, const Rect16 &r) {
    char b[160];
    snprintf(b, sizeof(b),
        "\"%s\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d}",
        key, r.left, r.top, r.right, r.bottom);
    j += b;
}

static const char *ditl_type_name(int t) {
    switch (t) {
        case 4:  return "button";
        case 5:  return "checkbox";
        case 6:  return "radio";
        case 7:  return "control";
        case 8:  return "staticText";
        case 16: return "editText";
        case 32: return "icon";
        case 64: return "picture";
        default: return "userItem";
    }
}

// Append the dialog's DITL items (globalized rects) as a JSON array `"items":[...]`. ox,oy = content
// origin (window content-region top-left). defItem = aDefItem. All guest reads are bounds-guarded.
static void serialize_dialog_items(uint32 win, int ox, int oy, int defItem, std::string &j) {
    j += "\"items\":[";
    uint32 ditlH = ReadMacInt32(win + kDlgItems);
    if (!ditlH || !guest_ptr_ok(ditlH)) { j += "]"; return; }
    uint32 ditl = ReadMacInt32(ditlH);
    if (!ditl || !guest_ptr_ok(ditl)) { j += "]"; return; }
    int count = (int16)ReadMacInt16(ditl) + 1;            // stored as N-1
    if (count < 0 || count > 255) { j += "]"; return; }
    uint32 p = ditl + 2;
    for (int i = 0; i < count; i++) {
        if (!guest_ptr_ok(p + 14)) break;                 // item header must be in RAM
        int16 top  = (int16)ReadMacInt16(p + 4),  left  = (int16)ReadMacInt16(p + 6);
        int16 bot  = (int16)ReadMacInt16(p + 8),  right = (int16)ReadMacInt16(p + 10);
        uint8 typeByte = Mac2HostAddr(p)[12];
        bool enabled = (typeByte & 0x80) == 0;
        int  type = typeByte & 0x7F;
        uint8 dlen = Mac2HostAddr(p)[13];
        std::string text;
        bool isTextItem = (type == 4 || type == 5 || type == 6 || type == 8 || type == 16);
        if (isTextItem && guest_range_ok(p + 14 + dlen)) {
            uint8 *d = Mac2HostAddr(p + 14);
            std::string raw;
            for (int k = 0; k < dlen; k++) { uint8 c = d[k]; if (c < 32) { raw.clear(); break; } raw.push_back((char)c); }
            text = macroman_to_utf8(raw);
        }
        // Control state: for control-type items the item's leading 4-byte field is the live
        // ControlHandle. Deref -> ControlRecord; read hilite/value + contrlRect (self-check vs the
        // item rect). All reads guarded; a wrong/nil handle just yields no control fields.
        bool isCtrl = (type == 4 || type == 5 || type == 6 || type == 7);
        bool haveCtrl = false; int cval = 0, chil = 0; int crl = 0, crt = 0, crr = 0, crb = 0;
        if (isCtrl) {
            uint32 ch = ReadMacInt32(p);                  // ControlHandle (item leading field)
            if (ch && guest_ptr_ok(ch)) {
                uint32 cr = ReadMacInt32(ch);             // -> ControlRecord
                if (cr && guest_ptr_ok(cr) && guest_ptr_ok(cr + 0x18)) {
                    crt = (int16)ReadMacInt16(cr + 0x08); crl = (int16)ReadMacInt16(cr + 0x0A);
                    crb = (int16)ReadMacInt16(cr + 0x0C); crr = (int16)ReadMacInt16(cr + 0x0E);
                    chil = Mac2HostAddr(cr)[0x11];        // contrlHilite (byte)
                    cval = (int16)ReadMacInt16(cr + 0x12);// contrlValue
                    haveCtrl = true;
                }
            }
        }
        bool hasParams = (text.find("^0") != std::string::npos || text.find("^1") != std::string::npos
                       || text.find("^2") != std::string::npos || text.find("^3") != std::string::npos);
        if (i) j += ",";
        char b[256];
        snprintf(b, sizeof(b),
            "{\"index\":%d,\"type\":\"%s\",\"rect\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d},"
            "\"enabled\":%s%s",
            i + 1, ditl_type_name(type),
            left + ox, top + oy, right + ox, bot + oy,
            enabled ? "true" : "false",
            (i + 1 == defItem) ? ",\"default\":true" : "");
        j += b;
        if (isTextItem) { j += ",\"text\":\""; j += json_escape(text); j += "\""; }
        if (haveCtrl) {
            char c[160];
            snprintf(c, sizeof(c),
                ",\"value\":%d,\"hilite\":%d,\"crect\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d}",
                cval, chil, crl + ox, crt + oy, crr + ox, crb + oy);   // crect globalized like rect
            j += c;
        }
        if (hasParams) j += ",\"hasParams\":true";
        j += "}";
        uint32 adv = 14 + dlen + (dlen & 1);              // 14-byte header is even; pad dlen to even
        p += adv;
    }
    j += "]";
}

// Walk the live menu bar (MenuList $0A1C) and append a "menuBar" JSON object. Read-only; every deref
// guarded. Handle-anchoring: a menu whose MenuInfo/title looks wild stops the walk (a wrong stride
// surfaces as a bad deref, not garbage output). cmd-keys are the inline cmdChar trailer byte.
static void serialize_menu_bar(std::string &j) {
    uint16 mbarHeight = ReadMacInt16(0x0BAA);
    j += "\"menuBar\":{";
    char hb[48]; snprintf(hb, sizeof(hb), "\"height\":%u,\"menus\":[", mbarHeight); j += hb;
    uint32 listH = ReadMacInt32(0x0A1C);
    if (!listH || !guest_ptr_ok(listH)) { j += "]}"; return; }
    uint32 lp = ReadMacInt32(listH);
    if (!lp || !guest_ptr_ok(lp)) { j += "]}"; return; }
    uint16 lastMenu = ReadMacInt16(lp);                 // = numMenus * 6
    if (lastMenu == 0 || (lastMenu % 6) != 0) { j += "]}"; return; }
    int numMenus = lastMenu / 6;
    if (numMenus > 64) { j += "]}"; return; }
    int emitted = 0;
    for (int i = 0; i < numMenus; i++) {
        uint32 entry = lp + 6 + i * 6;
        if (!guest_ptr_ok(entry + 6)) break;
        uint32 mh = ReadMacInt32(entry);                // MenuHandle
        if (!mh || !guest_ptr_ok(mh)) break;            // handle-anchoring: bad -> stop
        uint32 mi = ReadMacInt32(mh);                   // -> MenuInfo
        if (!mi || !guest_ptr_ok(mi + 0x0E)) break;
        int16 menuID = (int16)ReadMacInt16(mi);
        int32 enableFlags = (int32)ReadMacInt32(mi + 0x0A);
        uint8 *tp = Mac2HostAddr(mi + 0x0E);
        int titleLen = tp[0];
        if (titleLen > 63 || !guest_range_ok(mi + 0x0F + titleLen)) break;   // anchoring sanity (end-of-title byte may be odd)
        bool isApple = (titleLen == 1 && tp[1] == 0x14);
        std::string title;
        { std::string raw((const char *)tp + 1, titleLen);
          title = isApple ? std::string("\xef\xa3\xbf") /*U+F8FF*/ : macroman_to_utf8(raw); }
        if (emitted++) j += ",";
        char mb[160];
        snprintf(mb, sizeof(mb), "{\"id\":%d,\"enabled\":%s%s,\"title\":\"",
                 menuID, (enableFlags & 1) ? "true" : "false",
                 isApple ? ",\"role\":\"apple\"" : "");
        j += mb; j += json_escape(title); j += "\",\"items\":[";
        uint32 p = mi + 0x0F + titleLen;
        int k = 0;
        while (true) {
            if (!guest_range_ok(p + 1)) break;          // item length byte (extent end may be odd)
            int ilen = Mac2HostAddr(p)[0];
            if (ilen == 0) break;                       // zero-length item = end of menu
            if (ilen > 63 || !guest_range_ok(p + 1 + ilen + 4)) break;   // item text + 4-byte trailer in RAM
            k++;
            uint8 *ip = Mac2HostAddr(p + 1);
            std::string itext = macroman_to_utf8(std::string((const char *)ip, ilen));
            uint8 *tr = Mac2HostAddr(p + 1 + ilen);     // 4-byte trailer
            uint8 cmdChar = tr[1], markChar = tr[2];
            bool itemEnabled = (k <= 31) ? ((enableFlags & (1 << k)) != 0) : true;
            if (k > 1) j += ",";
            char ib[96];
            snprintf(ib, sizeof(ib), "{\"index\":%d,\"enabled\":%s,\"text\":\"",
                     k, itemEnabled ? "true" : "false");
            j += ib; j += json_escape(itext); j += "\"";
            if (cmdChar > 0x20) { char c[24]; snprintf(c, sizeof(c), ",\"cmdKey\":\"%c\"", (char)cmdChar); j += c; }
            else if (cmdChar == 0x1B) { char c[32]; snprintf(c, sizeof(c), ",\"submenu\":%d", (int)markChar); j += c; }
            j += "}";
            p += 1 + ilen + 4;
            if (k > 255) break;
        }
        j += "]}";
    }
    j += "]}";
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

    // screen from CrsrPin (Rect) + real depth from main GDevice.
    int screenW = 0, screenH = 0;
    {
        int16 t = (int16)ReadMacInt16(kCrsrPin + 0), l = (int16)ReadMacInt16(kCrsrPin + 2);
        int16 b = (int16)ReadMacInt16(kCrsrPin + 4), rt = (int16)ReadMacInt16(kCrsrPin + 6);
        screenW = rt - l;
        screenH = b - t;
        int depth = read_screen_depth();
        char sc[96];
        snprintf(sc, sizeof(sc), "\"screen\":{\"width\":%d,\"height\":%d,\"depth\":%d},", screenW, screenH, depth);
        j += sc;
    }
    serialize_menu_bar(j);
    j += ",";

    // Walk the window list, building per-window JSON into windows_json.
    // modalActive and frontWindowIndex are determined during the walk.
    uint32 front = ReadMacInt32(kWindowList);
    std::string windows_json;
    int idx = 0, front_index = -1;
    bool front_is_modal = false;

    for (uint32 win = front; win && guest_ptr_ok(win); win = ReadMacInt32(win + kWinNext)) {
        if (!guest_ptr_ok(win + 0xA0)) break;   // ensure the whole WindowRecord is in RAM before reading its fields

        int16 kind = (int16)ReadMacInt16(win + kWinKind);
        bool isDialog = (kind == 2);
        bool visible  = Mac2HostAddr(win)[kWinVisible] != 0;   // byte read
        bool active   = Mac2HostAddr(win)[kWinHilited] != 0;

        // title — must come before suspect computation
        bool tvalid;
        std::string title = window_title(win, &tvalid);

        // modality: from the window VARIANT (high byte of windowDefProc @+0x7E via GetWVariant),
        // gated to dialog windows ONLY. For non-dialog windows always "none".
        const char *modality = "none";
        if (isDialog) {
            uint8 variant = Mac2HostAddr(win)[kWinDefProc];   // high byte of windowDefProc = GetWVariant code
            if (variant == 1 || variant == 3)      modality = "modal";
            else if (variant == 5)                 modality = "movableModal";
            else                                   modality = "modeless";
        }

        // Fix 1: front_index = first VISIBLE window (list head can be hidden-but-linked).
        if (front_index < 0 && visible) {
            front_index = idx;
            front_is_modal = (strcmp(modality, "modal") == 0);
        }

        Rect16 sb = region_bbox(ReadMacInt32(win + kWinStruc));
        Rect16 cb = region_bbox(ReadMacInt32(win + kWinCont));
        bool suspect = !sb.ok || !cb.ok || !tvalid;

        // Geometry-based desktop detection: a non-dialog window whose bounds span essentially the
        // full screen (within a small tolerance). The Finder desktop window always occupies the
        // entire screen below the menu bar. This is v1 heuristic — good enough for a backdrop tag.
        const Rect16 &boundsRef = cb.ok ? cb : sb;
        bool isDesktop = !isDialog && screenW > 0 && screenH > 0
                      && boundsRef.left <= 0 && boundsRef.top <= 40
                      && boundsRef.right >= screenW - 1 && boundsRef.bottom >= screenH - 1;

        if (idx) windows_json += ",";
        char w[256];
        snprintf(w, sizeof(w),
            "{\"index\":%d,\"ptr\":\"0x%08x\",\"title\":\"", idx, win);
        windows_json += w;
        windows_json += json_escape(title);
        snprintf(w, sizeof(w),
            "\",\"windowClass\":\"%s\",\"modality\":\"%s\",\"isDialog\":%s,"
            "\"active\":%s,\"visible\":%s,\"collapsed\":false,",
            isDialog ? "dialog" : "document", modality,
            isDialog ? "true" : "false", active ? "true" : "false", visible ? "true" : "false");
        windows_json += w;
        append_rect(windows_json, "contentBounds", cb.ok ? cb : sb);
        windows_json += ",";
        append_rect(windows_json, "structBounds", sb.ok ? sb : cb);
        if (isDesktop) windows_json += ",\"role\":\"desktop\"";
        if (suspect) windows_json += ",\"suspect\":true";
        if (isDialog) {
            int32 refcon = (int32)ReadMacInt32(win + kWinRefCon);
            int16 defItem = (int16)ReadMacInt16(win + kDlgDefItem);
            char dd[64];
            snprintf(dd, sizeof(dd), ",\"refCon\":%d,\"defaultItem\":%d,", refcon, defItem);
            windows_json += dd;
            int ox = cb.ok ? cb.left : sb.left;            // content-region top-left = local->global origin
            int oy = cb.ok ? cb.top  : sb.top;
            serialize_dialog_items(win, ox, oy, defItem, windows_json);
        }
        windows_json += "}";
        idx++;
        if (idx > 256) break;     // runaway guard
    }

    // Assemble final JSON: modalActive depends on the front visible window discovered above.
    char tail[128];
    snprintf(tail, sizeof(tail), "\"modalActive\":%s,\"windows\":[",
        front_is_modal ? "true" : "false");
    j += tail;
    j += windows_json;
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
