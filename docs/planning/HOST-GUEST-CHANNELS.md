# Host-Guest Interaction Channels Reference

> **Status:** 📖 Reference · **Created:** 2026-06-05 · **Updated:** 2026-06-05
> **Why this doc exists:** Catalogues all host↔guest interaction channels in SheepShaver — what exists, what's achievable, and what's impossible without a guest agent. Reference for SiliconSheep integration work.

---

## 1. Existing Host-to-Guest Channels

| Channel | Mechanism | Code |
|---------|-----------|------|
| **ADB key/mouse injection** | `ADBKeyDown()`/`ADBKeyUp()`/`ADBMouseMoved()`/`ADBMouseDown()` buffer events into `key_buffer`; drained by `ADBInterrupt()` on the 60 Hz VBL tick | `adb.cpp`, `adb.h` |
| **Clipboard push** | `GetScrap()` EmulOp — host pasteboard polled, Mac scrap written via `WriteMacInt*` into guest heap; supports TEXT/styl/PICT | `clip_macosx64.mm`, `OP_GET_SCRAP` |
| **Clean shutdown** | SIGUSR1 sets `host_shutdown_requested`; idle-hook state machine injects ADB Power key (0x7F) with dwell, waits for dialog, confirms with Return (0x24) | `main_unix.cpp:274`, `emul_op.cpp:125` |
| **ROM trap patching** | Any 68k A-trap or PPC ROM routine can be overwritten at load time with an EMUL_OP opcode that vectors to host C++ | `rom_patches.cpp`, `rsrc_patches.cpp` |
| **Guest RAM writes** | `WriteMacInt{8,16,32}()` — write any guest address at any time from host context | `cpu_emulation.h` |
| **Execute68kTrap** | Call any A-trap from within an EMUL_OP handler, with full register context | `cpu_emulation.h:117` |
| **Timer manipulation** | Host replaces InsTime/RmvTime/PrimeTime — controls guest timer queue | `emul_op.cpp`, `timer.cpp` |

## 2. Existing Guest-to-Host Channels

| Channel | Mechanism | Code |
|---------|-----------|------|
| **Clipboard pull** | `PutScrap()` EmulOp — guest scrap data copied to host pasteboard | `clip_macosx64.mm`, `OP_PUT_SCRAP` |
| **ExtFS file system** | Guest HFS calls on the "Unix" volume dispatch to host `open()`/`read()`/`write()` on a single shared directory (`extfs` pref) | `extfs.cpp`, `OP_EXTFS_COMM`/`OP_EXTFS_HFS` |
| **Idle notification** | `OP_IDLE_TIME` fires from patched `SynchIdleTime` — host knows when guest event loop is idle | `emul_op.cpp:586` |
| **DebugStr** | Guest `DebugStr()` calls are intercepted and forwarded to host `WarningAlert()` | `OP_DEBUG_STR` |
| **NQD draw hooks** | QuickDraw bitblt/fillrect/invrect/sync routed to host-native acceleration (`gfxaccel.cpp`) | `NATIVE_NQD_*` |

## 3. Guest OS Structures Readable from Host

All via `ReadMacInt*()` / `Mac2HostAddr()` at any time (no guest cooperation needed):

| Structure | Address/Walk | What You Get |
|-----------|-------------|--------------|
| **CurApName** | `0x0910` (Str31) | Frontmost application name |
| **WindowList** | `0x09D6` (ptr to WindowRecord chain) | Walk `+0x18` (nextWindow) for Z-order; `+0x08` (titleHandle) for titles; `+0x04` (port.portRect) for bounds; `+0x6C` (windowKind: 2=dialog, 8+=DA) |
| **Ticks** | `0x016A` (uint32) | Guest ticks since boot (60 Hz) |
| **EventQueue head** | `0x014C` (EvQHdr.qHead) | Non-zero = events pending |
| **MenuList** | `0x0A1C` (handle to menu list) | Dereference handle for menu bar contents; each MenuInfo has title (Str255) and items |
| **CurApRefNum** | `0x0900` (int16) | Current app's resource file refnum |
| **TopMapHndl** | `0x0A50` (handle) | Top of resource map chain |
| **SysZone/ApplZone** | `0x02A6`/`0x02AA` | System and application heap zones |
| **BootDrive** | `0x0210` (int16) | Drive number of startup volume |
| **DefVRefNum** | `0x0384` (int16) | Default volume reference |
| **Time** | `0x020C` (uint32) | Seconds since 1904-01-01 (Mac epoch) |
| **MBarHeight** | `0x0BAA` (int16) | Menu bar height (useful for UI state) |
| **DeskHook** | `0x0A6C` (ptr) | Desktop drawing proc (non-zero = customized) |
| **GrayRgn** | `0x09EE` (handle) | Region handle for desktop area |
| **ScrnBase** | `0x0824` (ptr) | Base address of screen buffer |
| **ScreenRow** | `0x0106` (int16) | Screen row bytes |

**Process Manager** (Mac OS 8+): The Process Manager's internal process list is not exposed via a single low-memory global. However, you can call `GetNextProcess`/`GetProcessInformation` via `Execute68kTrap()` from an EMUL_OP context.

## 4. Achievable New Channels (via trap patching / EmulOp)

**New EmulOp selectors** are trivially added (enum in `emul_op.h`, handler in `emul_op.cpp`, patch site in `rom_patches.cpp`). Each is a host-guest call gate.

| Opportunity | How | Complexity |
|-------------|-----|------------|
| **Multi-directory ExtFS** | Add multiple `extfs` pref entries; register each as a separate VCB in `ExtFSComm` | Medium — ExtFS already handles one; the VCB/WDCBRec setup needs duplication |
| **Drag-and-drop file in** | Host writes file to ExtFS shared dir, then injects a Finder `odoc` AppleEvent via `Execute68kTrap(AESend)` | Medium — AppleEvent serialization is involved but documented |
| **Guest window list API** | Poll `WindowList` (0x9D6) from a host thread on a timer; walk the chain, extract titles + bounds + windowKind. No trap patch needed. | Low — pure read, already proven in `e2e_emit_boot_ready_once()` |
| **Frontmost app change notification** | Poll `CurApName` (0x910) periodically, or patch `_Launch`/`_ExitToShell` A-traps to emit an EmulOp | Low |
| **Sound output passthrough** | Audio dispatch (`OP_AUDIO_DISPATCH`) already routes to host; could expose guest audio state (volume, channels) to Tauri | Low — read guest `SndChannelPtr` structures |
| **Guest screenshot (non-VNC)** | Read `ScrnBase` (0x824) + `ScreenRow` (0x106) + depth from `GDevice` list (0xCC8) — raw framebuffer blit, no VNC server needed | Low |
| **Unicode clipboard** | Handle `utxt`/`UT16` types in `clip_macosx64.mm` (currently skipped) — Mac OS 8.5+ supports these natively | Low-medium |
| **Guest-initiated host command** | New EmulOp selector triggered from a patched trap (e.g., a custom Gestalt selector); guest code writes command to a mailbox address, EmulOp handler reads it | Low — proven pattern |
| **Network status** | Open Transport's `OTInetGetInterfaceInfo` can be called via `Execute68kTrap`; or read the interface config tables directly from guest RAM | Medium |

## 5. Hard Limits (Genuinely Impossible Without Guest Code)

| Limitation | Why |
|------------|-----|
| **Custom display driver / resolution switching** | The video driver is a ROM-level PPC native code module (`NATIVE_VIDEO_DO_DRIVER_IO`). SheepShaver already provides one. Replacing it requires exactly the existing mechanism; adding new modes means modifying `video.cpp`. This works but is host-side code, not a "guest agent" gap. |
| **Arbitrary guest code execution on demand** | `Execute68kTrap()` and `ExecuteNative()` can ONLY be called from within an EMUL_OP handler (i.e., when the guest has already yielded to the host via a patched trap). You cannot interrupt the guest at an arbitrary point and call a Toolbox routine. The guest must reach a patched call site first. |
| **File type/creator metadata on host files** | ExtFS maps Mac type/creator to host xattrs or `.finf` sidecar files. macOS APFS does support xattrs, but the mapping is incomplete and HFS-specific metadata (resource forks, Finder flags) is lossy on modern filesystems. |
| **True guest OS API interception (all calls)** | You can only patch traps whose dispatch vector you can locate and overwrite. OS calls that go through direct PPC function pointers (CFM imports, C++ vtables in SharedLibs) are not interceptable without binary-patching every call site. Mixed-mode (68k) trap dispatch IS fully interceptable. |
| **Guest-initiated async callbacks to host** | The guest cannot "call" the host except by hitting a patched trap. There is no interrupt or signal mechanism from guest-to-host. Polling from idle hooks (`OP_IDLE_TIME`) is the only async-like pattern. |
| **Printing integration** | Mac OS printing goes through the Chooser + printer driver (LaserWriter, StyleWriter). These are large native code modules. Intercepting print output would require replacing an entire printer driver with an EmulOp-backed stub — theoretically possible but very high effort. |

## 6. Key Architectural Facts for Launcher Integration

- **One instance at a time.** Shared prefs, disk images, and SDL window preclude concurrent VMs.
- **NATMEM_OFFSET = 0x400000000000.** Guest address `G` maps to host pointer `Mac2HostAddr(G)`. All reads are instant, no syscall.
- **60 Hz tick loop.** `ADBInterrupt()` drains the key buffer once per VBL. Injected keystrokes take effect within 16ms.
- **Idle hook runs at ~60 Hz when guest is idle.** `OP_IDLE_TIME` is the natural heartbeat for any host-guest polling protocol.
- **EmulOp enum has room.** `OP_MAX` is currently ~50; the 16-bit opcode space supports hundreds more selectors.
