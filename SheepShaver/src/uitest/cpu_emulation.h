// Test-only stub of cpu_emulation.h for the offline ui_introspect serializer harness.
// Backs the guest-memory accessors (ReadMacInt*/Mac2HostAddr) and RAMBase/RAMSize with a
// flat big-endian mock RAM buffer, so the REAL serializers in ui_introspect.cpp can be unit
// tested with no emulator. Guest address == offset into MOCK_RAM. The test TU defines the
// three globals. Selected over src/include/cpu_emulation.h only on the test include path.
#ifndef CPU_EMULATION_H
#define CPU_EMULATION_H

#include "sysdeps.h"

extern uint8 *MOCK_RAM;   // base of the flat mock guest RAM
extern uint32 RAMBase;    // 0 for the test
extern uint32 RAMSize;    // mock RAM size in bytes

// Guest addr -> host pointer (mock: a simple offset into MOCK_RAM, mirroring DIRECT_ADDRESSING).
static inline uint8 *Mac2HostAddr(uint32 a) { return MOCK_RAM + a; }

static inline uint32 ReadMacInt32(uint32 a) {
    const uint8 *p = MOCK_RAM + a;
    return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | (uint32)p[3];
}
static inline uint16 ReadMacInt16(uint32 a) {
    const uint8 *p = MOCK_RAM + a;
    return (uint16)(((uint16)p[0] << 8) | (uint16)p[1]);
}
static inline uint8 ReadMacInt8(uint32 a) { return MOCK_RAM[a]; }

#endif // CPU_EMULATION_H
