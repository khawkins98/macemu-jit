/*
 *  test_adb_stub.cpp - Unit tests for the minimal ADB bus stub (M3b Task 2).
 *  Conformance oracle: QEMU hw/input/adb.c + adb-kbd.c + adb-mouse.c
 *  @ de5d8bfd6105d3dd3ae668df9762df244a6d1506 (see adb_stub.h header notes).
 *  Covers the plan's CV-3 surface from the bus side (Listen R3 address-move).
 */
#include "adb_stub.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

// Command-byte builders: [addr:4][cmd:2][reg:2]
static uint8_t TALK(uint8_t addr, uint8_t reg)   { return (uint8_t)((addr << 4) | 0x0C | (reg & 3)); }
static uint8_t LISTEN(uint8_t addr, uint8_t reg) { return (uint8_t)((addr << 4) | 0x08 | (reg & 3)); }
static uint8_t FLUSH(uint8_t addr)               { return (uint8_t)((addr << 4) | 0x01); }
#define SENDRESET 0x00

static ADBStub adb;
static uint8_t reply[ADB_STUB_MAX_REPLY];

static int cmd(uint8_t c)
{
	memset(reply, 0xAA, sizeof(reply));
	return ADBStubCommand(&adb, c, NULL, 0, reply, ADB_STUB_MAX_REPLY);
}
static int listen(uint8_t c, const uint8_t *data, int len)
{
	memset(reply, 0xAA, sizeof(reply));
	return ADBStubCommand(&adb, c, data, len, reply, ADB_STUB_MAX_REPLY);
}

int main()
{
	ADBStubReset(&adb);

	// --- Power-on defaults (oracle: adb_kbd_reset / adb_mouse_reset) ---
	CHECK(adb.kbd_addr == 2 && adb.kbd_handler == 1);
	CHECK(adb.mouse_addr == 3 && adb.mouse_handler == 2);
	CHECK(!adb.autopoll_on && adb.autopoll_rate == 0);
	CHECK(adb.talks == 0 && adb.listens == 0 && adb.absent_talks == 0 && adb.resets == 0);

	// --- Talk R3 identification (present devices) -> [devaddr, handler] ---
	CHECK(cmd(TALK(2, 3)) == 2);
	CHECK(reply[0] == 2 && reply[1] == 1);
	CHECK(cmd(TALK(3, 3)) == 2);
	CHECK(reply[0] == 3 && reply[1] == 2);
	CHECK(adb.talks == 2);

	// --- Talk R3 at an absent address -> -1, counted ---
	CHECK(cmd(TALK(5, 3)) == -1);
	CHECK(adb.absent_talks == 1);
	CHECK(adb.talks == 2);                  // absent Talk is not a successful talk

	// --- Talk R0 = the deferral: no input data, ever -> 0 ---
	CHECK(cmd(TALK(2, 0)) == 0);
	CHECK(cmd(TALK(3, 0)) == 0);
	CHECK(cmd(TALK(8, 0)) == -1);           // absent
	CHECK(adb.talks == 4 && adb.absent_talks == 2);

	// --- Talk R1 (oracle: empty on both) / Talk R2 (kbd LED status; mouse empty) ---
	CHECK(cmd(TALK(2, 1)) == 0);
	CHECK(cmd(TALK(2, 2)) == 2);
	CHECK(reply[0] == 0x00 && reply[1] == 0x07);    // adb-kbd.c READREG case 2
	CHECK(cmd(TALK(3, 2)) == 0);                    // adb-mouse.c has no case 2
	CHECK(adb.talks == 7);

	// --- Listen R0/R1/R2: accepted + ignored, counted ---
	static const uint8_t junk[2] = { 0x12, 0x34 };
	CHECK(listen(LISTEN(2, 0), junk, 2) == 0);
	CHECK(listen(LISTEN(2, 2), junk, 2) == 0);      // LED status sink
	CHECK(listen(LISTEN(3, 1), junk, 2) == 0);
	CHECK(adb.listens == 3);
	CHECK(adb.kbd_addr == 2 && adb.mouse_addr == 3);    // nothing moved
	// Listen to an absent address -> -1, counted separately from absent_talks
	CHECK(listen(LISTEN(9, 3), junk, 2) == -1);
	CHECK(adb.absent_other == 1 && adb.absent_talks == 2);

	// --- Listen R3 address-move: kbd 2 -> 10 via handler byte 0xFE (CHANGE_ID),
	//     the boot scan's collision-resolution pattern (CV-3) ---
	static const uint8_t mv10[2] = { 0x0A, 0xFE };
	CHECK(listen(LISTEN(2, 3), mv10, 2) == 0);
	CHECK(adb.kbd_addr == 10 && adb.kbd_handler == 1);  // handler unchanged
	CHECK(adb.moves == 1);
	// Talks at the NEW address answer, echoing the new address; old address absent.
	CHECK(cmd(TALK(10, 3)) == 2);
	CHECK(reply[0] == 10 && reply[1] == 1);
	CHECK(cmd(TALK(2, 3)) == -1);
	// Mouse unaffected.
	CHECK(cmd(TALK(3, 3)) == 2);
	CHECK(reply[0] == 3 && reply[1] == 2);

	// --- Listen R3 with handler byte 0x00 (CHANGE_ID_AND_ENABLE): also moves,
	//     handler unchanged (oracle moves unconditionally for 0xFE/0xFD/0x00) ---
	static const uint8_t mv11[2] = { 0x0B, 0x00 };
	CHECK(listen(LISTEN(10, 3), mv11, 2) == 0);
	CHECK(adb.kbd_addr == 11 && adb.kbd_handler == 1);
	CHECK(adb.moves == 2);

	// --- Listen R3 with handler byte 0xFD (CHANGE_ID_AND_ACT): moves too ---
	static const uint8_t mv12[2] = { 0x0C, 0xFD };
	CHECK(listen(LISTEN(11, 3), mv12, 2) == 0);
	CHECK(adb.kbd_addr == 12 && adb.moves == 3);

	// --- Listen R3 with a REAL handler id: moves address AND changes handler
	//     (kbd supports 1/2/3) ---
	static const uint8_t mv2h3[2] = { 0x02, 0x03 };
	CHECK(listen(LISTEN(12, 3), mv2h3, 2) == 0);
	CHECK(adb.kbd_addr == 2 && adb.kbd_handler == 3);
	CHECK(adb.moves == 4 && adb.handler_changes == 1);
	CHECK(cmd(TALK(2, 3)) == 2);
	CHECK(reply[0] == 2 && reply[1] == 3);

	// --- Unsupported handler id: address still moves, handler stays (mouse: 1/2) ---
	static const uint8_t mvm_h4[2] = { 0x07, 0x04 };   // 0x04 Extended Mouse: unsupported
	CHECK(listen(LISTEN(3, 3), mvm_h4, 2) == 0);
	CHECK(adb.mouse_addr == 7 && adb.mouse_handler == 2);
	CHECK(adb.moves == 5 && adb.handler_changes == 1);
	// Supported mouse handler change:
	static const uint8_t mvm_h1[2] = { 0x03, 0x01 };
	CHECK(listen(LISTEN(7, 3), mvm_h1, 2) == 0);
	CHECK(adb.mouse_addr == 3 && adb.mouse_handler == 1);
	CHECK(adb.moves == 6 && adb.handler_changes == 2);

	// --- Self-test handler byte 0xFF: ignored entirely (no move, no change) ---
	static const uint8_t selftest[2] = { 0x0F, 0xFF };
	CHECK(listen(LISTEN(3, 3), selftest, 2) == 0);
	CHECK(adb.mouse_addr == 3 && adb.mouse_handler == 1);
	CHECK(adb.moves == 6);                          // unchanged: nothing applied

	// --- Listen R3 payload-length edge cases (no crash, oracle behavior) ---
	uint64_t ign0 = adb.listens_ignored;
	CHECK(listen(LISTEN(2, 3), NULL, 0) == 0);      // len 0: ignored
	CHECK(adb.kbd_addr == 2);
	static const uint8_t one = 0x09;
	CHECK(listen(LISTEN(2, 3), &one, 1) == 0);      // len 1: ignored (can't read handler byte)
	CHECK(adb.kbd_addr == 2);
	// Mouse: len > 2 is IGNORED whole — the oracle's Mac OS 9 driver-bug guard.
	static const uint8_t long3[3] = { 0x0D, 0xFE, 0x55 };
	CHECK(listen(LISTEN(3, 3), long3, 3) == 0);
	CHECK(adb.mouse_addr == 3);                     // did NOT move
	// Kbd: oracle has no length guard; extra bytes beyond the two used are ignored.
	CHECK(listen(LISTEN(2, 3), long3, 3) == 0);
	CHECK(adb.kbd_addr == 13);                      // moved to 0x0D
	CHECK(adb.listens_ignored == ign0 + 3);

	// --- Flush: ack on present (0), -1 on absent ---
	CHECK(cmd(FLUSH(13)) == 0);
	CHECK(adb.flushes == 1);
	CHECK(cmd(FLUSH(2)) == -1);                     // kbd moved away; 2 is empty now
	CHECK(adb.absent_other == 2);

	// --- SendReset: restores default addresses/handlers after the moves ---
	uint64_t talks0 = adb.talks, listens0 = adb.listens;
	CHECK(cmd(SENDRESET) == 0);
	CHECK(adb.resets == 1);
	CHECK(adb.kbd_addr == 2 && adb.kbd_handler == 1);
	CHECK(adb.mouse_addr == 3 && adb.mouse_handler == 2);
	CHECK(adb.talks == talks0 && adb.listens == listens0);  // telemetry preserved
	CHECK(cmd(TALK(2, 3)) == 2);
	CHECK(reply[0] == 2 && reply[1] == 1);
	CHECK(cmd(TALK(13, 3)) == -1);                  // old moved address gone

	// --- Listen R3 address byte: upper-nibble flag bits are masked off
	//     (oracle: d->devaddr = buf[1] & 0xf) ---
	static const uint8_t mv_flags[2] = { 0x6A, 0xFE };
	CHECK(listen(LISTEN(2, 3), mv_flags, 2) == 0);
	CHECK(adb.kbd_addr == 0x0A);

	// --- Kbd unsupported handler id (5, not in 1/2/3): address moves,
	//     handler unchanged ---
	{
		uint64_t hc0 = adb.handler_changes;
		static const uint8_t mv_h5[2] = { 0x02, 0x05 };
		CHECK(listen(LISTEN(10, 3), mv_h5, 2) == 0);
		CHECK(adb.kbd_addr == 2 && adb.kbd_handler == 1);
		CHECK(adb.handler_changes == hc0);
	}

	// --- Reserved cmd group (low nibble 0x2/0x3): present -> 0, absent -> -1
	//     (oracle: devreq matches neither WRITEREG nor READREG -> olen 0) ---
	CHECK(cmd((uint8_t)((2 << 4) | 0x02)) == 0);
	CHECK(cmd((uint8_t)((6 << 4) | 0x03)) == -1);

	// --- reply_max clamping: never write past the caller's buffer ---
	{
		uint8_t small[2] = { 0xAA, 0xAA };
		int n = ADBStubCommand(&adb, TALK(2, 3), NULL, 0, small, 1);
		CHECK(n <= 1);
		CHECK(small[1] == 0xAA);                    // byte beyond reply_max untouched
		n = ADBStubCommand(&adb, TALK(2, 3), NULL, 0, small, 0);
		CHECK(n == 0);
		// Negative reply_max is clamped to 0 — it must NOT alias the -1
		// absent-device sentinel for a present device.
		n = ADBStubCommand(&adb, TALK(2, 3), NULL, 0, small, -1);
		CHECK(n == 0);
	}

	// --- ADBStubReset (power-on) zeroes telemetry + autopoll plumbing ---
	adb.autopoll_on = true; adb.autopoll_rate = 11;
	ADBStubReset(&adb);
	CHECK(adb.talks == 0 && adb.listens == 0 && adb.absent_talks == 0 && adb.resets == 0);
	CHECK(!adb.autopoll_on && adb.autopoll_rate == 0);
	CHECK(adb.kbd_addr == 2 && adb.mouse_addr == 3);

	printf("test_adb_stub: RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
