// Guest UI introspection (Backend A) — read-only host-side walk of the classic Mac OS
// Toolbox structures. Serviced from the idle hook. See
// docs/superpowers/specs/2026-06-06-guest-ui-introspection-design.md
#ifndef UI_INTROSPECT_H
#define UI_INTROSPECT_H

#include "sysdeps.h"
#include "cpu_emulation.h"   // RAMBase/RAMSize here; ReadMacInt*/Mac2HostAddr for includers (Backend A)

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

// C2.0 RPC: get a JSON snapshot of guest UI state (windows, menu bar, screen, etc.)
extern "C" void ss_ui_snapshot_json(char *buf, int bufsz);

#endif // UI_INTROSPECT_H
