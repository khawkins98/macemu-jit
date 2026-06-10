/* Standalone unit test for mmio_machfault.cpp (M1 Task 6 Step 4, rev 2 finding C2).
 *
 * Exercises MMIOMachFaultDispatch() directly with a synthetic arm_thread_state64_t
 * (no Mach exception ports needed): a hand-assembled JIT memory-access word is placed
 * in a buffer, __pc points at it, and the device guest address is passed explicitly.
 *
 * PAC note: arm_thread_state64_get_pc / arm_thread_state64_set_pc_fptr are NOPs without
 * pointer-auth/entitlements in a standalone test (#if __has_feature(ptrauth_calls) is
 * false on the current M-series dev host), so raw __pc manipulation and the accessors
 * agree. We use the same accessors the implementation uses so they stay aligned.
 *
 * The undecodable-insn -> abort() branch is NOT in-process testable (it calls abort);
 * decode rejection of non-JIT forms is covered by test_a64_mmio_decode.cpp.
 *
 * Build: make -C SheepShaver/src/machine test_mmio_machfault
 */
#include "mmio_bus.h"
#include "a64_mmio_decode.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/thread_status.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

// --- Fake device ---
static uint64_t dev_read_value;
static uint32_t dev_last_read_addr;  static unsigned dev_last_read_size;  static int dev_reads;
static uint32_t dev_last_write_addr; static unsigned dev_last_write_size; static uint64_t dev_last_write_val; static int dev_writes;

static uint64_t fake_read(void *, uint32_t addr, unsigned size)
{ dev_last_read_addr = addr; dev_last_read_size = size; dev_reads++; return dev_read_value; }
static void fake_write(void *, uint32_t addr, unsigned size, uint64_t v)
{ dev_last_write_addr = addr; dev_last_write_size = size; dev_last_write_val = v; dev_writes++; }

// Verbatim JIT encoding constants (ppc-codegen-aarch64.h:150-181):
//   enc(baseop, rt, rn, rm) = baseop | rm<<16 | rn<<5 | rt
static uint32_t enc(uint32_t baseop, unsigned rt, unsigned rn, unsigned rm)
{ return baseop | (rm << 16) | (rn << 5) | rt; }

#define DEV_BASE 0xF3012000u

int main()
{
#if defined(_STRUCT_ARM_THREAD_STATE64)
	static const MMIODevice dev = { "scc-fake", 0, fake_read, fake_write, 0 };
	CHECK(MMIOBusRegister(DEV_BASE, 0x1000, MMIO_TRAPPED, &dev));
	MMIOBusActivate();
	CHECK(mmio_bus_active);

	arm_thread_state64_t ts;

	// (a) Word load: device yields architectural 0x12345678; impl re-swaps to raw BE
	// 0x78563412 and injects into __x[rt] (the JIT's REV will swap it back at pc+4).
	static uint32_t buf_ldw = 0; buf_ldw = enc(0xB8604800u, 1, 19, 0);  // LDR w1,[x19,w0,UXTW]
	memset(&ts, 0, sizeof ts);
	arm_thread_state64_set_pc_fptr(ts, &buf_ldw);
	ts.__x[2] = 0xCAFEBABEu;                 // sentinel in a valid (untouched) register
	dev_read_value = 0x12345678u; dev_reads = 0;
	CHECK(MMIOMachFaultDispatch(0xF3012002u, &ts));
	CHECK(dev_reads == 1 && dev_last_read_addr == 0xF3012002u && dev_last_read_size == 4);
	CHECK(ts.__x[1] == 0x78563412u);         // raw-BE injection (S2 endianness contract)
	CHECK((uintptr_t)arm_thread_state64_get_pc(ts) == (uintptr_t)&buf_ldw + 4);
	CHECK(ts.__x[2] == 0xCAFEBABEu);         // other registers untouched

	// (b) Word store: REV already ran in the JIT, so __x[rt] holds raw BE; impl swaps
	// back to architectural before handing to the device.
	static uint32_t buf_stw = 0; buf_stw = enc(0xB8204800u, 1, 19, 0);  // STR w1,[x19,w0,UXTW]
	memset(&ts, 0, sizeof ts);
	arm_thread_state64_set_pc_fptr(ts, &buf_stw);
	ts.__x[1] = 0x78563412u;                 // raw BE in the source register
	dev_writes = 0;
	CHECK(MMIOMachFaultDispatch(0xF3012006u, &ts));
	CHECK(dev_writes == 1 && dev_last_write_addr == 0xF3012006u && dev_last_write_size == 4);
	CHECK(dev_last_write_val == 0x12345678u); // architectural value reached the device
	CHECK((uintptr_t)arm_thread_state64_get_pc(ts) == (uintptr_t)&buf_stw + 4);

	// (c) rt==31 load (WZR): the device read is still dispatched, but no register is
	// written; PC still advances. Verify a valid sentinel register is untouched.
	static uint32_t buf_ld31 = 0; buf_ld31 = enc(0xB8604800u, 31, 19, 0);  // LDR wzr,[x19,w0,UXTW]
	memset(&ts, 0, sizeof ts);
	arm_thread_state64_set_pc_fptr(ts, &buf_ld31);
	ts.__x[2] = 0xCAFEBABEu;
	dev_reads = 0; dev_read_value = 0x11223344u;
	CHECK(MMIOMachFaultDispatch(0xF3012002u, &ts));
	CHECK(dev_reads == 1);                    // read dispatched despite discarded value
	CHECK(ts.__x[2] == 0xCAFEBABEu);          // no stray register write
	CHECK((uintptr_t)arm_thread_state64_get_pc(ts) == (uintptr_t)&buf_ld31 + 4);

	// (d) Byte load: size_log2==0 => A64SwapForWidth is identity, no REV pairing, so
	// the device value is injected as-is.
	static uint32_t buf_ldb = 0; buf_ldb = enc(0x38604800u, 1, 19, 0);  // LDRB w1,[x19,w0,UXTW]
	memset(&ts, 0, sizeof ts);
	arm_thread_state64_set_pc_fptr(ts, &buf_ldb);
	dev_read_value = 0xA5u; dev_reads = 0;
	CHECK(MMIOMachFaultDispatch(0xF3012000u, &ts));
	CHECK(dev_reads == 1 && dev_last_read_size == 1);
	CHECK(ts.__x[1] == 0xA5u);                // injected as-is (no byte swap)
	CHECK((uintptr_t)arm_thread_state64_get_pc(ts) == (uintptr_t)&buf_ldb + 4);

	// Telemetry: the fault path counted JIT faults on the region.
	int ri = MMIOBusLookup(0xF3012002u);
	char nm[32]; uint32_t base, size; MMIORegionStats st;
	CHECK(ri >= 0 && MMIOBusGetStats(ri, nm, &base, &size, &st));
	CHECK(st.jit_faults == 4 && st.reads == 3 && st.writes == 1);

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
#else
	printf("RESULT: ALL PASS (0 checks - not an arm64 host, dispatch is a no-op)\n");
#endif
	return 0;
}
