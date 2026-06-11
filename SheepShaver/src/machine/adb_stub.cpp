/*
 *  adb_stub.cpp - Minimal ADB bus stub behind the Cuda (Machine Layer M3b Task 2).
 *
 *  Behavioral oracle (consulted, not ported): QEMU hw/input/adb.c + adb-kbd.c +
 *  adb-mouse.c + adb-internal.h @ de5d8bfd6105d3dd3ae668df9762df244a6d1506.
 *  Full protocol notes + the deferred-work pointer (host input over ADB) live in
 *  adb_stub.h and donor study M3-PIC-CUDA-DONOR-STUDY.md §7.2.
 *
 *  §2g: reachable under a bus region lock from the Mach exception-handler thread —
 *  no malloc, no stdio on any path here.
 */

#include "adb_stub.h"
#include <string.h>

// ADB command-byte fields: [addr:4][cmd:2][reg:2] (oracle adb-internal.h:
// ADB_BUSRESET 0x00, ADB_FLUSH 0x01, ADB_WRITEREG 0x08, ADB_READREG 0x0C).
#define ADB_CMD_SENDRESET 0x00
#define ADB_CMD_FLUSH     0x01
#define ADB_CMD_LISTEN    0x08
#define ADB_CMD_TALK      0x0C

// Listen R3 handler-id pseudo-values (oracle adb-internal.h ADB_CMD_*).
#define ADB_HID_SELF_TEST            0xFF
#define ADB_HID_CHANGE_ID            0xFE
#define ADB_HID_CHANGE_ID_AND_ACT    0xFD
#define ADB_HID_CHANGE_ID_AND_ENABLE 0x00

#define DEV_NONE  (-1)
#define DEV_KBD   0
#define DEV_MOUSE 1

static void set_defaults(ADBStub *a)
{
	// Oracle defaults: adb_kbd_reset (devaddr 2, handler 1 = Apple Standard
	// Keyboard); adb_mouse_reset (devaddr 3, handler 2 = Classic 200 cpi).
	a->kbd_addr = 2;   a->kbd_handler = 1;
	a->mouse_addr = 3; a->mouse_handler = 2;
}

void ADBStubReset(ADBStub *a)
{
	memset(a, 0, sizeof(*a));
	set_defaults(a);
}

static int dev_at(const ADBStub *a, uint8_t addr)
{
	if (addr == a->kbd_addr)   return DEV_KBD;
	if (addr == a->mouse_addr) return DEV_MOUSE;
	return DEV_NONE;
}

// Talk (READREG). reply is filled with DATA bytes only; returns the data length.
static int do_talk(ADBStub *a, int dev, uint8_t reg, uint8_t *reply, int reply_max)
{
	uint8_t data[2];
	int len = 0;
	switch (reg) {
	case 0:
		// THE deferral: no input ever flows over ADB in this stub (Talk R0
		// empty -> 0 = success, zero data; Cuda frames [ADB_PACKET, 0x00, cmd]).
		break;
	case 1:
		break;          // oracle: empty on both devices
	case 2:
		if (dev == DEV_KBD) {
			data[0] = 0x00;     // oracle adb-kbd.c: "XXX: check this"
			data[1] = 0x07;     // LED status
			len = 2;
		}                       // mouse: no case 2 in the oracle -> empty
		break;
	case 3:
		// Identification: [devaddr, handler]. After a Listen-R3 move this
		// echoes the NEW address (it reads live device state).
		if (dev == DEV_KBD) { data[0] = a->kbd_addr;   data[1] = a->kbd_handler; }
		else                { data[0] = a->mouse_addr; data[1] = a->mouse_handler; }
		len = 2;
		break;
	}
	if (len > reply_max)
		len = reply_max;        // defensive clamp; never write past the caller
	for (int i = 0; i < len; i++)
		reply[i] = data[i];
	return len;
}

// Listen (WRITEREG). Only R3 has state effects; R0/R1/R2 are accepted + ignored.
static int do_listen(ADBStub *a, int dev, uint8_t reg,
                     const uint8_t *data, int len)
{
	if (reg != 3)
		return 0;               // R0/R1/R2 sinks (R2 = kbd LED status)

	// Payload shape: [new_addr_byte, handler_id]. The oracle's mouse rejects any
	// reg-3 write whose length isn't exactly right (Mac OS 9 ADB-driver-bug guard,
	// adb-mouse.c "if (len != 3) return 0" — 3 = cmd byte + 2 data bytes; our len
	// counts data bytes only). The oracle's kbd has no guard; we still require the
	// two bytes we must read (no stale-buffer reads) and ignore extras.
	bool bad_len = (dev == DEV_MOUSE) ? (len != 2) : (len < 2);
	if (data == NULL || bad_len) {
		a->listens_ignored++;
		return 0;
	}

	uint8_t new_addr = data[0] & 0x0F;
	uint8_t hid = data[1];
	uint8_t *addr_p    = (dev == DEV_KBD) ? &a->kbd_addr    : &a->mouse_addr;
	uint8_t *handler_p = (dev == DEV_KBD) ? &a->kbd_handler : &a->mouse_handler;

	switch (hid) {
	case ADB_HID_SELF_TEST:
		break;                  // ignored
	case ADB_HID_CHANGE_ID:
	case ADB_HID_CHANGE_ID_AND_ACT:
	case ADB_HID_CHANGE_ID_AND_ENABLE:
		// Address move, UNCONDITIONAL per the oracle (no collision modeling —
		// see adb_stub.h header notes). Handler unchanged.
		*addr_p = new_addr;
		a->moves++;
		break;
	default:
		// Real handler id: move address AND change handler if supported
		// (oracle: kbd accepts 1/2/3, mouse accepts 1/2).
		*addr_p = new_addr;
		a->moves++;
		{
			bool ok = (dev == DEV_KBD) ? (hid == 1 || hid == 2 || hid == 3)
			                           : (hid == 1 || hid == 2);
			if (ok && *handler_p != hid) {
				*handler_p = hid;
				a->handler_changes++;
			}
		}
		break;
	}
	return 0;
}

int ADBStubCommand(ADBStub *a, uint8_t cmd,
                   const uint8_t *listen_data, int listen_len,
                   uint8_t *reply, int reply_max)
{
	uint8_t low = cmd & 0x0F;

	// SendReset is bus-wide (oracle do_adb_request: cmd == ADB_BUSRESET resets
	// every device regardless of the address nibble). Restores default
	// addresses/handlers; telemetry and autopoll plumbing are untouched.
	if (low == ADB_CMD_SENDRESET) {
		set_defaults(a);
		a->resets++;
		return 0;
	}

	uint8_t addr = cmd >> 4;
	int dev = dev_at(a, addr);
	uint8_t grp = cmd & 0x0C;   // command group bits

	if (grp == ADB_CMD_TALK) {
		if (dev == DEV_NONE) {
			a->absent_talks++;
			return -1;          // Cuda frames as timeout status 0x02
		}
		a->talks++;
		return do_talk(a, dev, cmd & 3, reply, reply_max);
	}

	if (dev == DEV_NONE) {      // Listen/Flush/reserved to an empty address
		a->absent_other++;
		return -1;
	}

	if (grp == ADB_CMD_LISTEN) {
		a->listens++;
		return do_listen(a, dev, cmd & 3, listen_data, listen_len);
	}

	if (low == ADB_CMD_FLUSH) {
		a->flushes++;           // nothing buffered to flush in the stub
		return 0;
	}

	// Reserved low nibbles 0x2/0x3: oracle devreq matches neither WRITEREG nor
	// READREG -> zero-length success.
	return 0;
}
